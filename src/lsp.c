#include "lsp.h"
#include "documents.h"
#include "docs.h"
#include "index.h"

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

/* Converts an LSP position (UTF-16 units) to a byte offset. */
static uint32_t position_to_offset(const ltls_LineIndex* index, const char* text, uint32_t line, uint32_t character)
{
	if (!index->starts || line >= index->count) return (uint32_t)strlen(text);

	uint32_t offset = index->starts[line];
	uint32_t units = 0;

	while (units < character)
	{
		unsigned char byte = (unsigned char)text[offset];
		if (!byte || byte == '\n') break;

		uint32_t width = 1;
		uint32_t added = 1;
		if (byte < 0x80) { width = 1; added = 1; }
		else if ((byte & 0xE0) == 0xC0) { width = 2; added = 1; }
		else if ((byte & 0xF0) == 0xE0) { width = 3; added = 1; }
		else if ((byte & 0xF8) == 0xF0) { width = 4; added = 2; }

		offset += width;
		units += added;
	}

	return offset;
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
   Hover and definition
   ------------------------------------------------------------------------- */

typedef struct {
	const ltls_Document* document;
	uint32_t line;
	uint32_t character;
} ltls_PositionContext;

typedef struct {
	char* text;
	size_t length;
	size_t capacity;
} ltls_Markdown;

typedef struct {
	lt_VM* vm;
	lt_Analysis analysis;
	ltls_Index index;
	ltls_LineIndex lines;
} ltls_AnalysisState;

static void markdown_append(ltls_Markdown* markdown, const char* text)
{
	if (!text) return;

	size_t length = strlen(text);
	if (markdown->length + length + 1 > markdown->capacity)
	{
		size_t capacity = markdown->capacity ? markdown->capacity : 64;
		while (capacity < markdown->length + length + 1) capacity *= 2;
		char* grown = realloc(markdown->text, capacity);
		if (!grown) return;
		markdown->text = grown;
		markdown->capacity = capacity;
	}
	memcpy(markdown->text + markdown->length, text, length + 1);
	markdown->length += length;
}

static void analysis_state_open(ltls_AnalysisState* state, const ltls_Document* document)
{
	memset(state, 0, sizeof(*state));
	state->vm = lt_open(malloc, free, ignore_error);
	if (!state->vm) return;

	lt_analysis_run(state->vm, &state->analysis, document->text, document->uri);
	ltls_index_build(&state->index, &state->analysis);
	line_index_build(&state->lines, document->text);
}

static void analysis_state_close(ltls_AnalysisState* state)
{
	if (!state->vm) return;

	ltls_index_free(&state->index);
	line_index_free(&state->lines);
	lt_analysis_free(state->vm, &state->analysis);
	lt_destroy(state->vm);
	state->vm = 0;
}

static yyjson_mut_val* range_object(yyjson_mut_doc* document, const ltls_LineIndex* lines,
                                    const char* text, lt_Span span)
{
	yyjson_mut_val* range = yyjson_mut_obj(document);
	yyjson_mut_obj_add_val(document, range, "start", position_object(document, lines, text, span.start));
	yyjson_mut_obj_add_val(document, range, "end", position_object(document, lines, text, span.end));
	return range;
}

static void markdown_append_docs(ltls_Markdown* markdown, const ltls_Doc* docs, const char* suppress_class)
{
	if (docs->description && docs->description[0])
	{
		markdown_append(markdown, "\n");
		markdown_append(markdown, docs->description);
		markdown_append(markdown, "\n");
	}

	if (docs->class_name && !(suppress_class && strcmp(suppress_class, docs->class_name) == 0))
	{
		markdown_append(markdown, "\nclass ");
		markdown_append(markdown, docs->class_name);
		if (docs->class_base)
		{
			markdown_append(markdown, " : ");
			markdown_append(markdown, docs->class_base);
		}
	}

	for (uint32_t i = 0; i < docs->field_count; i++)
	{
		const ltls_DocField* field = &docs->fields[i];
		markdown_append(markdown, "\n- `");
		markdown_append(markdown, field->name);
		markdown_append(markdown, "`");
		if (field->type)
		{
			markdown_append(markdown, " *");
			markdown_append(markdown, field->type);
			markdown_append(markdown, "*");
		}
		if (field->description && field->description[0])
		{
			markdown_append(markdown, " — ");
			markdown_append(markdown, field->description);
		}
	}

	for (uint32_t i = 0; i < docs->param_count; i++)
	{
		const ltls_DocParam* param = &docs->params[i];
		markdown_append(markdown, "\n- `");
		markdown_append(markdown, param->name);
		markdown_append(markdown, "`");
		if (param->type)
		{
			markdown_append(markdown, " *");
			markdown_append(markdown, param->type);
			markdown_append(markdown, "*");
		}
		if (param->default_value)
		{
			markdown_append(markdown, " default `");
			markdown_append(markdown, param->default_value);
			markdown_append(markdown, "`");
		}
		if (param->description && param->description[0])
		{
			markdown_append(markdown, " — ");
			markdown_append(markdown, param->description);
		}
	}

	for (uint32_t i = 0; i < docs->return_count; i++)
	{
		const ltls_DocReturn* entry = &docs->returns[i];
		markdown_append(markdown, "\n- returns");
		if (entry->type)
		{
			markdown_append(markdown, " *");
			markdown_append(markdown, entry->type);
			markdown_append(markdown, "*");
		}
		if (entry->name)
		{
			markdown_append(markdown, " `");
			markdown_append(markdown, entry->name);
			markdown_append(markdown, "`");
		}
		if (entry->description && entry->description[0])
		{
			markdown_append(markdown, " — ");
			markdown_append(markdown, entry->description);
		}
	}

	for (uint32_t i = 0; i < docs->tag_count; i++)
	{
		markdown_append(markdown, "\n\n");
		markdown_append(markdown, docs->tags[i]);
	}
}

static lt_Token* token_at(const lt_Analysis* analysis, uint32_t offset);
static const char* self_detail(const ltls_Index* index, lt_AstNode* self, char* buffer, size_t size);

static yyjson_mut_val* build_hover(yyjson_mut_doc* document, void* context)
{
	ltls_PositionContext* position = context;
	if (!position->document) return 0;

	ltls_AnalysisState state;
	analysis_state_open(&state, position->document);
	if (!state.vm) return 0;

	uint32_t offset = position_to_offset(&state.lines, position->document->text, position->line, position->character);

	lt_Token* hovered = token_at(&state.analysis, offset);
	const char* hovered_name = hovered ? token_name(&state.analysis, hovered) : 0;
	if (hovered_name && strcmp(hovered_name, "this") == 0)
	{
		lt_AstNode* self = ltls_index_self_at(&state.index, offset);
		if (self)
		{
			char detail[160];
			char line[200];
			snprintf(line, sizeof(line), "this: %s", self_detail(&state.index, self, detail, sizeof(detail)));

			ltls_Markdown markdown = { 0 };
			markdown_append(&markdown, "```little\n");
			markdown_append(&markdown, line);
			markdown_append(&markdown, "\n```\n");

			yyjson_mut_val* result = yyjson_mut_obj(document);
			yyjson_mut_val* contents = yyjson_mut_obj(document);
			yyjson_mut_obj_add_strcpy(document, contents, "kind", "markdown");
			yyjson_mut_obj_add_strcpy(document, contents, "value", markdown.text);
			yyjson_mut_obj_add_val(document, result, "contents", contents);
			yyjson_mut_obj_add_val(document, result, "range",
				range_object(document, &state.lines, position->document->text, hovered->span));

			free(markdown.text);
			analysis_state_close(&state);
			return result;
		}
	}

	const ltls_Reference* reference = 0;
	const ltls_Declaration* declaration = ltls_index_resolve(&state.index, offset, &reference);

	yyjson_mut_val* result = 0;
	if (declaration)
	{
		ltls_TypeSet override = (reference && !reference->member) ? reference->types : 0;
		char* signature = ltls_declaration_signature(&state.index, declaration, override);
		ltls_Doc docs;
		uint8_t has_docs = !declaration->from_doc && declaration->name->line > 0 &&
			ltls_doc_for_line(&state.analysis.tokens, declaration->name->line, &docs);

		ltls_Markdown markdown = { 0 };
		markdown_append(&markdown, "```little\n");
		markdown_append(&markdown, signature ? signature : "");
		markdown_append(&markdown, "\n```\n");
		if (has_docs)
		{
			const char* own_class = declaration->kind == LTLS_DECL_CLASS
				? ltls_index_token_text(&state.index, declaration->name) : 0;
			markdown_append_docs(&markdown, &docs, own_class);
		}
		if (declaration->detail)
		{
			markdown_append(&markdown, "\n\n");
			markdown_append(&markdown, declaration->detail);
		}
		if (!markdown.text) markdown_append(&markdown, "");

		result = yyjson_mut_obj(document);
		yyjson_mut_val* contents = yyjson_mut_obj(document);
		yyjson_mut_obj_add_strcpy(document, contents, "kind", "markdown");
		yyjson_mut_obj_add_strcpy(document, contents, "value", markdown.text);
		yyjson_mut_obj_add_val(document, result, "contents", contents);

		lt_Span highlight = (declaration->external && reference) ? reference->token->span : declaration->name->span;
		yyjson_mut_obj_add_val(document, result, "range",
			range_object(document, &state.lines, position->document->text, highlight));

		free(markdown.text);
		free(signature);
		if (has_docs) ltls_doc_free(&docs);
	}

	analysis_state_close(&state);
	return result;
}

static yyjson_mut_val* build_definition(yyjson_mut_doc* document, void* context)
{
	ltls_PositionContext* position = context;
	if (!position->document) return 0;

	ltls_AnalysisState state;
	analysis_state_open(&state, position->document);
	if (!state.vm) return 0;

	uint32_t offset = position_to_offset(&state.lines, position->document->text, position->line, position->character);
	const ltls_Declaration* declaration = ltls_index_resolve(&state.index, offset, 0);

	yyjson_mut_val* result = 0;
	if (declaration && !declaration->external && !(declaration->name->flags & LT_TOKEN_SYNTHETIC))
	{
		result = yyjson_mut_obj(document);
		yyjson_mut_obj_add_strcpy(document, result, "uri", position->document->uri);
		yyjson_mut_obj_add_val(document, result, "range",
			range_object(document, &state.lines, position->document->text, declaration->name->span));
	}

	analysis_state_close(&state);
	return result;
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
	yyjson_mut_obj_add_bool(document, capabilities, "hoverProvider", true);
	yyjson_mut_obj_add_bool(document, capabilities, "definitionProvider", true);
	yyjson_mut_obj_add_bool(document, capabilities, "referencesProvider", true);
	yyjson_mut_obj_add_bool(document, capabilities, "documentHighlightProvider", true);
	yyjson_mut_val* rename = yyjson_mut_obj(document);
	yyjson_mut_obj_add_bool(document, rename, "prepareProvider", true);
	yyjson_mut_obj_add_val(document, capabilities, "renameProvider", rename);

	yyjson_mut_val* completion = yyjson_mut_obj(document);
	yyjson_mut_obj_add_bool(document, completion, "resolveProvider", false);
	yyjson_mut_val* completion_triggers = yyjson_mut_arr(document);
	yyjson_mut_arr_add_strcpy(document, completion_triggers, ".");
	yyjson_mut_arr_add_strcpy(document, completion_triggers, ":");
	yyjson_mut_arr_add_strcpy(document, completion_triggers, "@");
	yyjson_mut_obj_add_val(document, completion, "triggerCharacters", completion_triggers);
	yyjson_mut_obj_add_val(document, capabilities, "completionProvider", completion);

	yyjson_mut_val* signature = yyjson_mut_obj(document);
	yyjson_mut_val* signature_triggers = yyjson_mut_arr(document);
	yyjson_mut_arr_add_strcpy(document, signature_triggers, "(");
	yyjson_mut_arr_add_strcpy(document, signature_triggers, ",");
	yyjson_mut_obj_add_val(document, signature, "triggerCharacters", signature_triggers);
	yyjson_mut_obj_add_val(document, capabilities, "signatureHelpProvider", signature);

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

/* ------------------------------------------------------------------------- */
/* Completion and signature help */

static uint8_t is_name_character(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '_';
}

static lt_Token* token_ending_at(const lt_Analysis* analysis, uint32_t offset)
{
	lt_Token* best = 0;
	for (uint32_t i = 0; i < analysis->tokens.token_buffer.length; i++)
	{
		lt_Token* token = LTLS_BUFFER_AT(lt_Token, analysis->tokens.token_buffer, i);
		if (token->flags & LT_TOKEN_SYNTHETIC) continue;
		if (token->span.end != offset) continue;
		if (!best || token->span.start > best->span.start) best = token;
	}
	return best;
}

static lt_Token* token_at(const lt_Analysis* analysis, uint32_t offset)
{
	lt_Token* best = 0;
	for (uint32_t i = 0; i < analysis->tokens.token_buffer.length; i++)
	{
		lt_Token* token = LTLS_BUFFER_AT(lt_Token, analysis->tokens.token_buffer, i);
		if (token->flags & LT_TOKEN_SYNTHETIC) continue;
		if (offset < token->span.start || offset >= token->span.end) continue;
		if (!best || token->span.start > best->span.start) best = token;
	}
	return best;
}

/* Names the shape `this` actually refers to when it is known: the class, or
   the table literal that owns a receiver function. */
static const char* self_detail(const ltls_Index* index, lt_AstNode* self, char* buffer, size_t size)
{
	if (self && self->type == LT_AST_NODE_CLASS)
	{
		const char* name = ltls_index_token_text(index, self->class_decl.identifier);
		snprintf(buffer, size, "%s", name && name[0] ? name : "class");
		return buffer;
	}
	if (self && self->type == LT_AST_NODE_TABLE)
	{
		snprintf(buffer, size, "table");
		return buffer;
	}
	snprintf(buffer, size, "the enclosing receiver");
	return buffer;
}

static uint8_t scope_contains_offset(lt_Scope* scope, uint32_t offset)
{
	if (!scope) return 0;
	if (!scope->last) return 1; /* the module scope covers trailing whitespace */

	uint32_t start = scope->start ? scope->start->span.start : 0;
	uint32_t end = (scope->end && scope->end > scope->start)
		? (scope->end - 1)->span.end : UINT32_MAX;
	return offset >= start && offset <= end;
}

static int completion_kind(const ltls_Declaration* declaration)
{
	switch (declaration->kind)
	{
	case LTLS_DECL_CLASS: return 7;      /* Class */
	case LTLS_DECL_FUNCTION: return 3;   /* Function */
	case LTLS_DECL_METHOD: return 2;     /* Method */
	case LTLS_DECL_FIELD: return 5;      /* Field */
	case LTLS_DECL_PROPERTY: return 10;  /* Property */
	default: return 6;                   /* Variable */
	}
}

static yyjson_mut_val* completion_item(yyjson_mut_doc* document, const ltls_Index* index,
                                       const ltls_Declaration* declaration)
{
	const char* name = ltls_index_token_text(index, declaration->name);
	if (!name) return 0;

	yyjson_mut_val* item = yyjson_mut_obj(document);
	yyjson_mut_obj_add_strcpy(document, item, "label", name);
	yyjson_mut_obj_add_int(document, item, "kind", completion_kind(declaration));

	char* signature = ltls_declaration_signature(index, declaration, 0);
	if (signature)
	{
		yyjson_mut_obj_add_strcpy(document, item, "detail", signature);
		free(signature);
	}
	if (declaration->detail)
		yyjson_mut_obj_add_strcpy(document, item, "documentation", declaration->detail);

	return item;
}

static uint8_t member_already_listed(const ltls_Index* index, const ltls_Declaration** list,
                                     uint32_t count, const ltls_Declaration* candidate)
{
	const char* name = ltls_index_token_text(index, candidate->name);
	for (uint32_t i = 0; i < count; i++)
	{
		const char* other = ltls_index_token_text(index, list[i]->name);
		if (name && other && strcmp(name, other) == 0) return 1;
	}
	return 0;
}

static uint32_t collect_members(const ltls_Index* index, ltls_TypeSet types,
                                const ltls_Declaration** out, uint32_t max)
{
	uint32_t count = 0;
	const ltls_Declaration* batch[512];

	for (ltls_TypeSet type = types; type; type = type->next)
	{
		uint32_t batch_count = 0;
		if (type->kind == LTLS_TYPE_INSTANCE || type->kind == LTLS_TYPE_CLASS)
			batch_count = ltls_index_members(index, type->node, batch, 512);
		else if (type->kind == LTLS_TYPE_TABLE)
			batch_count = ltls_index_table_members(index, type->node, batch, 512);

		for (uint32_t i = 0; i < batch_count && count < max; i++)
		{
			if (member_already_listed(index, out, count, batch[i])) continue;
			out[count++] = batch[i];
		}
	}
	return count;
}

static void add_matching_members(yyjson_mut_doc* document, const ltls_Index* index,
                                 const ltls_Declaration** members, uint32_t count,
                                 const char* prefix, size_t prefix_length, yyjson_mut_val* items)
{
	for (uint32_t i = 0; i < count; i++)
	{
		const char* name = ltls_index_token_text(index, members[i]->name);
		if (!name || strncmp(name, prefix, prefix_length) != 0) continue;

		yyjson_mut_val* item = completion_item(document, index, members[i]);
		if (item) yyjson_mut_arr_add_val(items, item);
	}
}

static const ltls_Declaration* declaration_named(const ltls_Index* index, const char* name, size_t length)
{
	for (uint32_t i = 0; i < index->declaration_count; i++)
	{
		const ltls_Declaration* declaration = &index->declarations[i];
		if (declaration->kind == LTLS_DECL_FIELD || declaration->kind == LTLS_DECL_METHOD ||
		    declaration->kind == LTLS_DECL_PROPERTY) continue;
		const char* candidate = ltls_index_token_text(index, declaration->name);
		if (candidate && strlen(candidate) == length && strncmp(candidate, name, length) == 0)
			return declaration;
	}
	return 0;
}

static yyjson_mut_val* build_completion(yyjson_mut_doc* document, void* context)
{
	ltls_PositionContext* position = context;
	if (!position->document) return 0;

	ltls_AnalysisState state;
	analysis_state_open(&state, position->document);
	if (!state.vm) return 0;

	const char* text = position->document->text;
	uint32_t offset = position_to_offset(&state.lines, text, position->line, position->character);

	uint32_t start = offset;
	while (start > 0 && is_name_character(text[start - 1])) start--;
	const char* prefix = text + start;
	size_t prefix_length = offset - start;
	char separator = start > 0 ? text[start - 1] : 0;

	yyjson_mut_val* items = yyjson_mut_arr(document);

	if (separator == '.' || separator == ':' || separator == '@')
	{
		ltls_TypeSet types = 0;
		lt_AstNode* self_node = 0;

		if (separator == '@')
		{
			lt_AstNode* receiver = ltls_index_receiver_at(&state.index, offset);
			if (receiver && (receiver->type == LT_AST_NODE_CLASS || receiver->type == LT_AST_NODE_TABLE))
				self_node = receiver;
			else if (receiver) types = ltls_index_type_of(&state.index, receiver);
		}
		else
		{
			lt_Token* token = token_ending_at(&state.analysis, start - 1);
			if (token)
			{
				const char* receiver = token_name(&state.analysis, token);
				if (receiver && strcmp(receiver, "this") == 0)
					self_node = ltls_index_self_at(&state.index, offset);
				else
				{
					const ltls_Reference* reference = 0;
					const ltls_Declaration* declaration =
						ltls_index_resolve(&state.index, token->span.start, &reference);
					types = declaration ? declaration->types : (reference ? reference->types : 0);
				}
			}

			if (!types && !self_node)
			{
				uint32_t begin = start - 1;
				while (begin > 0 && is_name_character(text[begin - 1])) begin--;
				if (begin + 1 < start)
				{
					const char* name = text + begin;
					size_t length = (size_t)(start - 1 - begin);
					if (length == 4 && strncmp(name, "this", 4) == 0)
						self_node = ltls_index_self_at(&state.index, offset);
					else
					{
						const ltls_Declaration* declaration = declaration_named(&state.index, name, length);
						if (declaration) types = declaration->types;
					}
				}
			}
		}

		const ltls_Declaration* members[512];
		uint32_t count = !self_node ? collect_members(&state.index, types, members, 512)
			: self_node->type == LT_AST_NODE_TABLE
				? ltls_index_table_members(&state.index, self_node, members, 512)
				: ltls_index_members(&state.index, self_node, members, 512);
		add_matching_members(document, &state.index, members, count, prefix, prefix_length, items);
	}
	else
	{
		const char* claimed[512];
		uint32_t claimed_count = 0;
		lt_AstNode* self_node = ltls_index_self_at(&state.index, offset);

		for (uint32_t i = 0; i < state.index.declaration_count; i++)
		{
			const ltls_Declaration* declaration = &state.index.declarations[i];
			const char* name = ltls_index_token_text(&state.index, declaration->name);
			if (!name || strncmp(name, prefix, prefix_length) != 0) continue;

			if (declaration->external)
			{
				if (declaration->kind != LTLS_DECL_GLOBAL) continue;
			}
			else
			{
				if (declaration->kind == LTLS_DECL_FIELD || declaration->kind == LTLS_DECL_METHOD ||
				    declaration->kind == LTLS_DECL_PROPERTY) continue;
				if ((declaration->name->flags & LT_TOKEN_SYNTHETIC) && !declaration->from_doc) continue;
				if ((declaration->kind == LTLS_DECL_LOCAL || declaration->kind == LTLS_DECL_PARAM) &&
				    !scope_contains_offset(declaration->scope, offset)) continue;
				if (declaration->name->span.start > offset) continue;
			}

			uint8_t duplicate = 0;
			for (uint32_t j = 0; j < claimed_count; j++)
				if (strcmp(claimed[j], name) == 0) { duplicate = 1; break; }
			if (duplicate) continue;
			if (claimed_count < 512) claimed[claimed_count++] = name;

			if (self_node && strcmp(name, "this") == 0)
			{
				char detail[160];
				yyjson_mut_val* item = yyjson_mut_obj(document);
				yyjson_mut_obj_add_strcpy(document, item, "label", name);
				yyjson_mut_obj_add_int(document, item, "kind", 6);
				yyjson_mut_obj_add_strcpy(document, item, "detail",
					self_detail(&state.index, self_node, detail, sizeof(detail)));
				yyjson_mut_arr_add_val(items, item);
				continue;
			}

			yyjson_mut_val* item = completion_item(document, &state.index, declaration);
			if (item) yyjson_mut_arr_add_val(items, item);
		}

		static const char* keywords[] = {
			"and", "async", "await", "break", "class", "constructor", "else", "elseif",
			"extends", "false", "fn", "for", "get", "global", "if", "import", "in",
			"is", "isnt", "not", "null", "or", "override", "private", "public",
			"return", "set", "super", "true", "type", "typeof", "var", "while", "with",
			0
		};
		for (uint32_t i = 0; keywords[i]; i++)
		{
			if (strncmp(keywords[i], prefix, prefix_length) != 0) continue;

			uint8_t duplicate = 0;
			for (uint32_t j = 0; j < claimed_count; j++)
				if (strcmp(claimed[j], keywords[i]) == 0) { duplicate = 1; break; }
			if (duplicate) continue;
			if (claimed_count < 512) claimed[claimed_count++] = keywords[i];

			yyjson_mut_val* item = yyjson_mut_obj(document);
			yyjson_mut_obj_add_strcpy(document, item, "label", keywords[i]);
			yyjson_mut_obj_add_int(document, item, "kind", 14);
			yyjson_mut_arr_add_val(items, item);
		}

		if (self_node && strncmp("this", prefix, prefix_length) == 0)
		{
			uint8_t duplicate = 0;
			for (uint32_t j = 0; j < claimed_count; j++)
				if (strcmp(claimed[j], "this") == 0) { duplicate = 1; break; }

			if (!duplicate)
			{
				if (claimed_count < 512) claimed[claimed_count++] = "this";

				char detail[160];
				yyjson_mut_val* item = yyjson_mut_obj(document);
				yyjson_mut_obj_add_strcpy(document, item, "label", "this");
				yyjson_mut_obj_add_int(document, item, "kind", 14);
				yyjson_mut_obj_add_strcpy(document, item, "detail",
					self_detail(&state.index, self_node, detail, sizeof(detail)));
				yyjson_mut_arr_add_val(items, item);
			}
		}
	}

	yyjson_mut_val* result = yyjson_mut_obj(document);
	yyjson_mut_obj_add_bool(document, result, "isIncomplete", false);
	yyjson_mut_obj_add_val(document, result, "items", items);

	analysis_state_close(&state);
	return result;
}

typedef struct {
	char bracket;
	uint32_t open;
	uint32_t commas;
} ltls_Bracket;

static uint8_t find_active_call(const char* text, uint32_t offset, uint32_t* open, uint32_t* commas)
{
	ltls_Bracket stack[64];
	uint32_t depth = 0;

	for (uint32_t i = 0; i < offset; i++)
	{
		char c = text[i];
		if (c == '"' || c == '\'')
		{
			for (i++; i < offset && text[i] != c; i++)
				if (text[i] == '\\') i++;
			continue;
		}
		if (c == ';')
		{
			while (i < offset && text[i] != '\n') i++;
			continue;
		}
		if (c == '(' || c == '[' || c == '{')
		{
			if (depth < 64)
			{
				stack[depth].bracket = c;
				stack[depth].open = i;
				stack[depth].commas = 0;
				depth++;
			}
		}
		else if (c == ')' || c == ']' || c == '}')
		{
			if (depth) depth--;
		}
		else if (c == ',' && depth && stack[depth - 1].bracket == '(')
		{
			stack[depth - 1].commas++;
		}
	}

	if (!depth || stack[depth - 1].bracket != '(') return 0;
	*open = stack[depth - 1].open;
	*commas = stack[depth - 1].commas;
	return 1;
}

static uint32_t flush_parameter(char* current, uint32_t length, char names[][64],
                                uint32_t count, uint32_t max)
{
	while (length && (current[length - 1] == ' ' || current[length - 1] == ']')) length--;

	uint32_t leading = 0;
	while (leading < length && (current[leading] == ' ' || current[leading] == '[')) leading++;

	uint32_t size = length - leading;
	if (size && count < max)
	{
		memcpy(names[count], current + leading, size);
		names[count][size] = 0;
		count++;
	}
	return count;
}

static uint32_t split_parameters(const char* label, char names[][64], uint32_t max)
{
	const char* open = strchr(label, '(');
	const char* close = open ? strrchr(label, ')') : 0;
	if (!open || !close || close < open) return 0;

	uint32_t count = 0;
	uint32_t length = 0;
	char current[64];
	uint8_t bracket = 0;

	for (const char* cursor = open + 1; cursor < close; cursor++)
	{
		char c = *cursor;
		if (c == '[') bracket++;
		else if (c == ']' && bracket) bracket--;

		if (c == ',' && !bracket)
		{
			count = flush_parameter(current, length, names, count, max);
			length = 0;
			continue;
		}

		if (length < sizeof(current) - 1) current[length++] = c;
	}

	return flush_parameter(current, length, names, count, max);
}

static const char* parameter_doc(const char* detail, const char* name)
{
	if (!detail || !name) return 0;

	size_t name_length = strlen(name);
	for (const char* cursor = detail; (cursor = strstr(cursor, "\n- ")) != 0; cursor += 3)
	{
		const char* entry = cursor + 3;
		if (strncmp(entry, name, name_length) != 0 || entry[name_length] != ':') continue;
		const char* value = entry + name_length + 1;
		while (*value == ' ') value++;
		return *value ? value : 0;
	}
	return 0;
}

static yyjson_mut_val* build_signature_help(yyjson_mut_doc* document, void* context)
{
	ltls_PositionContext* position = context;
	if (!position->document) return 0;

	ltls_AnalysisState state;
	analysis_state_open(&state, position->document);
	if (!state.vm) return 0;

	const char* text = position->document->text;
	uint32_t offset = position_to_offset(&state.lines, text, position->line, position->character);
	uint32_t open = 0, commas = 0;

	yyjson_mut_val* result = 0;
	if (find_active_call(text, offset, &open, &commas))
	{
		uint32_t callee_end = open;
		while (callee_end > 0 && (text[callee_end - 1] == ' ' || text[callee_end - 1] == '\t')) callee_end--;

		lt_Token* token = token_ending_at(&state.analysis, callee_end);
		const ltls_Declaration* declaration = token
			? ltls_index_resolve(&state.index, token->span.start, 0) : 0;

		if (declaration)
		{
			char* signature = ltls_declaration_signature(&state.index, declaration, 0);
			if (signature)
			{
				char names[32][64];
				uint32_t count = split_parameters(signature, names, 32);

				yyjson_mut_val* parameters = yyjson_mut_arr(document);
				for (uint32_t i = 0; i < count; i++)
				{
					yyjson_mut_val* parameter = yyjson_mut_obj(document);
					yyjson_mut_obj_add_strcpy(document, parameter, "label", names[i]);
					const char* doc = parameter_doc(declaration->detail, names[i]);
					if (doc)
					{
						char text[256];
						size_t length = 0;
						while (doc[length] && doc[length] != '\n' && length < sizeof(text) - 1)
							text[length] = doc[length], length++;
						text[length] = 0;
						yyjson_mut_obj_add_strcpy(document, parameter, "documentation", text);
					}
					yyjson_mut_arr_add_val(parameters, parameter);
				}

				yyjson_mut_val* entry = yyjson_mut_obj(document);
				yyjson_mut_obj_add_strcpy(document, entry, "label", signature);
				yyjson_mut_obj_add_val(document, entry, "parameters", parameters);

				yyjson_mut_val* signatures = yyjson_mut_arr(document);
				yyjson_mut_arr_add_val(signatures, entry);

				result = yyjson_mut_obj(document);
				yyjson_mut_obj_add_val(document, result, "signatures", signatures);
				yyjson_mut_obj_add_int(document, result, "activeSignature", 0);
				uint32_t active = count ? (commas < count ? commas : count - 1) : commas;
				yyjson_mut_obj_add_int(document, result, "activeParameter", active);

				free(signature);
			}
		}
	}

	analysis_state_close(&state);
	return result;
}

/* ------------------------------------------------------------------------- */
/* References, highlights and rename */

typedef struct {
	ltls_PositionContext position;
	const char* new_name;
	uint8_t include_declaration;
} ltls_SymbolContext;

typedef struct {
	ltls_Reference* items[2048];
	uint32_t count;
} ltls_ReferenceList;

static uint8_t declaration_renameable(const ltls_Index* index, const ltls_Declaration* declaration)
{
	if (!declaration) return 0;
	if (declaration->external) return 0;
	if (declaration->is_global) return 0;
	if (declaration->name->flags & LT_TOKEN_SYNTHETIC) return 0;

	const char* name = ltls_index_token_text(index, declaration->name);
	return name && strcmp(name, "this") != 0;
}

/* A member reached through a union receiver may resolve to more than one
   declaration; renaming one of them would be a guess. */
static uint8_t reference_ambiguous(const ltls_Index* index, const ltls_Reference* reference)
{
	if (!reference->member || !reference->receiver_expr) return 0;

	ltls_TypeSet types = ltls_index_type_of(index, reference->receiver_expr);
	if (!types) return 1;

	uint32_t matches = 0;
	for (ltls_TypeSet type = types; type; type = type->next)
	{
		const ltls_Declaration* declaration = 0;
		if (type->kind == LTLS_TYPE_INSTANCE)
			declaration = ltls_index_member(index, type->node, reference->token);
		else if (type->kind == LTLS_TYPE_TABLE)
			declaration = ltls_index_table_field(index, type->node, reference->token);
		if (declaration) matches++;
	}
	return matches > 1;
}

static void collect_references(const ltls_Index* index, const ltls_Declaration* target, ltls_ReferenceList* list)
{
	list->count = 0;
	for (uint32_t i = 0; i < index->reference_count; i++)
	{
		ltls_Reference* reference = &index->references[i];
		if (reference->token->flags & LT_TOKEN_SYNTHETIC) continue;
		if (ltls_index_resolve(index, reference->token->span.start, 0) != target) continue;
		if (list->count < sizeof(list->items) / sizeof(list->items[0])) list->items[list->count++] = reference;
	}
}

static uint8_t declaration_blocks_rename(const ltls_Index* index, const ltls_Declaration* declaration, ltls_ReferenceList* list)
{
	if (!declaration_renameable(index, declaration)) return 1;
	collect_references(index, declaration, list);
	for (uint32_t i = 0; i < list->count; i++)
		if (reference_ambiguous(index, list->items[i])) return 1;
	return 0;
}

static yyjson_mut_val* location_object(yyjson_mut_doc* document, const ltls_LineIndex* lines,
                                       const char* text, const char* uri, lt_Span span)
{
	yyjson_mut_val* location = yyjson_mut_obj(document);
	yyjson_mut_obj_add_strcpy(document, location, "uri", uri);
	yyjson_mut_obj_add_val(document, location, "range", range_object(document, lines, text, span));
	return location;
}

static yyjson_mut_val* build_references(yyjson_mut_doc* document, void* context)
{
	ltls_SymbolContext* symbol = context;
	ltls_PositionContext* position = &symbol->position;
	if (!position->document) return 0;

	ltls_AnalysisState state;
	analysis_state_open(&state, position->document);
	if (!state.vm) return 0;

	const char* text = position->document->text;
	uint32_t offset = position_to_offset(&state.lines, text, position->line, position->character);
	const ltls_Declaration* declaration = ltls_index_resolve(&state.index, offset, 0);

	yyjson_mut_val* locations = yyjson_mut_arr(document);
	if (declaration && !declaration->external)
	{
		ltls_ReferenceList list;
		collect_references(&state.index, declaration, &list);

		if (symbol->include_declaration)
			yyjson_mut_arr_add_val(locations,
				location_object(document, &state.lines, text, position->document->uri, declaration->name->span));

		for (uint32_t i = 0; i < list.count; i++)
			yyjson_mut_arr_add_val(locations,
				location_object(document, &state.lines, text, position->document->uri, list.items[i]->token->span));
	}

	analysis_state_close(&state);
	return locations;
}

static yyjson_mut_val* build_highlights(yyjson_mut_doc* document, void* context)
{
	ltls_PositionContext* position = context;
	if (!position->document) return 0;

	ltls_AnalysisState state;
	analysis_state_open(&state, position->document);
	if (!state.vm) return 0;

	const char* text = position->document->text;
	uint32_t offset = position_to_offset(&state.lines, text, position->line, position->character);
	const ltls_Declaration* declaration = ltls_index_resolve(&state.index, offset, 0);

	yyjson_mut_val* highlights = yyjson_mut_arr(document);
	if (declaration && !declaration->external)
	{
		ltls_ReferenceList list;
		collect_references(&state.index, declaration, &list);

		uint32_t count = 0;
		lt_Span spans[2049];
		spans[count++] = declaration->name->span;
		for (uint32_t i = 0; i < list.count && count < 2049; i++) spans[count++] = list.items[i]->token->span;

		for (uint32_t i = 0; i < count; i++)
		{
			yyjson_mut_val* highlight = yyjson_mut_obj(document);
			yyjson_mut_obj_add_val(document, highlight, "range",
				range_object(document, &state.lines, text, spans[i]));
			yyjson_mut_obj_add_int(document, highlight, "kind", 1); /* Text */
			yyjson_mut_arr_add_val(highlights, highlight);
		}
	}

	analysis_state_close(&state);
	return highlights;
}

static uint8_t valid_name(const char* name)
{
	if (!name || !name[0]) return 0;
	for (const char* cursor = name; *cursor; cursor++)
	{
		if ((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') ||
		    (*cursor >= '0' && *cursor <= '9') || *cursor == '_') continue;
		return 0;
	}
	return 1;
}

static yyjson_mut_val* build_prepare_rename(yyjson_mut_doc* document, void* context)
{
	ltls_PositionContext* position = context;
	if (!position->document) return 0;

	ltls_AnalysisState state;
	analysis_state_open(&state, position->document);
	if (!state.vm) return 0;

	const char* text = position->document->text;
	uint32_t offset = position_to_offset(&state.lines, text, position->line, position->character);
	const ltls_Declaration* declaration = ltls_index_resolve(&state.index, offset, 0);

	yyjson_mut_val* result = 0;
	if (declaration && !declaration->external && declaration_renameable(&state.index, declaration))
	{
		ltls_ReferenceList list;
		if (declaration_blocks_rename(&state.index, declaration, &list) == 0)
		{
			const char* name = ltls_index_token_text(&state.index, declaration->name);
			result = yyjson_mut_obj(document);
			yyjson_mut_obj_add_val(document, result, "range",
				range_object(document, &state.lines, text, declaration->name->span));
			yyjson_mut_obj_add_strcpy(document, result, "placeholder", name ? name : "");
		}
	}

	analysis_state_close(&state);
	return result;
}

static yyjson_mut_val* build_rename(yyjson_mut_doc* document, void* context)
{
	ltls_SymbolContext* symbol = context;
	ltls_PositionContext* position = &symbol->position;
	if (!position->document || !valid_name(symbol->new_name)) return 0;

	ltls_AnalysisState state;
	analysis_state_open(&state, position->document);
	if (!state.vm) return 0;

	const char* text = position->document->text;
	uint32_t offset = position_to_offset(&state.lines, text, position->line, position->character);
	const ltls_Declaration* declaration = ltls_index_resolve(&state.index, offset, 0);

	yyjson_mut_val* result = 0;
	if (declaration && !declaration->external)
	{
		ltls_ReferenceList list;
		if (declaration_blocks_rename(&state.index, declaration, &list) == 0)
		{
			yyjson_mut_val* edits = yyjson_mut_arr(document);
			yyjson_mut_val* declaration_edit = yyjson_mut_obj(document);
			yyjson_mut_obj_add_val(document, declaration_edit, "range",
				range_object(document, &state.lines, text, declaration->name->span));
			yyjson_mut_obj_add_strcpy(document, declaration_edit, "newText", symbol->new_name);
			yyjson_mut_arr_add_val(edits, declaration_edit);

			for (uint32_t i = 0; i < list.count; i++)
			{
				yyjson_mut_val* edit = yyjson_mut_obj(document);
				yyjson_mut_obj_add_val(document, edit, "range",
					range_object(document, &state.lines, text, list.items[i]->token->span));
				yyjson_mut_obj_add_strcpy(document, edit, "newText", symbol->new_name);
				yyjson_mut_arr_add_val(edits, edit);
			}

			yyjson_mut_val* changes = yyjson_mut_obj(document);
			yyjson_mut_obj_add_val(document, changes, position->document->uri, edits);

			result = yyjson_mut_obj(document);
			yyjson_mut_obj_add_val(document, result, "changes", changes);
		}
	}

	analysis_state_close(&state);
	return result;
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
	else if (strcmp(method, "textDocument/hover") == 0 || strcmp(method, "textDocument/definition") == 0)
	{
		yyjson_val* document = params ? yyjson_obj_get(params, "textDocument") : 0;
		const char* uri = object_string(document, "uri");
		yyjson_val* position = params ? yyjson_obj_get(params, "position") : 0;
		ltls_PositionContext context;
		context.document = uri ? ltls_documents_find(documents, uri) : 0;
		context.line = (uint32_t)object_int(position, "line");
		context.character = (uint32_t)object_int(position, "character");

		if (id)
			send_message(id, method[13] == 'h' ? build_hover : build_definition, &context);
	}
	else if (strcmp(method, "textDocument/completion") == 0 || strcmp(method, "textDocument/signatureHelp") == 0)
	{
		yyjson_val* document = params ? yyjson_obj_get(params, "textDocument") : 0;
		const char* uri = object_string(document, "uri");
		yyjson_val* position = params ? yyjson_obj_get(params, "position") : 0;
		ltls_PositionContext context;
		context.document = uri ? ltls_documents_find(documents, uri) : 0;
		context.line = (uint32_t)object_int(position, "line");
		context.character = (uint32_t)object_int(position, "character");

		if (id)
			send_message(id, method[13] == 'c' ? build_completion : build_signature_help, &context);
	}
	else if (strcmp(method, "textDocument/references") == 0 || strcmp(method, "textDocument/rename") == 0)
	{
		yyjson_val* document = params ? yyjson_obj_get(params, "textDocument") : 0;
		const char* uri = object_string(document, "uri");
		yyjson_val* position = params ? yyjson_obj_get(params, "position") : 0;
		ltls_SymbolContext context;
		context.position.document = uri ? ltls_documents_find(documents, uri) : 0;
		context.position.line = (uint32_t)object_int(position, "line");
		context.position.character = (uint32_t)object_int(position, "character");
		context.new_name = object_string(params, "newName");
		context.include_declaration = 0;
		if (params)
		{
			yyjson_val* details = yyjson_obj_get(params, "context");
			yyjson_val* include = details ? yyjson_obj_get(details, "includeDeclaration") : 0;
			context.include_declaration = include && yyjson_get_bool(include);
		}

		if (id)
			send_message(id, strcmp(method, "textDocument/references") == 0 ? build_references : build_rename, &context);
	}
	else if (strcmp(method, "textDocument/documentHighlight") == 0 || strcmp(method, "textDocument/prepareRename") == 0)
	{
		yyjson_val* document = params ? yyjson_obj_get(params, "textDocument") : 0;
		const char* uri = object_string(document, "uri");
		yyjson_val* position = params ? yyjson_obj_get(params, "position") : 0;
		ltls_PositionContext context;
		context.document = uri ? ltls_documents_find(documents, uri) : 0;
		context.line = (uint32_t)object_int(position, "line");
		context.character = (uint32_t)object_int(position, "character");

		if (id)
			send_message(id, strcmp(method, "textDocument/documentHighlight") == 0 ? build_highlights : build_prepare_rename, &context);
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
