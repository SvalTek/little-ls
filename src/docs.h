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
	lt_Span span;
	char* description;
	ltls_DocParam* params;
	uint32_t param_count;
	ltls_DocReturn* returns;
	uint32_t return_count;
	char** tags;
	uint32_t tag_count;
} ltls_Doc;

/* Finds the `;;` comment block whose last line is the line before `line`
   (1-based) and parses its annotations. Returns 0 when there is none. */
uint8_t ltls_doc_for_line(const lt_Tokenizer* tokens, uint32_t line, ltls_Doc* doc);

void ltls_doc_free(ltls_Doc* doc);
