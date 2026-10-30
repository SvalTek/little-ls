#include "lsp.h"
#include "documents.h"

#include "little.h"
#include "yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LTLS_NAME "little-ls"
#define LTLS_VERSION "0.1.0"

/* LSP symbol kinds. */
#define LTLS_SYMBOL_CLASS 5
#define LTLS_SYMBOL_METHOD 6
#define LTLS_SYMBOL_PROPERTY 7
#define LTLS_SYMBOL_FIELD 8
#define LTLS_SYMBOL_CONSTRUCTOR 9
#define LTLS_SYMBOL_FUNCTION 12
#define LTLS_SYMBOL_VARIABLE 13

#define LTLS_BUFFER_AT(type, buffer, index) \
	((type*)((char*)(buffer).data + (buffer).element_size * (index)))

#ifdef _WIN32
#define ltls_strnicmp _strnicmp
#else
#define ltls_strnicmp strncasecmp
#endif

typedef struct {
	uint32_t* starts;
	uint32_t count;
} ltls_LineIndex;

static void ignore_error(lt_VM* vm, const char* message)
{
	(void)vm;
	(void)message;
}

/* -------------------------------------------------------------------------
   Transport
   ------------------------------------------------------------------------- */

static char* read_message(size_t* length)
{
	size_t content_length = (size_t)-1;

	for (;;)
	{
		size_t capacity = 128, line_length = 0;
		char* line = malloc(capacity);
		if (!line) return 0;

		int character;
		int saw_any = 0;
		while ((character = fgetc(stdin)) != EOF)
		{
			saw_any = 1;
			if (character == '\n') break;
			if (character == '\r') continue;

			if (line_length + 1 >= capacity)
			{
				capacity *= 2;
				char* grown = realloc(line, capacity);
				if (!grown) { free(line); return 0; }
				line = grown;
			}
			line[line_length++] = (char)character;
		}

		if (character == EOF)
		{
			free(line);
			return 0;
		}
		if (!saw_any)
		{
			free(line);
			return 0;
		}

		line[line_length] = 0;
		if (line_length == 0) { free(line); break; }

		if (line_length > 15 && (ltls_strnicmp(line, "Content-Length:", 15) == 0))
		{
			content_length = (size_t)strtoul(line + 15, 0, 10);
		}

		free(line);
	}

	if (content_length == (size_t)-1) return 0;

	char* body = malloc(content_length + 1);
	if (!body) return 0;

	size_t read = 0;
	while (read < content_length)
	{
		size_t got = fread(body + read, 1, content_length - read, stdin);
		if (got == 0) { free(body); return 0; }
		read += got;
	}

	body[content_length] = 0;
	*length = content_length;
	return body;
}

static void write_json(yyjson_mut_doc* document)
{
	size_t length = 0;
	char* text = yyjson_mut_write(document, 0, &length);
	if (!text) return;

	printf("Content-Length: %zu\r\n\r\n", length);
	fwrite(text, 1, length, stdout);
	fflush(stdout);
	free(text);
}

static void send_message(yyjson_val* id, yyjson_mut_val* (*build)(yyjson_mut_doc*, void*), void* context)
{
	yyjson_mut_doc* document = yyjson_mut_doc_new(0);
	if (!document) return;

	yyjson_mut_val* root = yyjson_mut_obj(document);
	yyjson_mut_obj_add_str(document, root, "jsonrpc", "2.0");
	yyjson_mut_obj_add_val(document, root, "id", yyjson_val_mut_copy(document, id));

	yyjson_mut_val* result = build(document, context);
	if (result) yyjson_mut_obj_add_val(document, root, "result", result);
	else yyjson_mut_obj_add_null(document, root, "result");

	yyjson_mut_doc_set_root(document, root);
	write_json(document);
	yyjson_mut_doc_free(document);
}

