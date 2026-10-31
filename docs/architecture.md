# Architecture

## Why a separate binary

`little-ls` embeds the C front end from the `little` repository — tokenizer,
parser, analysis wrapper — instead of reimplementing the language in
TypeScript. The VS Code extension is a thin client that launches the binary
over stdio (`little.server.path` or `LITTLE_LS_PATH`). The Little CLI and
runtime are unchanged by the server.

The server links only the analysis-capable core (`little.c`,
`little_analysis.c`, `little_buffer.c`, `little_common.c`, `little_async.c`)
and yyjson; the standard library is not linked because nothing is executed.

## Source layout

| File | Responsibility |
|---|---|
| `src/main.c` | argument handling, `--check`, stdio loop entry |
| `src/lsp.c` | JSON-RPC framing, capabilities, request builders, document store |
| `src/documents.c` | URI → text/version map |
| `src/docs.c` | `;;` doc comment parser (`@param`, `@return`, `@field`, `@class`, tags) |
| `src/index.c` | AST walk, declarations/references, doc shapes, std seeding, imports cloning, array/tuple tables |
| `src/types.c` | weak + flow inference, annotations, receivers, returns |
| `src/imports.c` | module path resolution, file loading, analysis, export cloning, import cache |
| `src/std_symbols.h` | generated from `[LittleBinding]` comments in `little` |

## Request lifecycle

Every request that needs semantics opens an `ltls_AnalysisState`:

1. `lt_open` creates a VM.
2. `lt_analysis_run` tokenizes and parses the document text. The document URI
   is passed as the module name, which is what import resolution uses.
3. `ltls_index_build` builds the semantic index:
   - walk the AST, adding declarations, references, implicit `this.X` fields
     and receiver state for `with`;
   - parse doc blocks: materialize `@class`/`@field` shapes and register
     structural types as anonymous tables;
   - seed the standard-library surface;
   - preload literal imports (see below);
   - infer types to a fixpoint.
4. A line index is built for offset ↔ position conversion.
5. The request builder reads the index and produces JSON.
6. Everything is freed; there is no cross-request cache.

`publishDiagnostics` uses a separate, shorter-lived VM because the parser can
abandon partially built buffers during recovery; a fresh VM bounds that
memory.

## Index model

- Declarations own their name token, kind, scope, weak type set, optional
  annotation text, signature/detail overrides and (for clones) origin URI and
  position.
- References record token, owning AST node, scope, class node, receiver
  expression and whether they are member accesses.
- Member lookup is text-based within an owner node, so synthetic declarations
  behave like real ones.
- Synthetic tokens carry text overrides because they have no source span.
  Anything synthetic has no definition location unless origin metadata was
  copied from a module.
- Types are immutable sets of `{kind, node}` alternatives, allocated in the
  index. Arrays and return streams are side tables keyed by node.
- Imported declarations are clones into the current index, so every consumer
  (hover, completion, references) sees one uniform declaration table.

## Type pass

`ltls_types_build` runs up to six passes over the module body:

1. apply annotations to declarations;
2. analyse the body with a flow environment for the current function;
3. repeat until nothing grows (forward references, recursion, forwarding).

Private uncaptured locals/params use the flow environment; everything else
uses the weak map. Function returns and `unpack` streams are written as the
body is analysed, so imports and forwarding settle over passes.

## Imports

Import calls are recognised by their synthetic callee name (`import`) because
the parser does not create a normal reference for them.

- Literal paths are preloaded during index build, before type inference, so
  cloning never reallocates the declaration table mid-pass.
- Resolution tries absolute paths, the document directory, the workspace
  root, cwd, then literal `module.addPath` roots; candidates are
  `spec.little` and `spec/init.little`.
- A resolved module is read, analysed in its own VM, indexed, and its exported
  value cloned into the caller's index. The module index and VM are freed
  immediately afterwards; only cloned declarations and origin metadata
  survive.
- The import cache is keyed by the literal specifier and lives on the index,
  so subsequent passes and sibling imports do not re-read the file.
- In-progress imports are marked before loading so cycles resolve to unknown.

## Testing

`test.ps1`:

1. builds and runs `tests/analysis.c` (parser/analysis behaviour);
2. builds and runs `tests/docs.c` (doc comment parsing);
3. regenerates `std_symbols.h` and fails if the checked-in file is stale;
4. builds the server;
5. runs `tests/lsp.mjs`, an end-to-end JSON-RPC client over stdio.

`tests/lsp.mjs` spawns the real binary, sends framed messages, waits for
matching response ids (`waitFor`) and asserts on JSON. It covers diagnostics,
recovery, symbols, hover, definition, completion, signature help, references,
highlights, rename, typing, receivers, structural types, arrays, multi-return
and cross-file imports. Cross-file tests create a real temporary project on
disk so import resolution exercises the filesystem.

## Conventions worth preserving

- The C front end is the single source of truth for parsing and diagnostics.
- Doc comments in `little` drive both the standard symbol table and the
  standard reference docs; do not hand-maintain either.
- Synthetic declarations must never be reported as definitions or renames
  unless they carry origin metadata.
- Protocol positions are UTF-16; internal spans are byte offsets.
- New std bindings need an `@signature` and, where inferable, `@return`;
  `test.ps1` enforces that the generated table matches the comments.
