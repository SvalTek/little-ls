#include "index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AT(type, buffer, index) \
	((type*)((char*)(buffer).data + (buffer).element_size * (index)))

static const char* token_text(const ltls_Index* index, const lt_Token* token)
{
	if (!token) return 0;
	if (token->idx >= index->tokens->identifier_buffer.length) return 0;
	return AT(lt_Identifier, index->tokens->identifier_buffer, token->idx)->name;
}

static uint8_t is_synthetic(const lt_Token* token)
{
	return token && (token->flags & LT_TOKEN_SYNTHETIC);
}

static void push_declaration(ltls_Index* index, ltls_Declaration declaration)
{
	ltls_Declaration* items = realloc(index->declarations, sizeof(ltls_Declaration) * (index->declaration_count + 1));
	if (!items) return;
	index->declarations = items;
	index->declarations[index->declaration_count++] = declaration;
}

static void push_reference(ltls_Index* index, ltls_Reference reference)
{
	ltls_Reference* items = realloc(index->references, sizeof(ltls_Reference) * (index->reference_count + 1));
	if (!items) return;
	index->references = items;
	index->references[index->reference_count++] = reference;
}

static void add_declaration(ltls_Index* index, lt_Token* name, lt_Span span, lt_Scope* scope,
                            lt_AstNode* node, const lt_ClassMember* member, uint8_t kind, uint8_t is_global)
{
	if (!name) return;

	ltls_Declaration declaration;
	declaration.name = name;
	declaration.span = span;
	declaration.scope = scope;
	declaration.node = node;
	declaration.member = member;
	declaration.kind = kind;
	declaration.is_global = is_global;
	push_declaration(index, declaration);
}

static void add_reference(ltls_Index* index, lt_Token* token, lt_Scope* scope, lt_AstNode* class_node,
                          uint8_t member, uint8_t in_with)
{
	if (!token || is_synthetic(token)) return;

	ltls_Reference reference;
	reference.token = token;
	reference.scope = scope;
	reference.class_node = class_node;
	reference.member = member;
	reference.in_with = in_with;
	push_reference(index, reference);
}

static void walk_node(ltls_Index* index, lt_AstNode* node, lt_Scope* scope, lt_AstNode* class_node, uint8_t in_with);

static void walk_body(ltls_Index* index, lt_Buffer* body, lt_Scope* scope, lt_AstNode* class_node, uint8_t in_with)
{
	for (uint32_t i = 0; i < body->length; i++)
		walk_node(index, *AT(lt_AstNode*, *body, i), scope, class_node, in_with);
}

static void walk_class_member(ltls_Index* index, lt_AstNode* klass, lt_ClassMember* member, lt_Scope* scope)
{
	if (!member->name) return;

	uint8_t kind = LTLS_DECL_FIELD;
	switch (member->type)
	{
	case LT_CLASS_METHOD: kind = LTLS_DECL_METHOD; break;
	case LT_CLASS_CONSTRUCTOR: kind = LTLS_DECL_METHOD; break;
	case LT_CLASS_GETTER: case LT_CLASS_SETTER: kind = LTLS_DECL_PROPERTY; break;
	default: kind = LTLS_DECL_FIELD; break;
	}

	lt_Span span = member->value ? member->value->span : member->name->span;
	add_declaration(index, member->name, span, scope, klass, member, kind, 0);

	if (member->value)
		walk_node(index, member->value, scope, klass, 0);
}