static void send_error(yyjson_val* id, int code, const char* message)
{
	yyjson_mut_doc* document = yyjson_mut_doc_new(0);
	if (!document) return;

	yyjson_mut_val* root = yyjson_mut_obj(document);
	yyjson_mut_obj_add_str(document, root, "jsonrpc", "2.0");
	yyjson_mut_obj_add_val(document, root, "id", yyjson_val_mut_copy(document, id));

	yyjson_mut_val* error = yyjson_mut_obj(document);
	yyjson_mut_obj_add_int(document, error, "code", code);
	yyjson_mut_obj_add_str(document, error, "message", message);
	yyjson_mut_obj_add_val(document, root, "error", error);

	yyjson_mut_doc_set_root(document, root);
	write_json(document);
	yyjson_mut_doc_free(document);
}

/* -------------------------------------------------------------------------
   Positions
   ------------------------------------------------------------------------- */

static void line_index_build(ltls_LineIndex* index, const char* text)
{
	uint32_t capacity = 32;
	uint32_t* starts = malloc(capacity * sizeof(uint32_t));
	if (!starts)
	{
		index->starts = 0;
		index->count = 0;
		return;
	}

	starts[0] = 0;
	uint32_t count = 1;

	for (uint32_t i = 0; text[i]; i++)
	{
		if (text[i] != '\n') continue;

		if (count == capacity)
		{
			capacity *= 2;
			uint32_t* grown = realloc(starts, capacity * sizeof(uint32_t));
			if (!grown) break;
			starts = grown;
		}
		starts[count++] = i + 1;
	}

	index->starts = starts;
	index->count = count;
}

static void line_index_free(ltls_LineIndex* index)
{
	free(index->starts);
	index->starts = 0;
	index->count = 0;
}

/* Counts UTF-16 code units between two byte offsets on one line. */
static uint32_t utf16_units(const char* text, uint32_t start, uint32_t end)
{
	uint32_t units = 0;
	uint32_t i = start;

	while (i < end)
	{
		unsigned char byte = (unsigned char)text[i];
		uint32_t width;

		if (byte < 0x80) { width = 1; units += 1; }
		else if ((byte & 0xE0) == 0xC0) { width = 2; units += 1; }
		else if ((byte & 0xF0) == 0xE0) { width = 3; units += 1; }
		else if ((byte & 0xF8) == 0xF0) { width = 4; units += 2; }
		else { width = 1; units += 1; }

		i += width;
	}

	return units;
}

static void offset_to_position(const ltls_LineIndex* index, const char* text, uint32_t offset,
                               uint32_t* line, uint32_t* character)
{
	*line = 0;
	*character = 0;

	if (!index->starts || index->count == 0) return;

	uint32_t low = 0, high = index->count;
	while (low + 1 < high)
	{
		uint32_t middle = (low + high) / 2;
		if (index->starts[middle] <= offset) low = middle;
		else high = middle;
	}

	*line = low;
	*character = utf16_units(text, index->starts[low], offset);
}

static yyjson_mut_val* position_object(yyjson_mut_doc* document, const ltls_LineIndex* index,
                                       const char* text, uint32_t offset)
{
	uint32_t line, character;
	offset_to_position(index, text, offset, &line, &character);

	yyjson_mut_val* position = yyjson_mut_obj(document);
	yyjson_mut_obj_add_uint(document, position, "line", line);
	yyjson_mut_obj_add_uint(document, position, "character", character);
	return position;
}

/* -------------------------------------------------------------------------
   Diagnostics
   ------------------------------------------------------------------------- */

