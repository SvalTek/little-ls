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

#define AT(type, buffer, index) \
	((type*)((char*)(buffer).data + (buffer).element_size * (index)))

typedef struct {
	ltls_Index* index;
	const lt_Analysis* analysis;
	ltls_TypeSet* env;
	lt_Scope* frame;
	lt_AstNode* function;
	uint8_t changed;
} ltls_Context;

static const char* token_text(const ltls_Index* index, const lt_Token* token)
{
	if (!token) return 0;
	if (token->idx >= index->tokens->identifier_buffer.length) return 0;
	return AT(lt_Identifier, index->tokens->identifier_buffer, token->idx)->name;
}

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

static ltls_TypeSet annotation_segment(ltls_Context* context, char* text)
{
	while (*text == ' ' || *text == '\t') text++;
	size_t length = strlen(text);
	while (length && (text[length - 1] == ' ' || text[length - 1] == '\t')) text[--length] = 0;
	if (length == 0) return 0;

	if (strcmp(text, "?") == 0 || strcmp(text, "any") == 0 || strcmp(text, "unknown") == 0) return 0;

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
		lt_AstNode* klass = find_class(context->index, text);
		if (!klass) return 0;
		kind = LTLS_TYPE_INSTANCE;
		node = klass;
	}

	return ltls_type_add(context->index, 0, kind, node);
}

/* `A | B`, `?`, class names, primitives. */
static ltls_TypeSet annotation_type(ltls_Context* context, const char* type)
{
	if (!type) return 0;

	char buffer[160];
	size_t length = strlen(type);
	if (length == 0 || length >= sizeof(buffer)) return 0;
	memcpy(buffer, type, length + 1);

	ltls_TypeSet result = 0;
	char* segment = buffer;
	for (char* position = buffer; ; position++)
	{
		if (*position != '|' && *position != 0) continue;

		char saved = *position;
		*position = 0;
		result = ltls_type_union(context->index, result, annotation_segment(context, segment));
		if (saved == 0) break;
		segment = position + 1;
	}
	return result;
}

/* `@type T` and `@class T` above a line describe the annotated value. */
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
		else if (strncmp(tag, "@class ", 7) == 0)
			result = ltls_type_union(context->index, result, annotation_type(context, tag + 7));
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

static ltls_TypeSet member_type(ltls_Context* context, ltls_TypeSet receiver, lt_Token* member)
{
	ltls_TypeSet result = 0;
	for (ltls_TypeSet type = receiver; type; type = type->next)
	{
		const ltls_Declaration* declaration = 0;
		if (type->kind == LTLS_TYPE_INSTANCE)
			declaration = ltls_index_member(context->index, type->node, member);
		else if (type->kind == LTLS_TYPE_TABLE)
			declaration = ltls_index_table_field(context->index, type->node, member);

		if (declaration) result = ltls_type_union(context->index, result, declaration->types);
	}
	return result;
}

static void apply_function_docs(ltls_Context* context, lt_AstNode* fn)
{
	ltls_Doc doc;
	if (!context->analysis || !ltls_doc_for_line(&context->analysis->tokens, fn->loc.line, &doc)) return;

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
	}

	ltls_doc_free(&doc);
}

static ltls_TypeSet analyze_function(ltls_Context* context, lt_AstNode* fn)
{
	ltls_TypeSet* saved_env = context->env;
	lt_Scope* saved_frame = context->frame;
	lt_AstNode* saved_function = context->function;

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

	return ltls_type_add(context->index, 0, LTLS_TYPE_FUNCTION, fn);
}

static ltls_TypeSet analyze_call(ltls_Context* context, lt_AstNode* node)
{
	ltls_TypeSet callee = analyze_expression(context, node->call.callee);
	for (uint8_t i = 0; i <= LT_MAX_CALL_ARGS + 1; i++)
		analyze_expression(context, node->call.args[i]);

	ltls_TypeSet result = 0;
	for (ltls_TypeSet type = callee; type; type = type->next)
	{
		if (type->kind == LTLS_TYPE_CLASS)
			result = ltls_type_add(context->index, result, LTLS_TYPE_INSTANCE, type->node);
		else if (type->kind == LTLS_TYPE_FUNCTION)
			result = ltls_type_union(context->index, result, ltls_index_returns(context->index, type->node));
	}
	return result;
}

static ltls_TypeSet analyze_index(ltls_Context* context, lt_AstNode* node)
{
	ltls_TypeSet receiver = analyze_expression(context, node->index.source);

	ltls_Reference* reference = reference_for_node(context->index, node, 1);
	if (reference) reference->types = receiver;

	lt_Token* member = member_token(node->index.idx);
	if (!member) return 0;

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

static void assign_target(ltls_Context* context, lt_AstNode* target, ltls_TypeSet types)
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
		for (ltls_TypeSet type = receiver; type; type = type->next)
		{
			ltls_Declaration* declaration = 0;
			if (type->kind == LTLS_TYPE_INSTANCE)
				declaration = (ltls_Declaration*)ltls_index_member(context->index, type->node, member);
			else if (type->kind == LTLS_TYPE_TABLE)
				declaration = (ltls_Declaration*)ltls_index_table_field(context->index, type->node, member);

			if (declaration) write_declaration(context, declaration, types);
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

	case LT_AST_NODE_ARRAY:
		for (uint32_t i = 0; i < node->array.values.length; i++)
			analyze_expression(context, *AT(lt_AstNode*, node->array.values, i));
		type = ltls_type_add(context->index, 0, LTLS_TYPE_ARRAY, node);
		break;

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
		assign_target(context, node->assign.left, value);
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
		if (context->function)
		{
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

		ltls_Declaration* declaration = declaration_for_token(context->index, node->declare.identifier);
		if (declaration)
		{
			if (type) write_declaration(context, declaration, type);
			ltls_index_set_type(context->index, node, type);
		}
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

		analyze_body(&context, &root->chunk.body);

		free(context.env);
		if (!context.changed) break;
	}
}
