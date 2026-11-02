# RBS type seeds (`spinel --rbs`)

`spinel --rbs DIR` feeds `*.rbs` signatures from `DIR` into the type
inferencer. Under the hood the driver runs `spinel_rbs_extract`
(`tools/spinel_rbs_extract.c`) over `DIR` to turn the signatures into a
line-oriented seed file, passes it to the compiler through the
`SPINEL_RBS_SEED` environment variable, and the analyzer applies it
before the inference fixpoint.

The same signatures can be written as comments in the Ruby source;
see [inline-rbs.md](inline-rbs.md). They pin the same slots.

A signature that spinel cannot represent is dropped, and dropping is
harmless: the seed line isn't emitted and the analyzer falls back to its
normal inference for that method or ivar. The rest of this document is
mostly the catalogue of what survives.

## A seed is an assertion, not a hint

A seed that IS applied is not advisory. It pins the slot, and the
widening and reset passes that make up the inference fixpoint are
forbidden to touch it. So a seed does not lose to inference; it
constrains inference from the start.

**Seeds are trusted, never verified**, but for two kinds of slot. A
container's (below) and a builtin class's with no subclass (Time, Thread,
Fiber, Random, IO) check a boxed value's class where it is narrowed into
the slot, in every build, and raise `TypeError` for another class (nil
passes): read as that class, the value would crash. For any other slot no
runtime check is generated. A signature that disagrees with what the
program actually stores does not degrade to the inferred type -- it makes
the emitted C reinterpret the value:

```ruby
class C
  def set(x); @v = x; end   # stores a String
  def get;    @v;      end
end
c = C.new
c.set(["hello", 1][0])
p c.get
```

```rbs
class C
  attr_accessor v: Integer   # a signature the program contradicts
end
```

| build            | output              |
| ---------------- | ------------------- |
| no `--rbs`       | `"hello"`           |
| with the seed    | `4296622121`        |
| CRuby            | `"hello"`           |