static void publish_diagnostics(const char* uri, int64_t version, const char* text)
{
	/* A fresh VM per analysis bounds the memory left behind by the parser's
	   error-recovery paths, which can abandon partially built buffers. */
	lt_VM* vm = lt_open(malloc, free, ignore_error);
	if (!vm) return;

	lt_Analysis analysis;
	lt_analysis_run(vm, &analysis, text, uri);

	ltls_LineIndex index;
	line_index_build(&index, text);
	uint32_t text_length = (uint32_t)strlen(text);

	yyjson_mut_doc* document = yyjson_mut_doc_new(0);
	if (!document) { line_index_free(&index); lt_analysis_free(vm, &analysis); return; }

	yyjson_mut_val* root = yyjson_mut_obj(document);
	yyjson_mut_obj_add_str(document, root, "jsonrpc", "2.0");
	yyjson_mut_obj_add_str(document, root, "method", "textDocument/publishDiagnostics");

	yyjson_mut_val* params = yyjson_mut_obj(document);
	yyjson_mut_obj_add_strcpy(document, params, "uri", uri);
	yyjson_mut_obj_add_int(document, params, "version", version);

	yyjson_mut_val* list = yyjson_mut_arr(document);
	for (uint32_t i = 0; i < lt_diagnostic_count(vm); i++)
	{
		const lt_Diagnostic* diagnostic = lt_diagnostic_get(vm, i);

		uint32_t start = diagnostic->span.start;
		uint32_t end = diagnostic->span.end;
		if (start > text_length) start = text_length;
		if (end > text_length) end = text_length;
		if (end <= start)
		{
			if (start == text_length && start > 0) start--;
			end = start < text_length ? start + 1 : text_length;
		}

		yyjson_mut_val* range = yyjson_mut_obj(document);
		yyjson_mut_obj_add_val(document, range, "start", position_object(document, &index, text, start));
		yyjson_mut_obj_add_val(document, range, "end", position_object(document, &index, text, end));

		yyjson_mut_val* entry = yyjson_mut_obj(document);
		yyjson_mut_obj_add_val(document, entry, "range", range);
		yyjson_mut_obj_add_int(document, entry, "severity", diagnostic->severity);
		yyjson_mut_obj_add_strcpy(document, entry, "source", "little");
		yyjson_mut_obj_add_strcpy(document, entry, "message", diagnostic->message);
		yyjson_mut_arr_add_val(list, entry);
	}

	yyjson_mut_obj_add_val(document, params, "diagnostics", list);
	yyjson_mut_obj_add_val(document, root, "params", params);
	yyjson_mut_doc_set_root(document, root);
	write_json(document);
	yyjson_mut_doc_free(document);

	line_index_free(&index);
	lt_analysis_free(vm, &analysis);
	lt_destroy(vm);
}

/* -------------------------------------------------------------------------
   Document symbols
   ------------------------------------------------------------------------- */

static const char* token_name(lt_Analysis* analysis, lt_Token* token)
{
	if (!token) return 0;
	if (token->type != LT_TOKEN_IDENTIFIER && token->type != LT_TOKEN_CONSTRUCTOR) return 0;
	if (token->idx >= analysis->tokens.identifier_buffer.length) return 0;
	return LTLS_BUFFER_AT(lt_Identifier, analysis->tokens.identifier_buffer, token->idx)->name;
}

static lt_Span span_union(lt_Span a, lt_Span b)
{
	lt_Span span = a;
	if (b.start < span.start) span.start = b.start;
	if (b.end > span.end) span.end = b.end;
	return span;
}

static void add_symbol(yyjson_mut_doc* document, yyjson_mut_val* list, const ltls_LineIndex* index,
                       const char* text, const char* name, int kind, lt_Token* name_token,
                       lt_Span span, yyjson_mut_val* children)
{
	if (!name || !name_token) return;
	if (span.end <= span.start) span = name_token->span;

	/* The range must enclose the selection range or clients drop the symbol. */
	if (name_token->span.start < span.start) span.start = name_token->span.start;
	if (name_token->span.end > span.end) span.end = name_token->span.end;

	yyjson_mut_val* symbol = yyjson_mut_obj(document);
	yyjson_mut_obj_add_strcpy(document, symbol, "name", name);
	yyjson_mut_obj_add_int(document, symbol, "kind", kind);

	yyjson_mut_val* range = yyjson_mut_obj(document);
	yyjson_mut_obj_add_val(document, range, "start", position_object(document, index, text, span.start));
	yyjson_mut_obj_add_val(document, range, "end", position_object(document, index, text, span.end));
	yyjson_mut_obj_add_val(document, symbol, "range", range);

	yyjson_mut_val* selection = yyjson_mut_obj(document);
	yyjson_mut_obj_add_val(document, selection, "start", position_object(document, index, text, name_token->span.start));
	yyjson_mut_obj_add_val(document, selection, "end", position_object(document, index, text, name_token->span.end));
	yyjson_mut_obj_add_val(document, symbol, "selectionRange", selection);

	if (children) yyjson_mut_obj_add_val(document, symbol, "children", children);

	yyjson_mut_arr_add_val(list, symbol);
}

