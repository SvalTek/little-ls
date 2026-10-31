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
	declaration.types = 0;
	declaration.kind = kind;
	declaration.is_global = is_global;
	push_declaration(index, declaration);
}

static void add_reference(ltls_Index* index, lt_Token* token, lt_AstNode* node, lt_Scope* scope,
                          lt_AstNode* class_node, lt_AstNode* receiver_expr, uint8_t member)
{
	if (!token || is_synthetic(token)) return;

	ltls_Reference reference;
	reference.token = token;
	reference.node = node;
	reference.scope = scope;
	reference.class_node = class_node;
	reference.receiver_expr = receiver_expr;
	reference.member = member;
	reference.types = 0;
	push_reference(index, reference);
}

static void walk_node(ltls_Index* index, lt_AstNode* node, lt_Scope* scope, lt_AstNode* class_node,
                      lt_Token* receiver_token, lt_AstNode* receiver_expr);

static void walk_body(ltls_Index* index, lt_Buffer* body, lt_Scope* scope, lt_AstNode* class_node,
                      lt_Token* receiver_token, lt_AstNode* receiver_expr)
{
	for (uint32_t i = 0; i < body->length; i++)
		walk_node(index, *AT(lt_AstNode*, *body, i), scope, class_node, receiver_token, receiver_expr);
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

	if (!member->value) return;

	/* Class members receive the instance as their first argument; that token is
	   the active receiver for `@name` inside the member. */
	lt_Token* receiver = 0;
	if (member->value->type == LT_AST_NODE_FN) receiver = member->value->fn.args[0];
	walk_node(index, member->value, scope, klass, receiver, 0);
}

static uint8_t is_name_token(const lt_Token* token)
{
	return token && (token->type == LT_TOKEN_IDENTIFIER || (token->type >= LT_TOKEN_FN && token->type <= LT_TOKEN_RETURN));
}

static lt_Token* member_token(lt_AstNode* idx)
{
	if (!idx) return 0;
	if (idx->type == LT_AST_NODE_LITERAL) return idx->literal.token;
	if (idx->type == LT_AST_NODE_IDENTIFIER) return idx->identifier.token;
	return 0;
}

