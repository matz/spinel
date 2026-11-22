# Spinel AST format

The parser front-end consumes a Ruby source file and fills the node
table the rest of the compiler reads. This file documents the table's
text form (`spinel --dump-ast` prints it) -- the record types, the
flattening conventions, and the per-node fields the consumers rely on.

## Pipeline position

```
.rb  ──[sp_parse_file]──▶  NodeTable  ──[analyze_program + codegen_program]──▶  .c
```

Spinel is a single binary (`spinel`). `sp_parse_file`
(`src/spinel_parse.c`) is the only stage that talks to libprism; its walk
over Prism's tree fills a `NodeTable` (`src/node_table.c`) directly, node
by node, through the `nt_put_*` calls, and the analyzer and code
generator both read the table through the `nt_*` accessor API. There is
no text intermediate; `nt_dump` prints the finished table in the text
form below for `spinel --dump-ast`. The node shapes are the stable
contract between Prism (a moving upstream) and Spinel's fixed-shape
backend.

There is one parser implementation: **`src/spinel_parse.c`**, which links
libprism directly (no Ruby is needed at build time once `make deps` has
fetched the sources). The retired Ruby self-host had a `spinel_parse.rb`
fallback; the C compiler does not.

## File format

Plain UTF-8 text, line-oriented (`\n`-terminated). Every line is one
record. Fields are space-separated.

Records are not length-limited by the format: large source constructs
produce `A` records longer than 4096 bytes.

The first record is always:

```
ROOT <int>
```

naming the AST node id that the rest of the file roots at (always `0`
in practice, since `flatten()` walks the tree top-down and assigns ids
in DFS order). It is followed by the source-file header records:

```
SOURCE_FILE <path>        # the primary source path
FILE <fid> <path>         # file-id -> path map, one per distinct source file
```

The `FILE` records back the multi-file `#line` map: a node's
`node_file` integer attribute (below) indexes this table, so an error in
generated C points at the right original `.rb` even across an inlined
require. `nt_file_path(nt, fid)` resolves the id.

Every subsequent record begins with a single-letter tag:

| Tag | Shape                              | Meaning                                                              |
|-----|------------------------------------|----------------------------------------------------------------------|
| `N` | `N <id> <NodeType>`                | Node header. Introduces a new node; binds its id to its Prism type.  |
| `S` | `S <id> <field> <string>`          | String-valued attribute (`name`, `content`, `pattern`, etc.).        |
| `I` | `I <id> <field> <int>`             | Integer attribute (`value`, `flags`, and the `node_line` / `node_file` / `node_col` source position). |
| `F` | `F <id> <field> <float>`           | Float attribute (`value`). Stored as the node's `content` string.    |
| `R` | `R <id> <field> <child-id-or--1>`  | Single child reference. `-1` means the child slot is empty.          |
| `A` | `A <id> <field> <id,id,…>`         | Array of child references. Body is comma-separated; empty allowed.   |

