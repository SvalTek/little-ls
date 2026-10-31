#pragma once

#include <stdint.h>

#include "little.h"

typedef struct {
	char* type;
	char* name;
	char* default_value;
	char* description;
} ltls_DocParam;

typedef struct {
	char* type;
	char* name;
	char* description;
} ltls_DocReturn;

typedef struct {
	char* type;
	char* name;
	char* description;
	lt_Span name_span;
	uint16_t line;
	uint16_t col;
} ltls_DocField;

typedef struct {
	lt_Span span;
	char* description;
	ltls_DocParam* params;
	uint32_t param_count;
	ltls_DocReturn* returns;
	uint32_t return_count;
	ltls_DocField* fields;
	uint32_t field_count;
	char* class_name;
	char* class_base;
	char** class_generics;
	uint32_t class_generic_count;
	char** generics;
	uint32_t generic_count;
	lt_Span class_span;
	uint16_t class_line;
	uint16_t class_col;
	char** tags;
	uint32_t tag_count;
} ltls_Doc;

/* Finds the `;;` comment block whose last line is the line before `line`
   (1-based) and parses its annotations. Returns 0 when there is none. */
uint8_t ltls_doc_for_line(const lt_Tokenizer* tokens, uint32_t line, ltls_Doc* doc);

/* Parses the doc block containing the comment at `comment_index`. */
uint8_t ltls_doc_block(const lt_Tokenizer* tokens, uint32_t comment_index, ltls_Doc* doc);

void ltls_doc_free(ltls_Doc* doc);
