#pragma once

#include <stdint.h>

#include "little.h"

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
	const lt_ClassMember* member;
	uint8_t kind;
	uint8_t is_global;
} ltls_Declaration;

typedef struct {
	lt_Token* token;
	lt_Scope* scope;
	lt_AstNode* class_node;
	uint8_t member;
	uint8_t in_with;
} ltls_Reference;

typedef struct {
	const lt_Tokenizer* tokens;
	const char* source;
	ltls_Declaration* declarations;
	uint32_t declaration_count;
	ltls_Reference* references;
	uint32_t reference_count;
} ltls_Index;

/* Walks the analysis tree once. Read-only: scopes are not modified. */
void ltls_index_build(ltls_Index* index, const lt_Analysis* analysis);
void ltls_index_free(ltls_Index* index);

/* The declaration whose name covers `offset`, if any. */
const ltls_Declaration* ltls_index_declaration_at(const ltls_Index* index, uint32_t offset);

/* Resolves the identifier at `offset` to its declaration. When the offset is
   on a reference, `*reference` receives it (may be NULL). */
const ltls_Declaration* ltls_index_resolve(const ltls_Index* index, uint32_t offset, const ltls_Reference** reference);

/* Renders a one-line signature such as `fn increment(amount)`. Caller frees. */
char* ltls_declaration_signature(const ltls_Index* index, const ltls_Declaration* declaration);
