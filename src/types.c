/* Type inference for tooling.

   Two layers:
   - a weak map: every declaration accumulates the union of all values it is
     ever assigned. Captured locals, globals and fields only ever use this;
   - a flow environment for the current function: private (uncaptured) locals
     get strong updates, so a use sees the value assigned before it rather
     than every value the name ever held.

   Branches merge with union, loops iterate to a fixed point. The module pass
   repeats until the weak maps and function returns stop growing, which settles
   forward references and recursion. */
#include "index.h"
#include "docs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdio.h>

#define AT(type, buffer, index) \
	((type*)((char*)(buffer).data + (buffer).element_size * (index)))

typedef struct {
	ltls_Index* index;
	const lt_Analysis* analysis;
	ltls_TypeSet* env;
	lt_Scope* frame;
	lt_AstNode* function;
	const char* const* generics;
	uint32_t generic_count;
	uint8_t verify;
	uint8_t changed;
} ltls_Context;

#define token_text ltls_index_token_text

static uint32_t declaration_index(const ltls_Index* index, const ltls_Declaration* declaration)
{
	return (uint32_t)(declaration - index->declarations);
}

static ltls_Declaration* declaration_for_token(const ltls_Index* index, lt_Token* token)
{
	for (uint32_t i = 0; i < index->declaration_count; i++)
	{
		if (index->declarations[i].name == token) return &index->declarations[i];
	}
	return 0;
}

static ltls_Reference* reference_for_node(const ltls_Index* index, lt_AstNode* node, uint8_t member)
{
	for (uint32_t i = 0; i < index->reference_count; i++)
	{
		ltls_Reference* reference = &index->references[i];
		if (reference->node == node && reference->member == member) return reference;
	}
	return 0;
}

static lt_AstNode* find_class(const ltls_Index* index, const char* name)
{
	for (uint32_t i = 0; i < index->declaration_count; i++)
	{
		const ltls_Declaration* declaration = &index->declarations[i];
		if (declaration->kind != LTLS_DECL_CLASS || !declaration->node) continue;

		const char* candidate = token_text(index, declaration->name);
		if (candidate && strcmp(candidate, name) == 0) return declaration->node;
	}
	return 0;
}

static ltls_TypeSet annotation_type(ltls_Context* context, const char* type);
static ltls_TypeSet annotation_generic(ltls_Context* context, char* text);
static ltls_TypeSet annotation_segment(ltls_Context* context, char* text);

/* Base<A, B> with balanced argument splitting. */
static ltls_TypeSet annotation_generic(ltls_Context* context, char* text)
{
	char* open = strchr(text, '<');
	if (!open) return 0;
	char* close = strrchr(open, '>');
	if (!close || close[1] != 0) return 0;

	*open = 0;
	char* base = text;
	while (*base == ' ' || *base == '\t') base++;
	char* base_end = open;
	while (base_end > base && (base_end[-1] == ' ' || base_end[-1] == '\t')) base_end--;
	*base_end = 0;

	lt_AstNode* klass = find_class(context->index, base);
	if (!klass) return 0;

	uint32_t generic_count = 0;
	ltls_index_generics(context->index, klass, &generic_count);

	ltls_TypeSet args[16];
	uint32_t count = 0;
	int depth = 0;
	char* start = open + 1;

	for (char* cursor = open + 1; cursor <= close; cursor++)
	{
		char c = *cursor;
		if (cursor < close)
		{
			if (c == '<' || c == '{' || c == '[') depth++;
			else if (c == '>' || c == '}' || c == ']') { if (depth) depth--; }
			if (depth || c != ',') continue;
		}

		char saved = *cursor;
		*cursor = 0;
		char* segment = start;
		while (*segment == ' ' || *segment == '\t') segment++;
		char* segment_end = start + strlen(start);
		while (segment_end > segment && (segment_end[-1] == ' ' || segment_end[-1] == '\t')) *--segment_end = 0;
		if (*segment && count < 16) args[count++] = annotation_type(context, segment);
		if (cursor < close) *cursor = saved;
		start = cursor + 1;
	}

	if (!generic_count || count != generic_count) return 0;
	return ltls_type_instance(context->index, klass, args, count);
}

static ltls_TypeSet annotation_segment(ltls_Context* context, char* text)
{
	while (*text == ' ' || *text == '\t') text++;
	size_t length = strlen(text);
	while (length && (text[length - 1] == ' ' || text[length - 1] == '\t')) text[--length] = 0;
	if (length == 0) return 0;

	if (strcmp(text, "?") == 0 || strcmp(text, "any") == 0 || strcmp(text, "unknown") == 0) return 0;
	for (uint32_t i = 0; i < context->generic_count; i++)
		if (context->generics[i] && strcmp(text, context->generics[i]) == 0)
			return ltls_type_param(context->index, text);

	if (strchr(text, '<')) return annotation_generic(context, text);

	size_t suffix = strlen(text);
	if (suffix > 2 && text[suffix - 1] == ']' && text[suffix - 2] == '[')
	{
		text[suffix - 2] = 0;
		ltls_TypeSet element = annotation_segment(context, text);
		return element ? ltls_type_array(context->index, element) : 0;
	}

	if (text[0] == '{')
	{
		lt_AstNode* shape = ltls_index_shape(context->index, text);
		return shape ? ltls_type_add(context->index, 0, LTLS_TYPE_TABLE, shape) : 0;
	}

	uint8_t kind = LTLS_TYPE_UNKNOWN;
	lt_AstNode* node = 0;

	if (strcmp(text, "number") == 0) kind = LTLS_TYPE_NUMBER;
	else if (strcmp(text, "string") == 0) kind = LTLS_TYPE_STRING;
	else if (strcmp(text, "bool") == 0 || strcmp(text, "boolean") == 0) kind = LTLS_TYPE_BOOL;
	else if (strcmp(text, "nil") == 0 || strcmp(text, "null") == 0) kind = LTLS_TYPE_NULL;
	else if (strcmp(text, "table") == 0) kind = LTLS_TYPE_TABLE;
	else if (strcmp(text, "array") == 0) kind = LTLS_TYPE_ARRAY;
	else if (strcmp(text, "fn") == 0 || strcmp(text, "function") == 0) kind = LTLS_TYPE_FUNCTION;
	else
	{
		lt_AstNode* structure = ltls_index_struct(context->index, text);
		if (structure)
		{
			kind = LTLS_TYPE_TABLE;
			node = structure;
		}
		else
		{
			lt_AstNode* klass = find_class(context->index, text);
			if (!klass) return 0;
			kind = LTLS_TYPE_INSTANCE;
			node = klass;
		}
	}

	return ltls_type_add(context->index, 0, kind, node);
}