static void walk_node(ltls_Index* index, lt_AstNode* node, lt_Scope* scope, lt_AstNode* class_node, uint8_t in_with)
{
	if (!node) return;

	switch (node->type)
	{
	case LT_AST_NODE_CHUNK:
		walk_body(index, &node->chunk.body, node->chunk.scope ? node->chunk.scope : scope, class_node, in_with);
		break;

	case LT_AST_NODE_DECLARE:
		if (node->declare.identifier)
		{
			uint8_t kind = LTLS_DECL_LOCAL;
			if (node->declare.expr && node->declare.expr->type == LT_AST_NODE_FN) kind = LTLS_DECL_FUNCTION;
			add_declaration(index, node->declare.identifier, node->span, scope, node, 0, kind, node->declare.is_global);
		}
		for (uint32_t i = 0; i < node->declare.entries.length; i++)
		{
			lt_DestructureEntry* entry = AT(lt_DestructureEntry, node->declare.entries, i);
			add_declaration(index, entry->local, entry->local ? entry->local->span : node->span, scope, node, 0,
				LTLS_DECL_LOCAL, node->declare.is_global);
		}
		walk_node(index, node->declare.expr, scope, class_node, in_with);
		break;

	case LT_AST_NODE_CLASS:
		add_declaration(index, node->class_decl.identifier, node->span, scope, node, 0, LTLS_DECL_CLASS, 0);
		for (uint32_t i = 0; i < node->class_decl.members.length; i++)
			walk_class_member(index, node, AT(lt_ClassMember, node->class_decl.members, i), scope);
		break;

	case LT_AST_NODE_FN: {
		lt_Scope* fn_scope = node->fn.scope ? node->fn.scope : scope;
		for (uint8_t i = 0; i <= LT_MAX_FUNCTION_PARAMS; i++)
		{
			lt_Token* argument = node->fn.args[i];
			if (!argument) break;
			add_declaration(index, argument, argument->span, fn_scope, node, 0, LTLS_DECL_PARAM, 0);
		}
		walk_body(index, &node->fn.body, fn_scope, class_node, in_with);
	} break;

	case LT_AST_NODE_WITH:
		walk_node(index, node->with_stmt.expr, scope, class_node, in_with);
		walk_body(index, &node->with_stmt.body, node->with_stmt.scope ? node->with_stmt.scope : scope, class_node, 1);
		break;

	case LT_AST_NODE_IF:
	case LT_AST_NODE_ELSEIF:
	case LT_AST_NODE_ELSE:
		walk_node(index, node->branch.expr, scope, class_node, in_with);
		walk_body(index, &node->branch.body, scope, class_node, in_with);
		walk_node(index, node->branch.next, scope, class_node, in_with);
		break;

	case LT_AST_NODE_FOR: {
		lt_Scope* frame = scope ? scope->frame : 0;
		if (frame && node->loop.identifier < frame->frame_slots.length)
		{
			lt_Token* variable = *AT(lt_Token*, frame->frame_slots, node->loop.identifier);
			if (variable) add_declaration(index, variable, variable->span, scope, node, 0, LTLS_DECL_LOCAL, 0);
		}
		walk_node(index, node->loop.iterator, scope, class_node, in_with);
		walk_body(index, &node->loop.body, scope, class_node, in_with);
	} break;

	case LT_AST_NODE_WHILE:
		walk_node(index, node->loop.iterator, scope, class_node, in_with);
		walk_body(index, &node->loop.body, scope, class_node, in_with);
		break;

	case LT_AST_NODE_INDEX: {
		lt_AstNode* source = node->index.source;
		lt_AstNode* idx = node->index.idx;

		if (source && source->type == LT_AST_NODE_IDENTIFIER)
		{
			const char* name = token_text(index, source->identifier.token);
			if (name && strcmp(name, "this") == 0)
			{
				lt_Token* field = 0;
				if (idx && idx->type == LT_AST_NODE_LITERAL) field = idx->literal.token;
				else if (idx && idx->type == LT_AST_NODE_IDENTIFIER) field = idx->identifier.token;
				if (field) add_reference(index, field, scope, class_node, 1, in_with);
				break;
			}
		}

		walk_node(index, source, scope, class_node, in_with);
		if (!idx || idx->type != LT_AST_NODE_LITERAL) walk_node(index, idx, scope, class_node, in_with);
	} break;

	case LT_AST_NODE_CALL:
		walk_node(index, node->call.callee, scope, class_node, in_with);
		for (uint8_t i = 0; i <= LT_MAX_CALL_ARGS + 1; i++)
			walk_node(index, node->call.args[i], scope, class_node, in_with);
		break;

	case LT_AST_NODE_BINARYOP:
		walk_node(index, node->binary_op.left, scope, class_node, in_with);
		walk_node(index, node->binary_op.right, scope, class_node, in_with);
		break;

	case LT_AST_NODE_UNARYOP:
		walk_node(index, node->unary_op.expr, scope, class_node, in_with);
		break;

	case LT_AST_NODE_ASSIGN:
		walk_node(index, node->assign.left, scope, class_node, in_with);
		walk_node(index, node->assign.right, scope, class_node, in_with);
		break;

	case LT_AST_NODE_AWAIT:
		walk_node(index, node->await.expr, scope, class_node, in_with);
		break;

	case LT_AST_NODE_RETURN:
		walk_node(index, node->ret.expr, scope, class_node, in_with);
		break;

	case LT_AST_NODE_ARRAY:
		walk_body(index, &node->array.values, scope, class_node, in_with);
		break;

	case LT_AST_NODE_TABLE:
		walk_body(index, &node->table.values, scope, class_node, in_with);
		break;

	case LT_AST_NODE_IDENTIFIER:
		add_reference(index, node->identifier.token, scope, class_node, 0, in_with);
		break;

	default:
		break;
	}
}

