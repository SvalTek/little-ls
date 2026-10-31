# Typing

Little is dynamically typed. The server infers a best-effort type for every
expression and declaration; nothing here changes what the runtime accepts.
Unknown is always a valid answer and is never reported as an error.

## Annotations

Annotations live in `;;` doc comments directly above the declaration (the
block's last line must be the line before it).

```little
;; builds a greeting
;; @param string name the person to greet
;; @return string
fn greet(name) {
    return "hello " + name
}
```

Recognized tags:

| Tag | Meaning |
|---|---|
| `@param TYPE name [default]` | parameter type; `[value]` is an optional default |
| `@return TYPE` | first/multi return type |
| `@field TYPE name [default]` | field of a `@struct`/`@class` shape; `[value]` makes it optional |
| `@struct Name` | defines a named table structure (interface-like) |
| `@class Name [: Base]` | documents a class, or attaches docs to an existing one |
| `@type TYPE` | type for the declaration on the next line |
| any other line | description text shown in hover |

Type grammar:

- primitives: `number`, `string`, `bool`/`boolean`, `table`, `array`,
  `fn`/`function`, `null`/`nil`;
- class names, `@struct` names and `@class` shapes;
- unions with `|`, split outside braces: `string | null`;
- structural shapes: `{ foo: boolean, bar: string }`, nestable;
- array suffix: `number[]`, `{ ok: boolean }[]`, `string[] | null`.

### Structs

`@struct` defines a reusable named table structure, like a TypeScript
interface. It is a type, never a value.

```little
;; @struct LLMProviderOpts
;; Options for talking to a provider.
;; @field string baseUrl The provider base URL.
;; @field string token The token to use.
;; @field number port ["80"] Optional port.

;; @param LLMProviderOpts opts
fn connect(opts) {
    io.print(opts.baseUrl)
    with opts { io.print(@baseUrl) }
}

connect({ baseUrl: "localhost" token: "none" })          ; ok, port omitted
connect({ baseUrl: "localhost" token: "none" port: 80 }) ; ok
```

Rules:

- Annotations resolve a struct name to a named table type; hover renders the
  struct name, member access and completion use its fields, and `with` works.
- Fields are optional when a default `[value]` is given; structural checks
  allow such fields to be missing. Required fields must be present.
- Extra fields are allowed by structural compatibility.
- Structs do not have methods, constructors or inheritance.
- `@struct` has no generic parameters yet.
- Virtual `@class` shapes (a `@class` with no matching class) remain supported
  for compatibility, but `@struct` is the intended form for table shapes.

`?`, `any` and `unknown` mean unknown. Type text is trimmed and matched after
trimming; two structural types that differ only in internal whitespace are
different shapes. Annotations are resolved through a fixed 512-byte buffer.

## Inference

Two layers:

1. **Weak map** — every declaration accumulates the union of all values ever
   assigned or returned. This is what fields, globals, captured locals and
   forward references see.
2. **Flow environment** — inside a function, private locals and parameters
   (not captured by a closure) receive strong updates, so a use sees the value
   assigned before it. Captured locals, globals and fields never get strong
   updates.

Additional rules:

- Branches union both sides; loops iterate to a fixed point.
- The whole module pass repeats up to six times so forward references,
  recursion and imports settle.
- Class members, including inherited ones, resolve through the superclass
  chain (up to eight levels). `obj:method()` and `obj.method` share member
  lookup.
- Fields are created from `this.x = …` assignments and from `@name`
  constructor parameters even when the class body omits them. These are weak.
- Annotations override inference for parameters, returns, fields and
  `@type`-annotated declarations.

Rendered type text uses `unknown`, class names and `|` unions. Array element
types are not printed in the type name; they appear only through indexing.

## Receivers: `this` and `@`

These are different values and the distinction is maintained everywhere:

| Expression | Means |
|---|---|
| `this` | the enclosing class instance, or the table literal that owns a receiver function |
| `@name` | a member of the *active* receiver |

A receiver function is a table function whose first parameter is `this`
(`add: fn(this, t) { … }`); colon calls bind it. `with expr { … }` rebinds
`@` to `expr` for the block, including nested blocks, but never changes
`this`. After the block, the previous receiver returns.

`@name` resolution uses every alternative of the active receiver's type.
`this.name` never uses the `with` receiver.

## Arrays

`array` is a distinct tool type even though arrays are tables at runtime.
Element types are attached to array values:

- `[1, 2]` → element `number`; `["a"]` → `string`; `[{ ok: true }]` → the
  table literal shape;
- a mixed or empty literal keeps the `array` type with an unknown element;
- annotations `T[]` provide a homogeneous element;
- `array.slice`/`array.reverse` preserve their source element type;
- the standard binding `string.split` is annotated `string[]`;
- `arr[i]` returns the element type when it is known, otherwise unknown;
- numeric and string indices are not treated as member access.

Mixed arrays are not relabelled `table`; `table` remains the explicit escape
hatch when element tracking is not wanted.

## Multiple returns

Little has no tuple syntax. Multiple values come from `unpack(array)`:

```little
fn pair() {
    return unpack([1, "two"])
}
var [a, b] = pair()      ; a: number, b: string
var first = pair()       ; scalar context: number
fn forward() { return pair() }
var [m, n] = forward()   ; forwarded positions
var [x, y, z] = unpack([1, 2])  ; z: null
var { name } = table     ; key-based, single table value
```

Modelling rules:

- `return unpack(literal)` records exact positional types;
- `return unpack(array)` with an annotated/literal variable records a
  homogeneous element type and unknown arity;
- `return <call>` forwards the callee's return stream;
- scalar contexts take the first value; `return unpack([])` is `null`. When
  arity is unknown the first element type is used (a possible zero-return
  path is not unioned with `null`);
- array destructuring binds known positions exactly and pads past a known
  literal arity with `null`; with unknown arity every position gets the
  element type (missing slots are not modelled as `null`);
- table destructuring binds by key through table fields;
- final-argument expansion (`fn(1, unpack([2, 3]))`) is not typed.

## Generics

Generics are a tooling-only type system; they do not exist at runtime.

```little
;; @generic T
;; @param T value
;; @return T
fn identity(value) { return value }

;; @class Box<T>
;; @field T value
class Box {
    ;; @param T value
    constructor(value) { this.value = value }

    ;; @return T
    read() { return this.value }
}

var n = identity(1)        ; number
var b = Box(1)             ; Box<number>
var v = b.read()           ; number
var sb = Box("text")       ; Box<string>
```

Declarations:

- `@generic T, U` on a function doc block introduces type variables for that
  block's `@param`/`@return` annotations.
- `@class Name<T, U>` introduces class type parameters. They are also attached
  to every member function, so constructor and method annotations may use them.
- Type variables are ordinary type atoms in annotations and can appear in
  unions, arrays and generic arguments: `T | null`, `T[]`, `Box<T>`.

Instantiation:

- `Box<number>` in an annotation parses as an instantiated class type.
- Calling a class infers its arguments from the constructor's annotated
  parameters: `Box(1)` is `Box<number>`.
- Calling a generic function unifies argument types with parameter types.
  Supported shapes are a direct `T`, `T[]`, and generic class arguments such
  as `Box<T>`.
- Unbound variables become unknown. There are no defaults, bounds or explicit
  call-site type arguments.
- Member access on an instantiated type substitutes class parameters in field
  and method return types, so `b.read()` and `b.value` above are `number`.

Generic types render as `Box<number>` in hover and variable signatures.
Specializations are identified by the class and its argument types; the same
class with different arguments yields distinct types.

## Type diagnostics

Annotated types are enforced as **warnings**, never errors, and the runtime
still accepts everything. Diagnostic checks run in a separate verification
pass after inference settles.

Checked:

- call arguments against annotated parameters, including constructors;
- `@return` against the returned expression;
- writes to annotated fields/members, including generic substitution
  (`Box<number>.value` expects `number`).

Rules:

- A warning is emitted only when both sides are fully known. Unknown values,
  dynamic tables and unbound type variables never warn.
- Exact alternatives win first. If none match, structural acceptance applies:
  - a `@struct`, a virtual `@class` shape, or an anonymous `{ ... }` shape
    accepts a table literal or instance whose fields satisfy it (extra fields
    allowed, optional fields may be missing, mismatched fields warn);
  - `table`, `array` and `fn` are wildcards for any value of that kind;
  - real classes stay nominal: a plain table does not satisfy `Point`.
- Arrays compare element types when both are known.
- Structural matching is depth-limited; deeply nested shapes fall back to
  "compatible".
- Every alternative of the assigned type must be accepted; `number | string`
  does not satisfy `number`.
- An explicit annotation is authoritative: assignments are checked against it
  and do not widen the hover type.
- Warnings carry severity `Warning` (2) and the range of the offending
  argument/expression.

Not checked yet: assignments to unannotated locals/parameters, method
arguments on instantiated generic receivers, and calls through dynamic
receivers.

## Standard library

Standard symbols are generated from `[LittleBinding]` comments in the `little`
C sources: module, members, signature, description and `@return` type. They
are seeded as external declarations after user declarations, so a user global
with the same name shadows the standard one.

External symbols have no definition location and are never renamed.

## Imports

Literal import paths are resolved statically; modules are analysed, never
executed.

Resolution order:

1. absolute path;
2. the importing file's directory;
3. the workspace root from `initialize` (`rootUri`, `rootPath`, or the first
   workspace folder);
4. the process working directory;
5. literal `module.addPath("...")` roots found in the importing document,
   resolved against those bases and expanded like the runtime (`?` receives
   the first path segment).

Candidates match the runtime: exact for an explicit `.little` suffix,
otherwise `spec.little` then `spec/init.little`.

The exported value is the first top-level `return`; a module with no return
exports `true`. The server clones the exported value into the current index:

- tables: field declarations, including functions and nested tables;
- classes: the class, its members and resolved superclass chain;
- functions: signature, docs, scalar return type and return stream;
- arrays: element type;
- structural shapes and unions.

Named imports (`import { greet } from "mod"`) reuse table destructuring and
bind local types; `var m = import "mod"` yields the module table, so `m.greet`
and `m.` completion work. Definition on imported members jumps to the module
file. Cloned declarations carry their origin URI and token position.

Imports are preloaded before type inference, so cloning never resizes the
declaration table during analysis. Cyclic imports resolve to unknown.

Not resolved: dynamic import expressions, non-literal `module.addPath`
arguments, or search paths registered by code that is not in the importing
document. Diagnostics inside imported modules are not published.

Imported documentation is copied into the cloned declaration's detail text
(description and `- name: text` parameter bullets), not rendered from a live
doc block.