static void walk_node(ltls_Index* index, lt_AstNode* node, lt_Scope* scope, lt_AstNode* class_node,
                      lt_Token* receiver_token, lt_AstNode* receiver_expr)
{
	if (!node) return;

	switch (node->type)
	{
	case LT_AST_NODE_CHUNK:
		walk_body(index, &node->chunk.body, node->chunk.scope ? node->chunk.scope : scope,
			class_node, receiver_token, receiver_expr);
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
		walk_node(index, node->declare.expr, scope, class_node, receiver_token, receiver_expr);
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

		/* The parser only replaces the receiver when a function declares `this`
		   as its first parameter; otherwise nested functions inherit it. */
		lt_Token* fn_receiver = receiver_token;
		lt_AstNode* fn_receiver_expr = receiver_expr;
		const char* first_name = token_text(index, node->fn.args[0]);
		if (first_name && strcmp(first_name, "this") == 0)
		{
			fn_receiver = node->fn.args[0];
			fn_receiver_expr = 0;
		}

		walk_body(index, &node->fn.body, fn_scope, class_node, fn_receiver, fn_receiver_expr);
	} break;

	case LT_AST_NODE_WITH:
		walk_node(index, node->with_stmt.expr, scope, class_node, receiver_token, receiver_expr);
		/* `with` swaps the active receiver for the block; the meaning of `@` is
		   unchanged, and the outer receiver returns afterwards. */
		walk_body(index, &node->with_stmt.body, node->with_stmt.scope ? node->with_stmt.scope : scope,
			class_node, node->with_stmt.receiver, node->with_stmt.expr);
		break;

	case LT_AST_NODE_IF:
	case LT_AST_NODE_ELSEIF:
	case LT_AST_NODE_ELSE:
		walk_node(index, node->branch.expr, scope, class_node, receiver_token, receiver_expr);
		walk_body(index, &node->branch.body, scope, class_node, receiver_token, receiver_expr);
		walk_node(index, node->branch.next, scope, class_node, receiver_token, receiver_expr);
		break;

	case LT_AST_NODE_FOR: {
		lt_Scope* frame = scope ? scope->frame : 0;
		if (frame && node->loop.identifier < frame->frame_slots.length)
		{
			lt_Token* variable = *AT(lt_Token*, frame->frame_slots, node->loop.identifier);
			if (variable) add_declaration(index, variable, variable->span, scope, node, 0, LTLS_DECL_LOCAL, 0);
		}
		walk_node(index, node->loop.iterator, scope, class_node, receiver_token, receiver_expr);
		walk_body(index, &node->loop.body, scope, class_node, receiver_token, receiver_expr);
	} break;

	case LT_AST_NODE_WHILE:
		walk_node(index, node->loop.iterator, scope, class_node, receiver_token, receiver_expr);
		walk_body(index, &node->loop.body, scope, class_node, receiver_token, receiver_expr);
		break;

	case LT_AST_NODE_INDEX: {
		lt_AstNode* source = node->index.source;
		lt_AstNode* idx = node->index.idx;

		if (source && source->type == LT_AST_NODE_IDENTIFIER)
		{
			const char* name = token_text(index, source->identifier.token);
			uint8_t is_receiver = receiver_token && source->identifier.token == receiver_token;
			uint8_t is_this = name && strcmp(name, "this") == 0;

			if (is_receiver || is_this)
			{
				lt_Token* field = member_token(idx);

				/* `this.name` always means the instance; `@name` means whichever
				   receiver is active, which `with` changes. */
				lt_AstNode* target = is_receiver ? receiver_expr : 0;
				if (field) add_reference(index, field, node, scope, class_node, target, 1);
				break;
			}
		}

		lt_Token* member = member_token(idx);
		if (member && is_name_token(member))
		{
			/* obj.name / p:method() resolve through the receiver's type. */
			add_reference(index, member, node, scope, class_node, source, 1);
			walk_node(index, source, scope, class_node, receiver_token, receiver_expr);
			break;
		}

		walk_node(index, source, scope, class_node, receiver_token, receiver_expr);
		if (!idx || idx->type != LT_AST_NODE_LITERAL)
			walk_node(index, idx, scope, class_node, receiver_token, receiver_expr);
	} break;

	case LT_AST_NODE_CALL:
		walk_node(index, node->call.callee, scope, class_node, receiver_token, receiver_expr);
		for (uint8_t i = 0; i <= LT_MAX_CALL_ARGS + 1; i++)
			walk_node(index, node->call.args[i], scope, class_node, receiver_token, receiver_expr);
		break;

	case LT_AST_NODE_BINARYOP:
		walk_node(index, node->binary_op.left, scope, class_node, receiver_token, receiver_expr);
		walk_node(index, node->binary_op.right, scope, class_node, receiver_token, receiver_expr);
		break;

	case LT_AST_NODE_UNARYOP:
		walk_node(index, node->unary_op.expr, scope, class_node, receiver_token, receiver_expr);
		break;

	case LT_AST_NODE_ASSIGN:
		walk_node(index, node->assign.left, scope, class_node, receiver_token, receiver_expr);
		walk_node(index, node->assign.right, scope, class_node, receiver_token, receiver_expr);
		break;

	case LT_AST_NODE_AWAIT:
		walk_node(index, node->await.expr, scope, class_node, receiver_token, receiver_expr);
		break;

	case LT_AST_NODE_RETURN:
		walk_node(index, node->ret.expr, scope, class_node, receiver_token, receiver_expr);
		break;

	case LT_AST_NODE_ARRAY:
		walk_body(index, &node->array.values, scope, class_node, receiver_token, receiver_expr);
		break;

	case LT_AST_NODE_TABLE:
		/* Literal keys are declarations so a `with { ... }` receiver can resolve
		   its own `@name` references. */
		for (uint32_t i = 0; i < node->table.keys.length; i++)
		{
			lt_AstNode* key = *AT(lt_AstNode*, node->table.keys, i);
			if (key && key->type == LT_AST_NODE_LITERAL && is_name_token(key->literal.token))
				add_declaration(index, key->literal.token, key->literal.token->span, scope, node, 0,
					LTLS_DECL_FIELD, 0);
		}
		walk_body(index, &node->table.values, scope, class_node, receiver_token, receiver_expr);
		break;

	case LT_AST_NODE_IDENTIFIER:
		add_reference(index, node->identifier.token, node, scope, class_node, 0, 0);
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
	walk_body(index, &root->chunk.body, root->chunk.scope, 0, 0, 0);
	ltls_types_build(index, analysis);
}

