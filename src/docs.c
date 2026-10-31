#include "docs.h"

#include <stdlib.h>
#include <string.h>

#define BUFFER_AT(type, buffer, index) \
	((type*)((char*)(buffer).data + (buffer).element_size * (index)))

static uint8_t is_space(char character)
{
	return character == ' ' || character == '\t';
}

static char* duplicate(const char* text, size_t length)
{
	char* copy = malloc(length + 1);
	if (!copy) return 0;
	memcpy(copy, text, length);
	copy[length] = 0;
	return copy;
}

static char* trim_copy(const char* text, size_t length)
{
	while (length && is_space(text[0])) { text++; length--; }
	while (length && (is_space(text[length - 1]) || text[length - 1] == '\r')) length--;
	return duplicate(text, length);
}

static uint8_t is_doc_comment(const char* source, const lt_Comment* comment)
{
	const char* text = source + comment->span.start;
	return comment->span.end >= comment->span.start + 2 && text[0] == ';' && text[1] == ';';
}

/* Doc comments must own their line; a trailing `;;` after code is not one. */
static uint8_t is_standalone(const char* source, uint32_t offset)
{
	while (offset > 0)
	{
		char character = source[offset - 1];
		if (character == '\n') return 1;
		if (!is_space(character)) return 0;
		offset--;
	}
	return 1;
}

static char* next_word(const char** cursor, const char* end)
{
	const char* position = *cursor;
	while (position < end && is_space(*position)) position++;

	const char* start = position;
	while (position < end && !is_space(*position)) position++;
	*cursor = position;

	if (position == start) return 0;
	return duplicate(start, (size_t)(position - start));
}

static char* rest_text(const char* cursor, const char* end)
{
	return trim_copy(cursor, (size_t)(end - cursor));
}

/* Like next_word, but records the word's span in the source. */
static char* next_word_at(const char** cursor, const char* end, const char* source, lt_Span* span)
{
	const char* position = *cursor;
	while (position < end && is_space(*position)) position++;

	const char* start = position;
	while (position < end && !is_space(*position)) position++;
	*cursor = position;

	if (span)
	{
		span->start = (uint32_t)(start - source);
		span->end = (uint32_t)(position - source);
	}
	if (position == start) return 0;
	return duplicate(start, (size_t)(position - start));
}

/* `[value]`, with spaces allowed inside the brackets. */
static char* bracket_value(const char** cursor, const char* end)
{
	const char* position = *cursor;
	while (position < end && is_space(*position)) position++;
	if (position >= end || *position != '[') return 0;

	const char* start = position + 1;
	const char* close = start;
	while (close < end && *close != ']') close++;
	*cursor = close < end ? close + 1 : close;
	return trim_copy(start, (size_t)(close - start));
}

static void push_param(ltls_Doc* doc, ltls_DocParam param)
{
	ltls_DocParam* items = realloc(doc->params, sizeof(ltls_DocParam) * (doc->param_count + 1));
	if (!items)
	{
		free(param.type);
		free(param.name);
		free(param.default_value);
		free(param.description);
		return;
	}
	doc->params = items;
	doc->params[doc->param_count++] = param;
}

static void push_return(ltls_Doc* doc, ltls_DocReturn entry)
{
	ltls_DocReturn* items = realloc(doc->returns, sizeof(ltls_DocReturn) * (doc->return_count + 1));
	if (!items)
	{
		free(entry.type);
		free(entry.name);
		free(entry.description);
		return;
	}
	doc->returns = items;
	doc->returns[doc->return_count++] = entry;
}

static void push_tag(ltls_Doc* doc, const char* text, size_t length)
{
	char** items = realloc(doc->tags, sizeof(char*) * (doc->tag_count + 1));
	char* copy = trim_copy(text, length);
	if (!items || !copy)
	{
		free(items);
		free(copy);
		return;
	}
	doc->tags = items;
	doc->tags[doc->tag_count++] = copy;
}