/* `A | B`, `?`, class names, primitives. */
static ltls_TypeSet annotation_type(ltls_Context* context, const char* type)
{
	if (!type) return 0;

	char buffer[512];
	size_t length = strlen(type);
	if (length == 0 || length >= sizeof(buffer)) return 0;
	memcpy(buffer, type, length + 1);

	ltls_TypeSet result = 0;
	char* segment = buffer;
	int depth = 0;
	int angle = 0;
	for (char* position = buffer; ; position++)
	{
		if (*position == '{') depth++;
		else if (*position == '}' && depth) depth--;
		else if (*position == '<') angle++;
		else if (*position == '>' && angle) angle--;

		if (depth || angle) continue;
		if (*position != '|' && *position != 0) continue;

		char saved = *position;
		*position = 0;
		result = ltls_type_union(context->index, result, annotation_segment(context, segment));
		if (saved == 0) break;
		segment = position + 1;
	}
	return result;
}

/* `@type T` above a line describes the annotated value; `@class` defines. */
static ltls_TypeSet annotated_type(ltls_Context* context, uint32_t line)
{
	ltls_Doc doc;
	if (!context->analysis || !ltls_doc_for_line(&context->analysis->tokens, line, &doc)) return 0;

	ltls_TypeSet result = 0;
	for (uint32_t i = 0; i < doc.tag_count; i++)
	{
		const char* tag = doc.tags[i];
		if (strncmp(tag, "@type ", 6) == 0)
			result = ltls_type_union(context->index, result, annotation_type(context, tag + 6));
	}

	ltls_doc_free(&doc);
	return result;
}

static uint8_t declaration_is_captured(const ltls_Declaration* declaration)
{
	if (!declaration->scope) return 1;

	lt_Scope* scope = declaration->scope;
	for (uint32_t i = 0; i < scope->locals.length; i++)
	{
		lt_Token* local = AT(lt_Token, scope->locals, i);
		if (local->span.start != declaration->name->span.start || local->span.end != declaration->name->span.end)
			continue;

		uint16_t slot = *AT(uint16_t, scope->local_slots, i);
		lt_Scope* frame = scope->frame;
		for (uint32_t j = 0; j < frame->captured.length; j++)
			if (*AT(uint16_t, frame->captured, j) == slot) return 1;
		return 0;
	}

	return 1;
}

static uint8_t declaration_is_private(const ltls_Declaration* declaration)
{
	if (declaration->is_global) return 0;
	if (declaration->kind != LTLS_DECL_LOCAL && declaration->kind != LTLS_DECL_PARAM) return 0;
	if (!declaration->scope) return 0;
	return !declaration_is_captured(declaration);
}

static uint8_t declaration_is_live(const ltls_Context* context, const ltls_Declaration* declaration)
{
	return declaration->scope && declaration->scope->frame == context->frame;
}

static ltls_TypeSet read_declaration(ltls_Context* context, const ltls_Declaration* declaration)
{
	if (declaration_is_private(declaration) && declaration_is_live(context, declaration))
		return context->env[declaration_index(context->index, declaration)];
	return declaration->types;
}

static uint8_t write_declaration(ltls_Context* context, ltls_Declaration* declaration, ltls_TypeSet types)
{
	if (declaration_is_private(declaration) && declaration_is_live(context, declaration))
		context->env[declaration_index(context->index, declaration)] = types;

	/* An explicit annotation is authoritative: assignments are checked
	   against it instead of widening it. */
	if (declaration->annotation) return 0;

	ltls_TypeSet before = declaration->types;
	declaration->types = ltls_type_union(context->index, declaration->types, types);
	if (!ltls_type_equals(before, declaration->types))
	{
		context->changed = 1;
		return 1;
	}
	return 0;
}

static ltls_TypeSet* env_copy(const ltls_Context* context, const ltls_TypeSet* source)
{
	uint32_t count = context->index->declaration_count;
	ltls_TypeSet* env = malloc(sizeof(ltls_TypeSet) * (count ? count : 1));
	if (env && count) memcpy(env, source, sizeof(ltls_TypeSet) * count);
	return env;
}

static uint8_t env_merge(ltls_Context* context, ltls_TypeSet* destination, const ltls_TypeSet* source)
{
	uint8_t changed = 0;
	for (uint32_t i = 0; i < context->index->declaration_count; i++)
	{
		if (ltls_type_equals(destination[i], source[i])) continue;
		destination[i] = ltls_type_union(context->index, destination[i], source[i]);
		changed = 1;
	}
	return changed;
}

static ltls_TypeSet analyze_expression(ltls_Context* context, lt_AstNode* node);
static void analyze_statement(ltls_Context* context, lt_AstNode* node);
static void analyze_body(ltls_Context* context, lt_Buffer* body);

static lt_Token* member_token(lt_AstNode* idx)
{
	if (!idx) return 0;
	if (idx->type == LT_AST_NODE_LITERAL) return idx->literal.token;
	if (idx->type == LT_AST_NODE_IDENTIFIER) return idx->identifier.token;
	return 0;
}

static uint8_t types_compatible(ltls_Index* index, ltls_TypeSet expected, ltls_TypeSet value, uint8_t depth);

/* A class materialized from `@class` (synthetic name, no real declaration in
   source) describes an object shape rather than a nominal runtime class. */
static uint8_t class_is_virtual(ltls_Index* index, lt_AstNode* class_node)
{
	(void)index;
	if (!class_node || class_node->type != LT_AST_NODE_CLASS) return 0;
	lt_Token* name = class_node->class_decl.identifier;
	if (!name) return 1;
	return (name->flags & LT_TOKEN_SYNTHETIC) != 0;
}

static const ltls_Declaration* owner_member(ltls_Index* index, lt_AstNode* owner, const char* name)
{
	for (uint32_t i = 0; i < index->declaration_count; i++)
	{
		const ltls_Declaration* declaration = &index->declarations[i];
		if (declaration->node != owner) continue;
		if (declaration->kind < LTLS_DECL_FIELD || declaration->kind > LTLS_DECL_PROPERTY) continue;

		const char* candidate = token_text(index, declaration->name);
		if (candidate && strcmp(candidate, name) == 0) return declaration;
	}
	return 0;
}

/* Every field described by the expected shape must exist on the actual table
   or instance with a compatible type. Extra fields are allowed. */
static uint8_t structural_match(ltls_Index* index, lt_AstNode* expected, lt_AstNode* actual, uint8_t depth)
{
	if (depth > 4 || !expected || !actual) return 1;

	const ltls_Declaration* members[256];
	uint32_t count = expected->type == LT_AST_NODE_CLASS
		? ltls_index_members(index, expected, members, 256)
		: ltls_index_table_members(index, expected, members, 256);

	for (uint32_t i = 0; i < count; i++)
	{
		const ltls_Declaration* member = members[i];
		const char* name = token_text(index, member->name);
		if (!name) continue;

		const ltls_Declaration* actual_member = owner_member(index, actual, name);
		if (!actual_member)
		{
			if (member->optional) continue;
			return 0;
		}
		if (member->types && actual_member->types &&
		    !types_compatible(index, member->types, actual_member->types, depth + 1)) return 0;
	}
	return 1;
}

