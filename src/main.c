#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "little_frontend.h"
#include "little_api.h"
#include "lsp.h"

#define LTLS_VERSION "0.1.0"

static void ignore_error(lt_VM* vm, const char* message)
{
	(void)vm;
	(void)message;
}

static char* read_file(const char* path, size_t* length)
{
	FILE* file = fopen(path, "rb");
	if (!file) return 0;

	if (fseek(file, 0, SEEK_END) != 0)
	{
		fclose(file);
		return 0;
	}

	long size = ftell(file);
	if (size < 0 || fseek(file, 0, SEEK_SET) != 0)
	{
		fclose(file);
		return 0;
	}

	char* text = malloc((size_t)size + 1);
	if (!text)
	{
		fclose(file);
		return 0;
	}

	size_t read = fread(text, 1, (size_t)size, file);
	fclose(file);

	text[read] = 0;
	*length = read;
	return text;
}

static int check_file(lt_VM* vm, const char* path)
{
	size_t length = 0;
	char* source = read_file(path, &length);
	if (!source)
	{
		fprintf(stderr, "little-ls: cannot read %s\n", path);
		return 2;
	}

	lt_Analysis analysis;
	lt_analysis_run(vm, &analysis, source, path);

	uint32_t count = lt_diagnostic_count(vm);
	for (uint32_t i = 0; i < count; i++)
	{
		const lt_Diagnostic* diagnostic = lt_diagnostic_get(vm, i);
		fprintf(stderr, "%s|%u:%u: %s\n", path, diagnostic->line, diagnostic->col, diagnostic->message);
	}

	lt_analysis_free(vm, &analysis);
	free(source);

	return count ? 1 : 0;
}

static void usage(FILE* stream)
{
	fprintf(stream,
		"Usage: little-ls [--stdio]\n"
		"       little-ls --check FILE...\n"
		"\n"
		"  --stdio        Serve the language server protocol on stdin/stdout (default).\n"
		"  --check FILE   Parse FILE and report diagnostics without executing it.\n"
		"  -v, --version  Show the server version.\n"
		"  -h, --help     Show this help.\n");
}

int main(int argc, char** argv)
{
	if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))
	{
		usage(stdout);
		return 0;
	}

	if (argc >= 2 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0))
	{
		printf("%s %s\n", "little-ls", LTLS_VERSION);
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "--check") == 0)
	{
		if (argc < 3)
		{
			usage(stderr);
			return 2;
		}

		lt_VM* vm = lt_open(malloc, free, ignore_error);
		if (!vm)
		{
			fprintf(stderr, "little-ls: failed to create VM\n");
			return 2;
		}

		int status = 0;
		for (int i = 2; i < argc; i++)
		{
			int result = check_file(vm, argv[i]);
			if (result == 2) status = 2;
			else if (result == 1 && status == 0) status = 1;
		}

		lt_destroy(vm);
		return status;
	}

	if (argc == 1 || (argc == 2 && strcmp(argv[1], "--stdio") == 0))
	{
#ifdef _WIN32
		_setmode(_fileno(stdin), _O_BINARY);
		_setmode(_fileno(stdout), _O_BINARY);
#endif
		ltls_serve_stdio();
		return 0;
	}

	usage(stderr);
	return 2;
}
