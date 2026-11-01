/* Static module loading for tooling: resolves `import "path"` without running
   the module, analyses the file and clones its exported value into the current
   index so hover, completion and definition work across files. */
#include "index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#define AT(type, buffer, index) \
	((type*)((char*)(buffer).data + (buffer).element_size * (index)))

static char* workspace_root;

static void ignore_error(lt_VM* vm, const char* message)
{
	(void)vm;
	(void)message;
}

static char* copy_text(const char* text)
{
	if (!text) return 0;
	size_t length = strlen(text);
	char* copy = malloc(length + 1);
	if (copy) memcpy(copy, text, length + 1);
	return copy;
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* file:///x:/a%20b -> x:/a b (the leading slash is dropped for drive paths). */
static char* path_from_uri(const char* uri)
{
	if (!uri) return 0;
	const char* start = uri;
	if (strncmp(uri, "file://", 7) == 0) start = uri + 7;

	char* result = malloc(strlen(start) + 1);
	if (!result) return 0;

	char* out = result;
	for (const char* cursor = start; *cursor; cursor++)
	{
		if (*cursor == '%' && hex_value(cursor[1]) >= 0 && hex_value(cursor[2]) >= 0)
		{
			*out++ = (char)(hex_value(cursor[1]) * 16 + hex_value(cursor[2]));
			cursor += 2;
		}
		else *out++ = *cursor;
	}
	*out = 0;

	if (result[0] == '/' && result[1] && result[2] == ':') memmove(result, result + 1, strlen(result));
	return result;
}

static char* uri_from_path(const char* path)
{
	if (!path) return 0;

	size_t length = strlen(path);
	char* result = malloc(length * 3 + 12);
	if (!result) return 0;

	char* out = result;
	memcpy(out, path[0] == '/' ? "file://" : "file:///", path[0] == '/' ? 7 : 8);
	out += path[0] == '/' ? 7 : 8;

	for (const char* cursor = path; *cursor; cursor++)
	{
		if (*cursor == '\\') *out++ = '/';
		else if (*cursor == ' ') { memcpy(out, "%20", 3); out += 3; }
		else if (*cursor == '%') { memcpy(out, "%25", 3); out += 3; }
		else if (*cursor == '#') { memcpy(out, "%23", 3); out += 3; }
		else if (*cursor == '?') { memcpy(out, "%3F", 3); out += 3; }
		else *out++ = *cursor;
	}
	*out = 0;
	return result;
}

void ltls_imports_configure(const char* workspace_root_uri)
{
	free(workspace_root);
	workspace_root = workspace_root_uri ? path_from_uri(workspace_root_uri) : 0;
}

static char* directory_of(const char* path)
{
	if (!path) return 0;
	const char* slash = strrchr(path, '/');
	const char* backslash = strrchr(path, '\\');
	const char* separator = slash > backslash ? slash : backslash;
	if (!separator) return copy_text(".");

	size_t length = (size_t)(separator - path);
	if (length == 0) length = 1;
	if (length == 2 && path[1] == ':') length = 3;

	char* result = malloc(length + 1);
	if (!result) return 0;
	memcpy(result, path, length);
	result[length] = 0;
	return result;
}

static char* join_path(const char* base, const char* name)
{
	if (!base || !base[0]) return copy_text(name);

	size_t base_length = strlen(base);
	size_t name_length = strlen(name);
	char* result = malloc(base_length + name_length + 2);
	if (!result) return 0;

	memcpy(result, base, base_length);
	char* out = result + base_length;
	if (out[-1] != '/' && out[-1] != '\\') *out++ = '/';
	memcpy(out, name, name_length + 1);
	return result;
}

static uint8_t has_little_extension(const char* path)
{
	size_t length = strlen(path);
	if (length < 7) return 0;
	const char* suffix = path + length - 7;
	return suffix[0] == '.' &&
	       (suffix[1] == 'l' || suffix[1] == 'L') &&
	       (suffix[2] == 'i' || suffix[2] == 'I') &&
	       (suffix[3] == 't' || suffix[3] == 'T') &&
	       (suffix[4] == 't' || suffix[4] == 'T') &&
	       (suffix[5] == 'l' || suffix[5] == 'L') &&
	       (suffix[6] == 'e' || suffix[6] == 'E');
}

static char* read_file(const char* path)
{
	FILE* file = fopen(path, "rb");
	if (!file) return 0;

	if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
	long size = ftell(file);
	if (size < 0) { fclose(file); return 0; }
	rewind(file);

	char* source = malloc((size_t)size + 1);
	if (!source) { fclose(file); return 0; }

	size_t read = fread(source, 1, (size_t)size, file);
	fclose(file);
	source[read] = 0;
	return source;
}

static char* file_exists(const char* path)
{
	FILE* file = fopen(path, "rb");
	if (!file) return 0;
	fclose(file);
	return copy_text(path);
}

/* `base/spec.little` then `base/spec/init.little`; an explicit `.little`
   suffix is used as-is. */
static char* try_candidate(const char* base, const char* specifier)
{
	if (!base) return 0;

	if (has_little_extension(specifier))
	{
		char* path = join_path(base, specifier);
		char* found = path ? file_exists(path) : 0;
		free(path);
		return found;
	}

	char* path = join_path(base, specifier);
	if (!path) return 0;

	size_t length = strlen(path);
	char* suffixed = malloc(length + 8);
	if (!suffixed) { free(path); return 0; }
	memcpy(suffixed, path, length);
	memcpy(suffixed + length, ".little", 8);

	char* found = file_exists(suffixed);
	free(suffixed);
	if (found) { free(path); return found; }

	char* init = join_path(path, "init.little");
	free(path);
	found = init ? file_exists(init) : 0;
	free(init);
	return found;
}

static char* expand_root(const char* root, const char* specifier)
{
	const char* marker = strchr(root, '?');
	if (!marker) return join_path(root, specifier);

	const char* separator = strchr(specifier, '/');
	const char* backslash = strchr(specifier, '\\');
	if (!separator || (backslash && backslash < separator)) separator = backslash;

	size_t package_length = separator ? (size_t)(separator - specifier) : strlen(specifier);
	const char* subpath = separator ? separator + 1 : "";

	size_t total = strlen(root) + strlen(subpath) + package_length + 2;
	char* result = malloc(total);
	if (!result) return 0;

	char* out = result;
	for (const char* cursor = root; *cursor; cursor++)
	{
		if (*cursor == '?')
		{
			memcpy(out, specifier, package_length);
			out += package_length;
		}
		else *out++ = *cursor;
	}
	if (subpath[0])
	{
		if (out > result && out[-1] != '/' && out[-1] != '\\') *out++ = '/';
		memcpy(out, subpath, strlen(subpath));
		out += strlen(subpath);
	}
	*out = 0;
	return result;
}

static char* literal_string(const ltls_Index* index, const lt_Token* token)
{
	if (!token || token->type != LT_TOKEN_STRING_LITERAL) return 0;
	if (token->idx >= index->tokens->literal_buffer.length) return 0;

	lt_Literal* literal = AT(lt_Literal, index->tokens->literal_buffer, token->idx);
	return literal->string ? copy_text(literal->string) : 0;
}

typedef struct {
	char* roots[32];
	uint32_t count;
} ltls_Roots;

static void add_root(ltls_Roots* roots, const char* root)
{
	if (!root || !root[0] || roots->count >= 32) return;
	roots->roots[roots->count++] = copy_text(root);
}

/* Literal `module.addPath("...")` calls in this document. */
static void collect_add_paths(const ltls_Index* index, ltls_Roots* roots)
{
	const lt_Tokenizer* tokens = index->tokens;
	for (uint32_t i = 0; i + 4 < tokens->token_buffer.length; i++)
	{
		lt_Token* module = AT(lt_Token, tokens->token_buffer, i);
		const char* module_name = ltls_index_token_text(index, module);
		if (!module_name || strcmp(module_name, "module") != 0) continue;

		lt_Token* period = AT(lt_Token, tokens->token_buffer, i + 1);
		lt_Token* add = AT(lt_Token, tokens->token_buffer, i + 2);
		lt_Token* open = AT(lt_Token, tokens->token_buffer, i + 3);
		if (period->type != LT_TOKEN_PERIOD || open->type != LT_TOKEN_OPENPAREN) continue;

		const char* add_name = ltls_index_token_text(index, add);
		if (!add_name || strcmp(add_name, "addPath") != 0) continue;

		for (uint32_t j = i + 4; j < tokens->token_buffer.length; j++)
		{
			lt_Token* token = AT(lt_Token, tokens->token_buffer, j);
			if (token->type == LT_TOKEN_CLOSEPAREN) break;
			if (token->type == LT_TOKEN_COMMA) continue;

			char* path = literal_string(index, token);
			if (path)
			{
				add_root(roots, path);
				free(path);
			}
		}
	}
}

static const ltls_ImportEntry* find_import(const ltls_Index* index, const char* key)
{
	for (uint32_t i = 0; i < index->import_count; i++)
		if (strcmp(index->imports[i].key, key) == 0) return &index->imports[i];
	return 0;
}

static void remember_import(ltls_Index* index, const char* key, ltls_TypeSet value)
{
	ltls_ImportEntry* entries = realloc(index->imports, sizeof(ltls_ImportEntry) * (index->import_count + 1));
	if (!entries) return;

	index->imports = entries;
	index->imports[index->import_count].key = copy_text(key);
	index->imports[index->import_count].value = value;
	index->import_count++;
}

static char* current_directory(void)
{
	char buffer[4096];
	if (!getcwd(buffer, sizeof(buffer))) return 0;
	return copy_text(buffer);
}

static char* resolve_module(ltls_Index* index, const char* specifier)
{
	char* from = index->analysis && index->analysis->module ? path_from_uri(index->analysis->module) : 0;
	char* from_dir = directory_of(from);
	char* cwd = current_directory();
	char* found = 0;

	if (specifier[0] == '/' || (specifier[0] && specifier[1] == ':'))
		found = try_candidate("", specifier);

	const char* bases[3] = { from_dir, workspace_root, cwd };
	for (uint32_t i = 0; i < 3 && !found; i++)
		if (bases[i]) found = try_candidate(bases[i], specifier);

	if (!found)
	{
		ltls_Roots roots;
		memset(&roots, 0, sizeof(roots));
		collect_add_paths(index, &roots);

		for (uint32_t i = 0; i < roots.count && !found; i++)
		{
			for (uint32_t b = 0; b < 3 && !found; b++)
			{
				if (!bases[b]) continue;
				char* expanded = expand_root(roots.roots[i], specifier);
				if (!expanded) continue;
				char* joined = join_path(bases[b], expanded);
				free(expanded);
				char* candidate = joined ? try_candidate("", joined) : 0;
				free(joined);
				if (candidate) found = candidate;
			}
		}

		for (uint32_t i = 0; i < roots.count; i++) free(roots.roots[i]);
	}

	free(from);
	free(from_dir);
	free(cwd);
	return found;
}

ltls_TypeSet ltls_import_cached(const ltls_Index* index, const char* specifier)
{
	if (!index || !specifier) return 0;
	const ltls_ImportEntry* cached = find_import(index, specifier);
	return cached ? cached->value : 0;
}

ltls_TypeSet ltls_import_value(ltls_Index* index, const char* specifier)
{
	if (!index || !specifier || !specifier[0]) return 0;

	const ltls_ImportEntry* cached = find_import(index, specifier);
	if (cached) return cached->value;

	char* path = resolve_module(index, specifier);
	if (!path) return 0;

	/* Mark as loading so a cycle resolves to unknown instead of recursing. */
	remember_import(index, specifier, 0);

	char* source = read_file(path);
	if (!source)
	{
		free(path);
		return 0;
	}

	lt_VM* vm = lt_open(malloc, free, ignore_error);
	ltls_TypeSet value = 0;

	if (vm)
	{
		lt_Analysis analysis;
		char* uri = uri_from_path(path);
		lt_analysis_run(vm, &analysis, source, uri ? uri : path);

		ltls_Index module_index;
		ltls_index_build(&module_index, &analysis);
		value = ltls_index_clone_export(index, &module_index, &analysis, uri ? uri : path);
		ltls_index_free(&module_index);
		lt_analysis_free(vm, &analysis);
		lt_destroy(vm);
		free(uri);
	}

	free(source);

	for (uint32_t i = 0; i < index->import_count; i++)
		if (strcmp(index->imports[i].key, specifier) == 0) index->imports[i].value = value;

	free(path);
	return value;
}

/* Loads every literal import path in the document before type inference runs,
   so cloning never resizes the declaration table mid-analysis. */
void ltls_index_preload_imports(ltls_Index* index)
{
	const lt_Tokenizer* tokens = index->tokens;
	if (!tokens) return;

	for (uint32_t i = 0; i + 1 < tokens->token_buffer.length; i++)
	{
		lt_Token* token = AT(lt_Token, tokens->token_buffer, i);
		lt_Token* next = AT(lt_Token, tokens->token_buffer, i + 1);

		if (token->type == LT_TOKEN_IMPORT)
		{
			if (next->type == LT_TOKEN_STRING_LITERAL)
			{
				char* specifier = literal_string(index, next);
				if (specifier)
				{
					ltls_import_value(index, specifier);
					free(specifier);
				}
				continue;
			}

			if (next->type != LT_TOKEN_OPENBRACE) continue;
			/* `import { ... } from "x"` — find the path after `from`. */
			for (uint32_t j = i + 2; j + 1 < tokens->token_buffer.length; j++)
			{
				lt_Token* candidate = AT(lt_Token, tokens->token_buffer, j);
				if (candidate->type == LT_TOKEN_STRING_LITERAL)
				{
					char* specifier = literal_string(index, candidate);
					if (specifier)
					{
						ltls_import_value(index, specifier);
						free(specifier);
					}
					break;
				}
				if (candidate->type == LT_TOKEN_CLOSEBRACE) continue;
			}
		}
	}
}