static uint8_t type_accepts(ltls_Index* index, ltls_TypeSet expected, ltls_TypeSet value, uint8_t depth)
{
	if (expected->kind == LTLS_TYPE_PARAM) return 1;

	if (expected->kind != value->kind)
	{
		if (expected->kind == LTLS_TYPE_TABLE)
		{
			if (!expected->node) return value->kind == LTLS_TYPE_TABLE || value->kind == LTLS_TYPE_INSTANCE;
			if (value->kind == LTLS_TYPE_TABLE || value->kind == LTLS_TYPE_INSTANCE)
				return structural_match(index, expected->node, value->node, depth + 1);
			return 0;
		}

		if (expected->kind == LTLS_TYPE_INSTANCE && expected->node &&
		    class_is_virtual(index, expected->node) &&
		    (value->kind == LTLS_TYPE_TABLE || value->kind == LTLS_TYPE_INSTANCE))
			return structural_match(index, expected->node, value->node, depth + 1);

		return 0;
	}

	switch (expected->kind)
	{
	case LTLS_TYPE_TABLE:
		if (!expected->node) return 1;
		if (value->node == expected->node) return 1;
		if (expected->node->type == LT_AST_NODE_TABLE && value->node)
			return structural_match(index, expected->node, value->node, depth + 1);
		return 0;

	case LTLS_TYPE_ARRAY:
		if (!expected->node) return 1;
		if (value->node == expected->node) return 1;
		return types_compatible(index,
			ltls_index_array_element(index, expected->node),
			ltls_index_array_element(index, value->node), depth + 1);

	case LTLS_TYPE_FUNCTION:
		return !expected->node || value->node == expected->node;

	case LTLS_TYPE_INSTANCE:
		if (!expected->node) return 1;
		if (value->node == expected->node) return 1;
		if (class_is_virtual(index, expected->node))
			return structural_match(index, expected->node, value->node, depth + 1);
		return 0;

	case LTLS_TYPE_PARAM:
		return 1;

	default:
		return value->node == expected->node;
	}
}

/* Exact matching first; then wildcard/structural acceptance. */
static uint8_t types_compatible(ltls_Index* index, ltls_TypeSet expected, ltls_TypeSet value, uint8_t depth)
{
	if (!expected || !value || depth > 8) return 1;

	for (ltls_TypeSet type = expected; type; type = type->next)
		if (type->kind == LTLS_TYPE_PARAM) return 1;

	for (ltls_TypeSet actual = value; actual; actual = actual->next)
	{
		if (actual->kind == LTLS_TYPE_PARAM) return 1;

		uint8_t accepted = 0;
		for (ltls_TypeSet candidate = expected; candidate; candidate = candidate->next)
		{
			if (candidate->kind != actual->kind) continue;
			if (candidate->kind == LTLS_TYPE_PARAM || candidate->node == actual->node)
			{
				accepted = 1;
				break;
			}
		}

		if (!accepted)
			for (ltls_TypeSet candidate = expected; candidate; candidate = candidate->next)
				if (type_accepts(index, candidate, actual, depth)) { accepted = 1; break; }

		if (!accepted) return 0;
	}
	return 1;
}

static uint8_t type_includes(ltls_Index* index, ltls_TypeSet expected, ltls_TypeSet value)
{
	return types_compatible(index, expected, value, 0);
}

static void report_mismatch(ltls_Context* context, lt_Span span, ltls_TypeSet expected,
                            ltls_TypeSet actual, const char* what)
{
	if (!context->verify || !expected || !actual) return;
	if (type_includes(context->index, expected, actual)) return;

	char expected_text[128], actual_text[128], message[320];
	ltls_type_describe(context->index, expected, expected_text, sizeof(expected_text));
	ltls_type_describe(context->index, actual, actual_text, sizeof(actual_text));
	snprintf(message, sizeof(message), "%s: expected %s, got %s", what, expected_text, actual_text);
	ltls_index_add_type_diagnostic(context->index, span, 2, message);
}

static void check_arguments(ltls_Context* context, lt_AstNode* fn, lt_AstNode* call, uint8_t start,
                            uint32_t argument_count, ltls_TypeSet* argument_types,
                            const char* const* names, ltls_TypeSet* bindings, uint32_t generic_count,
                            const char* label)
{
	if (!context->verify || !fn) return;

	for (uint32_t i = 0; i < argument_count; i++)
	{
		lt_Token* parameter = fn->fn.args[start + i];
		if (!parameter) break;
		ltls_Declaration* declaration = declaration_for_token(context->index, parameter);
		if (!declaration || !declaration->types) continue;

		ltls_TypeSet expected = declaration->types;
		if (generic_count && names && bindings)
			expected = ltls_index_substitute(context->index, expected, names, bindings, generic_count);

		lt_Span span = call->call.args[i] ? call->call.args[i]->span : call->span;
		char what[160];
		snprintf(what, sizeof(what), "argument %u of %s", i + 1, label ? label : "function");
		report_mismatch(context, span, expected, argument_types[i], what);
	}
}

static ltls_TypeSet member_type(ltls_Context* context, ltls_TypeSet receiver, lt_Token* member)
{
	ltls_TypeSet result = 0;
	for (ltls_TypeSet type = receiver; type; type = type->next)
	{
		const ltls_Declaration* declaration = 0;
		ltls_TypeSet value = 0;

		if (type->kind == LTLS_TYPE_INSTANCE)
		{
			lt_AstNode* base = ltls_index_class_base(context->index, type->node);
			declaration = ltls_index_member(context->index, base, member);
			if (declaration)
			{
				uint32_t argument_count = 0;
				const ltls_TypeSet* arguments = ltls_index_type_args(context->index, type, &argument_count);
				uint32_t generic_count = 0;
				const char* const* names = ltls_index_generics(context->index, base, &generic_count);
				if (arguments && names && argument_count == generic_count)
					value = ltls_index_substitute(context->index, declaration->types,
						names, arguments, argument_count);
			}
		}
		else if (type->kind == LTLS_TYPE_TABLE)
		{
			declaration = ltls_index_table_field(context->index, type->node, member);
		}

		if (declaration) result = ltls_type_union(context->index, result,
			value ? value : declaration->types);
	}
	return result;
}

#define LTLS_TUPLE_MAX 16

static lt_Token* callee_token(lt_AstNode* callee)
{
	if (!callee) return 0;
	if (callee->type == LT_AST_NODE_IDENTIFIER) return callee->identifier.token;
	if (callee->type == LT_AST_NODE_INDEX) return member_token(callee->index.idx);
	return 0;
}