Every node carries `node_line` / `node_file` / `node_col` integer
attributes (the source position Prism reports); codegen reads them via
`nt_int(nt, id, "node_line", 0)` etc. to emit `#line` directives under
`--line-map` (on by default) and `--debug`. Under `--emit-types` a node
also carries `node_end_line` / `node_end_col` (Prism's exclusive end),
which the JSON reports as the node's span; the ordinary compile has no
use for them and does not pay for them.

A node is printed as its `N` line followed by its attribute lines,
`S`, `I`, `F`, `R` and `A` in that order, each kind in the order the
parser set them. Downstream code addresses attributes by *field name*,
not position; when a node carries a field twice, the first one is the
one the accessors answer.

### Example

```ruby
puts "hi"
```

prints as (without the source and file records, the position attributes
and the `fzl` flag):

```
ROOT 0
N 0 ProgramNode
R 0 statements 1
N 1 StatementsNode
A 1 body 2
N 2 CallNode
S 2 name puts
R 2 receiver -1
R 2 arguments 3
R 2 block -1
N 3 ArgumentsNode
A 3 arguments 4
N 4 StringNode
S 4 content hi
```

A call written `recv&.name` also carries `I 2 safe_nav 1` (Prism's
safe-navigation flag, on the call-operator write nodes too); an ordinary
call carries no such field, and `nt_safe_nav` answers 0 for it.
A call written as an assignment (`recv.name = v`, `recv.name=(v)`,
`recv[i] = v`) carries `I 2 attr_write 1`, Prism's attribute-write flag;
`recv.send(:name=, v)` does not. A desugar that builds such an assignment
sets it, and `nt_attr_write` answers 0 for a node without the field.
An ArgumentsNode carries none of Prism's ArgumentsNodeFlags: the desugars
rewrite argument lists, so a bit set when the list was parsed would go
stale. What those flags say (a splat, keywords, a keyword splat, `...`) is
asked of the list itself through the `nt_argv_*` predicates, which answer
from its current elements; a call's `&blk` is its `block` field
(`nt_call_block_arg`).

Node ids are assigned in DFS pre-order during parse, so a child's id is
greater than its parent's id, except for the StatementsNode made for an
embedded instance variable (`"#@iv"`), which follows the variable it holds.

### Encoding rules

The table holds string values as their bytes (with their length, so a
NUL byte survives). `nt_dump` prints the bytes that would end a record
or a field, and the escape character itself, as `%XX`:

| Byte           | Printed as |
|----------------|------------|
| `%`            | `%25`      |
| `\n`           | `%0A`      |
| `\r`           | `%0D`      |
| `\t`           | `%09`      |
| space          | `%20`      |
| `\0`           | `%00`      |
| other          | passed through (bytes ≥ `0x80` verbatim, UTF-8) |

The same escaping applies to the `SOURCE_FILE` and `FILE` paths.

## Node-type taxonomy

`src/spinel_parse.c`'s `flatten()` enumerates every Prism node it
understands. Adding support for a new Ruby idiom usually means adding
one `case PM_<NODE>:` arm to the switch and a matching consumer-side
handler in `src/analyze*.c` / `src/codegen*.c`. The list below groups
the recognised nodes by category; field names per node are the ones
Prism documents, with a few Spinel-specific aliases for clarity.

### Program / scopes

| Node                | Fields used                              | Notes                                                 |
|---------------------|------------------------------------------|-------------------------------------------------------|
| `ProgramNode`       | `statements` (R)                         | Top-level wrapper. The root node.                     |
| `StatementsNode`    | `body` (A)                               | Sequence of statements -- methods bodies, blocks, etc.|
| `ClassNode`         | `constant_path` (R), `superclass` (R), `body` (R) | Class definition. `superclass` is `-1` for no parent. |
| `ModuleNode`        | `constant_path` (R), `body` (R)          | Module definition.                                    |
| `SingletonClassNode`| `expression` (R), `body` (R)             | `class << recv; ... end`.                             |
| `DefNode`           | `name` (S), `parameters` (R), `body` (R), `receiver` (R) | Method definition. `receiver` is non-`-1` for `def self.x` / `def obj.x`. |
| `LambdaNode`        | `locals` (S), `parameters` (R), `block_locals` (A), `body` (R) | `-> { ... }` / `lambda { ... }`. `parameters` is the ParametersNode itself; `block_locals` are the `; x` declarations of `->(a; x) {}`. |
| `BlockNode`         | `locals` (S), `parameters` (R), `body` (R) | `do ... end` / `{ ... }` attached to a call. `parameters` is a BlockParametersNode (or a NumberedParametersNode or ItParametersNode). |
| `BlockParametersNode` | `parameters` (R), `locals` (A)         | The `|a, b; x, y|` list of a block. `locals` holds one BlockLocalVariableNode per `; x` declaration, `[]` when there is none. |
| `BlockLocalVariableNode` | `name` (S)                          | One `; x` declaration: the name is fresh to the block, whatever the enclosing scope calls it. |
| `ParametersNode`    | `requireds` (A), `optionals` (A), `keywords` (A), `posts` (A), `rest` (R), `block` (R) | Per-method or per-block parameter list. Each parameter node (`RequiredParameterNode`, `OptionalParameterNode`, `RestParameterNode`, the keyword, keyword-rest and `BlockParameterNode` kinds) carries `name` (S), and `wname` (S) where a rename gave it another (see "References & assignment"). |
| `NumberedParametersNode` | `value` (I) (max param `_1` .. `_N`) | `{ _1 + _2 }` shorthand.                              |

### Control flow

| Node                | Fields used                                          |
|---------------------|------------------------------------------------------|
| `IfNode`            | `predicate` (R), `statements` (R), `subsequent` (R)  |
| `UnlessNode`        | `predicate` (R), `statements` (R), `consequent` (R)  |
| `ElseNode`          | `statements` (R)                                     |
| `CaseNode`          | `predicate` (R), `conditions` (A), `else_clause` (R) |
| `CaseMatchNode`     | `predicate` (R), `conditions` (A), `else_clause` (R) |
| `WhenNode`          | `conditions` (A), `statements` (R)                   |
| `InNode`            | `pattern` (R), `statements` (R)                      |
| `WhileNode`         | `predicate` (R), `statements` (R)                    |
| `UntilNode`         | `predicate` (R), `statements` (R)                    |
| `BreakNode` / `NextNode` / `RedoNode` / `ReturnNode` | `arguments` (R)         |
| `BeginNode`         | `statements` (R), `rescue_clause` (R), `else_clause` (R), `ensure_clause` (R) |
| `RescueNode`        | `exceptions` (A), `reference` (R), `statements` (R), `subsequent` (R) |
| `EnsureNode`        | `statements` (R)                                     |
| `YieldNode`         | `arguments` (R)                                      |
| `SuperNode` / `ForwardingSuperNode` | `arguments` (R), `block` (R)         |
| `CatchNode` / `ThrowNode` | (encoded as CallNode-style; no dedicated Prism node -- the parser injects helpers) |

### Calls

`CallNode` is the workhorse: every method invocation -- including
operators, attribute access, indexing -- compiles to one. The
disambiguating field is `name`, plus the recv / args / block triple.

| Node                | Fields used                                          | Notes                                       |
|---------------------|------------------------------------------------------|---------------------------------------------|
| `CallNode`          | `name` (S), `receiver` (R), `arguments` (R), `block` (R), `safe_nav` (I), `attr_write` (I), `vcall` (I) | Universal method call. `safe_nav` is 1 for `recv&.name`, `attr_write` is 1 for `recv.name = v`, `recv.name=(v)` and `recv[i] = v`, and `vcall` is 1 for a bare identifier; each is present only when Prism set the flag (a desugar that writes an assignment sets `attr_write`). |
| `ArgumentsNode`     | `arguments` (A)                                      | Positional + keyword + splat args.          |
| `KeywordHashNode`   | `elements` (A)                                       | Trailing keyword hash literal.              |
| `AssocNode`         | `key` (R), `value` (R)                               | One key=>value pair inside a Hash or kwargs.|
| `SplatNode`         | `expression` (R)                                     | `*args` -- splat in args or LHS.            |
| `BlockArgumentNode` | `expression` (R)                                     | `&block` -- pass block by name.             |
| `ForwardingArgumentsNode` | (no fields)                                    | `def f(...); g(...); end` shape.            |
| `BlockParameterNode`| `name` (S)                                           | `&blk` in a method's parameter list.        |

### Literals

| Node                  | Fields used                                | Notes                          |
|-----------------------|--------------------------------------------|--------------------------------|
| `IntegerNode`         | `value` (I)                                | Decimal, hex, oct, bin.        |
| `FloatNode`           | `value` (F)                                | Includes exponent forms.       |
| `RationalNode` / `ImaginaryNode` | `numeric` (R)                   | Wrapping number suffix.        |
| `StringNode`          | `content` (S)                              | Literal string.                |
| `SymbolNode`          | `content` (S)                              | Symbol literal.                |
| `InterpolatedStringNode` / `InterpolatedSymbolNode` | `parts` (A) | Each part is either a StringNode or an `EmbeddedStatementsNode`. |
| `EmbeddedStatementsNode` | `statements` (R)                        | `#{...}` inside a string.      |
| `RegularExpressionNode` | `content` (S), `flags` (I)               | `/pat/`; `flags` are Prism's options (`nt_regexp_opts`). |
| `InterpolatedRegularExpressionNode` | `parts` (A), `flags` (I)     | `/#{x}/`.                      |
| `ArrayNode`           | `elements` (A)                             | `[1, 2, 3]`.                   |
| `HashNode`            | `elements` (A) -- each an AssocNode        | `{a: 1}`.                      |
| `RangeNode`           | `left` (R), `right` (R), `flags` (I)       | `flags` & 4 = exclusive end (`nt_range_excl`). |
| `LambdaNode`          | (see above)                                | `-> x { x*2 }`.                |
| `TrueNode` / `FalseNode` / `NilNode` / `SelfNode` | (no fields)    | Singleton literals.            |

### References & assignment

| Node                              | Fields used                                |
|-----------------------------------|--------------------------------------------|
| `LocalVariableReadNode`           | `name` (S), `depth` (I)                    |
| `LocalVariableWriteNode`          | `name` (S), `depth` (I), `value` (R)       |
| `LocalVariableTargetNode`         | `name` (S), `depth` (I) -- LHS of `MultiWriteNode` |
| `LocalVariableOperatorWriteNode`  | `name` (S), `depth` (I), `value` (R), `operator` (S) |
| `LocalVariableOrWriteNode` / `LocalVariableAndWriteNode` | `name` (S), `depth` (I), `value` (R) |
| `InstanceVariableReadNode`        | `name` (S)                                 |
| `InstanceVariableWriteNode`       | `name` (S), `value` (R)                    |
| `InstanceVariableOperatorWriteNode` | (same triple, plus `operator` S)         |
| `ClassVariableReadNode` / `ClassVariableWriteNode` | `name` (S), `value` (R) |
| `GlobalVariableReadNode` / `GlobalVariableWriteNode` | `name` (S), `value` (R) |
| `ConstantReadNode`                | `name` (S)                                 |
| `ConstantPathNode`                | `parent` (R), `name` (S)                   |
| `ConstantWriteNode`               | `name` (S), `value` (R)                    |
| `MultiWriteNode`                  | `targets` (A), `expression` (R), `rest` (R) |
| `MultiTargetNode`                 | `lefts` (A), `rest` (R), `rights` (A)      |

`depth` (I) is Prism's: how many scopes up the local is bound (0: the scope the
node is in). `wname` (S) is the name the program wrote, on a local-binding or
local-reference node (the nodes above, a block-local, and the parameter nodes
below) that a pass renamed to give it a slot of its own: `name` is then the
slot, and `nt_wname` answers the written one, falling back to `name` for a
node nothing renamed or a desugar made. `nt_rename_local` renames a local
that way and keeps the first name in `wname`: the shadow rename of block
parameters and block-locals, and the keyword slots of a block handed to a
builtin. Three renames do not go through it yet and leave no `wname`: the
slot of a reassigned block parameter (`__bpin`), a class-body capture
(`__cb`) and the colliding numbered parameters (`_1__b…`).

