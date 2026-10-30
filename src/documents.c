#include "documents.h"

#include <stdlib.h>
#include <string.h>

static char* copy_string(const char* value)
{
	size_t length = strlen(value);
	char* copy = malloc(length + 1);
	if (copy) memcpy(copy, value, length + 1);
	return copy;
}

ltls_Document* ltls_documents_find(ltls_Documents* documents, const char* uri)
{
	for (uint32_t i = 0; i < documents->length; i++)
	{
		if (strcmp(documents->items[i].uri, uri) == 0) return &documents->items[i];
	}
	return 0;
}

ltls_Document* ltls_documents_set(ltls_Documents* documents, const char* uri, const char* text, int64_t version)
{
	ltls_Document* document = ltls_documents_find(documents, uri);
	if (!document)
	{
		if (documents->length == documents->capacity)
		{
			uint32_t capacity = documents->capacity ? documents->capacity * 2 : 8;
			ltls_Document* items = realloc(documents->items, capacity * sizeof(ltls_Document));
			if (!items) return 0;

			documents->items = items;
			documents->capacity = capacity;
		}

		document = &documents->items[documents->length++];
		document->uri = copy_string(uri);
		if (!document->uri)
		{
			documents->length--;
			return 0;
		}
		document->text = 0;
	}

	char* copy = copy_string(text);
	if (!copy) return 0;

	free(document->text);
	document->text = copy;
	document->version = version;
	return document;
}

void ltls_documents_remove(ltls_Documents* documents, const char* uri)
{
	for (uint32_t i = 0; i < documents->length; i++)
	{
		if (strcmp(documents->items[i].uri, uri) != 0) continue;

		free(documents->items[i].uri);
		free(documents->items[i].text);
		documents->items[i] = documents->items[documents->length - 1];
		documents->length--;
		return;
	}
}

void ltls_documents_clear(ltls_Documents* documents)
{
	for (uint32_t i = 0; i < documents->length; i++)
	{
		free(documents->items[i].uri);
		free(documents->items[i].text);
	}
	free(documents->items);
	documents->items = 0;
	documents->length = 0;
	documents->capacity = 0;
}