static uint8_t is_import_expression(ltls_Context* context, lt_AstNode* expression)
{
	if (!expression || expression->type != LT_AST_NODE_CALL) return 0;
	lt_Token* token = callee_token(expression->call.callee);
	const char* name = token ? token_text(context->index, token) : 0;
	return name && strcmp(name, "import") == 0;
}

static const ltls_Declaration* call_declaration(ltls_Context* context, lt_AstNode* call)
{
	if (!call || call->type != LT_AST_NODE_CALL) return 0;
	lt_Token* token = callee_token(call->call.callee);
	return token ? ltls_index_resolve(context->index, token->span.start, 0) : 0;
}

static uint8_t declaration_is_unpack(const ltls_Declaration* declaration)
{
	return declaration && declaration->external && declaration->signature &&
	       strncmp(declaration->signature, "fn unpack(", 10) == 0;
}

static char* literal_string(ltls_Context* context, lt_AstNode* node)
{
	if (!node || node->type != LT_AST_NODE_LITERAL || !node->literal.token) return 0;
	lt_Token* token = node->literal.token;
	if (token->type != LT_TOKEN_STRING_LITERAL) return 0;
	if (token->idx >= context->index->tokens->literal_buffer.length) return 0;

	lt_Literal* literal = AT(lt_Literal, context->index->tokens->literal_buffer, token->idx);
	if (!literal->string) return 0;

	size_t length = strlen(literal->string);
	char* copy = malloc(length + 1);
	if (copy) memcpy(copy, literal->string, length + 1);
	return copy;
}

static ltls_TypeSet array_element_types(ltls_Context* context, ltls_TypeSet types)
{
	ltls_TypeSet element = 0;
	for (ltls_TypeSet type = types; type; type = type->next)
		if (type->kind == LTLS_TYPE_ARRAY)
			element = ltls_type_union(context->index, element,
				ltls_index_array_element(context->index, type->node));
	return element;
}

/* The values `unpack(array)` produces: positional when the argument is an
   array literal, otherwise a homogeneous element with unknown arity. */
static void unpack_stream(ltls_Context* context, lt_AstNode* call, ltls_TypeSet* element,
                          ltls_TypeSet* items, uint32_t* count, uint8_t* known)
{
	*element = 0;
	*count = 0;
	*known = 0;

	lt_AstNode* argument = call->call.args[0];
	if (!argument) return;

	*element = array_element_types(context, ltls_index_type_of(context->index, argument));
	if (argument->type != LT_AST_NODE_ARRAY) return;

	uint32_t length = argument->array.values.length;
	if (length > LTLS_TUPLE_MAX) return;

	*known = 1;
	*count = length;
	for (uint32_t i = 0; i < length; i++)
	{
		lt_AstNode* value = *AT(lt_AstNode*, argument->array.values, i);
		items[i] = ltls_index_type_of(context->index, value);
		*element = ltls_type_union(context->index, *element, items[i]);
	}
}

/* Return stream of a call that forwards multiple values: `unpack(...)` or a
   call to a function already known to produce several values. */
static uint8_t call_stream(ltls_Context* context, lt_AstNode* call, ltls_TypeSet* element,
                           ltls_TypeSet* items, uint32_t* count, uint8_t* known)
{
	*element = 0;
	*count = 0;
	*known = 0;

	const ltls_Declaration* declaration = call_declaration(context, call);
	if (declaration_is_unpack(declaration))
	{
		unpack_stream(context, call, element, items, count, known);
		return 1;
	}

	lt_AstNode* fn = declaration ? ltls_index_function_node(declaration) : 0;
	const ltls_Tuple* tuple = fn ? ltls_index_tuple(context->index, fn) : 0;
	if (tuple)
	{
		*element = tuple->element;
		*known = tuple->known;
		*count = tuple->count;
		if (tuple->known)
			for (uint32_t i = 0; i < tuple->count && i < LTLS_TUPLE_MAX; i++) items[i] = tuple->items[i];
		return 1;
	}

	return 0;
}

static void apply_function_docs(ltls_Context* context, lt_AstNode* fn)
{
	context->generics = ltls_index_generics(context->index, fn, &context->generic_count);

	ltls_Doc doc;
	if (!context->analysis || !ltls_doc_for_line(&context->analysis->tokens, fn->loc.line, &doc)) return;

	if (doc.generic_count)
	{
		ltls_index_set_generics(context->index, fn, doc.generics, doc.generic_count);
		context->generics = ltls_index_generics(context->index, fn, &context->generic_count);
	}

	for (uint32_t i = 0; i < doc.param_count; i++)
	{
		ltls_TypeSet type = annotation_type(context, doc.params[i].type);
		if (!type) continue;

		for (uint8_t a = 0; a <= LT_MAX_FUNCTION_PARAMS; a++)
		{
			lt_Token* argument = fn->fn.args[a];
			if (!argument) break;

			const char* name = token_text(context->index, argument);
			if (!name || strcmp(name, doc.params[i].name) != 0) continue;

			ltls_Declaration* declaration = declaration_for_token(context->index, argument);
			if (declaration) write_declaration(context, declaration, type);
			break;
		}
	}

	for (uint32_t i = 0; i < doc.return_count; i++)
	{
		ltls_TypeSet type = annotation_type(context, doc.returns[i].type);
		if (!type) continue;

		ltls_TypeSet before = ltls_index_returns(context->index, fn);
		ltls_TypeSet after = ltls_index_add_return(context->index, fn, type);
		if (!ltls_type_equals(before, after)) context->changed = 1;
		ltls_index_set_declared_return(context->index, fn, type);
	}

	ltls_doc_free(&doc);
}

static ltls_TypeSet analyze_function(ltls_Context* context, lt_AstNode* fn)
{
	ltls_TypeSet* saved_env = context->env;
	lt_Scope* saved_frame = context->frame;
	lt_AstNode* saved_function = context->function;
	const char* const* saved_generics = context->generics;
	uint32_t saved_generic_count = context->generic_count;

	context->env = calloc(context->index->declaration_count ? context->index->declaration_count : 1,
		sizeof(ltls_TypeSet));
	context->frame = fn->fn.scope ? fn->fn.scope->frame : saved_frame;
	context->function = fn;

	apply_function_docs(context, fn);

	analyze_body(context, &fn->fn.body);

	free(context->env);
	context->env = saved_env;
	context->frame = saved_frame;
	context->function = saved_function;
	context->generics = saved_generics;
	context->generic_count = saved_generic_count;

	return ltls_type_add(context->index, 0, LTLS_TYPE_FUNCTION, fn);
}

static void bind_type_name(ltls_Context* context, const char* const* names, uint32_t count,
                           const char* name, ltls_TypeSet value, ltls_TypeSet* bindings)
{
	if (!name || !value) return;
	for (uint32_t i = 0; i < count; i++)
	{
		if (strcmp(names[i], name) != 0) continue;
		bindings[i] = ltls_type_union(context->index, bindings[i], value);
		return;
	}
}