void ltls_index_free(ltls_Index* index)
{
	for (uint32_t i = 0; i < index->allocation_count; i++) free(index->allocations[i]);
	free(index->allocations);
	free(index->node_types);
	free(index->function_returns);
	free(index->declarations);
	free(index->references);
	memset(index, 0, sizeof(ltls_Index));
}

static ltls_TypeSetNode* allocate_type(ltls_Index* index, uint8_t kind, lt_AstNode* node)
{
	ltls_TypeSetNode* item = malloc(sizeof(ltls_TypeSetNode));
	if (!item) return 0;

	item->kind = kind;
	item->node = node;
	item->next = 0;

	ltls_TypeSetNode** allocations = realloc(index->allocations, sizeof(ltls_TypeSetNode*) * (index->allocation_count + 1));
	if (!allocations)
	{
		free(item);
		return 0;
	}

	index->allocations = allocations;
	index->allocations[index->allocation_count++] = item;
	return item;
}

static uint8_t type_has(ltls_TypeSet types, uint8_t kind, lt_AstNode* node)
{
	for (ltls_TypeSet item = types; item; item = item->next)
		if (item->kind == kind && item->node == node) return 1;
	return 0;
}

ltls_TypeSet ltls_type_add(ltls_Index* index, ltls_TypeSet types, uint8_t kind, lt_AstNode* node)
{
	if (type_has(types, kind, node)) return types;

	ltls_TypeSetNode* item = allocate_type(index, kind, node);
	if (!item) return types;

	item->next = types;
	return item;
}

ltls_TypeSet ltls_type_union(ltls_Index* index, ltls_TypeSet left, ltls_TypeSet right)
{
	ltls_TypeSet result = left;
	for (ltls_TypeSet item = right; item; item = item->next)
		result = ltls_type_add(index, result, item->kind, item->node);
	return result;
}

uint8_t ltls_type_equals(ltls_TypeSet left, ltls_TypeSet right)
{
	for (ltls_TypeSet item = left; item; item = item->next)
		if (!type_has(right, item->kind, item->node)) return 0;
	for (ltls_TypeSet item = right; item; item = item->next)
		if (!type_has(left, item->kind, item->node)) return 0;
	return 1;
}

void ltls_index_set_type(ltls_Index* index, lt_AstNode* node, ltls_TypeSet types)
{
	for (uint32_t i = 0; i < index->node_type_count; i++)
	{
		if (index->node_types[i].node != node) continue;
		index->node_types[i].types = types;
		return;
	}

	ltls_TypedNode* items = realloc(index->node_types, sizeof(ltls_TypedNode) * (index->node_type_count + 1));
	if (!items) return;
	index->node_types = items;
	index->node_types[index->node_type_count].node = node;
	index->node_types[index->node_type_count].types = types;
	index->node_type_count++;
}

ltls_TypeSet ltls_index_type_of(const ltls_Index* index, lt_AstNode* node)
{
	for (uint32_t i = 0; i < index->node_type_count; i++)
		if (index->node_types[i].node == node) return index->node_types[i].types;
	return 0;
}

ltls_TypeSet ltls_index_returns(const ltls_Index* index, lt_AstNode* node)
{
	for (uint32_t i = 0; i < index->function_return_count; i++)
		if (index->function_returns[i].node == node) return index->function_returns[i].types;
	return 0;
}

