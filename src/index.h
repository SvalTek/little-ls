#pragma once

#include <stdint.h>

#include "little.h"

typedef enum {
	LTLS_TYPE_UNKNOWN = 0,
	LTLS_TYPE_NUMBER,
	LTLS_TYPE_STRING,
	LTLS_TYPE_BOOL,
	LTLS_TYPE_NULL,
	LTLS_TYPE_TABLE,
	LTLS_TYPE_ARRAY,
	LTLS_TYPE_FUNCTION,
	LTLS_TYPE_CLASS,
	LTLS_TYPE_INSTANCE
} ltls_TypeKind;

/* Immutable union of alternatives. `node` identifies the shape for table,
   function, class and instance kinds. */
typedef struct ltls_TypeSetNode {
	uint8_t kind;
	lt_AstNode* node;
	struct ltls_TypeSetNode* next;
} ltls_TypeSetNode;

typedef ltls_TypeSetNode* ltls_TypeSet;

typedef enum {
	LTLS_DECL_LOCAL,
	LTLS_DECL_PARAM,
	LTLS_DECL_GLOBAL,
	LTLS_DECL_FUNCTION,
	LTLS_DECL_CLASS,
	LTLS_DECL_FIELD,
	LTLS_DECL_METHOD,
	LTLS_DECL_PROPERTY
} ltls_DeclKind;

typedef struct {
	lt_Token* name;
	lt_Span span;
	lt_Scope* scope;
	lt_AstNode* node;
	lt_AstNode* function_node; /* synthetic function whose returns std text decorates */
	const lt_ClassMember* member;
	ltls_TypeSet types;
	char* annotation;   /* field type text from a doc shape */
	const char* returns_text; /* return type text for standard library functions */
	char* detail;       /* doc description for synthetic members */
	const char* signature; /* display signature override (std symbols) */
	uint8_t kind;
	uint8_t is_global;
	uint8_t from_doc;
	uint8_t external;
} ltls_Declaration;

typedef struct {
	lt_Token* token;
	lt_AstNode* node;
	lt_Scope* scope;
	lt_AstNode* class_node;
	lt_AstNode* receiver_expr;
	ltls_TypeSet types;
	uint8_t member;
} ltls_Reference;

typedef struct {
	lt_AstNode* node;
	ltls_TypeSet types;
} ltls_TypedNode;

typedef struct {
	const lt_Token* token;
	char* text;
} ltls_TokenText;

typedef struct {
	char* text;
	lt_AstNode* node;
} ltls_Shape;

typedef struct {
	lt_AstNode* node;
	uint8_t known;        /* arity and per-position types are exact */
	uint32_t count;
	ltls_TypeSet* items;  /* one per position when known */
	ltls_TypeSet element; /* homogeneous element when arity is unknown */
} ltls_Tuple;

typedef struct {
	const lt_Tokenizer* tokens;
	const char* source;
	ltls_Declaration* declarations;
	uint32_t declaration_count;
	ltls_Reference* references;
	uint32_t reference_count;

	/* expression node -> type, function node -> return union */
	ltls_TypedNode* node_types;
	uint32_t node_type_count;
	ltls_TypedNode* function_returns;
	uint32_t function_return_count;

	/* array node -> element union (absent or null means unknown) */
	ltls_TypedNode* array_elements;
	uint32_t array_element_count;

	/* function node -> return stream from `unpack(...)` or a forwarded call */
	ltls_Tuple* tuples;
	uint32_t tuple_count;

	/* text for synthetic tokens created from doc shapes */
	ltls_TokenText* token_texts;
	uint32_t token_text_count;

	/* structural type text -> anonymous table shape */
	ltls_Shape* shapes;
	uint32_t shape_count;
	lt_Token** token_allocations;
	uint32_t token_allocation_count;
	lt_AstNode** node_allocations;
	uint32_t node_allocation_count;

	ltls_TypeSetNode** allocations;
	uint32_t allocation_count;

	const lt_Analysis* analysis; /* borrowed from ltls_index_build */
} ltls_Index;

/* Walks the analysis tree once. Read-only: scopes are not modified. */
void ltls_index_build(ltls_Index* index, const lt_Analysis* analysis);
void ltls_index_free(ltls_Index* index);

/* Infers expression and declaration types (called by ltls_index_build). */
void ltls_types_build(ltls_Index* index, const lt_Analysis* analysis);

