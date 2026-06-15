# Spinel -- Ruby AOT Compiler

Spinel compiles Ruby source code into standalone native executables.
It performs whole-program type inference and generates optimized C code,
achieving significant speedups over CRuby.

Spinel is **self-hosting**: the compiler backend is written in Ruby and
compiles itself into a native binary.

## How It Works

```
Ruby (.rb)
    |
    v
spinel_parse           Parse with Prism (libprism), serialize AST
    |                  (C binary, or CRuby + Prism gem as fallback)
    v
AST text file (.ast)
    |
    v
spinel_analyze         Whole-program type inference (self-hosted)
    |                  Walks AST to fixpoint: param / return / ivar
    |                  types, value-type detection, DCE markers,
    |                  per-node inferred-type cache.
    v
IR text file (.ir)
    |
    v
spinel_codegen         C code generation (self-hosted)
    |                  Consumes .ast + .ir, emits one C file.
    v
C source (.c)
    |
    v
cc -O2 -Ilib -lm      Standard C compiler + runtime header
    |
    v
Native binary           Standalone, no runtime dependencies
```

The analyze / codegen split is by design -- they live in separate
binaries (`spinel_analyze`, `spinel_codegen`) that share no in-memory
state. Everything analyze decides is serialized through the IR file;
codegen reconstructs its inferred view from that contract. See
[docs/ANALYZE-IR.md](docs/ANALYZE-IR.md) for the per-record format
and [docs/AST.md](docs/AST.md) for the AST format the analyze stage
consumes.

## Quick Start

```bash
# Fetch libprism sources (from the prism gem on rubygems.org):
make deps

# Build everything:
make

# Write a Ruby program:
cat > hello.rb <<'RUBY'
def fib(n)
  if n < 2
    n
  else
    fib(n - 1) + fib(n - 2)
  end
end

puts fib(34)
RUBY

# Compile and run:
./spinel hello.rb
./hello               # prints 5702887 (instantly)
```

### Options

```bash
./spinel app.rb              # compiles to ./app
./spinel app.rb -o myapp     # compiles to ./myapp
./spinel app.rb -c           # generates app.c only
./spinel app.rb -S           # prints C to stdout
./spinel app.rb --rbs sig    # seed inference from .rbs files under sig/
./spinel app.rb --int-overflow=promote   # overflow promotes to Bigint (CRuby-compatible)
```

#### Integer overflow

Integers are native fixed-width words. `--int-overflow=MODE` selects how
`+`/`-`/`*` behave when a result exceeds that width:

- `raise` (default) -- raise on overflow. Safe (never silently wrong or
  undefined), but `9223372036854775807 + 1` raises instead of returning a
  Bignum.
- `wrap` -- silent two's-complement wrap. Fastest, no check.
- `promote` -- escalate to arbitrary-precision `Integer` (Bigint), matching
  CRuby. `9223372036854775807 + 1` returns `9223372036854775808`.

In the default mode, integer locals that an obvious growth pattern would
overflow (e.g. a `q = q * k` accumulator) are still auto-promoted to Bigint;
`promote` extends that to every integer operation.

### RBS type signatures

Spinel can read RBS files to seed the analyzer. When invoked with
`--rbs DIR`, the `spinel` wrapper runs `spinel_rbs_extract` over a
directory of `*.rbs` files (the same layout `rbs` and Steep use) and
passes the resulting seed file to `spinel_analyze` as a positional
argument. Seeds are advisory — inference still runs on top and
widens on observed contradiction, so a wrong or unrepresentable seed
is at worst a no-op. See [docs/RBS-EXTRACT.md](docs/RBS-EXTRACT.md)
for the supported subset.

## Self-Hosting

Spinel compiles its own backend. Both `spinel_analyze.rb` and
`spinel_codegen.rb` are written in a Ruby subset that compiles through
the same pipeline that compiles user code. The bootstrap chain
exercises each side in both dimensions (IR fixpoint, C fixpoint):