static void bind_parameter(ltls_Context* context, const char* const* names, uint32_t count,
                           ltls_TypeSet parameter, ltls_TypeSet argument, ltls_TypeSet* bindings)
{
	for (ltls_TypeSet type = parameter; type; type = type->next)
	{
		if (type->kind == LTLS_TYPE_PARAM)
		{
			bind_type_name(context, names, count, type->name, argument, bindings);
			continue;
		}

		if (type->kind == LTLS_TYPE_INSTANCE)
		{
			uint32_t parameter_count = 0;
			const ltls_TypeSet* parameters = ltls_index_type_args(context->index, type, &parameter_count);
			if (!parameters || !parameter_count) continue;

			for (ltls_TypeSet value = argument; value; value = value->next)
			{
				if (value->kind != LTLS_TYPE_INSTANCE) continue;
				uint32_t argument_count = 0;
				const ltls_TypeSet* actual = ltls_index_type_args(context->index, value, &argument_count);
				if (!actual || argument_count != parameter_count) continue;

				for (uint32_t i = 0; i < parameter_count; i++)
					for (ltls_TypeSet variable = parameters[i]; variable; variable = variable->next)
						if (variable->kind == LTLS_TYPE_PARAM)
							bind_type_name(context, names, count, variable->name, actual[i], bindings);
			}
			continue;
		}

		if (type->kind != LTLS_TYPE_ARRAY) continue;
		ltls_TypeSet element = ltls_index_array_element(context->index, type->node);
		if (!element) continue;

		for (ltls_TypeSet value = argument; value; value = value->next)
		{
			if (value->kind != LTLS_TYPE_ARRAY) continue;
			ltls_TypeSet argument_element = ltls_index_array_element(context->index, value->node);
			for (ltls_TypeSet variable = element; variable; variable = variable->next)
				if (variable->kind == LTLS_TYPE_PARAM)
					bind_type_name(context, names, count, variable->name, argument_element, bindings);
		}
	}
}

static const ltls_Declaration* member_named(const ltls_Index* index, lt_AstNode* class_node, const char* name)
{
	for (uint32_t i = 0; i < index->declaration_count; i++)
	{
		const ltls_Declaration* declaration = &index->declarations[i];
		if (declaration->node != class_node) continue;
		if (declaration->kind < LTLS_DECL_FIELD || declaration->kind > LTLS_DECL_PROPERTY) continue;
		const char* candidate = token_text(index, declaration->name);
		if (candidate && strcmp(candidate, name) == 0) return declaration;
	}
	return 0;
}

static ltls_TypeSet analyze_call(ltls_Context* context, lt_AstNode* node)
{
	ltls_TypeSet callee = analyze_expression(context, node->call.callee);
	ltls_TypeSet argument_types[16];
	uint32_t argument_count = 0;
	for (uint8_t i = 0; i <= LT_MAX_CALL_ARGS + 1; i++)
	{
		if (!node->call.args[i]) break;
		ltls_TypeSet type = analyze_expression(context, node->call.args[i]);
		if (argument_count < 16) argument_types[argument_count++] = type;
	}
	ltls_TypeSet first = argument_count ? argument_types[0] : 0;

	/* The parser builds an import expression with a synthetic callee, so it is
	   recognized by name rather than through the reference table. */
	lt_Token* import_token = callee_token(node->call.callee);
	const char* import_name = import_token ? ltls_index_token_text(context->index, import_token) : 0;
	if (import_name && strcmp(import_name, "import") == 0)
	{
		char* specifier = literal_string(context, node->call.args[0]);
		if (specifier)
		{
			ltls_TypeSet imported = ltls_import_cached(context->index, specifier);
			free(specifier);
			if (imported) return imported;
		}
	}

	ltls_TypeSet result = 0;
	for (ltls_TypeSet type = callee; type; type = type->next)
	{
		if (type->kind == LTLS_TYPE_CLASS)
			result = ltls_type_add(context->index, result, LTLS_TYPE_INSTANCE, type->node);
		else if (type->kind == LTLS_TYPE_FUNCTION)
			result = ltls_type_union(context->index, result, ltls_index_returns(context->index, type->node));
	}

	/* `array.slice` and `array.reverse` return a new array with the same
	   elements; carry the source element type through. */
	lt_Token* callee_identifier = 0;
	if (node->call.callee->type == LT_AST_NODE_IDENTIFIER)
		callee_identifier = node->call.callee->identifier.token;
	else if (node->call.callee->type == LT_AST_NODE_INDEX)
		callee_identifier = member_token(node->call.callee->index.idx);

	const ltls_Declaration* declaration = callee_identifier
		? ltls_index_resolve(context->index, callee_identifier->span.start, 0) : 0;

	if (declaration_is_unpack(declaration))
	{
		ltls_TypeSet element = 0;
		ltls_TypeSet items[LTLS_TUPLE_MAX];
		uint32_t count = 0;
		uint8_t known = 0;
		unpack_stream(context, node, &element, items, &count, &known);
		result = known
			? (count ? items[0] : ltls_type_add(context->index, 0, LTLS_TYPE_NULL, 0))
			: element;
		return result;
	}

	if (declaration && declaration->external && declaration->signature &&
	    strncmp(declaration->signature, "fn import(", 10) == 0)
	{
		char* specifier = literal_string(context, node->call.args[0]);
		if (specifier)
		{
			ltls_TypeSet imported = ltls_import_cached(context->index, specifier);
			free(specifier);
			if (imported) return imported;
		}
	}

	if (declaration && declaration->external && declaration->signature &&
	    (strstr(declaration->signature, "array.slice(") || strstr(declaration->signature, "array.reverse(")))
	{
		ltls_TypeSet preserved = 0;
		for (ltls_TypeSet type = first; type; type = type->next)
		{
			if (type->kind != LTLS_TYPE_ARRAY) continue;
			ltls_TypeSet element = ltls_index_array_element(context->index, type->node);
			if (element) preserved = ltls_type_union(context->index, preserved, ltls_type_array(context->index, element));
		}
		if (preserved) result = preserved;
	}

	lt_Token* named_callee = callee_token(node->call.callee);
	const char* call_label = named_callee ? token_text(context->index, named_callee) : "function";

	/* Bind generic parameters from argument types. */
	ltls_TypeSet inferred_instances = 0;
	for (ltls_TypeSet type = callee; type; type = type->next)
	{
		if (type->kind == LTLS_TYPE_FUNCTION)
		{
			lt_AstNode* fn = type->node;
			uint32_t generic_count = 0;
			const char* const* names = ltls_index_generics(context->index, fn, &generic_count);
			if (generic_count > 16) continue;

			ltls_TypeSet bindings[16];
			memset(bindings, 0, sizeof(bindings));

			uint8_t start = 0;
			const char* first_name = fn->fn.args[0] ? token_text(context->index, fn->fn.args[0]) : 0;
			if (first_name && strcmp(first_name, "this") == 0) start = 1;

			for (uint32_t i = 0; i < argument_count && generic_count; i++)
			{
				lt_Token* parameter = fn->fn.args[start + i];
				if (!parameter) break;
				ltls_Declaration* declaration = declaration_for_token(context->index, parameter);
				if (!declaration || !declaration->types) continue;
				bind_parameter(context, names, generic_count, declaration->types, argument_types[i], bindings);
			}

			check_arguments(context, fn, node, start, argument_count, argument_types,
				names, bindings, generic_count, call_label);

			if (generic_count)
			{
				ltls_TypeSet substituted = ltls_index_substitute(context->index,
					ltls_index_returns(context->index, fn), names, bindings, generic_count);
				if (substituted) result = substituted;
			}
		}
		else if (type->kind == LTLS_TYPE_CLASS)
		{
			lt_AstNode* klass = type->node;
			uint32_t generic_count = 0;
			const char* const* names = ltls_index_generics(context->index, klass, &generic_count);
			if (generic_count > 16) continue;

			const ltls_Declaration* constructor = member_named(context->index, klass, "constructor");
			lt_AstNode* fn = constructor ? ltls_index_function_node(constructor) : 0;
			if (!fn) continue;

			ltls_TypeSet bindings[16];
			memset(bindings, 0, sizeof(bindings));

			uint8_t start = 0;
			const char* first_name = fn->fn.args[0] ? token_text(context->index, fn->fn.args[0]) : 0;
			if (first_name && strcmp(first_name, "this") == 0) start = 1;

			uint8_t bound = 0;
			for (uint32_t i = 0; i < argument_count && generic_count; i++)
			{
				lt_Token* parameter = fn->fn.args[start + i];
				if (!parameter) break;
				ltls_Declaration* declaration = declaration_for_token(context->index, parameter);
				if (!declaration || !declaration->types) continue;
				bind_parameter(context, names, generic_count, declaration->types, argument_types[i], bindings);
			}
			for (uint32_t i = 0; i < generic_count; i++) if (bindings[i]) bound = 1;

			check_arguments(context, fn, node, start, argument_count, argument_types,
				names, bindings, generic_count, call_label);

			if (generic_count && bound)
				inferred_instances = ltls_type_union(context->index, inferred_instances,
					ltls_type_instance(context->index, klass, bindings, generic_count));
		}
	}

	if (inferred_instances) result = inferred_instances;
	return result;
}

