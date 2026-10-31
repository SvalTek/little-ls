# Limitations and oddities

This page lists behaviours that are deliberately absent, conservative, or
surprising. It is not a bug list; most of these are bounded choices.

## Server model

- No incremental parsing. Every request builds a fresh VM, tokenizes, parses,
  indexes and infers from scratch. There is no cache between requests, so
  latency grows with file size.
- Single-file analysis plus statically cloned imports. Other workspace files
  are invisible: no workspace symbols, no cross-file references or rename, no
  file watching.
- No user configuration. Triggers, capabilities and search order are fixed.
- No request cancellation and no diagnostics debounce; diagnostics are pushed
  synchronously on open/change.

## Diagnostics

- Syntax only. Unresolved names, wrong argument counts, bad annotations and
  type mismatches produce no diagnostics.
- The parser recovers per statement. A statement can be dropped while the rest
  of the file still analyses; the server does not try to repair the AST.
- Error ranges sometimes point at an operator (dangling `.`/`:`) rather than
  the missing operand, which is intentional but can look surprising.

## Types

- Unknown is silent and normal. There is no "any" and no error for it.
- Type diagnostics are warnings only, and only for fully known types.
  Assignments to unannotated locals/params, method arguments on instantiated
  generic receivers, and calls through dynamic receivers are unchecked.
- Annotated declarations are authoritative: mismatched assignments warn and
  do not widen the hover type.
- Structural matching applies only to object/table shapes: virtual `@class`
  types and anonymous `{ ... }` shapes against tables or instances. Real
  classes remain nominal, and function types compare by identity.
- Missing expected fields fail, extra fields are accepted, and matching stops
  at a fixed depth. Anonymous-shape diagnostics render both sides as
  `table`, so the message does not name the offending field.
- `table`, `array` and `fn` annotations are wildcards for their kind.
- Dynamic member dispatch is unresolved: `obj:method()` or `obj.method` where
  `obj`'s type is unknown yields unknown, and completion offers nothing.
- A union receiver resolves member access to the first alternative that has
  the member; hover shows that declaration only. Ambiguity is only surfaced
  by refusing rename.
- `with` over an unannotated or untyped value gives `@` an unknown receiver.
- Mixed and empty array literals keep the `array` type with an unknown
  element; they are not relabelled `table`.
- Array destructuring from an unknown-arity array gives every position the
  element type; runtime `null` padding for missing positions is only modelled
  when the unpacked literal's arity is known.
- Final-argument `unpack(...)` expansion is not typed; arguments are treated
  as single values.
- Receiver functions are recognised only when they are values of a table
  literal with `this` as the first parameter. `t.f = fn(this, x) { … }` has no
  owning shape, so `this.f` inside it is unknown.
- Function values returned or stored dynamically do not carry their source
  signature unless they were declared in a class, table literal or module
  export.
- Type text renders arrays as `array`; element types appear only through
  indexing, not in the rendered type name.

## Generics

- Generics are tooling-only. No runtime effect, no diagnostics for mismatched
  arguments.
- `@generic` is only read on function docs. Class parameters must be declared
  with `@class Name<T>`.
- Inference unifies only a direct `T`, arrays `T[]`, and generic arguments
  such as `Box<T>`. Callbacks, structural shapes and deeper nesting are not
  unified.
- There are no explicit call-site type arguments, bounds, constraints,
  variance or defaults. An unbound variable is unknown.
- Generic superclass chains are not substituted; only the class's own members
  are specialised.
- Method and constructor parameter annotations keep `T`; substitution applies
  to member result types (fields, getters, method returns) reached through an
  instantiated receiver.
- Instantiation is capped at 16 type arguments and substitution recursion at
  8 levels.
- `Box<number>` is matched by exact argument sets; there is no widening or
  normalisation beyond type union identity.

## Annotations and docs

- Doc blocks must sit directly above the declaration: no blank line, `;;`
  only. A trailing `;;` after code is not a doc comment.
- Structural type identity is the trimmed type text. `{a: number}` and
  `{ a: number }` are different shapes, even though they describe the same
  fields.
- Annotations longer than 511 bytes are ignored.
- `@param` reads a type expression (word, balanced shape, flat `|` union) and
  then one name; extra words become the description.
- `@field` inside a doc block above an existing class only adds a type or
  description; it cannot add a new field to a real class.
- Virtual `@class`/`@field` and structural-shape fields have no source
  location: definition returns `null` and rename is refused.

## Completion

- No context filtering. Keywords are offered inside member access and inside
  strings/comments; the client is expected to filter.
- No auto-import, no snippet placeholders, no sorting beyond insertion order.
- Only fields/methods are offered after `.`/`:`; array indexing and table
  string keys are not completion contexts.
- Standard and imported members are offered, but their definitions are
  external (imported definitions jump to the module).

## Signature help

- Parameter documentation comes only from `- name: text` bullets in
  `declaration->detail`, i.e. standard-library and imported bindings. User doc
  comments are not shown here.
- Bracketed optional parameters are flattened (`[start,]` becomes a label).
- Does not model forwarded or expanded multi-value arguments.

## References, highlights, rename

- Same-file only, even for imported symbols.
- Identity is the resolved declaration, so shadowed locals and same-named
  members of different classes are kept apart.
- Rename is refused for external symbols, user `global`s, synthetic names,
  ambiguous union members and unknown receivers.
- Shorthand destructuring binds the key and local to the same token:
  `import { greet } from "mod"` and `var { name } = t`. Renaming such a local
  also renames the key, changing what is imported or read. Alias forms
  (`{ greet: g }`) avoid this.
- Rename edits the open file only; it will not fix other files that import a
  renamed exported symbol (imported declarations are refused outright).

## Definition

- `null` for standard-library symbols and for synthetic declarations with no
  source location.
- Named-import locals resolve to the local import declaration, not to the
  module's export.
- Cross-file ranges are reconstructed from stored line/column/length. They
  assume a single-line ASCII identifier; exotic names could shift the end
  column.

## Imports

- Literal paths only. `import someVariable` or computed paths are not
  resolved.
- `module.addPath(...)` is honoured only for literal arguments in the
  importing document; calls in imported modules do not affect the importing
  file's resolution.
- Search order prefers the importing file's directory, then the workspace
  root, then the process working directory. The runtime resolves relative to
  its working directory first; the editor order is chosen for project
  ergonomics.
- Modules are re-read and re-analysed on every request. There is no cache
  across requests.
- Cycles resolve to unknown. There is no cycle diagnostic.
- Diagnostics inside imported modules are not published.
- Bundled/archive loaders and `loadLibrary` are not modelled.
- Imported docs are flattened into text; live doc hover and doc-derived
  signature help are not available across files.

## Other requests

Not implemented: formatting, code actions/quick fixes, workspace symbols,
folding ranges, semantic tokens, inlay hints, code lens, call hierarchy, pull
diagnostics, execute command, file rename handling, or any execution and
debugging.