ltls_TypeSet ltls_index_add_return(ltls_Index* index, lt_AstNode* node, ltls_TypeSet types)
{
	for (uint32_t i = 0; i < index->function_return_count; i++)
	{
		if (index->function_returns[i].node != node) continue;
		index->function_returns[i].types = ltls_type_union(index, index->function_returns[i].types, types);
		return index->function_returns[i].types;
	}

	ltls_TypedNode* items = realloc(index->function_returns, sizeof(ltls_TypedNode) * (index->function_return_count + 1));
	if (!items) return 0;
	index->function_returns = items;
	index->function_returns[index->function_return_count].node = node;
	index->function_returns[index->function_return_count].types = types;
	index->function_return_count++;
	return types;
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

const ltls_Declaration* ltls_index_lookup(const ltls_Index* index, lt_Scope* scope, lt_Token* token)
{
	const ltls_Declaration* declaration = resolve_lexical(index, scope, token);
	if (declaration) return declaration;
	return resolve_global(index, token);
}

static const ltls_Declaration* resolve_table_field(const ltls_Index* index, lt_AstNode* table, lt_Token* token)
{
	for (uint32_t i = 0; i < index->declaration_count; i++)
	{
		const ltls_Declaration* declaration = &index->declarations[i];
		if (declaration->kind == LTLS_DECL_FIELD && declaration->node == table && declaration->name->idx == token->idx)
			return declaration;
	}
	return 0;
}

const ltls_Declaration* ltls_index_member(const ltls_Index* index, lt_AstNode* class_node, lt_Token* token)
{
	return resolve_member(index, class_node, token);
}

const ltls_Declaration* ltls_index_table_field(const ltls_Index* index, lt_AstNode* table, lt_Token* token)
{
	return resolve_table_field(index, table, token);
}

/* `obj.name` and `@name` resolve through every alternative of the receiver's
   type: class instances look up members, tables look up literal keys. */
static const ltls_Declaration* resolve_receiver_types(const ltls_Index* index, ltls_TypeSet types, lt_Token* token)
{
	for (ltls_TypeSet type = types; type; type = type->next)
	{
		if (type->kind == LTLS_TYPE_INSTANCE)
		{
			const ltls_Declaration* declaration = resolve_member(index, type->node, token);
			if (declaration) return declaration;
		}
		else if (type->kind == LTLS_TYPE_TABLE)
		{
			const ltls_Declaration* declaration = resolve_table_field(index, type->node, token);
			if (declaration) return declaration;
		}
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
			if (candidate->receiver_expr)
				return resolve_receiver_types(index, ltls_index_type_of(index, candidate->receiver_expr), candidate->token);
			return resolve_member(index, candidate->class_node, candidate->token);
		}

		declaration = ltls_index_lookup(index, candidate->scope, candidate->token);
		return declaration;
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

static const char* type_node_name(const ltls_Index* index, lt_AstNode* node)
{
	if (!node) return "?";
	if (node->type == LT_AST_NODE_CLASS) return token_text(index, node->class_decl.identifier);
	return "?";
}

void ltls_type_describe(const ltls_Index* index, ltls_TypeSet types, char* buffer, size_t size)
{
	if (size == 0) return;
	buffer[0] = 0;

	if (!types)
	{
		snprintf(buffer, size, "unknown");
		return;
	}

	size_t used = 0;
	for (ltls_TypeSet type = types; type; type = type->next)
	{
		const char* name = "unknown";
		switch (type->kind)
		{
		case LTLS_TYPE_NUMBER: name = "number"; break;
		case LTLS_TYPE_STRING: name = "string"; break;
		case LTLS_TYPE_BOOL: name = "bool"; break;
		case LTLS_TYPE_NULL: name = "null"; break;
		case LTLS_TYPE_TABLE: name = "table"; break;
		case LTLS_TYPE_ARRAY: name = "array"; break;
		case LTLS_TYPE_FUNCTION: name = "fn"; break;
		case LTLS_TYPE_CLASS: name = type_node_name(index, type->node); break;
		case LTLS_TYPE_INSTANCE: name = type_node_name(index, type->node); break;
		default: name = "unknown"; break;
		}

		int written = snprintf(buffer + used, size - used, "%s%s", used ? " | " : "", name);
		if (written < 0) break;
		used += (size_t)written;
		if (used >= size) break;
	}
}

char* ltls_declaration_signature(const ltls_Index* index, const ltls_Declaration* declaration, ltls_TypeSet type_override)
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

	if (declaration->kind == LTLS_DECL_LOCAL || declaration->kind == LTLS_DECL_PARAM ||
	    declaration->kind == LTLS_DECL_FIELD || declaration->kind == LTLS_DECL_GLOBAL)
	{
		char type_name[128];
		ltls_type_describe(index, type_override ? type_override : declaration->types, type_name, sizeof(type_name));
		if (strcmp(type_name, "unknown") != 0)
		{
			string_append(&string, ": ");
			string_append(&string, type_name);
		}
	}

	if (!string.text) string_append(&string, "");
	return string.text;
}
