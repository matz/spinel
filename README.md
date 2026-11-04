# Spinel -- Ruby AOT Compiler

Spinel compiles Ruby source code into standalone native executables.
It performs whole-program type inference and generates optimized C code,
achieving significant speedups over CRuby.

The compiler is a **single self-contained C binary**: it parses Ruby (via
libprism), infers types across the whole program, emits C, and invokes the
system `cc` to produce a native executable -- no Ruby runtime and no chained
helper binaries at compile time. (Spinel was previously written in a
self-hosting Ruby subset; that backend is preserved on the `self-host`
branch -- see [History](#history).)

## How It Works

```
Ruby (.rb)
    |
    v
parse (libprism)       Parse with Prism, linked in as a C library
    |                  (a CRuby + Prism-gem path produces the same AST)
    v
NodeTable              The walk fills an in-memory node table
    |
    v
analyze                Whole-program type inference (src/analyze*.c).
    |                  Walks the AST to a fixpoint: param / return / ivar
    |                  types, value-type detection, dead-code markers,
    |                  a per-node inferred-type cache.
    v
codegen                C code generation (src/codegen*.c). Reads the AST
    |                  plus the analysis just computed in the same process,
    |                  emits one C file.
    v
C source (.c)
    |
    v
cc -O2 -Ilib -lm      System C compiler + runtime
    |
    v
Native binary           Standalone, no runtime dependencies
```

Analyze and codegen run in one process and share the in-memory model:
codegen reads the types analyze just inferred directly, with no
serialization step in between. See [docs/internals/AST.md](docs/internals/AST.md)
for the node table the parser fills and the analyzer consumes.

## Quick Start

Spinel is built from source: clone this repository and build with `make`.
It is not distributed as a RubyGem, and there is no `gem install spinel`.
(Spinel *compiles* certain gems into your program, but the compiler itself
ships as source.)

```bash
# Fetch libprism sources (from the prism gem on rubygems.org):
make deps

# Build everything (the compiler `spinel` and the project tool `spin`):
make
sudo make install     # optional: puts spinel and spin on PATH

# Start a project:
spin new myapp && cd myapp
spin run              # compile bin/myapp.rb and run it
```

Releases are dated tags, `YYYY.MM.DD` (`spinel --version` names the one a
build belongs to, `+N` when the build is N commits past it, then the git
revision in parentheses); the name says when a release was cut,
and what it promises is in [docs/limitations.md](docs/limitations.md) and the
tests. The first one is `2026.09.12`. Each release carries a source archive
(`spinel-<release>.tar.xz`, what `make dist` produces) with the vendored
parsers included, so it builds with `make` alone and no network.

`spin` is the day-to-day interface -- cargo/mix style. It scaffolds
projects, resolves dependencies, drives the compiler, and runs tests; no
Makefile, no hand-written `-I` flags:

```bash
spin add ansi --version "~> 1.0"   # from the spin package index
spin add mylib --path ../mylib     # or a local checkout / --git URL
spin test                          # snapshot tests (CRuby is the oracle)
spin build && ./build/bin/myapp
```

Dependencies (packages) are source trees compiled into your binary -- no
runtime loading, no extension builds; a package can even carry `.c` files.
See **[docs/spin.md](docs/spin.md)** for the full guide, including how to
write and publish a library.

### Single files: the compiler directly

`spinel` is the underlying compiler -- gcc-like, one job -- and stays the
right tool for single-file scripts and experiments:

```bash
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

./spinel hello.rb
./hello               # prints 5702887 (instantly)
```

```bash
./spinel app.rb              # compiles to ./app
./spinel app.rb -o myapp     # compiles to ./myapp
./spinel app.rb -c           # generates app.c only
./spinel app.rb -S           # prints C to stdout
./spinel -E app.rb a b c     # compile, run with ARGV=[a, b, c], discard binary
./spinel -e 'puts 42'        # compile inline source
./spinel app.rb --int-overflow=wrap   # +/-/* wrap silently instead of raising
./spinel app.rb --incremental   # dev build: cache unchanged C units, recompile only what changed
```

`--incremental` is a development-build mode: it gives up inlining across
units for rebuilds that compile only the units an edit changed; release
builds stay a single unit.

`./spinel` is a single native binary (`build/spinel`; the repo-root
`spinel` is a convenience symlink `make` creates) that parses, infers
types, emits C, invokes `cc` to link it, and can run the result -- no shell
wrapper or chained helper binaries, no network, no manifest knowledge
(that separation is what keeps builds hermetic; `spin` owns the stateful
side). It supports the full option set, including `--rbs DIR` (RBS-seeded
inference), the `--emit-rbs` / `--emit-types` / `--emit-symbol-map`
analysis modes, and `--warn-widen` (a warning at each parameter or
return that widened to untyped, the boxed slow path).

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
`promote` extends that to every integer operation. `wrap` never widens an
Integer implicitly: a growing loop local wraps like any other arithmetic.

### RBS type signatures

Spinel can read RBS files to seed the analyzer. When invoked with
`--rbs DIR`, `spinel` runs `spinel_rbs_extract` over a directory of
`*.rbs` files (the same layout `rbs` and Steep use) and feeds the
resulting seed into the analyzer. A signature Spinel cannot represent
is dropped, and inference falls back to what it would infer anyway. One
it can is an assertion, not a hint: it pins the slot. A contradiction
the compiler can see is a compile error (see
[Contradictions](docs/rbs-extract.md#contradictions)); a value that only
turns out wrong at run time, narrowed out of a box into the slot, is
reinterpreted rather than widened. See
[docs/rbs-extract.md](docs/rbs-extract.md) for the supported subset.

The same signatures can be written inline, as RBS comments in the Ruby
source (`#: (Integer) -> String` above a `def`, `attr_reader :name #:
String`); Spinel applies them on every compile, the same way. See
[docs/inline-rbs.md](docs/inline-rbs.md).

## Benchmarks

3,651 tests pass. 62 benchmarks pass.
Geometric mean: **~8.5x faster** than Ruby 4.0.4 with `--yjit` across
the 28 benchmarks below (~15.2x against the interpreter). Measured
2026-09-17 on spinel 74c8e0a4 (32-core Linux box, gcc 13.3). Baseline is CRuby 4.0.4 (stable,
revision b89eb1bc), run with `--disable-gems` and with `--yjit` for
the JIT column. Each timing is the mean of five wall-clock runs under
`perf stat -r 5`, which held every cell inside a few percent. A compiled binary
starts in about 0.8 ms, so a sub-10 ms Spinel cell is mostly process
startup and its ratio should be read as a lower bound on the machine,
not as a measurement of the loop.

### Computation

| Benchmark | Spinel | Ruby 4.0.4 | + YJIT | Speedup vs YJIT |
|---|---|---|---|---|
| mandelbrot | 18 ms | 939 ms | 938 ms | 53.6x |
| fib (recursive) | 1.1 ms | 387 ms | 49 ms | 44.6x |
| matmul | 6.1 ms | 200 ms | 201 ms | 32.9x |
| nqueens | 6.0 ms | 147 ms | 141 ms | 23.6x |
| sieve | 14 ms | 274 ms | 274 ms | 19.2x |
| partial_sums | 41 ms | 786 ms | 777 ms | 19.0x |
| life (Conway's GoL) | 19 ms | 499 ms | 229 ms | 12.1x |
| sudoku | 3.3 ms | 63 ms | 32 ms | 9.6x |
| fannkuch | 1.3 ms | 9.2 ms | 8.9 ms | 6.8x |
| fasta (DNA seq gen) | 1.5 ms | 8.4 ms | 8.8 ms | 5.9x |
| tak | 7.8 ms | 320 ms | 46 ms | 5.9x |
| ackermann | 6.7 ms | 290 ms | 39 ms | 5.8x |
| tarai | 6.9 ms | 255 ms | 38 ms | 5.5x |

### Data Structures & GC

| Benchmark | Spinel | Ruby 4.0.4 | + YJIT | Speedup vs YJIT |
|---|---|---|---|---|
| so_lists | 12 ms | 259 ms | 159 ms | 13.1x |
| huffman (encoding) | 5.4 ms | 38 ms | 39 ms | 7.2x |
| linked_list | 23 ms | 189 ms | 153 ms | 6.7x |
| gcbench | 196 ms | 2_114 ms | 952 ms | 4.9x |
| binary_trees | 2.7 ms | 23 ms | 13 ms | 4.7x |
| rbtree (red-black tree) | 15 ms | 355 ms | 67 ms | 4.5x |
| splay tree | 8.7 ms | 116 ms | 39 ms | 4.5x |

### Real-World Programs

| Benchmark | Spinel | Ruby 4.0.4 | + YJIT | Speedup vs YJIT |
|---|---|---|---|---|
| ao_render (ray tracer) | 70 ms | 1_646 ms | 611 ms | 8.7x |
| bigint_fib (1000 digits) | 0.9 ms | 5.7 ms | 6.1 ms | 6.8x |
| pidigits (bigint) | 1.0 ms | 5.8 ms | 6.2 ms | 6.2x |
| template engine | 56 ms | 460 ms | 328 ms | 5.9x |
| json_parse | 28 ms | 212 ms | 133 ms | 4.7x |
| str_concat | 1.3 ms | 6.4 ms | 6.0 ms | 4.6x |
| csv_process | 95 ms | 491 ms | 396 ms | 4.2x |
| io_wordcount | 14 ms | 49 ms | 44 ms | 3.1x |

A few notes on what YJIT does and doesn't change. On some integer-loop
workloads (mandelbrot, nqueens, matmul, partial_sums, sieve) YJIT's
numbers are essentially identical to interpreted Ruby; the benchmark
is bound by integer / float operations that the interpreter already
runs at native speed. On call-heavy code (ackermann, tarai, tak, rbtree)
YJIT gives a real 5-8x lift over the interpreter, which is why those
rows carry Spinel's smaller multiples.

One row deserves a caveat rather than a boast. `fib` is not 45x faster
per call: its whole call tree is a pure function of a literal, so the C
compiler collapses most of it at build time -- `fib(42)` retires 231M
instructions where the recursion itself would make 866M calls. Read it
as "the C compiler got to see the whole program," which is the point of
compiling ahead of time, but not as a per-call number.

The genuinely narrow cells are `io_wordcount`, bound by line reading and
hash updates that neither compiler can specialize, and `csv_process` and
`gcbench`, bound by allocation.

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

**Types**: Integer, Float, String (immutable + mutable), Symbol, Array,
Hash, Range, Time, StringIO, File, Regexp, MatchData, Complex, Rational,
Bigint (auto-promoted), Enumerator, Set, Fiber, Thread, Mutex, Queue,
SizedQueue, ConditionVariable, Marshal (dump/load). Polymorphic values
via tagged unions. Nullable object types (`T?`) for self-referential
data structures (linked lists, trees).

**Inspect / `p`**: `Object#inspect` produces CRuby-byte-identical
output across the whole type surface -- primitives, typed and
heterogeneous arrays, every Hash variant, Range, Struct, and
user-class instances (`#<Name:0x... @ivar=...>`, or a user-defined
`inspect` if present), including values reached through a polymorphic
(tagged-union) binding. `p obj`, `obj.inspect`, `obj.to_s`, and
`"#{obj}"` interpolation all agree.

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

**Fiber**: Cooperative concurrency via a small portable assembly
context switch (x86-64 / arm64; no `ucontext` dependency). `Fiber.new`,
`Fiber#resume`, `Fiber.yield` with value passing, `Fiber#raise`/`#kill`,
external `Enumerator`s and `Enumerator::Lazy` ride the same machinery.
Captures free variables via heap-promoted cells. Per-fiber storage via
`Fiber[:k]` / `Fiber[:k] = v` (and the `Fiber.current[:k]` aliases) --
symbol-keyed poly-valued, lazily allocated, shallow-snapshot inherited
from the parent at `Fiber.new` time.

**Threads**: `Thread` runs with **true parallelism and no GVL** -- an
M:N scheduler multiplexes green threads onto OS workers (one per core,
cap with `SPINEL_WORKERS`), with work stealing and a ~10 ms preemption
quantum, over a stop-the-world GC. `Thread.new/#join/#value/#alive?`,
`Thread.current/main/list/pass`, thread-locals, and the synchronization
primitives `Mutex` (`#synchronize/#lock/#unlock/#try_lock`), `Queue` /
`SizedQueue` (blocking `#pop`/`#push`), and `ConditionVariable`
(`#wait/#signal/#broadcast`) are supported. Unsynchronized shared
mutation is a data race, as in JRuby/TruffleRuby -- see
[docs/thread.md](docs/thread.md) for the model and the full API list.
The threaded runtime is a separate archive linked only when a program
actually uses `Thread`; single-threaded programs keep the byte-identical
fast path.

**Memory**: Generational mark-and-sweep GC over a slab of size-classed
chunks whose generation, mark and finalizer state are bitmaps, so a sweep
never touches a dead object and an allocation is a bump into a run of
free slots claimed in one bitmap write; the sweep runs beside the program
on the worker that allocated. Small classes (≤8 scalar fields, no
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
  loop stops as soon as a signature over the refined tables stops
  changing. Most programs converge in a couple of iterations, keeping
  compile time low.
- **Warning-free build**: generated C compiles cleanly at the default
  warning level across every test and benchmark; the harness uses
  `-Werror` so regressions surface immediately.

## Architecture

```
spinel                Single binary: parse + analyze + codegen + cc driver
                      (repo-root symlink to build/spinel)
src/spinel_parse.c    Frontend: libprism → NodeTable
src/node_table.c      The in-memory NodeTable and its --dump-ast printer
src/analyze*.c        Whole-program type inference
src/codegen*.c        C code generation
src/main.c            CLI driver: pipeline + cc invocation
src/csplit.c          C splitter: --jobs parts and --incremental units
src/unit_cache.c      Object cache, parallel cc and link for --incremental
lib/spinel_rt.h       Runtime library header (GC, arrays, hashes, strings)
lib/sp_*.c            Out-of-line runtime (bigint, GC, fiber, I/O, time, ...)
lib/regexp/           Built-in regexp engine; all linked into libspinel_rt.a
test/                 2,959 feature tests
benchmark/            61 benchmarks
docs/                 User docs (require, FFI, RBS, limitations); internals/ for compiler structure
Makefile              Build automation
```

The analyzer and code generator are C (`src/analyze*.c`,
`src/codegen*.c`) sharing a common `Compiler` over the in-memory
`NodeTable`. The pipeline accepts the Ruby subset Spinel targets:
classes, `def`, `attr_accessor`, `if`/`case`/`while`,
`each`/`map`/`select`, `yield`, blocks (including `...` argument
forwarding), `begin`/`rescue`, String/Array/Hash operations, File I/O.

No dynamic metaprogramming or `eval`. Compile-time class-body
declarations with compile-time-known literal inputs are supported for
Struct-style method synthesis.

### What analyze does

The analyze stage (`analyze_program`) owns whole-program type
inference. It's a sequence of passes over the AST, each one filling in
or refining one piece of the static model:

1. **Registration** -- a walk that registers every class, module,
   top-level method, instance/class method, ivar, FFI declaration,
   regexp literal, and constant, then resolves parents, includes,
   prepends, attr/alias declarations and inherited members. After
   this the compiler's tables carry every name the program defines.

2. **Call-site widening** -- each scope's call sites feed their arg
   types into the callee's param slots. The unifier widens to `poly`
   only when two call sites disagree -- the conservative direction.

3. **Iterative refinement loop** -- return types, ivar types from
   writers, param types (array / hash / string specializations),
   block-param types, default-param types, and `for`/bigint loop
   locals are re-inferred to a fixpoint. The loop terminates when a
   signature over those tables stops changing; most programs converge
   in a couple of rounds.

4. **Feature detection** -- value-type detection (which small
   immutable classes become stack structs), GC need, symbol
   collection, proc-capture marking, and reachability. These set the
   flags that gate runtime-helper emission and mark classes / methods
   for dead-code elimination.

5. **Node-type annotation** -- a final pass fills a per-node
   inferred-type cache that codegen reads directly while emitting,
   avoiding any re-inference at emit time.

### What codegen does

The codegen stage runs in the same process on the same `Compiler`,
then emits one C file:

- It emits the preamble (`#include "sp_runtime.h"`, the per-program
  runtime helpers gated on the analysis flags, the sp_Class tables for
  hierarchy-using programs, the symbol intern table, class structs and
  constructors, forward declarations), walks every reachable method /
  class-method body to emit its definition, then emits `int main()`.

- Per-program runtime helpers are gated. A `puts "hi"` program emits a
  handful of lines of C; a program that touches Method instances, hash
  literals, the class hierarchy, etc. gets the matching runtime blocks.

The runtime is split between a header (`lib/sp_runtime.h`: GC,
array/hash/string implementations, inline hot paths) and out-of-line
sources (`lib/sp_*.c`, `lib/regexp/`) archived into `libspinel_rt.a`.
Generated C includes the header and links the archive; `--gc-sections`
drops every unused runtime function from the final binary.

The parser is **src/spinel_parse.c**, which links libprism directly (no
CRuby needed) and fills the node table the compiler consumes.
`require_relative` is resolved at parse time by inlining the referenced
file.

## Building

```bash
make deps         # fetch libprism into vendor/prism (one-time)
make              # build the compiler (parser + regexp library + spinel) and spin
make test         # run the feature tests (always a fresh run)
make bench        # run benchmarks vs CRuby
make optcarrot    # end-to-end optcarrot integration test
sudo make install # install to /usr/local (spinel and spin in PATH)
make clean        # remove build artifacts
```

Override install prefix: `make install PREFIX=$HOME/.local`

[Prism](https://github.com/ruby/prism) is the Ruby parser used by
`spinel_parse`. `make deps` downloads the prism gem tarball from
rubygems.org and extracts its C sources to `vendor/prism`. If you
already have the prism gem installed, the build auto-detects it; you
can also point at a custom location with `PRISM_DIR=/path/to/prism`.

CRuby is not needed to build or run the C compiler -- only as an
optional parser fallback (the Prism-gem path) and in the test harness,
which compares Spinel's output against CRuby on the same source.

## Portability

Spinel can emit C without invoking the C compiler -- useful when you
want to build the Ruby program on one machine and ship the generated
sources to another:

```sh
spinel app.rb -c            # writes app.c next to the source
spinel app.rb -c -o app.c   # specify output path
spinel app.rb -S            # print the C to stdout
```

The output is one self-contained `.c` file. A downstream consumer
compiles it against the `lib/` headers (`sp_runtime.h` and friends) and
links `libspinel_rt.a` (the out-of-line runtime: bigint, regexp, GC,
fiber, I/O); `--gc-sections` then drops whatever the program doesn't
use. No CRuby and no `spinel` binary are needed on the target machine.

The runtime is POSIX-flavoured and targets POSIX platforms:

| Platform | Status | Compiler |
|---|---|---|
| Linux (x86-64, arm64) | Supported | gcc, clang |
| macOS (Intel, Apple Silicon) | Supported | clang |
| *BSD | Expected to work; not in CI | clang |
| Windows | Use [WSL](https://learn.microsoft.com/windows/wsl/) (builds/runs as Linux) | gcc, clang |
| Windows, native (x64) | Community-maintained, best effort; may lag behind master; see [docs/windows.md](docs/windows.md) | MinGW-w64 gcc (UCRT) |

Every PR runs `ubuntu-latest / gcc`, `ubuntu-latest / clang`, and
`macos-latest / clang` jobs end-to-end (parser build, codegen build,
full test + benchmark suites). WSL is the dependable way to run Spinel
on Windows: it builds and runs there as a native Linux toolchain. A native
build with MinGW-w64 gcc (as MSYS2 and RubyInstaller's Devkit ship it) is
community-maintained and best effort: the runtime is written against POSIX,
and `lib/win32/` supplies the POSIX surface MinGW leaves out, the
arrangement `lib/wasi/` is for WebAssembly, so the POSIX build is
unchanged by it. CI does not run it; it may lag behind master, and if it
gets in the way of a POSIX change it may be removed. See
[docs/windows.md](docs/windows.md).

## Limitations

- **No eval**: `eval`, `instance_eval("str")`, `class_eval("str")`
  (the block forms *are* compiled)
- **No dynamic metaprogramming**: `method_missing`, `define_method` /
  `send` with a runtime-computed name (a literal `send(:name)` and
  literal `define_method` do work)
- **No encoding**: assumes UTF-8/ASCII
- **No general lambda calculus**: deeply nested `-> x { }` with `[]` calls

A few cases deliberately diverge from CRuby because the CRuby behavior needs a
feature Spinel does not implement (e.g. `Integer#**` with a negative exponent
raises instead of returning a `Rational`). These are catalogued under "By
design" in [docs/limitations.md](docs/limitations.md).

## Dependencies

- **Build time**: [libprism](https://github.com/ruby/prism) (C library),
  CRuby (bootstrap only)
- **Run time**: None. Generated binaries need only libc + libm.
- **Regexp**: Built-in engine, no external library needed.
- **Bigint**: Built-in (from mruby-bigint), linked only when used.

## Contributing

Contributions are welcome. The issue tracker doubles as the roadmap --
anything open is fair game; the most useful entry points are
reproducer-shaped bug reports (a 5-line Ruby that fails in Spinel but
passes in CRuby) and codegen fixes that close one such report.

For a bug report, start from `spinel diff`: it runs the same program
under CRuby and under Spinel, folds what cannot be compared (addresses,
paths, times, the backtrace) and says what differs, under one label:

```
$ spinel diff app.rb -- arg1 arg2
spinel diff: exception-diff
  program: app.rb 'arg1' 'arg2'
  ruby:    exit 0
  spinel:  exit 1
  exception (ruby):   (none)
  exception (spinel): FrozenError: can't modify frozen String: "lit"
```

The labels are `compile-error`, `link-error`, `crash`, `timeout`,
`nondeterministic`, `exception-diff`, `output-diff` and `same`; the exit
status is 0 for no difference, 1 for a difference, 2 when Spinel could not
build the program, 3 when its binary crashed, 4 for the tool's own error
(CI can read it). `--emit-issue PATH` writes the report to a file,
`--timeout SEC` bounds each run (default 30), `--ruby PATH` picks the
reference ruby, `--keep-tmp` leaves the C and the binary. Paste the report
into the issue. Two cautions: the program runs twice, with the arguments
you give, so do not point it at code you would not run; and a program
whose output carries a per-run value (`rand`, a pid, `object_id`, a time
not on a line of its own) reads as `nondeterministic` or as a difference,
since Spinel's generator is not CRuby's.

Workflow:

- Open a focused PR. Small and contained merges faster than sweeping
  refactors.
- Run `make gate` on your branch merged with the current `master`, and
  paste its `Tests:`, `scale-test` and `gate:` lines into the PR. The gate
  runs the test corpus, the benchmarks, optcarrot, rubyboy, the ruby/spec
  retention gate, scale-test, spin-check and the other property tests; see
  [CONTRIBUTING.md](CONTRIBUTING.md).
- Add a regression test under `test/` for any fix or new feature; the
  harness compares Spinel's output against CRuby on the same source,
  so the test usually doesn't need to assert anything beyond `puts`.
- Reference issues with `Closes #N` / `Fixes #N` / `Refs #N` trailers
  in the commit message.
- If the work was assisted by an AI, add a `Co-Authored-By:` trailer
  for the assistant alongside any human co-authors. The maintainer's
  own AI-assisted commits use `Co-Authored-By: Claude Opus 4.8`.

Adjacent ecosystem (community-built, not part of this repo):

- [rubocop_spinel](https://github.com/gurgeous/rubocop_spinel) --
  a RuboCop custom cop that flags Ruby code Spinel doesn't yet
  support.
- [spinel-dev](https://github.com/OriPekelman/spinel-dev): developer
  tooling for debugging Spinel builds (a CRuby-vs-Spinel value bisector
  for silent miscompiles, a ruby-lsp type addon, and perf/flamegraph
  analysis). The zero-dependency tools -- `spinel-doctor` (health check),
  `spinel-reduce` (minimal-repro reducer), `spinel-flatten` and
  `spinel diff` (CRuby-vs-Spinel comparison) -- now ship in the box; see
  [`tools/`](tools/).
- [spin packages](https://github.com/OriPekelman/spinelgems): a survey of
  which RubyGems compile and run under Spinel, plus bundler-spinel, a
  Bundler plugin that vendors and compatibility-gates `Gemfile`
  dependencies.
- [roundhouse](https://github.com/rubys/roundhouse): transpiles Rails
  applications (models, controllers, views, routes) to plain Ruby
  without metaprogramming, in a form Spinel compiles to a standalone
  binary. Under Spinel,
  [Campfire](https://github.com/basecamp/once-campfire) (37signals'
  group chat app) passes its own test suite in full, and current
  upstream [Lobsters](https://github.com/lobsters/lobsters) (the
  link-aggregation site behind lobste.rs) compiles and links. It also
  emits other target languages from the same analysis.

## History

Spinel was originally implemented in C (branch `c-version`), then
rewritten in Ruby (branch `ruby-v1`), then rewritten again in a
self-hosting Ruby subset that compiled itself. The current `master` is a
fresh C implementation of the analyze and codegen stages, which builds
far faster than the self-hosted backend while producing equivalent
output; it is what `master` ships and what the gate (`make test` /
`make bench` / `make optcarrot`) checks. The self-hosting Ruby
implementation is preserved on the
[`self-host`](../../tree/self-host) branch for historical reference; it
is no longer carried in `master`.

## License

MIT License. See [LICENSE](LICENSE).