static ltls_TypeSet analyze_index(ltls_Context* context, lt_AstNode* node)
{
	ltls_TypeSet receiver = analyze_expression(context, node->index.source);

	ltls_Reference* reference = reference_for_node(context->index, node, 1);
	if (reference && reference->member && reference->receiver_expr)
	{
		ltls_TypeSet active = ltls_index_type_of(context->index, reference->receiver_expr);
		if (active) receiver = active;
	}
	if (reference) reference->types = receiver;

	lt_Token* member = member_token(node->index.idx);
	if (member && member->type == LT_TOKEN_IDENTIFIER)
	{
		if (reference && reference->member && !reference->receiver_expr && reference->class_node)
		{
			const ltls_Declaration* declaration = ltls_index_member(context->index, reference->class_node, member);
			if (declaration)
			{
				reference->types = declaration->types;
				return declaration->types;
			}
		}

		return member_type(context, receiver, member);
	}

	ltls_TypeSet element = 0;
	for (ltls_TypeSet type = receiver; type; type = type->next)
		if (type->kind == LTLS_TYPE_ARRAY)
			element = ltls_type_union(context->index, element, ltls_index_array_element(context->index, type->node));
	return element;
}

static void assign_target(ltls_Context* context, lt_AstNode* target, ltls_TypeSet types, lt_Span span)
{
	if (target->type == LT_AST_NODE_IDENTIFIER)
	{
		ltls_Reference* reference = reference_for_node(context->index, target, 0);
		if (!reference) return;

		ltls_Declaration* declaration = (ltls_Declaration*)ltls_index_lookup(context->index, reference->scope, target->identifier.token);
		if (declaration) write_declaration(context, declaration, types);
		return;
	}

	if (target->type == LT_AST_NODE_INDEX)
	{
		analyze_expression(context, target);
		ltls_Reference* reference = reference_for_node(context->index, target, 1);
		lt_Token* member = member_token(target->index.idx);
		if (!reference || !member) return;

		if (!reference->receiver_expr)
		{
			ltls_Declaration* declaration = (ltls_Declaration*)ltls_index_member(context->index, reference->class_node, member);
			if (declaration) write_declaration(context, declaration, types);
			return;
		}

		ltls_TypeSet receiver = ltls_index_type_of(context->index, reference->receiver_expr);
		const char* member_name = token_text(context->index, member);
		for (ltls_TypeSet type = receiver; type; type = type->next)
		{
			ltls_Declaration* declaration = 0;
			ltls_TypeSet expected = 0;

			if (type->kind == LTLS_TYPE_INSTANCE)
			{
				lt_AstNode* base = ltls_index_class_base(context->index, type->node);
				declaration = (ltls_Declaration*)ltls_index_member(context->index, base, member);
				if (declaration)
				{
					uint32_t argument_count = 0;
					const ltls_TypeSet* arguments = ltls_index_type_args(context->index, type, &argument_count);
					uint32_t generic_count = 0;
					const char* const* names = ltls_index_generics(context->index, base, &generic_count);
					expected = declaration->types;
					if (arguments && names && argument_count == generic_count)
						expected = ltls_index_substitute(context->index, expected, names, arguments, argument_count);
				}
			}
			else if (type->kind == LTLS_TYPE_TABLE)
			{
				declaration = (ltls_Declaration*)ltls_index_table_field(context->index, type->node, member);
				if (declaration) expected = declaration->types;
			}

			if (!declaration) continue;
			if (declaration->annotation && member_name)
			{
				char what[160];
				snprintf(what, sizeof(what), "assignment to %s", member_name);
				report_mismatch(context, span, expected, types, what);
			}
			write_declaration(context, declaration, types);
		}
	}
}

static ltls_TypeSet analyze_literal(ltls_Context* context, lt_AstNode* node)
{
	if (!node->literal.token) return 0;

	switch (node->literal.token->type)
	{
	case LT_TOKEN_NUMBER_LITERAL: return ltls_type_add(context->index, 0, LTLS_TYPE_NUMBER, 0);
	case LT_TOKEN_STRING_LITERAL: return ltls_type_add(context->index, 0, LTLS_TYPE_STRING, 0);
	case LT_TOKEN_TRUE_LITERAL:
	case LT_TOKEN_FALSE_LITERAL: return ltls_type_add(context->index, 0, LTLS_TYPE_BOOL, 0);
	case LT_TOKEN_NULL_LITERAL: return ltls_type_add(context->index, 0, LTLS_TYPE_NULL, 0);
	default: return 0;
	}
}

