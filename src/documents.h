#pragma once

#include <stdint.h>

typedef struct {
	char* uri;
	char* text;
	int64_t version;
} ltls_Document;

typedef struct {
	ltls_Document* items;
	uint32_t length;
	uint32_t capacity;
} ltls_Documents;

ltls_Document* ltls_documents_find(ltls_Documents* documents, const char* uri);
ltls_Document* ltls_documents_set(ltls_Documents* documents, const char* uri, const char* text, int64_t version);
void ltls_documents_remove(ltls_Documents* documents, const char* uri);
void ltls_documents_clear(ltls_Documents* documents);
