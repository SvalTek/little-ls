# little-ls

A language server for Little. It embeds the C tokenizer, parser and analysis
front end from the `little` repository; it never executes user code and does
not reimplement the language in TypeScript.

```text
little-ls --stdio        language server over stdio
little-ls --check FILE   print parse diagnostics for one file
```

## Documentation

- [features.md](features.md) — what each LSP request does.
- [typing.md](typing.md) — annotations, inference, receivers and imports.
- [limitations.md](limitations.md) — unsupported requests, conservative
  choices and oddities worth knowing.
- [architecture.md](architecture.md) — pipeline, modules and testing.

## Supported capabilities

| Capability | Status |
|---|---|
| `textDocument/didOpen`, `didChange`, `didClose` | full document sync |
| `textDocument/publishDiagnostics` | syntax errors only |
| `textDocument/documentSymbol` | top-level declarations and class members |
| `textDocument/hover` | signatures, docs, inferred types |
| `textDocument/definition` | same file and imported declarations |
| `textDocument/completion` | members, in-scope names, keywords |
| `textDocument/signatureHelp` | call under the cursor, active parameter |
| `textDocument/references` | same file, resolved declaration |
| `textDocument/documentHighlight` | same file |
| `textDocument/prepareRename` / `rename` | gated, same file |

Not implemented: formatting, code actions/quick fixes, workspace symbols,
folding, semantic tokens, inlay hints, code lens, call hierarchy, pull
diagnostics, file watching, or any execution/debugging.

## Quick model

- Every request analyses the document from scratch, with a fresh VM and
  index. There is no incremental parse or cache.
- Diagnostics include syntax errors and conservative type warnings for
  annotated declarations. Warnings only appear when both sides are known.
- Positions are UTF-16, as required by LSP.
- The server is single-file unless the file imports another module; imports
  are resolved and analysed statically (see [typing.md](typing.md#imports)).
