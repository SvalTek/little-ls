/* Tests for doc comment capture and annotation parsing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "docs.h"
#include "little_frontend.h"
#include "little_api.h"

static int failures = 0;

#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			failures++; \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

static void ignore_error(lt_VM* vm, const char* message)
{
	(void)vm;
	(void)message;
}

static uint8_t attach(lt_VM* vm, const char* source, uint32_t line, lt_Tokenizer* tokens, ltls_Doc* doc)
{
	*tokens = lt_tokenize(vm, source, "test.little");
	return ltls_doc_for_line(tokens, line, doc);
}

int main(void)
{
	lt_VM* vm = lt_open(malloc, free, ignore_error);
	lt_Tokenizer tokens;
	ltls_Doc doc;

	/* Comments are captured with source spans and lines. */
	{
		const char* source = "var x = 1 ; trailing\n;; doc line\n";
		tokens = lt_tokenize(vm, source, "test.little");
		CHECK(tokens.comment_buffer.length == 2, "expected 2 comments, got %u", tokens.comment_buffer.length);

		lt_Comment* first = (lt_Comment*)tokens.comment_buffer.data;
		lt_Comment* second = (lt_Comment*)tokens.comment_buffer.data + 1;
		CHECK(first->line == 1 && second->line == 2, "comment lines are %u and %u", first->line, second->line);
		CHECK(strncmp(source + second->span.start, ";; doc line", second->span.end - second->span.start) == 0,
			"comment span does not cover the text");
		lt_free_tokenizer(vm, &tokens);
	}

	/* A `;;` inside a string is not a comment. */
	{
		const char* source = "var s = \";; not a comment\"\n";
		tokens = lt_tokenize(vm, source, "test.little");
		CHECK(tokens.comment_buffer.length == 0, "expected no comments, got %u", tokens.comment_buffer.length);
		lt_free_tokenizer(vm, &tokens);
	}

	/* Doc block with description, parameters, defaults and returns. */
	{
		const char* source =
			"; ordinary header\n"
			";; Adds one to a counter.\n"
			";;\n"
			";; More detail.\n"
			";; @param number amount [1] how much to add\n"
			";; @return number the new total\n"
			"fn increment(amount) {\n"
			"}\n";

		CHECK(attach(vm, source, 7, &tokens, &doc) == 1, "doc block not attached");
		CHECK(doc.description && strcmp(doc.description, "Adds one to a counter.\n\nMore detail.") == 0,
			"unexpected description: '%s'", doc.description ? doc.description : "(null)");
		CHECK(doc.param_count == 1, "expected 1 param, got %u", doc.param_count);
		if (doc.param_count == 1)
		{
			CHECK(strcmp(doc.params[0].type, "number") == 0, "param type '%s'", doc.params[0].type);
			CHECK(strcmp(doc.params[0].name, "amount") == 0, "param name '%s'", doc.params[0].name);
			CHECK(doc.params[0].default_value && strcmp(doc.params[0].default_value, "1") == 0,
				"param default '%s'", doc.params[0].default_value ? doc.params[0].default_value : "(null)");
			CHECK(strcmp(doc.params[0].description, "how much to add") == 0,
				"param description '%s'", doc.params[0].description);
		}
		CHECK(doc.return_count == 1, "expected 1 return, got %u", doc.return_count);
		if (doc.return_count == 1)
		{
			CHECK(strcmp(doc.returns[0].type, "number") == 0, "return type '%s'", doc.returns[0].type);
			CHECK(doc.returns[0].name == 0, "return name should be empty");
			CHECK(strcmp(doc.returns[0].description, "the new total") == 0,
				"return description '%s'", doc.returns[0].description);
		}
		CHECK(doc.span.start == strstr(source, ";;") - source, "block span start is wrong");

		ltls_doc_free(&doc);
		lt_free_tokenizer(vm, &tokens);
	}

	/* Named returns and unknown tags. */
	{
		const char* source =
			";; @return number [count] the total\n"
			";; @deprecated use total()\n"
			"fn size() {\n"
			"}\n";

		CHECK(attach(vm, source, 3, &tokens, &doc) == 1, "doc block not attached");
		CHECK(doc.return_count == 1 && doc.returns[0].name && strcmp(doc.returns[0].name, "count") == 0,
			"named return missing");
		CHECK(doc.tag_count == 1 && strcmp(doc.tags[0], "@deprecated use total()") == 0,
			"tag not preserved");
		ltls_doc_free(&doc);
		lt_free_tokenizer(vm, &tokens);
	}

	/* Ordinary comments do not attach. */
	{
		const char* source = "; note\nfn f() {\n}\n";
		CHECK(attach(vm, source, 2, &tokens, &doc) == 0, "ordinary comment attached");
		lt_free_tokenizer(vm, &tokens);
	}

	/* A blank line breaks attachment. */
	{
		const char* source = ";; doc\n\nfn f() {\n}\n";
		CHECK(attach(vm, source, 3, &tokens, &doc) == 0, "doc attached across a blank line");
		lt_free_tokenizer(vm, &tokens);
	}

	/* A trailing `;;` after code is not a doc comment. */
	{
		const char* source = "var x = 1 ;; nope\nfn f() {\n}\n";
		CHECK(attach(vm, source, 2, &tokens, &doc) == 0, "trailing comment attached");
		lt_free_tokenizer(vm, &tokens);
	}

	/* A doc block only attaches to the immediately following line. */
	{
		const char* source = ";; doc\n\nfn f() {\n}\nfn g() {\n}\n";
		CHECK(attach(vm, source, 5, &tokens, &doc) == 0, "doc skipped lines to attach");
		lt_free_tokenizer(vm, &tokens);
	}

	lt_destroy(vm);
	printf(failures ? "docs tests failed: %d\n" : "docs tests passed\n", failures);
	return failures ? 1 : 0;
}