That number is the String pointer read back as an `Integer`, so it
varies with where the String was allocated. The
String arrives boxed, read out of a mixed Array; stored straight from
a literal it would be a statically known contradiction, which is
refused at compile time (see [Contradictions](#contradictions)).

So write signatures that describe the program, not signatures you would
like to be true. A seed is closer to a `reinterpret_cast` than to a
`static_cast`.

## Contradictions

A seed the program *statically* contradicts is a compile error:

```
$ spinel main.rb --rbs sig -o a
spinel: main.rb:6: --rbs seed contradicted: @v is declared Integer but this assigns String
  A seed is trusted, so the emitted code would reinterpret the value rather than convert it.
  Fix the signature or the assignment.
```

The rule is deliberately narrow. It fires only when both the seed and the
value are concretely typed and one is a **flat scalar** (`Integer`,
`Float`, `bool`, `Symbol`) while the other is a **heap pointer**
(`String`, an Array, a Hash, an object, a Proc). No conversion exists
between those and none is emitted, so the result would be a pointer read
as an integer.

Everything else is left alone on purpose:

- Two types in the same family may still disagree -- a `String` slot fed
  a `Symbol` -- but the emitter has coercions for many such pairs, and a
  false error breaks a build that works.
- `nil` is legal in every slot.
- `Integer` and a bignum convert at the boundary.
- A by-value object (`Range`, `Time`, `Complex`, `Rational`) is a struct,
  not a pointer, so the families do not apply -- except at a return, where
  a `Time` or a `Range` is a family of its own: the function returns it by
  value, and nothing converts it into another declared type.
- A value that is `poly` or not yet inferred makes no static claim. That
  is the dynamic case, and it is what `-DSP_RBS_CHECK` is for.

The two halves are complementary by construction: this one needs both
types known and costs nothing to run, the other needs the path to
execute and sees the real tag.

## Checking seeds

Building the generated C with **`-DSP_RBS_CHECK`** turns that silence into
an abort:

```
$ spinel main.rb --rbs sig --cc="cc -DSP_RBS_CHECK" -o b && ./b
spinel: --rbs seed violated: @v is declared Integer but holds String
  The signature is trusted and the emitted code reinterprets the value.
  Fix the signature or the program; this build was made with -DSP_RBS_CHECK.
```

The assertion sits where a boxed value is narrowed into a seeded slot --
a seeded ivar, parameter, or return -- because that is the moment a
seed's truth becomes checkable at all. Without the define the macro is
the value itself, so a release build is unchanged and there is no reason
not to leave the checks emitted.

What it does and does not catch:

- **Tag level only.** A pointer landing in a scalar slot and the reverse,
  which is the reinterpretation family. Object identity is not checked:
  a subclass in an ancestor-typed slot is correct and layout-compatible.
  A builtin value with no subclass (a Time, a Thread, a Fiber, a Random,
  an IO) is checked by its builtin class as well, so a Range in a `Time`
  slot aborts.
- **A nil always passes.** An unset slot reads nil, and a non-nullable
  seed still sees `@x = nil` in an `initialize`.
- **Only dynamic narrowings.** If the value is already statically typed,
  no narrowing is emitted and there is nothing to assert at run time.
  That case is [a compile error](#contradictions) instead.
- **Only paths that execute.** It is a test-time tool, not a proof.

Run your test suite once with it. `make rbs-seed-test` does, over both an
honest fixture (which must run identically either way) and a deliberately
contradicted one (which must abort and name the slot).

## What a seed buys

Inference has to be conservative where two shapes meet; a seed does not.
That is the whole benefit, and it takes three distinguishable forms.

**It stops a widening the evidence forces.** In
`test/rbs-seed/pinned_container.rb`, `@kids` is only ever
lazy-initialized (`@kids ||= []`) and pushed through a reader, so the
usage pass never witnesses a direct push and the ivar widens to a boxed
value. `Array[untyped]` says nothing about the element type but does fix
the *storage kind*:

```c
// no seed
struct sp_PinBox_s { sp_int cls_id; sp_RbVal iv_kids; sp_RbVal iv_meta; };
static sp_RbVal sp_PinBox_kids(sp_PinBox *self);
sp_poly_length(sp_PinBox_kids(...))          // runtime tag dispatch

// Array[untyped] + Hash[String, untyped]
struct sp_PinBox_s { sp_int cls_id; sp_PolyArray * iv_kids; sp_StrPolyHash * iv_meta; };
static sp_PolyArray * sp_PinBox_kids(sp_PinBox *self);
sp_PolyArray_length(sp_PinBox_kids(...))     // direct call
```

19 `sp_poly_*` dispatching calls in that file become 6.

**It reaches slots inference has no evidence for at all.** A method with
no call sites has nothing to infer its parameters from. This is not an
optimization there; the seed is the only source of information. (The
compiler does the same thing internally: `analyze.c` pins a synthesized
wrapper's parameter with the comment "pin: no call sites exist".)

**It breaks self-referential inference.** `@data = flatten.to_a` makes
`@data`'s type depend on itself and widens it to poly. A seed cuts the
cycle.

## Ruby has one Array; spinel has several

Worth stating separately, because it is where a seed earns the most and
where a wrong one costs the most. `Array` is one type in Ruby and
`sp_IntArray` / `sp_StrArray` / `sp_PolyArray` (and the six hash
variants) in generated C. Inference picks the storage kind from
evidence; an empty `[]` or `{}` carries none.

These kinds have **different layouts**. Pinning a slot to the wrong one
is not a slower program, it is a program that reads its own data back as
garbage -- unlike, say, an object pinned to one of its ancestors, where
the field layouts coincide by construction.

## Supported

### Type vocabulary

| RBS                                    | Spinel tag                            |
| -------------------------------------- | ------------------------------------- |
| `Integer`                              | `int`                                 |
| `Float`                                | `float`                               |
| `String`                               | `string`                              |
| `Symbol`                               | `symbol`                              |
| `TrueClass`, `FalseClass`              | `bool`                                |
| `true`, `false` (literal types)        | `bool`                                |
| `NilClass`, `nil`                      | `nil`                                 |
| `bool`                                 | `bool`                                |
| `void` (return only)                   | `-` (no pin: unused, not typed)       |
| `Foo`, `Foo::Bar` (nominal)            | `obj_Foo`, `obj_Foo_Bar`              |
| `Time`, `Thread`, `Fiber`, `Random`, `File`, `IO`, a socket class, `Class` | `obj_Time`, ... (the builtin's own kind) |
| `Range`, an exception class (`StandardError`, `Errno::ENOENT`) | `obj_Range`, ... (applies; the slot stays inferred) |
| `IO::Buffer` (a native class)          | `obj_IO_Buffer`                       |
| `self`, `instance` (a class's instance method or attribute) | `obj_<the class>`; in `class Object` `poly`, as `Object` is; in a top-level builtin with a kind of its own, that kind (`-> self` in `class String` is `-> String`, `string`) |
| `top` as a type argument (`Hash[Symbol, top]`) | as `untyped`: `sym_poly_hash`  |
| `Array[Integer]`                       | `int_array`                           |
| `Array[Float]`                         | `float_array`                         |
| `Array[String]`                        | `str_array`                           |
| `Array[Symbol]`                        | `sym_array`                           |
| `Array[Foo]`                           | `obj_Foo_ptr_array`                   |
| `Array[Array[Integer]]`                | `int_array_array`                     |
| `Array[Array[Float]]`                  | `float_array_array`                   |
| `Array[<other>]`                       | `poly_array`                          |
| `Hash[String, Integer]`                | `str_int_hash`                        |
| `Hash[String, String]`                 | `str_str_hash`                        |
| `Hash[String, <other>]`                | `str_poly_hash`                       |
| `Hash[Symbol, <any>]`                  | `sym_poly_hash`                       |
| `T?`                                   | `<T>?`  (recursive)                   |
| `T \| nil` / `nil \| T`                | `<T>?`                                |
| `name` of a `type name = T`            | what `T` maps to                      |
| `{ name: T, ... }` (a record)          | `sym_poly_hash`                       |
| `{ "name" => T, ... }`                 | `str_poly_hash`                       |

A nominal type that names no class of the program but a builtin class
with values of its own pins the slot to that kind: a `Time` parameter is
Spinel's `sp_Time *`, an `IO` or a `File` its IO handle, as inference types
the same values. A class of the program's own by that name, or a reopened
builtin, is found first. `T?` of a kind with no nil of its own (`Time?`)
is the boxed value. `Range` and an exception class name more values than
one kind holds -- a Float or a String Range, an exception class of the
program's own -- so they pin nothing: the annotation applies, and that
slot keeps the type inference gives it. A
native class a package declares under a qualified name (`IO::Buffer`) is
matched by that name. The other builtin classes (`Regexp`, `MatchData`,
`Proc`, `Method`, `Mutex`, `Queue`, `ConditionVariable`, `Complex`,
`Rational`, `Enumerator`, `Dir`, `Process::Status`, `File::Stat`) still
name no type the analyzer pins, and the declaration is dropped.

The analyzer has one Symbol-keyed Hash kind, the poly-valued one, so every
`Hash[Symbol, V]` maps to it and the values stay boxed.

A type alias (`type patch_t = Array[untyped]`, at the top level or in a
class or module) stands for its type wherever its name is referenced,
in the file that declares it or another: the extractor declares every
alias, then maps their types to a fixpoint, before it reads a signature,
so an alias whose type names another alias follows it, and a name
resolves to the nearest declaration (as a class name does, below)
whatever the order of the declarations or the files. A generic alias
(`type listed[T] = Array[T]`), an alias whose type is out of the subset
and an alias in a cycle stay out of it.

A record is the Hash its fields make: Symbol-keyed for `name:` and
`:name =>` fields, String-keyed for `"name" =>` fields, with the values
boxed whatever the field types say, as `Hash[Symbol, V]` is. A record
with keys of another class, or of both, maps to `poly`.

### `Array[Array[Integer]]` and `Array[Array[Float]]` on an instance variable

These are the two nested element types with an unboxed table form (`sp_PtrArray`
of `sp_IntArray*` / `sp_FloatArray*`). The seed **supplies the row kind**, the
way `Array[Float]` supplies the element kind of a flat array: both are
performance declarations, and a table whose rows are all empty literals
has no element kind of its own to be read from the code.

```ruby
class Model
  def initialize(n)
    @feat_thr = Array.new(n) { [] }    # rows have no kind yet
  end
end
```

```rbs
class Model
  @feat_thr: Array[Array[Float]]
end
```

With the seed, `@feat_thr` is the float table and every empty row, wherever
it enters (`Array.new(n) { [] }`, `[[], []]`, `@t << []`, `@t[i] = []`,
`@t = []` grown later), is built as an `sp_FloatArray`. Without it the
program would have to write `Array.new(n) { Array.new(0, 0.0) }` to say the
same thing.

The seed is evidence for the narrowing pass, not a type pin: the pass still
has to be able to honour it. A use the unboxed table has no emitter for
(`each`, `map`, a read from outside the class's own instance methods) keeps
the table boxed and warns, naming the ivar, as an `Array[Foo]` request does.
A row of the **other** kind (an `Array.new(0, 0)` row under an
`Array[Array[Float]]` seed) is a contradiction, and is refused at compile
time the way a contradicted flat seed is: a seed is trusted, so the emitted
table would otherwise hand a row of one layout to a reader of the other.

Every other nesting stays `poly_array`: there is no table of string arrays,
symbol arrays, object arrays, or of tables.

### `Array[Foo]` on an instance variable

`obj_Foo_ptr_array` is a request, not a pin. The unboxed object array
(`sp_PtrArray`) has emitters for only a few operations -- `[]`, `[]=`,
`push`/`<<`, `pop`, `shift`, `delete_at`, `clear`, `length`/`size`, `empty?`,
`first`/`last`, and the no-block `min`/`max`/`sort` when `Foo` has `<=>` --
so the compiler narrows an ivar to it only when every use is one of those,
from the class's own instance methods or through its `attr_reader` on a
receiver statically of that class. That analysis runs with or without the seed (a `@items = Array.new(n)
{ Foo.new }`, or `Array.new(n, Foo.new)` with its one object in every slot, read as `h.items[i]` in a loop
narrows on its own); the seed's
effect is a warning when the request cannot be honoured, naming the ivar,
so the boxed fallback is never silent.

### Members emitted

- `def name: (...) -> R` -- instance method (`meth`)
- `def self.name: (...) -> R` -- class method (`cmeth`)
- `def self?.name: (...) -> R` -- emits both `meth` and `cmeth`
- `attr_accessor`, `attr_reader`, `attr_writer` -- emits `ivar`
- `@name: T` instance variable declarations -- emits `ivar`

### Unqualified type resolution

Inside `module Foo; class Bar`, an unqualified reference like
`def record: () -> Base` is resolved to `obj_Foo_Base` (single-level
parent fallback). A wrong guess is a no-op because the analyzer
silently drops seeds for unknown types -- no symbol table is required.
Covers the common sibling-in-module pattern; full lexical lookup is
not implemented.

## Parameters and returns

A method's seed has one field per parameter of the Ruby def. The
positionals take plain fields in the def's order: required positionals,
optional positionals (`?T`), the rest parameter (`*T`), trailing
positionals. A keyword (`name: T`, `?name: T`) takes a `name:type`
field, and the analyzer applies it to the parameter of that name: Ruby
matches a keyword by name, and a def lists its keywords in any order
(`def m(b:, a: 1)` under `(?a: Integer, b: String)` seeds both right).
The keyword fields are written in the signature's source order. A
`**rest` is the def's last slot and takes no field. A field is empty
where the seed has nothing to say, and the analyzer leaves that
parameter to inference:

- the rest parameter: the analyzer types it a poly array by construction
- a parameter whose type is outside the subset (the type-level list
  below)

A field that lands on the slot of the def's rest parameter or `**rest`
is not applied, since the signature declares an optional positional where
the def gathers its arguments (`?Integer c` for `def m(*c)`): that slot
is typed by construction, and a scalar type on it would make every call
pass the gathered array to a slot of another C type.

A return outside the subset reads `-`, as `void` does: nothing is pinned
and the parameters keep their seeds. In a class, `instance` and the
`self` of an instance method are an instance of that class
(`obj_<QualifiedName>`); the `self` of a singleton method (`def
self.make: () -> self`) is the class object (`class`, as `singleton(K)`
reads), and the `self` of a `def self?.` method is both, so `-`. In a
module `self` and `instance` are the including object, which the seed
cannot name, so `-`.

### A container return whose body answers a boxed value

A container return seed (`-> Hash[String, String]`, `-> Array[Integer]`,
`Hash[..]?` and `Array[..]?` too) whose body answers a value only the run
time types -- a read out of a Hash of mixed values, say -- keeps the box:
the container it holds has a form (`Hash[String, String]` can be held with
String values, or with boxed keys; `Array[Integer]` can be an Array built
with mixed values that holds only Integers) the compiler cannot know, and
unboxing it into the seed's form would be a copy, a different object from
the one the method answers. What the seed still says is that the value is
a Hash (an Array), and that is checked:

```ruby
CONF = { "a" => { "x" => "1" }, "b" => 2 }

class Cfg
  #: (String) -> Hash[String, String]
  def for(key) = CONF[key]
end

Cfg.new.for("a")["x"]   # => "1"
Cfg.new.for("b")        # TypeError: the return value of Cfg#for is declared a Hash but is Integer
```

The check runs on every return of the method, explicit or not, in every
build: with or without `-DSP_RBS_CHECK`, a value that is no Hash (no Array,
for an Array seed; nil passes only for `Hash[..]?` and `Array[..]?`)
raises `TypeError`. A container of any form passes, and stays the object
the body answered. A parameter seeded a container that is handed a box
keeps the box too, the caller's object, unchecked. The analysis then takes a call of
the method to answer no proc or Method, so a String handed to `[]` on its
result is not refused as one a proc might append to. A body whose Hashes
the compiler does know keeps the seed's form, as before; a yielding
method, which can be spliced into its caller, keeps the box unchecked.

A method that only entry points call (the public method of a library,
with no call site in the program) has no call site to bind its
parameters, so without a seed each of them widens to untyped. Dropping
the whole signature over one optional positional or a `self` return
made exactly those methods lose their declarations; each parameter now
stands on its own.

## Dropped (silently skipped)

### Method signature shapes

- Overloaded methods -- several `|`-separated signatures, or one that
  adds to another declaration with `| ...` (in any file of the
  directory). A seed pins one signature, and the method takes what every
  overload takes, so seeding any single overload would pin it narrower
  than its calls: `Array.wrap: (nil) -> Array[untyped] | ...` would pin
  its parameter to nil. The method is left to inference, as an
  overloaded inline signature is (see [inline-rbs.md](inline-rbs.md)).
- Generic method params (`[X]`)
- Proc / untyped function types

### Type-level (an empty field as a parameter, `-` as a return)

- `self` and `instance` as a parameter, or as the return of a module's
  method; `class`, `top`, `bot`
- Interfaces (`_Foo`)
- Intersections (`T & U`)
- Literal types other than `true` and `false` (`:foo`, `42`, `"x"`)
- Generic type aliases (`type listed[T] = Array[T]`)
- Type variables (generics with parameters)
- Tuples (`[T, U]`)
- Proc types (`^(T) -> U`)
- `Hash[K, V]` where K is not `String` or `Symbol`

### Members

- `include`, `extend`, `prepend` -- mixin ancestry not modeled
- `public`, `private`
- `alias`
- `@@cvar` declarations

## Seed file format

Read and applied by `apply_rbs_seeds` in `src/analyze.c` (it both parses
the seed file and pins the named params / returns / ivars before the
inference fixpoint):

```
class <QualifiedName>           # enter class scope; nested names use `_`
meth <name> <ret> <ptypes>      # `-` means "leave alone"; ptypes is
cmeth <name> <ret> <ptypes>     # comma-separated, or `-` for nullary;
                                # a keyword's field is `name:type`
ivar <name> <type>
src <file>:<line>               # --positions only: the .rbs line the next
                                # seed is from (named when an inline RBS
                                # annotation disagrees with it)
```

Lines whose first token isn't a keyword are treated as comments.

### How the analyzer reads a `<T>?` token

`parse_seed_type` pins a nilable token to the base type only where that
type's C slot still has an inhabitant left to spell nil with:

| Token                         | Pinned to        | nil is        |
|-------------------------------|------------------|---------------|
| `int?`                        | `sp_oint`       | the `nil` flag beside the word |
| `float?`                      | `sp_ofloat`     | the `nil` flag beside the word |
| `string?`, `obj_X?`, `*_array?`, `*_hash?` | the pointer | `NULL`   |
| `bool?`, `symbol?`            | `sp_RbVal`       | `SP_TAG_NIL`  |

`sp_int` and `sp_float` keep every bit pattern as a value (`INT64_MIN`, a
NaN of any payload), so `int?` and `float?` carry their nil out of band in
a flag. `sp_bool` and `sp_sym` have no spare inhabitant -- `0` is `false`,
and symbol `0` is a real symbol -- so `bool?` and `Symbol?` pin to the boxed
tagged union rather than collapsing nil onto `false` / `:""` (#3412).
A poly value narrowed into a flagged slot goes through `sp_unbox_oint`
(and its Float counterpart), which maps the nil tag to the flag instead of
reading the zero payload beneath it.

## Follow-up

Tracked as out-of-scope for the initial spike (see matz/spinel#7 +
OriPekelman/tep#6):

- Multi-overload resolution
- Generics with type variables
- Mixin ancestry from `include` / `extend`
- Return-type pinning where the body would otherwise cascade-widen, other
  than a Hash return's check (above)