static void push_field(ltls_Doc* doc, ltls_DocField field)
{
	ltls_DocField* items = realloc(doc->fields, sizeof(ltls_DocField) * (doc->field_count + 1));
	if (!items)
	{
		free(field.type);
		free(field.name);
		free(field.description);
		return;
	}
	doc->fields = items;
	doc->fields[doc->field_count++] = field;
}

static void append_description(ltls_Doc* doc, const char* text, size_t length)
{
	size_t used = doc->description ? strlen(doc->description) : 0;
	char* next = realloc(doc->description, used + (used ? 1 : 0) + length + 1);
	if (!next) return;

	doc->description = next;
	if (used) doc->description[used++] = '\n';
	memcpy(doc->description + used, text, length);
	doc->description[used + length] = 0;
}

static void parse_annotation(ltls_Doc* doc, const char* text, size_t length, const char* source,
                             uint16_t line, uint32_t line_start)
{
	const char* end = text + length;
	const char* tag_start = text + 1;
	const char* tag_end = tag_start;
	while (tag_end < end && !is_space(*tag_end)) tag_end++;

	size_t tag_length = (size_t)(tag_end - tag_start);

	if (tag_length == 5 && memcmp(tag_start, "param", 5) == 0)
	{
		const char* cursor = tag_end;
		char* type = next_word(&cursor, end);
		char* name = next_word(&cursor, end);
		if (type && name)
		{
			char* default_value = bracket_value(&cursor, end);
			ltls_DocParam param = { type, name, default_value, rest_text(cursor, end) };
			push_param(doc, param);
		}
		else
		{
			free(type);
			free(name);
			push_tag(doc, text, length);
		}
		return;
	}

	if (tag_length == 6 && memcmp(tag_start, "return", 6) == 0)
	{
		const char* cursor = tag_end;
		char* type = next_word(&cursor, end);
		if (type)
		{
			char* name = bracket_value(&cursor, end);
			ltls_DocReturn entry = { type, name, rest_text(cursor, end) };
			push_return(doc, entry);
		}
		else push_tag(doc, text, length);
		return;
	}

	if (tag_length == 5 && memcmp(tag_start, "class", 5) == 0)
	{
		const char* cursor = tag_end;
		lt_Span name_span = { 0, 0 };
		char* name = next_word_at(&cursor, end, source, &name_span);
		if (!name)
		{
			push_tag(doc, text, length);
			return;
		}

		char* base = 0;
		const char* position = cursor;
		while (position < end && is_space(*position)) position++;
		if (position < end && *position == ':')
		{
			cursor = position + 1;
			base = next_word(&cursor, end);
		}

		doc->class_name = name;
		doc->class_base = base;
		doc->class_span = name_span;
		doc->class_line = line;
		doc->class_col = (uint16_t)(name_span.start - line_start);
		return;
	}

	if (tag_length == 5 && memcmp(tag_start, "field", 5) == 0)
	{
		const char* cursor = tag_end;
		char* type = next_word(&cursor, end);
		lt_Span name_span = { 0, 0 };
		char* name = next_word_at(&cursor, end, source, &name_span);

		if (!type || !name)
		{
			free(type);
			free(name);
			push_tag(doc, text, length);
			return;
		}

		ltls_DocField field;
		field.type = type;
		field.name = name;
		field.description = rest_text(cursor, end);
		field.name_span = name_span;
		field.line = line;
		field.col = (uint16_t)(name_span.start - line_start);
		push_field(doc, field);
		return;
	}

	push_tag(doc, text, length);
}

static uint32_t block_start(const lt_Tokenizer* tokens, uint32_t last)
{
	const char* source = tokens->source;
	uint32_t first = last;

	while (first > 0)
	{
		lt_Comment* previous = BUFFER_AT(lt_Comment, tokens->comment_buffer, first - 1);
		lt_Comment* current = BUFFER_AT(lt_Comment, tokens->comment_buffer, first);
		if (previous->line + 1 != current->line) break;
		if (!is_doc_comment(source, previous)) break;
		if (!is_standalone(source, previous->span.start)) break;
		first--;
	}

	return first;
}