```
CRuby + spinel_parse(.rb)     → analyze.ast / codegen.ast
CRuby + spinel_analyze.rb     → analyze1.ir, codegen1.ir
CRuby + spinel_codegen.rb     → analyze1.c, codegen1.c    → bin1 (analyze + codegen)

bin1 + analyze.ast            → analyze2.ir
bin1 + analyze.ast + .ir      → analyze2.c                → bin2 (analyze)
bin1 + codegen.ast            → codegen2.ir
bin1 + codegen.ast + .ir      → codegen2.c                → bin2 (codegen)

bin2 + analyze.ast            → analyze3.ir
bin2 + analyze.ast + .ir      → analyze3.c
bin2 + codegen.ast            → codegen3.ir
bin2 + codegen.ast + .ir      → codegen3.c

analyze2.ir == analyze3.ir    (analyze.rb: IR fixpoint OK)
analyze2.c  == analyze3.c     (analyze.rb: C  fixpoint OK)
codegen2.ir == codegen3.ir    (codegen.rb: IR fixpoint OK)
codegen2.c  == codegen3.c     (codegen.rb: C  fixpoint OK)
```

All four `==` checks have to hold for `make bootstrap` to declare
success. Any change that affects deterministic output -- record order,
default-value handling, hash iteration -- breaks one of them, and the
bootstrap stops with a clear `analyze.rb: IR fixpoint FAIL` (or the
matching codegen / C variant) so the regression surfaces immediately.

## Benchmarks

384 tests pass. 52 benchmarks pass.
Geometric mean: **~7.8x faster** than Ruby 4.0.4 with `--yjit` across
the 28 benchmarks below. Baseline is CRuby 4.0.4 (stable), run with
`--disable-gems` and with `--yjit` for the JIT column. Each timing is
the best of three wall-clock runs; sub-10 ms cells are dominated by
interpreter / runtime startup and should be read as "noise floor."

### Computation