### Index access

| Node                              | Fields used                                |
|-----------------------------------|--------------------------------------------|
| `IndexOrWriteNode` / `IndexAndWriteNode` / `IndexOperatorWriteNode` | `receiver` (R), `arguments` (R), `value` (R), plus `operator` for op-write |
| `CallOperatorWriteNode` / `CallAndWriteNode` / `CallOrWriteNode` | `receiver` (R), `read_name` (S), `write_name` (S), `value` (R), `operator` (S) -- `obj.x op= v` shape |
| (`a[i]` reads itself parse as a `CallNode` with `name == "[]"`. The `*OrWrite` / `*AndWrite` / `*OperatorWrite` nodes only appear for the assignment shape.) |

### Pattern matching

| Node                              | Fields used                                | Notes                                 |
|-----------------------------------|--------------------------------------------|---------------------------------------|
| `ArrayPatternNode`                | `requireds` (A), `rest` (R), `posts` (A)   | `case x; in [a, b]`                   |
| `HashPatternNode`                 | `elements` (A), `rest` (R)                 | `case x; in {a: 1}`                   |
| `FindPatternNode`                 | `left` (R), `requireds` (A), `right` (R)   | `case x; in [*, a, *]`                |
| `PinnedVariableNode`              | `variable` (R)                             | `^var` in a pattern.                  |
| `ImplicitNode`                    | `value` (R)                                | Sugar elision (`a:` short-hand).      |

