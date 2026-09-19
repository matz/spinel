# `--emit-types`: the inferred types as JSON

```sh
spinel app.rb --emit-types            # -> app.types.json
spinel app.rb --emit-types -o out.json
```

`--emit-types` runs the whole compile (parse, analyze, codegen to a
discarded buffer) and writes what the compiler knew as JSON: a type for
every node it typed, and the diagnostics. No binary is written. The exit
status is the compile's: a program the compiler refuses exits 1 and the
JSON still carries the refusals. With `-S` as well
(`spinel app.rb --emit-types -o app.json -S`) the C of that same compile
goes to stdout, so a consumer showing both runs the compiler once. It is the surface the out-of-tree
editor tools read (rubys/spinel-ide); nothing in this tree consumes it
beyond the gate's own check.

```json
{
  "types": [
    {"file":"app.rb","line":11,"col":5,"end_line":11,"end_col":8,
     "kind":"LocalVariableReadNode","name":"pts",
     "type":"poly_array","rbs":"Array[untyped]"},
    {"file":"app.rb","line":4,"col":2,"end_line":4,"end_col":47,
     "kind":"DefNode","name":"dist2","type":"symbol","rbs":"Symbol",
     "owner":"Point","signature":"(untyped) -> Integer","widened":true},
    ...
  ],
  "diagnostics": [
    {"file":"app.rb","line":4,"col":12,"end_line":4,"end_col":13,
     "severity":"warning","method":"dist2","slot":"param","param":"o",
     "message":"Spinel: parameter `o` of `dist2` widened to untyped (boxed poly slow path)"},
    {"file":"app.rb","line":9,"col":0,"end_line":9,"end_col":19,
     "severity":"warning","method":"widen","slot":"return",
     "message":"Spinel: the return of `widen` widened to untyped (boxed poly slow path)"},
    {"file":"app.rb","line":20,"col":0,"severity":"error",
     "message":"unsupported class variable read (no class scope): node 4 (ClassVariableReadNode)"}
  ],
  "codegen": [
    {"file":"app.rb","line":11,"col":18,"end_line":11,"end_col":32,
     "kind":"CallNode","name":"dist2","dispatch":"switch"},
    {"file":"app.rb","line":11,"col":12,"end_line":11,"end_col":34,
     "kind":"BlockNode","inlined":true},
    ...
  ]
}
```

## `types`

One record per node the analyzer gave a concrete type (nodes typed
unknown or void are left out), in node order.

- `file`, `line`, `col`: the node's start, as Prism reports it (the line
  is 1-based, the column 0-based).
- `end_line`, `end_col`: Prism's exclusive end. Several expressions can
  start at one column (`pts`, `pts.map { }`, `pts.map { }.inspect`); the
  tightest span containing a position is the expression at it.
- `kind`: the Prism node type (`CallNode`, `LocalVariableReadNode`,
  `DefNode`, ...).
- `name`: present where the node names something: a call's method, a
  variable, a constant, a def, a parameter.
- A parameter has a record of its own (`RequiredParameterNode`,
  `OptionalParameterNode`, `RestParameterNode`, the keyword and block
  kinds, as Prism names them) at the parameter's span, whose `type` and
  `rbs` are the slot's: `untyped` for a widened one, so hover on the
  parameter says what the warning on it says, and go-to-definition on a
  read of it has a declaration to land on.
- `type`: spinel's internal type tag; `rbs`: the same type as RBS, which
  is the type language to read. A `DefNode`'s own type is the def
  expression's value (a `Symbol`); the method type it declares is its
  `signature`, `(Integer, Integer) -> Array[Integer]`, the text
  `--emit-rbs` writes for that method, with `"widened":true` when a slot
  of it degraded to untyped, `owner` naming the class it is defined in
  (`Object` at the top level) and `"singleton":true` for a `def self.x`.
  The signatures are all here, placed: a consumer does not need the
  `--emit-rbs` pass or a text scan for the defs.

## `diagnostics`