static void emit_body_symbols(yyjson_mut_doc* document, yyjson_mut_val* list, lt_Analysis* analysis,
                              const ltls_LineIndex* index, const char* text, lt_Buffer* body)
{
	for (uint32_t i = 0; i < body->length; i++)
	{
		lt_AstNode* node = *LTLS_BUFFER_AT(lt_AstNode*, *body, i);
		if (!node) continue;

		if (node->type == LT_AST_NODE_DECLARE)
		{
			int kind = (node->declare.expr && node->declare.expr->type == LT_AST_NODE_FN)
				? LTLS_SYMBOL_FUNCTION : LTLS_SYMBOL_VARIABLE;
			add_symbol(document, list, index, text, token_name(analysis, node->declare.identifier),
				kind, node->declare.identifier, node->span, 0);
		}
		else if (node->type == LT_AST_NODE_CLASS)
		{
			yyjson_mut_val* children = yyjson_mut_arr(document);
			lt_Buffer* members = &node->class_decl.members;

			for (uint32_t m = 0; m < members->length; m++)
			{
				lt_ClassMember* member = LTLS_BUFFER_AT(lt_ClassMember, *members, m);

				int kind = LTLS_SYMBOL_FIELD;
				switch (member->type)
				{
				case LT_CLASS_METHOD: kind = LTLS_SYMBOL_METHOD; break;
				case LT_CLASS_CONSTRUCTOR: kind = LTLS_SYMBOL_CONSTRUCTOR; break;
				case LT_CLASS_GETTER: case LT_CLASS_SETTER: kind = LTLS_SYMBOL_PROPERTY; break;
				default: kind = LTLS_SYMBOL_FIELD; break;
				}

				lt_Span span = member->name ? member->name->span : (lt_Span){ 0, 0 };
				if (member->value) span = span_union(span, member->value->span);
				add_symbol(document, children, index, text, token_name(analysis, member->name),
					kind, member->name, span, 0);
			}

			add_symbol(document, list, index, text, token_name(analysis, node->class_decl.identifier),
				LTLS_SYMBOL_CLASS, node->class_decl.identifier, node->span, children);
		}
	}
}

/* -------------------------------------------------------------------------
   Requests
   ------------------------------------------------------------------------- */

static yyjson_mut_val* build_initialize(yyjson_mut_doc* document, void* context)
{
	(void)context;

	yyjson_mut_val* result = yyjson_mut_obj(document);

	yyjson_mut_val* capabilities = yyjson_mut_obj(document);
	yyjson_mut_obj_add_int(document, capabilities, "textDocumentSync", 1);
	yyjson_mut_obj_add_bool(document, capabilities, "documentSymbolProvider", true);
	yyjson_mut_obj_add_val(document, result, "capabilities", capabilities);

	yyjson_mut_val* info = yyjson_mut_obj(document);
	yyjson_mut_obj_add_str(document, info, "name", LTLS_NAME);
	yyjson_mut_obj_add_str(document, info, "version", LTLS_VERSION);
	yyjson_mut_obj_add_val(document, result, "serverInfo", info);

	return result;
}

static yyjson_mut_val* build_shutdown(yyjson_mut_doc* document, void* context)
{
	(void)document;
	(void)context;
	return 0;
}