void ltls_index_build(ltls_Index* index, const lt_Analysis* analysis)
{
	memset(index, 0, sizeof(ltls_Index));
	index->tokens = &analysis->tokens;
	index->source = analysis->source;

	if (!analysis->parser.root) return;
	lt_AstNode* root = analysis->parser.root;
	walk_body(index, &root->chunk.body, root->chunk.scope, 0, 0);
}

void ltls_index_free(ltls_Index* index)
{
	free(index->declarations);
	free(index->references);
	memset(index, 0, sizeof(ltls_Index));
}

static uint8_t span_covers(lt_Span span, uint32_t offset)
{
	return offset >= span.start && offset < span.end;
}

const ltls_Declaration* ltls_index_declaration_at(const ltls_Index* index, uint32_t offset)
{
	for (uint32_t i = 0; i < index->declaration_count; i++)
	{
		if (span_covers(index->declarations[i].name->span, offset)) return &index->declarations[i];
	}

	/* The cursor usually sits just after the last character. */
	for (uint32_t i = 0; i < index->declaration_count; i++)
	{
		lt_Span span = index->declarations[i].name->span;
		if (span.end > span.start && offset == span.end) return &index->declarations[i];
	}

	return 0;
}

static const ltls_Declaration* declaration_for_span(const ltls_Index* index, lt_Span span)
{
	for (uint32_t i = 0; i < index->declaration_count; i++)
	{
		lt_Span candidate = index->declarations[i].name->span;
		if (candidate.start == span.start && candidate.end == span.end) return &index->declarations[i];
	}
	return 0;
}

static const ltls_Declaration* resolve_lexical(const ltls_Index* index, lt_Scope* scope, lt_Token* token)
{
	for (lt_Scope* current = scope; current; current = current->last)
	{
		for (uint32_t i = 0; i < current->locals.length; i++)
		{
			lt_Token* local = AT(lt_Token, current->locals, i);
			if (local->idx != token->idx) continue;

			const ltls_Declaration* declaration = declaration_for_span(index, local->span);
			/* The parser only sees locals declared before the reference. */
			if (declaration && declaration->name->span.start <= token->span.start) return declaration;
		}
	}
	return 0;
}

static const ltls_Declaration* resolve_global(const ltls_Index* index, lt_Token* token)
{
	for (uint32_t i = 0; i < index->declaration_count; i++)
	{
		const ltls_Declaration* declaration = &index->declarations[i];
		if (declaration->is_global && declaration->name->idx == token->idx) return declaration;
	}
	return 0;
}

static const ltls_Declaration* resolve_member(const ltls_Index* index, lt_AstNode* class_node, lt_Token* token)
{
	if (!class_node) return 0;

	for (uint32_t i = 0; i < index->declaration_count; i++)
	{
		const ltls_Declaration* declaration = &index->declarations[i];
		if (declaration->member && declaration->node == class_node && declaration->name->idx == token->idx)
			return declaration;
	}
	return 0;
}

