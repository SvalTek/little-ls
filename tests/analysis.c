/* Regression tests for the analysis API that backs the language server:
   token spans, source positions and structured diagnostics. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "little.h"

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

static lt_Token* find_token(lt_Analysis* analysis, lt_TokenType type, int occurrence)
{
	for (uint32_t i = 0; i < analysis->tokens.token_buffer.length; i++)
	{
		lt_Token* token = (lt_Token*)analysis->tokens.token_buffer.data + i;
		if (token->type == type && occurrence-- == 0) return token;
	}
	return 0;
}

int main(void)
{
	lt_VM* vm = lt_open(malloc, free, ignore_error);
	lt_Analysis analysis;

	/* Two-character operators cover two bytes. */
	{
		const char* source = "var a = 1 >= 2";
		lt_analysis_run(vm, &analysis, source, "test");

		lt_Token* token = find_token(&analysis, LT_TOKEN_GTE, 0);
		CHECK(token != 0, "GTE token missing");
		if (token)
		{
			CHECK(token->span.start == 10 && token->span.end == 12, "GTE span %u..%u, expected 10..12",
				token->span.start, token->span.end);
			CHECK(strncmp(source + token->span.start, ">=", 2) == 0, "GTE span does not cover the operator");
		}
		CHECK(lt_diagnostic_count(vm) == 0, "valid source produced %u diagnostics", lt_diagnostic_count(vm));

		lt_analysis_free(vm, &analysis);
	}

	/* Identifier and keyword spans point at their source text. */
	{
		const char* source = "fn greet(name) {\n  return name\n}";
		lt_analysis_run(vm, &analysis, source, "test");

		lt_Token* fn = find_token(&analysis, LT_TOKEN_FN, 0);
		lt_Token* name = find_token(&analysis, LT_TOKEN_IDENTIFIER, 1);
		CHECK(fn && fn->span.start == 0 && fn->span.end == 2, "fn span is wrong");
		CHECK(name && strncmp(source + name->span.start, "name", 4) == 0, "identifier span is wrong");
		CHECK(analysis.is_valid == 1, "valid program reported invalid");

		lt_analysis_free(vm, &analysis);
	}

	/* Multi-line strings report their start position and keep column
	   tracking correct for the tokens that follow. */
	{
		const char* source = "var s = \"a\nb\"\nvar t = 1";
		lt_analysis_run(vm, &analysis, source, "test");

		lt_Token* string = find_token(&analysis, LT_TOKEN_STRING_LITERAL, 0);
		lt_Token* var = find_token(&analysis, LT_TOKEN_VAR, 1);
		CHECK(string && string->line == 1 && string->col == 8, "string starts at %u:%u, expected 1:8",
			string ? string->line : 0, string ? string->col : 0);
		CHECK(string && source[string->span.start] == '"' && source[string->span.end - 1] == '"',
			"string span does not cover the quotes");
		CHECK(var && var->line == 3 && var->col == 0, "token after string at %u:%u, expected 3:0",
			var ? var->line : 0, var ? var->col : 0);

		lt_analysis_free(vm, &analysis);
	}

	/* A comment that ends the file must not scan past the terminator. */
	{
		const char* source = "var x = 1 ;no newline";
		lt_analysis_run(vm, &analysis, source, "test");

		CHECK(analysis.tokens.token_buffer.length == 5, "expected 5 tokens, got %u",
			analysis.tokens.token_buffer.length);
		lt_Token* end = find_token(&analysis, LT_TOKEN_END, 0);
		CHECK(end && end->span.start == strlen(source), "end token span %u, expected %u",
			end ? end->span.start : 0, (unsigned)strlen(source));
		CHECK(end && end->idx == 0, "end token carries a stale literal index");

		lt_analysis_free(vm, &analysis);
	}

	/* Diagnostics carry the offending span, not just a formatted string. */
	{
		const char* source = "var 10 = 1";
		lt_analysis_run(vm, &analysis, source, "test");

		CHECK(analysis.is_valid == 0, "invalid program reported valid");
		CHECK(lt_diagnostic_count(vm) == 1, "expected 1 diagnostic, got %u", lt_diagnostic_count(vm));

		const lt_Diagnostic* diagnostic = lt_diagnostic_get(vm, 0);
		CHECK(diagnostic && diagnostic->span.start == 4 && diagnostic->span.end == 6,
			"diagnostic span %u..%u, expected 4..6",
			diagnostic ? diagnostic->span.start : 0, diagnostic ? diagnostic->span.end : 0);
		CHECK(diagnostic && strstr(diagnostic->message, "Expected identifier") != 0,
			"unexpected message: %s", diagnostic ? diagnostic->message : "(null)");
		CHECK(diagnostic && diagnostic->severity == LT_DIAG_ERROR, "unexpected severity");

		lt_analysis_free(vm, &analysis);
	}

	/* Incomplete input still yields a diagnostic and a partial tree. */
	{
		const char* source = "fn greet(name) {\n  var message = string.\n";
		lt_analysis_run(vm, &analysis, source, "test");

		CHECK(lt_diagnostic_count(vm) == 1, "expected 1 diagnostic, got %u", lt_diagnostic_count(vm));
		CHECK(analysis.parser.root != 0, "partial syntax tree missing");

		lt_analysis_free(vm, &analysis);
	}

	/* Repeated analysis replaces diagnostics instead of accumulating them. */
	{
		const char* source = "var 1 = 2";
		for (int i = 0; i < 100; i++)
		{
			lt_analysis_run(vm, &analysis, source, "test");
			CHECK(lt_diagnostic_count(vm) == 1, "diagnostics accumulated to %u", lt_diagnostic_count(vm));
			lt_analysis_free(vm, &analysis);
		}
	}

	lt_destroy(vm);
	printf(failures ? "analysis tests failed: %d\n" : "analysis tests passed\n", failures);
	return failures ? 1 : 0;
}