- `"severity":"warning"`, one per widened slot of a method whose
  signature degraded to untyped: `method` names the def, `slot` is
  `"param"` (with `param`, the parameter's name; the position is the
  parameter's) or `"return"` (the position is the def's). The
  `signature` in `types` at the def shows the whole method type.
- `"severity":"error"`, one per refusal, in the order the compile met
  them, at the refused construct: the same lines the compile prints on
  stderr ([limitations.md](limitations.md) says what a refusal is).
- A program that does not parse exits 1 too, and the JSON is still
  written: `types` and `codegen` empty, and one `"severity":"error"` per
  parse error at its span (`end_line`/`end_col` included), in the file
  it is in when a `require_relative` spliced it — the same
  `file:line:col: message` lines the compile prints on stderr (where the
  column is 1-based, as an editor reads it; the JSON's is 0-based).

A plain compile says nothing about a widened slot. `--warn-widen` prints
the same warnings on stderr during any compile, one per slot at the
slot, in the form the other warnings take, and under each the *why*:
the chain from the value that widened the slot to the expression the
untyped was born at, one `note:` per hop:

```
spinel: app.rb:4:13: warning: parameter `o` of `dist2` widened to untyped (boxed poly slow path)
spinel: app.rb:11:28: note: passed `pts[0]` is untyped
spinel: app.rb:10:7: note: from `[Point.new(1, 2), Point.new(3, 4)]` is Array[untyped] — born here: no untyped input
```

The column is 1-based there, as an editor reads a compiler's warning;
the JSON's is 0-based. The same chain is the record's `why`, an array
of hops `{file, line, col, end_line, end_col, role, rbs, note?}` in
order from the slot outward. `role` is what the first hop is to the
slot -- `passed` (an argument), `written` (an assigned value),
`returned` (a returned value) -- then `from` for each expression the
untyped came in through, and `and` for the other side of a meeting. A
chain ends one of five ways, said in the last hop's `note`:

- **born here: no untyped input** -- the expression produced the untyped
  with no untyped operand (a literal array of objects, a `map` over
  them, `&.`); the rule is the compiler's, and this is where to look.
- **two kinds meet** -- the slot held one concrete kind and this value
  brought another; the `and` hop is the earlier kind's site.
- **a transient** -- the value is not untyped in the end, but was on the
  round the slot took it; the fixpoint kept the slot there. A compiler
  imprecision, and a report worth filing with the program.
- **never bound** -- no call site gives the parameter a type (on stderr
  only; the slot is untyped by default).
- **by construction** -- a `*rest` or `**kwrest` parameter.
- **untraced** -- a widening this version does not record the source of.

## `codegen`

What codegen decided, one record per call it placed and per block:

- a `CallNode` carries `dispatch`: `"direct"` (one statically bound C
  call, or a builtin emitted in place: the fast path), `"switch"` (a
  switch over the classes or tags the receiver can hold, each arm a
  direct call), or `"boxed"` (the receiver is a boxed value and a runtime
  helper answers over its tag; an unresolved call is here too).
- a `"direct"` call to a user def carries `callee`, the def it bound to:
  `Point#dist2` for an instance method, `Point.make` for a singleton, the
  bare name for a top-level def (the spelling a `DefNode`'s `owner`,
  `singleton` and `name` compose to). A `"switch"` carries `candidates`,
  the arms' defs in arm order (`["A#f","B#f"]`); an inherited def appears
  once, under the class that defines it. A builtin emitted in place and a
  boxed send carry neither.
- a `BlockNode` carries `inlined`: `true` when the block was spliced into
  its caller (an iterator's body, a yielding method's block), `false`
  when it became a function of its own (a proc or lambda, a `Fiber.new`
  or `Thread.new` body).

A call the compiler never placed (unreachable, or folded into something
else) has no record. The lens is per site: the one call in a method that
took a switch or the boxed path is the one to look at.

What the JSON does not say: why a slot widened (which call site or
assignment unified it to untyped). That is an open question, not a
field.