const ltls_Declaration* ltls_index_resolve(const ltls_Index* index, uint32_t offset, const ltls_Reference** reference)
{
	if (reference) *reference = 0;

	const ltls_Declaration* declaration = ltls_index_declaration_at(index, offset);
	if (declaration) return declaration;

	for (uint32_t i = 0; i < index->reference_count; i++)
	{
		ltls_Reference* candidate = &index->references[i];
		if (!span_covers(candidate->token->span, offset)) continue;

		if (reference) *reference = candidate;
		if (candidate->member)
		{
			if (candidate->in_with) return 0;
			return resolve_member(index, candidate->class_node, candidate->token);
		}

		declaration = resolve_lexical(index, candidate->scope, candidate->token);
		if (declaration) return declaration;
		return resolve_global(index, candidate->token);
	}

	return 0;
}

/* ------------------------------------------------------------------------- */

typedef struct {
	char* text;
	size_t length;
	size_t capacity;
} ltls_String;

static void string_append(ltls_String* string, const char* text)
{
	size_t length = strlen(text);
	if (string->length + length + 1 > string->capacity)
	{
		size_t capacity = string->capacity ? string->capacity : 32;
		while (capacity < string->length + length + 1) capacity *= 2;
		char* grown = realloc(string->text, capacity);
		if (!grown) return;
		string->text = grown;
		string->capacity = capacity;
	}
	memcpy(string->text + string->length, text, length + 1);
	string->length += length;
}

static lt_AstNode* function_node(const ltls_Declaration* declaration)
{
	lt_AstNode* node = declaration->node;
	if (declaration->member)
	{
		if (declaration->member->value && declaration->member->value->type == LT_AST_NODE_FN)
			return declaration->member->value;
		return 0;
	}
	if (node && node->type == LT_AST_NODE_FN) return node;
	if (node && node->type == LT_AST_NODE_DECLARE && node->declare.expr && node->declare.expr->type == LT_AST_NODE_FN)
		return node->declare.expr;
	return 0;
}

static void append_arguments(ltls_String* string, const ltls_Index* index, lt_AstNode* fn)
{
	string_append(string, "(");
	uint8_t first = 1;
	for (uint8_t i = 0; i <= LT_MAX_FUNCTION_PARAMS; i++)
	{
		lt_Token* argument = fn->fn.args[i];
		if (!argument) break;
		if (is_synthetic(argument)) continue;

		const char* name = token_text(index, argument);
		if (!name) continue;

		if (!first) string_append(string, ", ");
		string_append(string, name);
		first = 0;
	}
	string_append(string, ")");
}

char* ltls_declaration_signature(const ltls_Index* index, const ltls_Declaration* declaration)
{
	ltls_String string = { 0 };
	string_append(&string, declaration->is_global ? "global " : "");

	const char* name = token_text(index, declaration->name);
	if (!name) name = "?";

	lt_AstNode* fn = function_node(declaration);

	switch (declaration->kind)
	{
	case LTLS_DECL_CLASS:
		string_append(&string, "class ");
		string_append(&string, name);
		break;
	case LTLS_DECL_PARAM:
		string_append(&string, "param ");
		string_append(&string, name);
		break;
	case LTLS_DECL_FIELD:
		string_append(&string, "field ");
		string_append(&string, name);
		break;
	case LTLS_DECL_PROPERTY:
		string_append(&string, declaration->member && declaration->member->type == LT_CLASS_SETTER ? "set " : "get ");
		string_append(&string, name);
		break;
	case LTLS_DECL_FUNCTION:
	case LTLS_DECL_METHOD:
		string_append(&string, "fn ");
		string_append(&string, name);
		if (fn) append_arguments(&string, index, fn);
		break;
	default:
		string_append(&string, "var ");
		string_append(&string, name);
		break;
	}

	if (!string.text) string_append(&string, "");
	return string.text;
}