static uint8_t build_block(const lt_Tokenizer* tokens, uint32_t first, uint32_t last, ltls_Doc* doc)
{
	const char* source = tokens->source;

	lt_Comment* start_comment = BUFFER_AT(lt_Comment, tokens->comment_buffer, first);
	lt_Comment* end_comment = BUFFER_AT(lt_Comment, tokens->comment_buffer, last);
	doc->span.start = start_comment->span.start;
	doc->span.end = end_comment->span.end;

	for (uint32_t i = first; i <= last; i++)
	{
		lt_Comment* comment = BUFFER_AT(lt_Comment, tokens->comment_buffer, i);
		const char* text = source + comment->span.start + 2;
		size_t length = (size_t)(comment->span.end - comment->span.start - 2);

		if (length && is_space(*text)) { text++; length--; }
		while (length && (is_space(text[length - 1]) || text[length - 1] == '\r')) length--;

		uint32_t line_start = comment->span.start - comment->col;

		if (length && text[0] == '@') parse_annotation(doc, text, length, source, comment->line, line_start);
		else
		{
			const char* description = text;
			size_t description_length = length;
			while (description_length && is_space(*description)) { description++; description_length--; }
			append_description(doc, description, description_length);
		}
	}

	return 1;
}

uint8_t ltls_doc_for_line(const lt_Tokenizer* tokens, uint32_t line, ltls_Doc* doc)
{
	memset(doc, 0, sizeof(ltls_Doc));

	const char* source = tokens->source;
	uint32_t count = tokens->comment_buffer.length;
	if (!source || line < 2 || count == 0) return 0;

	uint32_t last = (uint32_t)-1;
	for (uint32_t i = 0; i < count; i++)
	{
		lt_Comment* comment = BUFFER_AT(lt_Comment, tokens->comment_buffer, i);
		if (comment->line != line - 1) continue;
		if (!is_doc_comment(source, comment)) continue;
		if (!is_standalone(source, comment->span.start)) continue;
		last = i;
	}
	if (last == (uint32_t)-1) return 0;

	return build_block(tokens, block_start(tokens, last), last, doc);
}

uint8_t ltls_doc_block(const lt_Tokenizer* tokens, uint32_t comment_index, ltls_Doc* doc)
{
	memset(doc, 0, sizeof(ltls_Doc));

	const char* source = tokens->source;
	uint32_t count = tokens->comment_buffer.length;
	if (!source || count == 0 || comment_index >= count) return 0;

	lt_Comment* comment = BUFFER_AT(lt_Comment, tokens->comment_buffer, comment_index);
	if (!is_doc_comment(source, comment)) return 0;
	if (!is_standalone(source, comment->span.start)) return 0;

	uint32_t first = block_start(tokens, comment_index);
	uint32_t last = comment_index;

	while (last + 1 < count)
	{
		lt_Comment* next = BUFFER_AT(lt_Comment, tokens->comment_buffer, last + 1);
		lt_Comment* current = BUFFER_AT(lt_Comment, tokens->comment_buffer, last);
		if (next->line != current->line + 1) break;
		if (!is_doc_comment(source, next)) break;
		if (!is_standalone(source, next->span.start)) break;
		last++;
	}

	return build_block(tokens, first, last, doc);
}

void ltls_doc_free(ltls_Doc* doc)
{
	free(doc->description);
	for (uint32_t i = 0; i < doc->param_count; i++)
	{
		free(doc->params[i].type);
		free(doc->params[i].name);
		free(doc->params[i].default_value);
		free(doc->params[i].description);
	}
	free(doc->params);
	for (uint32_t i = 0; i < doc->return_count; i++)
	{
		free(doc->returns[i].type);
		free(doc->returns[i].name);
		free(doc->returns[i].description);
	}
	free(doc->returns);
	for (uint32_t i = 0; i < doc->field_count; i++)
	{
		free(doc->fields[i].type);
		free(doc->fields[i].name);
		free(doc->fields[i].description);
	}
	free(doc->fields);
	free(doc->class_name);
	free(doc->class_base);
	for (uint32_t i = 0; i < doc->tag_count; i++) free(doc->tags[i]);
	free(doc->tags);
	memset(doc, 0, sizeof(ltls_Doc));
}