static ltls_TypeSet analyze_table(ltls_Context* context, lt_AstNode* node)
{
	for (uint32_t i = 0; i < node->table.values.length; i++)
	{
		lt_AstNode* value = *AT(lt_AstNode*, node->table.values, i);
		ltls_TypeSet value_type = analyze_expression(context, value);

		lt_AstNode* key = i < node->table.keys.length ? *AT(lt_AstNode*, node->table.keys, i) : 0;
		if (key && key->type == LT_AST_NODE_LITERAL && key->literal.token)
		{
			ltls_Declaration* field = (ltls_Declaration*)ltls_index_table_field(context->index, node, key->literal.token);
			if (field) write_declaration(context, field, value_type);
		}
	}

	return ltls_type_add(context->index, 0, LTLS_TYPE_TABLE, node);
}

static ltls_TypeSet analyze_expression(ltls_Context* context, lt_AstNode* node)
{
	if (!node) return 0;

	ltls_TypeSet type = 0;
	switch (node->type)
	{
	case LT_AST_NODE_IDENTIFIER: {
		ltls_Reference* reference = reference_for_node(context->index, node, 0);
		if (reference)
		{
			const ltls_Declaration* declaration = ltls_index_lookup(context->index, reference->scope, node->identifier.token);
			if (declaration)
			{
				if (declaration->kind == LTLS_DECL_CLASS && declaration->node)
					type = ltls_type_add(context->index, 0, LTLS_TYPE_CLASS, declaration->node);
				else type = read_declaration(context, declaration);
			}
			reference->types = type;
		}
	} break;

	case LT_AST_NODE_LITERAL:
		type = analyze_literal(context, node);
		break;

	case LT_AST_NODE_TABLE:
		type = analyze_table(context, node);
		break;

	case LT_AST_NODE_ARRAY: {
		ltls_TypeSet elements = 0;
		for (uint32_t i = 0; i < node->array.values.length; i++)
			elements = ltls_type_union(context->index, elements,
				analyze_expression(context, *AT(lt_AstNode*, node->array.values, i)));

		/* A mixed literal is dynamic: the array keeps its kind, the element
		   type stays unknown. */
		uint32_t alternatives = 0;
		for (ltls_TypeSet element = elements; element; element = element->next) alternatives++;
		ltls_index_set_array_element(context->index, node, alternatives == 1 ? elements : 0);
		type = ltls_type_add(context->index, 0, LTLS_TYPE_ARRAY, node);
	} break;

	case LT_AST_NODE_FN:
		type = analyze_function(context, node);
		break;

	case LT_AST_NODE_INDEX:
		type = analyze_index(context, node);
		break;

	case LT_AST_NODE_CALL:
		type = analyze_call(context, node);
		break;

	case LT_AST_NODE_ASSIGN: {
		ltls_TypeSet value = analyze_expression(context, node->assign.right);
		assign_target(context, node->assign.left, value, node->assign.right ? node->assign.right->span : node->span);
		type = value;
	} break;

	case LT_AST_NODE_BINARYOP:
		analyze_expression(context, node->binary_op.left);
		analyze_expression(context, node->binary_op.right);
		break;

	case LT_AST_NODE_UNARYOP:
		analyze_expression(context, node->unary_op.expr);
		break;

	case LT_AST_NODE_AWAIT:
		type = analyze_expression(context, node->await.expr);
		break;

	case LT_AST_NODE_RETURN: {
		type = analyze_expression(context, node->ret.expr);

		if (context->function && node->ret.expr && node->ret.expr->type == LT_AST_NODE_CALL)
		{
			ltls_TypeSet element = 0;
			ltls_TypeSet items[LTLS_TUPLE_MAX];
			uint32_t count = 0;
			uint8_t known = 0;
			if (call_stream(context, node->ret.expr, &element, items, &count, &known))
				ltls_index_set_tuple(context->index, context->function, element, items, count, known);
		}

		if (context->function)
		{
			if (context->verify)
			{
				ltls_TypeSet declared = ltls_index_declared_return(context->index, context->function);
				lt_Span span = node->ret.expr ? node->ret.expr->span : node->span;
				report_mismatch(context, span, declared, type, "return value");
			}

			ltls_TypeSet before = ltls_index_returns(context->index, context->function);
			ltls_TypeSet after = ltls_index_add_return(context->index, context->function, type);
			if (!ltls_type_equals(before, after)) context->changed = 1;
		}
	} break;

	default:
		break;
	}

	ltls_index_set_type(context->index, node, type);
	return type;
}

static void analyze_conditional(ltls_Context* context, lt_AstNode* node)
{
	ltls_TypeSet* base = env_copy(context, context->env);
	analyze_expression(context, node->branch.expr);
	analyze_body(context, &node->branch.body);
	ltls_TypeSet* then_env = context->env;

	ltls_TypeSet* else_env;
	if (node->branch.next)
	{
		context->env = env_copy(context, base);
		analyze_conditional(context, node->branch.next);
		else_env = context->env;
	}
	else else_env = env_copy(context, base);

	env_merge(context, else_env, then_env);

	free(then_env);
	free(base);
	context->env = else_env;
}

static void analyze_loop(ltls_Context* context, lt_AstNode* node, lt_AstNode* iterator)
{
	ltls_TypeSet* base = env_copy(context, context->env);

	for (uint32_t pass = 0; pass < 6; pass++)
	{
		context->env = env_copy(context, base);
		analyze_expression(context, iterator);
		analyze_body(context, &node->loop.body);

		uint8_t grew = env_merge(context, base, context->env);
		free(context->env);
		if (!grew) break;
	}

	context->env = env_copy(context, base);
	analyze_expression(context, iterator);
	analyze_body(context, &node->loop.body);
	env_merge(context, base, context->env);

	free(context->env);
	context->env = base;
}

static void analyze_class(ltls_Context* context, lt_AstNode* node)
{
	for (uint32_t i = 0; i < node->class_decl.members.length; i++)
	{
		lt_ClassMember* member = AT(lt_ClassMember, node->class_decl.members, i);
		if (!member->value) continue;

		ltls_TypeSet value_type = analyze_expression(context, member->value);
		ltls_Declaration* declaration = (ltls_Declaration*)ltls_index_member(context->index, node, member->name);
		if (!declaration) continue;

		ltls_TypeSet annotated = annotated_type(context, member->name->line);
		if (annotated && member->type == LT_CLASS_FIELD)
		{
			write_declaration(context, declaration, annotated);
			continue;
		}

		/* A field's value is the return of its synthesized initializer. */
		if (member->type == LT_CLASS_FIELD && member->value->type == LT_AST_NODE_FN)
			write_declaration(context, declaration, ltls_index_returns(context->index, member->value));
		else
			write_declaration(context, declaration, value_type);
	}
}