### Forwarding / synthetic

| Node                              | Fields used                                |
|-----------------------------------|--------------------------------------------|
| `ForwardingArgumentsNode`         | (no fields)                                |
| `ForwardingParameterNode`         | (no fields)                                |

The parser also injects synthetic `CallNode`s for built-in idioms that
Prism parses as separate constructs: `catch` / `throw`, `defined?`, and
the `*-write` shorthand operators. Those are documented at their
emit sites in `src/spinel_parse.c`.

## Field semantics by category

### `R <id> <field> <child>`

A single reference. `-1` means "no child" -- consumers must check before
indexing arrays by this id. A handful of field names appear repeatedly
across node types with a consistent meaning:

- `receiver` -- the recv of a CallNode; `-1` for bare calls.
- `arguments` -- usually points at an ArgumentsNode, sometimes directly
  at the single expression (BreakNode / ReturnNode).
- `block` -- points at a BlockNode (literal block) or BlockArgumentNode
  (`&block` forward) attached to a call; `-1` otherwise.
- `value` / `expression` -- the RHS of an assignment / write.
- `statements` -- the body of a control-flow construct.
- `parameters` -- a ParametersNode attached to a method / lambda / block.

A scope's `locals` (S) -- a block's, lambda's, def's, class body's -- is its
names comma-joined, in the order Prism lists them: parameters first, then the
block-locals, then the names its body first assigns. `nt_scope_locals` answers
them as names, split once per string; `nt_scope_has_local`,
`nt_scope_locals_set` and `nt_block_local_node` (the declaration of a
block-local) sit beside it in `node_table.c`.

