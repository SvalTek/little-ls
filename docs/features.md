# Features

All behaviours below were verified against the current server. "Same file"
means the request only indexes the open document plus its imports' cloned
exports; other workspace files are not scanned.

## Diagnostics

`textDocument/publishDiagnostics` is pushed on open and on every change, and
cleared when the document closes.

- Only tokenizer and parser diagnostics are reported. Types, unresolved names
  and unused values never produce diagnostics.
- The parser recovers per statement, so a broken statement usually produces
  one diagnostic and the rest of the file still analyses. A dangling `.` or
  `:` reports at the operator rather than at the next line.
- Severity is error.

## Document symbols

`textDocument/documentSymbol` returns top-level `var`/`fn` declarations as
variables/functions and classes as classes with their members as children
(fields, methods, constructors, getters/setters). Declarations nested inside
functions or blocks are not emitted.

## Hover

`textDocument/hover` resolves the token under the cursor to a declaration.

- Markdown block with the declaration's signature, followed by its `;;` doc
  block when present, followed by binding text for standard-library members.
- `this` is special-cased: hovering it reports `this: ClassName` inside a
  class, or `this: table` inside a receiver function, and highlights the
  hovered token.
- Standard-library symbols show the signature and docs from the
  `[LittleBinding]` comments in the `little` sources. No definition location
  exists for them.
- Imported declarations show the signature and docs copied from the module;
  the hover range highlights the hovered token, not a location in this file.
- `@name` hovers resolve against the active receiver; `this.name` against the
  enclosing class or owning table.
- Virtual `@class`/`@field` declarations are hoverable; their fields resolve
  like real members.

## Definition

`textDocument/definition` returns one location.

- Same-file declarations: the declaration's name range.
- Imported declarations: the module file URI and the original name range.
- Named-import locals point at the local import declaration, not the module.
- `null` for standard-library/external symbols and for synthetic
  declarations that have no source location (doc-shape classes/fields and
  structural-shape fields).

## Completion

`textDocument/completion` returns a `CompletionList` (no `isIncomplete`).

Member context, after `.`, `:` or `@`:

| Receiver | Members |
|---|---|
| class instance | fields/methods/properties through the superclass chain |
| table | literal keys; standard modules; imported module tables; structural shapes |
| `@` | active receiver: innermost `with`, else `this` |
| `this.` | enclosing class or the table literal that owns a receiver function, ignoring `with` |

General context (no receiver):

- locals and parameters whose scope contains the cursor, declared before it;
- classes and functions declared before the cursor;
- standard-library global modules and functions (`io`, `string`, `pcall`,
  `setTimeout`, …); members such as `string.split` appear after a receiver;
- virtual classes from `@class`;
- keywords.

Items carry `label`, `kind`, `detail` (signature or `var x: T`) and
`documentation` where one exists. There is no context filtering: keywords are
offered everywhere, and there is no auto-import or snippet insertion.

## Signature help

`textDocument/signatureHelp` finds the innermost unclosed `(` before the
cursor, ignoring strings and line comments, and resolves its callee.

- Label is the declaration signature (`fn string.format(template, ...)`).
- Parameters are split on commas outside `[]`, e.g. `array.slice(array,
  start [, count])` becomes three labels with the optional marker trimmed.
- `activeParameter` is the comma depth inside that call.
- Parameter documentation is read from the declaration's `- name: text`
  bullets, i.e. standard-library and imported bindings. User doc comments do
  not currently populate parameter documentation here.
- Works for method calls (`obj:method(`) and standard functions.
- Final-argument `unpack(...)` expansion is not modelled by signature help.

## References and highlights

`textDocument/references` returns every token that resolves to the same
declaration in the open file, optionally including the declaration itself.

- Identity is the resolved declaration, so same-named members of different
  classes are distinct.
- Standard-library/external symbols return no references.
- Imported declarations are matched through their cloned declarations; only
  the open file is searched.
- `includeDeclaration` adds the declaration itself; imported declarations
  point at their module location, and synthetic declarations are omitted.
- `documentHighlight` returns declaration + references as `Text` highlights;
  synthetic declarations contribute only their references.

## Rename

`textDocument/prepareRename` returns the declaration name range and
placeholder, or `null`. `textDocument/rename` returns a `changes` edit for the
open file.

Rename is refused when the declaration:

- is external (standard library) or a user `global`;
- has a synthetic name (doc-shape `@class`/`@field`, structural-shape fields,
  cloned imports, implicit `this`);
- is a member reached through a union receiver where more than one
  alternative defines the name;
- is a member whose receiver type is unknown (dynamic `obj:method` or `@`
  without a known receiver).

Accepted renames edit the declaration and every same-file reference. Rename
is same-file only; there is no workspace scan.

Known footgun: shorthand destructuring keys share the key and local token
(`import { greet } from "mod"`, `var { name } = t`). Renaming such a local
renames the key too, which changes what is imported or read. Alias forms
(`{ greet: g }`) are safe.

## Protocol details

| Setting | Value |
|---|---|
| `textDocumentSync` | 1 (full) |
| completion triggers | `.` `:` `@` |
| signature help triggers | `(` `,` |
| rename | `prepareProvider: true` |
| position encoding | UTF-16 |
| server binary | external process over stdio (`little.server.path` / `LITTLE_LS_PATH`) |