| Benchmark | Spinel | Ruby 4.0.4 | + YJIT | Speedup vs YJIT |
|---|---|---|---|---|
| mandelbrot | 24 ms | 1_339 ms | 1_340 ms | 55.8x |
| nqueens | 9 ms | 312 ms | 311 ms | 34.6x |
| matmul | 9 ms | 302 ms | 303 ms | 33.7x |
| life (Conway's GoL) | 23 ms | 861 ms | 497 ms | 21.6x |
| partial_sums | 80 ms | 1_324 ms | 1_310 ms | 16.4x |
| sieve | 25 ms | 406 ms | 409 ms | 16.4x |
| sudoku | 6 ms | 103 ms | 54 ms | 9.0x |
| fannkuch | 2 ms | 15 ms | 15 ms | 7.5x |
| fasta (DNA seq gen) | 2 ms | 14 ms | 14 ms | 7.0x |
| ackermann | 9 ms | 343 ms | 60 ms | 6.7x |
| fib (recursive) | 15 ms | 538 ms | 95 ms | 6.3x |
| tarai | 28 ms | 400 ms | 70 ms | 2.5x |
| tak | 34 ms | 502 ms | 82 ms | 2.4x |

### Data Structures & GC

| Benchmark | Spinel | Ruby 4.0.4 | + YJIT | Speedup vs YJIT |
|---|---|---|---|---|
| huffman (encoding) | 7 ms | 63 ms | 64 ms | 9.1x |
| so_lists | 35 ms | 485 ms | 299 ms | 8.5x |
| rbtree (red-black tree) | 20 ms | 521 ms | 113 ms | 5.6x |
| splay tree | 14 ms | 179 ms | 65 ms | 4.6x |
| binary_trees | 5 ms | 36 ms | 19 ms | 3.8x |
| linked_list | 71 ms | 306 ms | 236 ms | 3.3x |
| gcbench | 553 ms | 3_455 ms | 1_783 ms | 3.2x |

### Real-World Programs

| Benchmark | Spinel | Ruby 4.0.4 | + YJIT | Speedup vs YJIT |
|---|---|---|---|---|
| ao_render (ray tracer) | 103 ms | 3_033 ms | 1_122 ms | 10.9x |
| str_concat | 1 ms | 10 ms | 10 ms | 10.0x |
| bigint_fib (1000 digits) | 1 ms | 10 ms | 10 ms | 10.0x |
| json_parse | 41 ms | 413 ms | 275 ms | 6.7x |
| pidigits (bigint) | 2 ms | 10 ms | 10 ms | 5.0x |
| template engine | 170 ms | 1_020 ms | 723 ms | 4.3x |
| io_wordcount | 27 ms | 107 ms | 100 ms | 3.7x |
| csv_process | 255 ms | 1_050 ms | 924 ms | 3.6x |

A few notes on what YJIT does and doesn't change. On some integer-loop
workloads (mandelbrot, nqueens, matmul, partial_sums, sieve) YJIT's
numbers are essentially identical to interpreted Ruby; the benchmark
is bound by integer / float operations that the interpreter already
runs at native speed. On call-heavy code (ackermann, fib, tarai, tak,
rbtree) YJIT gives a real 4-6x lift, but Spinel still wins by ahead-
of-time specialization. The two YJIT-only weak spots remaining are
`tarai` and `tak`, where YJIT inlines the recursive call site so well
that Spinel's compiled C only beats it by ~2.4x.

## Supported Ruby Features

**Core**: Classes, inheritance, `super`, `include` (mixin), `attr_accessor`,
`Struct.new`, `alias`, module constants, open classes for built-in types.

**Control Flow**: `if`/`elsif`/`else`, `unless`, `case`/`when`,
`case`/`in` (pattern matching), `while`, `until`, `loop`, `for..in`
(range and array), `break`, `next`, `return`, `catch`/`throw`,
`&.` (safe navigation).

**Blocks**: `yield`, `block_given?`, `&block`, `proc {}`, `Proc.new`,
lambda `-> x { }`, `method(:name)`. Block methods: `each`,
`each_with_index`, `map`, `select`, `reject`, `reduce`, `sort_by`,
`any?`, `all?`, `none?`, `times`, `upto`, `downto`.

**Exceptions**: `begin`/`rescue`/`ensure`/`retry`, `raise`,
custom exception classes.

**Types**: Integer, Float, String (immutable + mutable), Array, Hash,
Range, Time, StringIO, File, Regexp, Bigint (auto-promoted), Fiber.
Polymorphic values via tagged unions. Nullable object types (`T?`)
for self-referential data structures (linked lists, trees).

**Inspect / `p`**: `Object#inspect` is implemented for all primitive
types (Integer, Float, String, Symbol, Boolean, nil), for typed
arrays (`int_array`, `float_array`, `str_array`, `sym_array`), and
for heterogeneous arrays (`poly_array`, e.g. `[1, "x", :y]`).
Scalar polymorphic values (the tagged-union values from the `Types`
section above) also inspect correctly.
`Array#to_s` is aliased to `Array#inspect`, matching CRuby.
`Kernel#p` dispatches through `compile_inspect_for` so `p obj`,
`obj.inspect`, `obj.to_s`, and `"#{obj.inspect}"` interpolation
all produce CRuby-byte-identical output. User-class instances
inside a polymorphic value currently render as the placeholder
`"#<Object>"` (the runtime has no class-name table yet); Hash,
Range, and Struct inspect are not yet implemented.

**Global Variables**: `$name` compiled to static C variables with
type-mismatch detection at compile time.

**Strings**: `<<` automatically promotes to mutable strings (`sp_String`)
for O(n) in-place append. `+`, interpolation, `tr`, `ljust`/`rjust`/`center`,
and all standard methods work on both. Character comparisons like
`s[i] == "c"` are optimized to direct char array access (zero allocation).
Chained concatenation (`a + b + c + d`) collapses to a single malloc
via `sp_str_concat4` / `sp_str_concat_arr` -- N-1 fewer allocations.
Loop-local `str.split(sep)` reuses the same `sp_StrArray` across
iterations (csv_process: 4 M allocations eliminated).

**Regexp**: Built-in NFA regexp engine (no external dependency).
`=~`, `$1`-`$9`, `match?`, `gsub(/re/, str)`, `sub(/re/, str)`,
`scan(/re/)`, `split(/re/)`.

**Bigint**: Arbitrary precision integers via mruby-bigint. Auto-promoted
from loop multiplication patterns (e.g. `q = q * k`), or from every integer
operation under `--int-overflow=promote` (see [Integer overflow](#integer-overflow)).
Linked as static library -- only included when used.

**Fiber**: Cooperative concurrency via `ucontext_t`. `Fiber.new`,
`Fiber#resume`, `Fiber.yield` with value passing. Captures free
variables via heap-promoted cells. Per-fiber storage via `Fiber[:k]`
/ `Fiber[:k] = v` (and the `Fiber.current[:k]` aliases) — symbol-keyed
poly-valued, lazily allocated, shallow-snapshot inherited from the
parent at `Fiber.new` time.

**Memory**: Mark-and-sweep GC with size-segregated free lists, non-recursive
marking, and sticky mark bits. Small classes (≤8 scalar fields, no
inheritance, no mutation through parameters) are automatically
stack-allocated as **value types** -- 1M allocations of a 5-field class
drop from 85 ms to 2 ms. Programs using only value types emit no GC
runtime at all.

**Symbols**: Separate `sp_sym` type, distinct from strings (`:a != "a"`).
Symbol literals are interned at compile time (`SPS_name` constants);
`String#to_sym` uses a dynamic pool only when needed. Symbol-keyed
hashes (`{a: 1}`) use a dedicated `sp_SymIntHash` that stores
`sp_sym` (integer) keys directly rather than strings -- no strcmp, no
dynamic string allocation.

**I/O**: `puts`, `print`, `printf`, `p`, `gets`, `ARGV`, `ENV[]`,
`File.read/write/open` (with blocks), `system()`, backtick.

**FFI**: Direct C calls without an extension compiler. Declarations
(`ffi_func`, `ffi_lib`, `ffi_const`, `ffi_buffer`, `ffi_read_*`) live
inside a `module` body; the codegen emits externs and the `spinel`
wrapper picks up `-l` flags from marker comments. Scalars, strings,
opaque `:ptr`, integer constants, raw byte buffers, and struct-field
reads are covered. See [docs/FFI.md](docs/FFI.md) for the full spec
and [examples/ffi/](examples/ffi/) for runnable demos against libc/
libm and sqlite3.

## Optimizations

Whole-program type inference drives several compile-time optimizations:

- **Value-type promotion**: small immutable classes (≤8 scalar fields)
  become C structs on the stack, eliminating GC overhead entirely.
- **Constant propagation**: simple literal constants (`N = 100`) are
  inlined at use sites instead of going through `cst_N` runtime lookup.
- **Loop-invariant length hoisting**: `while i < arr.length` evaluates
  `arr.length` once before the loop; `while i < str.length` hoists
  `strlen`. Mutation of the receiver inside the body (e.g. `arr.push`)
  correctly disables the hoist.
- **Method inlining**: short methods (≤3 statements, non-recursive)
  get `static inline` so gcc can inline them at call sites.
- **String concat chain flattening**: `a + b + c + d` compiles to a
  single `sp_str_concat4` / `sp_str_concat_arr` call -- one malloc
  instead of N-1 intermediate strings.
- **Bigint auto-promotion**: loops with `x = x * y` or fibonacci-style
  `c = a + b` self-referential addition auto-promote to bigint.
- **Bigint `to_s`**: divide-and-conquer O(n log²n) via mruby-bigint's
  `mpz_get_str` instead of naive O(n²).
- **Static symbol interning**: `"literal".to_sym` resolves to a
  compile-time `SPS_<name>` constant; the runtime dynamic pool is
  only emitted when dynamic interning is actually used.
- **`strlen` caching in sub_range**: when a string's length is
  hoisted, `str[i]` accesses use `sp_str_sub_range_len` to skip the
  internal strlen call.
- **split reuse**: `fields = line.split(",")` inside a loop reuses
  the existing `sp_StrArray` rather than allocating a new one.
- **Dead-code elimination**: compiled with `-ffunction-sections
  -fdata-sections` and linked with `--gc-sections`; each unused
  runtime function is stripped from the final binary.
- **Iterative inference early exit**: the param/return/ivar fixed-point
  loop stops as soon as a signature of the three refined arrays stops
  changing. Most programs converge in 1-2 iterations instead of the
  full 4, cutting bootstrap time by ~14%.
- **`parse_id_list` byte walk**: the AST-field list parser (called
  ~120 K times during self-compile) walks bytes manually via
  `s.bytes[i]` instead of `s.split(",")`, dropping N+1 allocations
  per call to 2.
- **Warning-free build**: generated C compiles cleanly at the default
  warning level across every test and benchmark; the harness uses
  `-Werror` so regressions surface immediately.

## Architecture

```
spinel                One-command wrapper script (POSIX shell)
spinel_parse.c        C frontend: libprism → text AST (1_608 lines)
spinel_analyze.rb     Type inference: AST → IR (21_162 lines, self-hosted)
spinel_codegen.rb     C emission: AST + IR → C (30_411 lines, self-hosted)
lib/sp_runtime.h      Runtime library header (1_537 lines)
lib/sp_bigint.c       Arbitrary precision integers (5_400 lines)
lib/regexp/           Built-in regexp engine
test/                 384 feature tests
benchmark/            52 benchmarks
docs/                 Format specs (AST, IR, FFI, sp_Class design)
Makefile              Build automation
```

The two backend stages -- `spinel_analyze.rb` and `spinel_codegen.rb`
-- are both written in the Ruby subset that Spinel itself can compile:
classes, `def`, `attr_accessor`, `if`/`case`/`while`,
`each`/`map`/`select`, `yield`, `begin`/`rescue`, String/Array/Hash
operations, File I/O.

No dynamic metaprogramming or `eval` in either backend. Compile-time class-body
declarations with compile-time-known literal inputs are supported for
Struct-style method synthesis.

### What spinel_analyze does

The analyze stage owns whole-program type inference. It's a sequence
of passes over the AST, each one filling in or refining one piece of
the static model:

1. **`collect_all`** -- single walk that registers every class,
   module, top-level method, instance method, class method, ivar
   declaration, FFI declaration, regexp literal, and constant. After
   this pass the parallel tables (`@cls_names`, `@meth_names`,
   `@cls_ivar_names`, ...) carry every name the program defines.

2. **Per-scope call-site widening** -- `infer_main_call_types`,
   `infer_function_body_call_types`, `infer_class_body_call_types`,
   `infer_ieval_body_call_types`. Walks each scope's call sites and
   feeds the arg types into the callee's param-type slots via
   `unify_call_types`. The unifier widens to `poly` only when two
   call sites disagree -- the conservative direction.

3. **Iterative refinement loop (≤ 4 rounds)** --
   `infer_all_returns`, `infer_function_body_call_types`,
   `infer_class_body_call_types`, `infer_ivar_types_from_writers`,
   `infer_param_array_type_from_body`,
   `narrow_param_types_from_body_method_calls`,
   `narrow_param_hash_types_from_body_writes`,
   `widen_cmeths_via_hash_each_blocks` (#424),
   `detect_poly_params`. The loop terminates when
   `inference_signature` -- a fingerprint over return types, ivar
   types, param types, cmeth ptypes -- stops changing. Most programs
   converge in 1-2 rounds; the cap at 4 catches pathological cases
   without exploding compile time.

4. **Post-loop fixups** -- `fix_nil_ivar_self_refs` (e.g. `@left =
   nil` on an attr_accessor inside `class Node` resolves to
   `obj_Node?`), `fix_lambda_return_types`, then re-run the inference
   passes so dependent types pick up the corrections.

5. **`refine_all_module_ivar_types`** -- with stable param types in
   hand, module-level `@@h[k] = v` ivar writes can now refine the
   hash variant from the placeholder `str_int_hash` default to the
   actual key/value shape.

6. **Feature detection** -- `pre_detect_bigint`, `detect_features`,
   `detect_value_types`, `recalc_needs_gc`, `collect_sym_names`,
   `scan_toplevel_ivars`, `compute_live_cls_methods`,
   `compute_live_instance_methods`. Sets the `@needs_*` flags that
   gate runtime helper emission and marks classes / methods for DCE.

7. **`precompute_all_scope_decls`** -- runs the multi-pass local-decl
   refinement per method / cmeth / ieval / main scope and stores the
   result in `@nd_scope_names[bid]` / `@nd_scope_types[bid]` so
   codegen doesn't have to re-run `scan_locals`.

8. **`annotate_all_node_types`** -- post-order walks every reachable
   AST node, calls `infer_type`, fills `@nd_inferred_type[nid]`.
   Codegen's own `infer_type` hits this cache > 99 % of the time at
   emit; only block-body expressions (whose scope is
   iterator-specific) and a few `@current_class_idx`-dependent arms
   fall through.

9. **`dump_analysis_buf`** -- serializes the result. See
   [docs/ANALYZE-IR.md](docs/ANALYZE-IR.md) for the line-oriented
   text format that lands in the `.ir` file.

### What spinel_codegen does

The codegen stage reads `.ast` + `.ir`, then emits one C file:

- `load_analysis_buf` reconstructs every analysis-derived ivar from
  the IR. After this, `@cls_names`, `@meth_return_types`,
  `@nd_inferred_type`, `@nd_scope_names`, etc. are populated as if
  the analyze passes had just run.

- `generate_code` emits the standard preamble (`#include
  "sp_runtime.h"`, the per-program runtime helpers gated on
  `@needs_*` flags, the sp_Class tables for hierarchy-using
  programs, the symbol intern table, class structs and constructors,
  forward declarations), then walks every reachable method / cmeth
  body to emit its `static inline` definition, then emits
  `int main()`.

- Per-program runtime helpers are gated. A `puts "hi"` program emits
  ~10 lines of C; a program that touches Method instances, hash
  literals, the class hierarchy, etc. gets the matching runtime
  blocks. The gating ladder is set by pre-scan passes in
  `generate_code` so the gates have the right value before the
  emission decisions land.

The runtime (`lib/sp_runtime.h`) contains GC, array/hash/string
implementations, and all runtime support as a single header file.
Generated C includes this header, and the linker pulls only the
needed parts from `libspinel_rt.a` (bigint + regexp engine).

The parser has two implementations:
- **spinel_parse.c** links libprism directly (no CRuby needed)
- **spinel_parse.rb** uses the Prism gem (CRuby fallback)

Both produce identical AST output. The `spinel` wrapper prefers the
C binary if available. `require_relative` is resolved at parse time
by inlining the referenced file.

## Building

```bash
make deps         # fetch libprism into vendor/prism (one-time)
make              # build parser + regexp library + bootstrap analyze + codegen
make test         # run 384 feature tests (requires bootstrap)
make bench        # run 52 benchmarks (requires bootstrap)
make bootstrap    # rebuild analyze + codegen from source (4-way fixpoint check)
sudo make install # install to /usr/local (spinel in PATH)
make clean        # remove build artifacts
```

Override install prefix: `make install PREFIX=$HOME/.local`

[Prism](https://github.com/ruby/prism) is the Ruby parser used by
`spinel_parse`. `make deps` downloads the prism gem tarball from
rubygems.org and extracts its C sources to `vendor/prism`. If you
already have the prism gem installed, the build auto-detects it; you
can also point at a custom location with `PRISM_DIR=/path/to/prism`.

CRuby is needed only for the initial bootstrap. After `make`, the
entire pipeline runs without Ruby.

## Portability

Spinel can emit C without invoking the C compiler — useful when you
want to build the Ruby program on one machine and ship the generated
sources to another:

```sh
spinel app.rb -c            # writes app.c next to the source
spinel app.rb -c -o app.c   # specify output path
spinel app.rb -S            # print the C to stdout
```

The output is one self-contained `.c` file that compiles against
`lib/sp_runtime.h`. The two together are everything a downstream
consumer needs — no link to `libspinel`.

The runtime is POSIX-flavoured but covers every platform CI exercises:

| Platform | Status | Compiler |
|---|---|---|
| Linux (x86-64, arm64) | Supported | gcc, clang |
| macOS (Intel, Apple Silicon) | Supported | clang |
| *BSD | Expected to work; not in CI | clang |
| Windows | Supported via [MSYS2](https://www.msys2.org/) / MinGW | gcc |
| Windows native (MSVC) | Not supported | -- |

Every PR runs `ubuntu-latest / gcc`, `ubuntu-latest / clang`,
`macos-latest / clang`, and `windows-mingw` jobs end-to-end (parser
build, codegen build, fixed-point bootstrap, full test + benchmark
suites). MSVC isn't supported because the runtime relies on POSIX
assumptions (`<ucontext.h>` for `Fiber`, `<sys/mman.h>` for the
regexp engine's executable buffers, GCC's `__attribute__((cleanup))`
for the GC root stack); a port would either replace those or guard
them behind compile-time switches.

## Limitations

- **No eval**: `eval`, `instance_eval`, `class_eval`
- **No dynamic metaprogramming**: `send`, `method_missing`, dynamic
  `define_method`
- **No threads**: `Thread`, `Mutex` (Fiber is supported)
- **No encoding**: assumes UTF-8/ASCII
- **No general lambda calculus**: deeply nested `-> x { }` with `[]` calls

A few cases deliberately diverge from CRuby because the CRuby behavior needs a
feature Spinel does not implement (e.g. `Integer#**` with a negative exponent
raises instead of returning a `Rational`). These are listed in
[docs/INCOMPATIBILITIES.md](docs/INCOMPATIBILITIES.md).

## Dependencies

- **Build time**: [libprism](https://github.com/ruby/prism) (C library),
  CRuby (bootstrap only)
- **Run time**: None. Generated binaries need only libc + libm.
- **Regexp**: Built-in engine, no external library needed.
- **Bigint**: Built-in (from mruby-bigint), linked only when used.

## Contributing

Contributions are welcome. The issue tracker doubles as the roadmap —
anything open is fair game; the most useful entry points are
reproducer-shaped bug reports (a 5-line Ruby that fails in Spinel but
passes in CRuby) and codegen fixes that close one such report.

Workflow:

- Open a focused PR. Small and contained merges faster than sweeping
  refactors.
- Make `make` close (`gen2.c == gen3.c`) and `make test` / `make bench`
  pass before pushing.
- Add a regression test under `test/` for any fix or new feature; the
  harness compares Spinel's output against CRuby on the same source,
  so the test usually doesn't need to assert anything beyond `puts`.
- Reference issues with `Closes #N` / `Fixes #N` / `Refs #N` trailers
  in the commit message.
- If the work was assisted by an AI, add a `Co-Authored-By:` trailer
  for the assistant alongside any human co-authors. The maintainer's
  own AI-assisted commits use `Co-Authored-By: Claude Opus 4.7`.

Adjacent ecosystem (community-built, not part of this repo):

- [rubocop_spinel](https://github.com/gurgeous/rubocop_spinel) —
  a RuboCop custom cop that flags Ruby code Spinel doesn't yet
  support.
- [spinel-dev](https://github.com/OriPekelman/spinel-dev) —
  developer-experience tooling that surfaces what the compiler
  already knows: `spinel doctor` (a one-shot risk report —
  unresolved calls, widened slots, inference↔codegen
  disagreements, C-build failures), a differential
  CRuby-vs-Spinel value-bisector for silent miscompiles, a
  delta-debugging minimal-repro reducer, an inferred-types
  ruby-lsp addon, and flamegraph-based perf analysis. Plus a
  migration toolset for keeping real projects building against a
  fast-moving compiler — a capability/layout probe, a two-compiler
  parity check, a multi-file repro reducer, and a commit-bisector —
  which has driven several minimal reproducers and fixes upstream.
- [spinelgems](https://github.com/OriPekelman/spinelgems) —
  a gem-compatibility survey and differential-verification
  harness that determines which RubyGems compile and run
  correctly under Spinel, with bundler-spinel for vendoring
  pinned dependencies.

## History

Spinel was originally implemented in C (18K lines, branch `c-version`),
then rewritten in Ruby (branch `ruby-v1`), and finally rewritten in a
self-hosting Ruby subset (current `master`).

## License

MIT License. See [LICENSE](LICENSE).