### `A <id> <field> <id,id,…>`

A list of references, comma-separated, empty body allowed. The empty
string after `<field>` means "zero elements". Used for collections
whose size isn't fixed at parse time: statement bodies, argument lists,
WhenNode condition lists, hash element lists, etc.

The order is the source order. Consumers walk left-to-right via
`nt_arr`, so reordering
within an `A` record changes program semantics.

### `S <id> <field> <string>`

The string is printed escaped (see "Encoding rules" above). Common
fields:

- `name` -- the method name on CallNode, the variable name on
  `LocalVariableReadNode`, etc.
- `content` -- the literal text of `StringNode` / `SymbolNode` /
  `RegularExpressionNode`.
- `operator` -- `+=`, `-=`, `*=`, etc. on the op-write nodes.
- `pattern` -- regex source for `RegularExpressionNode`.

### `I <id> <field> <int>`

Decimal integer, possibly signed. Used for `IntegerNode.value` (the
parsed literal), `flags` bitfields, and Prism's `NumberedParametersNode`
arity.

### Inline RBS facts

An applied inline RBS comment ([inline-rbs.md](../inline-rbs.md)) is
written onto the node it describes, and only onto that node:

- `S <def> rbs_ret <tag>`, `S <def> rbs_params <tag>,<tag>,...` -- a
  method's signature, as seed-file tags (`int`, `string?`, `obj_Foo`, ...;
  see [rbs-extract.md](../rbs-extract.md#seed-file-format)), one parameter
  field per parameter in the def's order, empty where unsaid, `-` for none.
- `S <attr_* call> rbs_ivar <tag>` -- the type of each attribute the call
  declares.
- `S <class body StatementsNode> rbs_ivars @x=<tag>@<line>@<file>@<col>,...` --
  the class body's `# @rbs @x: T` declarations, each with its position.
- `I <node> rbs_line <line>`, `I <node> rbs_col <col>`, `I <node> rbs_file
  <file id>` -- the original position of the comment (its `#`), for anything
  reported about it later.
- `S <node> rbs_names <tag>=<type>...` -- each tag above beside the type the
  comment wrote (`obj_Time=Time`), one per line (escaped), so a type the
  analyzer cannot pin is reported as written.
- `I <program> rbs_any 1` -- present when any of the above is.

`apply_inline_rbs` (src/analyze.c) reads them.

### `F <id> <field> <float>`

Float literal in `printf("%.17g")` form, always with a decimal point.

## How consumers read the AST

The parser fills a `NodeTable`: an array of `SpNode`
records indexed by node id (`src/node_table.h`). Unlike the retired Ruby
self-host -- which built one flat `@nd_<field>` array per attribute --
each `SpNode` stores its own small field lists (string / int / ref /
array), and the analyzer and code generator both query them by name
through the accessor API:

| Accessor                              | Reads                                                  |
|---------------------------------------|--------------------------------------------------------|
| `nt_type(nt, id)`                     | Node type name (the `N` record); `NULL` if unset.      |
| `nt_str(nt, id, "name")`              | An `S` string attribute; `NULL` if absent.             |
| `nt_int(nt, id, "value", dflt)`       | An `I` integer attribute; `dflt` if absent.            |
| `nt_ref(nt, id, "receiver")`          | An `R` child reference; `-1` if absent / empty.        |
| `nt_arr(nt, id, "arguments", &n)`     | An `A` child-id list and its length; `NULL,0` if absent. |
| `nt_kind(nt, id)`                     | The cached `NodeKind` enum (a fast switch key vs. `strcmp` on the type string). |

A reference field absent from a node reads back as `-1` and an absent
array as empty, so consumers check before indexing. An attribute the
parser sets but no accessor ever reads is simply inert. New parser
fields are picked up automatically by `nt_str` / `nt_int` / `nt_ref` /
`nt_arr` once a consumer asks for them by name -- there is no separate
per-field arm to keep in sync.

The analyzer's results do **not** live in the `NodeTable`; they live in
the `Compiler` struct (the per-node type cache plus the class / method /
scope tables). The `NodeTable` is the immutable parsed program; the
`Compiler` is the derived analysis. See
[analyze-ir.md](analyze-ir.md).

## Limitations

- **Integer overflow**: `IntegerNode.value` is encoded as a signed
  64-bit decimal. Literals outside `[INT64_MIN, INT64_MAX]` are
  truncated by `pm_int_value()`; the AOT compiler then auto-promotes
  to bigint at use sites it can recognise (loop multiplication,
  fibonacci-style addition). See [README.md](../README.md) §Bigint.
- **Floating point**: `%.17g` round-trips IEEE-754 doubles but the
  surface text isn't human-readable for boundary cases. Codegen uses
  the literal text directly in C output, so the encoded form drives
  what the C compiler sees.
- **Encoding**: bytes ≥ 0x80 pass through verbatim, so the parser
  assumes UTF-8. ASCII-7 sources are always safe; mixed-encoding
  sources are not supported (matches the README's "no encoding"
  limitation).
- **Source locations**: emitted. Each node carries `node_line` /
  `node_file` / `node_col` integer attributes and the file table is
  carried in `SOURCE_FILE` / `FILE` records, so codegen can map a
  generated-C line back to the original `.rb` (the `--line-map` /
  `--debug` `#line` directives, and the `--line-map` diagnostics that
  re-point cc errors at Ruby source).

## See also

- [docs/analyze-ir.md](analyze-ir.md) -- the in-memory analyze ↔ codegen
  contract (the `Compiler` struct that replaced the old `.ir` file).
- [docs/FFI.md](../FFI.md) -- direct C-call declarations.
- `src/spinel_parse.c` -- the C front-end that fills the table.
- `src/node_table.[ch]` -- the `nt_put_*` fill calls, `nt_dump` and the
  `nt_*` accessor API.
- `src/analyze*.c` -- type inference; fills the `Compiler` node type cache.
- `src/codegen*.c` -- C emission; reads the `NodeTable` + `Compiler`.