/* Resolves a name through the lexical chain, then globals. */
const ltls_Declaration* ltls_index_lookup(const ltls_Index* index, lt_Scope* scope, lt_Token* token);

/* Token name, including synthetic doc-shape tokens. */
const char* ltls_index_token_text(const ltls_Index* index, const lt_Token* token);

/* Anonymous table shape prepared for a structural type like `{ a: number }`. */
lt_AstNode* ltls_index_shape(const ltls_Index* index, const char* text);

/* Member/field lookup by class node or table literal node. */
const ltls_Declaration* ltls_index_member(const ltls_Index* index, lt_AstNode* class_node, lt_Token* token);
const ltls_Declaration* ltls_index_table_field(const ltls_Index* index, lt_AstNode* table, lt_Token* token);

/* Members visible on a class (including superclasses, child first) or a table,
   deduplicated by name. Returns the number written, capped at max. */
uint32_t ltls_index_members(const ltls_Index* index, lt_AstNode* class_node,
                            const ltls_Declaration** out, uint32_t max);
uint32_t ltls_index_table_members(const ltls_Index* index, lt_AstNode* table,
                                  const ltls_Declaration** out, uint32_t max);

/* The innermost `with` receiver expression or enclosing class node at offset,
   or NULL. Used for `@` completion. */
lt_AstNode* ltls_index_receiver_at(const ltls_Index* index, uint32_t offset);

/* The enclosing class node or owning table literal at offset, ignoring `with`
   receivers. Used for `this` completion. */
lt_AstNode* ltls_index_self_at(const ltls_Index* index, uint32_t offset);

/* Flow type recorded for an expression occurrence, or NULL. */
ltls_TypeSet ltls_index_type_of(const ltls_Index* index, lt_AstNode* node);

/* Union helpers; type sets are immutable and owned by the index. */
ltls_TypeSet ltls_type_add(ltls_Index* index, ltls_TypeSet types, uint8_t kind, lt_AstNode* node);
ltls_TypeSet ltls_type_union(ltls_Index* index, ltls_TypeSet left, ltls_TypeSet right);
uint8_t ltls_type_equals(ltls_TypeSet left, ltls_TypeSet right);
void ltls_index_set_type(ltls_Index* index, lt_AstNode* node, ltls_TypeSet types);
ltls_TypeSet ltls_index_returns(const ltls_Index* index, lt_AstNode* node);
ltls_TypeSet ltls_index_add_return(ltls_Index* index, lt_AstNode* node, ltls_TypeSet types);

/* Array values carry their element union: annotations `T[]`, literals and
   element-preserving producers. `element` null means unknown. */
ltls_TypeSet ltls_type_array(ltls_Index* index, ltls_TypeSet element);
ltls_TypeSet ltls_index_array_element(const ltls_Index* index, lt_AstNode* node);
void ltls_index_set_array_element(ltls_Index* index, lt_AstNode* node, ltls_TypeSet element);

/* Return stream of a function that produces multiple values. */
void ltls_index_set_tuple(ltls_Index* index, lt_AstNode* node, ltls_TypeSet element,
                          const ltls_TypeSet* items, uint32_t count, uint8_t known);
const ltls_Tuple* ltls_index_tuple(const ltls_Index* index, lt_AstNode* node);

/* The FN node for a declaration, when it declares one. */
lt_AstNode* ltls_index_function_node(const ltls_Declaration* declaration);

/* Renders a type set such as `LLMProvider` or `number | string`. */
void ltls_type_describe(const ltls_Index* index, ltls_TypeSet types, char* buffer, size_t size);

/* The declaration whose name covers `offset`, if any. */
const ltls_Declaration* ltls_index_declaration_at(const ltls_Index* index, uint32_t offset);

/* Resolves the identifier at `offset` to its declaration. When the offset is
   on a reference, `*reference` receives it (may be NULL). */
const ltls_Declaration* ltls_index_resolve(const ltls_Index* index, uint32_t offset, const ltls_Reference** reference);

/* Renders a one-line signature such as `fn increment(amount)`. When
   `type_override` is set it is used instead of the declaration's weak type. */
char* ltls_declaration_signature(const ltls_Index* index, const ltls_Declaration* declaration, ltls_TypeSet type_override);