static void analyze_statement(ltls_Context* context, lt_AstNode* node)
{
	if (!node) return;

	switch (node->type)
	{
	case LT_AST_NODE_CHUNK:
		analyze_body(context, &node->chunk.body);
		break;

	case LT_AST_NODE_DECLARE: {
		ltls_TypeSet type = analyze_expression(context, node->declare.expr);
		ltls_TypeSet annotated = annotated_type(context, node->loc.line);
		if (annotated) type = annotated;

		if (node->declare.destructure == LT_DESTRUCT_ARRAY)
		{
			ltls_TypeSet element = 0;
			ltls_TypeSet items[LTLS_TUPLE_MAX];
			uint32_t count = 0;
			uint8_t known = 0;
			lt_AstNode* source = node->declare.expr;

			if (!call_stream(context, source, &element, items, &count, &known))
			{
				if (source && source->type == LT_AST_NODE_ARRAY)
				{
					uint32_t length = source->array.values.length;
					if (length <= LTLS_TUPLE_MAX)
					{
						known = 1;
						count = length;
						for (uint32_t i = 0; i < length; i++)
						{
							items[i] = ltls_index_type_of(context->index,
								*AT(lt_AstNode*, source->array.values, i));
							element = ltls_type_union(context->index, element, items[i]);
						}
					}
				}
				else element = array_element_types(context, type);
			}

			for (uint32_t i = 0; i < node->declare.entries.length; i++)
			{
				lt_DestructureEntry* entry = AT(lt_DestructureEntry, node->declare.entries, i);
				ltls_Declaration* target = declaration_for_token(context->index, entry->local);
				if (!target) continue;

				ltls_TypeSet value = known
					? (i < count ? items[i] : ltls_type_add(context->index, 0, LTLS_TYPE_NULL, 0))
					: element;
				if (value) write_declaration(context, target, value);
			}
		}
		else if (node->declare.destructure == LT_DESTRUCT_TABLE)
		{
			uint8_t imported = is_import_expression(context, node->declare.expr);
			for (uint32_t i = 0; i < node->declare.entries.length; i++)
			{
				lt_DestructureEntry* entry = AT(lt_DestructureEntry, node->declare.entries, i);
				ltls_Declaration* target = declaration_for_token(context->index, entry->local);
				if (!target || !entry->key) continue;

				ltls_TypeSet value = 0;
				const ltls_Declaration* alias = 0;
				for (ltls_TypeSet type_set = type; type_set; type_set = type_set->next)
				{
					if (type_set->kind != LTLS_TYPE_TABLE) continue;
					const ltls_Declaration* field =
						ltls_index_table_field(context->index, type_set->node, entry->key);
					if (!field) continue;
					value = ltls_type_union(context->index, value, field->types);
					if (!alias) alias = field;
				}
				if (imported && alias && !target->alias) target->alias = alias;
				if (value) write_declaration(context, target, value);
			}
		}
		else
		{
			ltls_Declaration* declaration = declaration_for_token(context->index, node->declare.identifier);
			if (declaration && type) write_declaration(context, declaration, type);
		}

		ltls_index_set_type(context->index, node, type);
	} break;

	case LT_AST_NODE_CLASS:
		analyze_class(context, node);
		break;

	case LT_AST_NODE_WITH:
		analyze_expression(context, node->with_stmt.expr);
		analyze_body(context, &node->with_stmt.body);
		break;

	case LT_AST_NODE_IF:
	case LT_AST_NODE_ELSEIF:
	case LT_AST_NODE_ELSE:
		analyze_conditional(context, node);
		break;

	case LT_AST_NODE_FOR:
		analyze_loop(context, node, node->loop.iterator);
		break;

	case LT_AST_NODE_WHILE:
		analyze_loop(context, node, node->loop.iterator);
		break;

	case LT_AST_NODE_RETURN:
		analyze_expression(context, node);
		break;

	case LT_AST_NODE_ASSIGN:
		analyze_expression(context, node);
		break;

	default:
		analyze_expression(context, node);
		break;
	}
}

static void analyze_body(ltls_Context* context, lt_Buffer* body)
{
	for (uint32_t i = 0; i < body->length; i++)
		analyze_statement(context, *AT(lt_AstNode*, *body, i));
}

static void apply_declaration_annotations(ltls_Context* context)
{
	for (uint32_t i = 0; i < context->index->declaration_count; i++)
	{
		ltls_Declaration* declaration = &context->index->declarations[i];
		const char* const* saved_generics = context->generics;
		uint32_t saved_generic_count = context->generic_count;
		context->generics = declaration->node
			? ltls_index_generics(context->index, declaration->node, &context->generic_count) : 0;
		if (!declaration->node) context->generic_count = 0;

		if (declaration->annotation)
		{
			ltls_TypeSet type = annotation_type(context, declaration->annotation);
			if (type)
			{
				ltls_TypeSet before = declaration->types;
				declaration->types = ltls_type_union(context->index, declaration->types, type);
				if (!ltls_type_equals(before, declaration->types)) context->changed = 1;
			}
		}

		if (declaration->returns_text && declaration->function_node)
		{
			ltls_TypeSet returns = annotation_type(context, declaration->returns_text);
			if (returns)
			{
				ltls_TypeSet before = ltls_index_returns(context->index, declaration->function_node);
				ltls_TypeSet after = ltls_index_add_return(context->index, declaration->function_node, returns);
				if (!ltls_type_equals(before, after)) context->changed = 1;
			}
		}

		context->generics = saved_generics;
		context->generic_count = saved_generic_count;
	}
}

void ltls_types_build(ltls_Index* index, const lt_Analysis* analysis)
{
	if (!analysis->parser.root) return;

	lt_AstNode* root = analysis->parser.root;

	for (uint32_t pass = 0; pass < 6; pass++)
	{
		ltls_Context context;
		memset(&context, 0, sizeof(context));
		context.index = index;
		context.analysis = analysis;
		context.env = calloc(index->declaration_count ? index->declaration_count : 1, sizeof(ltls_TypeSet));
		context.frame = root->chunk.scope ? root->chunk.scope->frame : 0;

		apply_declaration_annotations(&context);
		analyze_body(&context, &root->chunk.body);

		free(context.env);
		if (!context.changed) break;
	}

	if (index->verify)
	{
		ltls_Context context;
		memset(&context, 0, sizeof(context));
		context.index = index;
		context.analysis = analysis;
		context.env = calloc(index->declaration_count ? index->declaration_count : 1, sizeof(ltls_TypeSet));
		context.frame = root->chunk.scope ? root->chunk.scope->frame : 0;
		context.verify = 1;

		analyze_body(&context, &root->chunk.body);

		free(context.env);
	}
}