static yyjson_mut_val* build_document_symbols(yyjson_mut_doc* document, void* context)
{
	ltls_Document* target = context;

	yyjson_mut_val* list = yyjson_mut_arr(document);
	if (!target) return list;

	lt_VM* vm = lt_open(malloc, free, ignore_error);
	if (!vm) return list;

	lt_Analysis analysis;
	lt_analysis_run(vm, &analysis, target->text, target->uri);

	ltls_LineIndex index;
	line_index_build(&index, target->text);
	if (analysis.parser.root)
		emit_body_symbols(document, list, &analysis, &index, target->text, &analysis.parser.root->chunk.body);
	line_index_free(&index);

	lt_analysis_free(vm, &analysis);
	lt_destroy(vm);
	return list;
}

static const char* object_string(yyjson_val* object, const char* key)
{
	yyjson_val* value = yyjson_obj_get(object, key);
	return yyjson_is_str(value) ? yyjson_get_str(value) : 0;
}

static int64_t object_int(yyjson_val* object, const char* key)
{
	yyjson_val* value = yyjson_obj_get(object, key);
	return yyjson_is_int(value) ? yyjson_get_sint(value) : 0;
}

static void handle_message(ltls_Documents* documents, char* body, size_t length,
                           int* shutting_down)
{
	yyjson_doc* message = yyjson_read(body, length, 0);
	if (!message) return;

	yyjson_val* root = yyjson_doc_get_root(message);
	yyjson_val* id = yyjson_obj_get(root, "id");
	yyjson_val* params = yyjson_obj_get(root, "params");
	const char* method = object_string(root, "method");

	if (!method) { yyjson_doc_free(message); return; }

	if (strcmp(method, "initialize") == 0)
	{
		if (id) send_message(id, build_initialize, 0);
	}
	else if (strcmp(method, "shutdown") == 0)
	{
		*shutting_down = 1;
		if (id) send_message(id, build_shutdown, 0);
	}
	else if (strcmp(method, "exit") == 0)
	{
		yyjson_doc_free(message);
		exit(*shutting_down ? 0 : 1);
	}
	else if (strcmp(method, "textDocument/didOpen") == 0)
	{
		yyjson_val* document = params ? yyjson_obj_get(params, "textDocument") : 0;
		const char* uri = object_string(document, "uri");
		const char* text = object_string(document, "text");
		if (uri && text)
		{
			int64_t version = object_int(document, "version");
			if (ltls_documents_set(documents, uri, text, version))
				publish_diagnostics(uri, version, text);
		}
	}
	else if (strcmp(method, "textDocument/didChange") == 0)
	{
		yyjson_val* document = params ? yyjson_obj_get(params, "textDocument") : 0;
		const char* uri = object_string(document, "uri");
		yyjson_val* changes = params ? yyjson_obj_get(params, "contentChanges") : 0;
		yyjson_val* change = changes ? yyjson_arr_get(changes, 0) : 0;
		const char* text = change ? object_string(change, "text") : 0;

		if (uri && text)
		{
			int64_t version = object_int(document, "version");
			if (ltls_documents_set(documents, uri, text, version))
				publish_diagnostics(uri, version, text);
		}
	}
	else if (strcmp(method, "textDocument/didClose") == 0)
	{
		yyjson_val* document = params ? yyjson_obj_get(params, "textDocument") : 0;
		const char* uri = object_string(document, "uri");
		if (uri)
		{
			ltls_documents_remove(documents, uri);
			publish_diagnostics(uri, 0, "");
		}
	}
	else if (strcmp(method, "textDocument/documentSymbol") == 0)
	{
		yyjson_val* document = params ? yyjson_obj_get(params, "textDocument") : 0;
		const char* uri = object_string(document, "uri");
		void* context = uri ? ltls_documents_find(documents, uri) : 0;
		if (id) send_message(id, build_document_symbols, context);
	}
	else if (id)
	{
		send_error(id, -32601, "Method not found");
	}

	yyjson_doc_free(message);
}

void ltls_serve_stdio(void)
{
	ltls_Documents documents = { 0 };
	int shutting_down = 0;

	for (;;)
	{
		size_t length = 0;
		char* body = read_message(&length);
		if (!body) break;

		handle_message(&documents, body, length, &shutting_down);
		free(body);
	}

	ltls_documents_clear(&documents);
}
