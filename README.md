# Spinel — AOT Compiler for Ruby

Spinel compiles Ruby source code to standalone C executables via
[Prism](https://github.com/ruby/prism) parsing and whole-program type inference.
Classes become C structs, methods become direct function calls, and numeric
operations compile to native C arithmetic with zero dynamic dispatch overhead.
Generated binaries have no runtime dependencies — no mruby, no GC, just libc and libm.

## Quick Start

```bash
# 1. Fetch and build the Prism parser library
make deps

# 2. Build the spinel compiler
make

# 3. Compile a Ruby program to C, then to a native binary
./spinel --source=app.rb --output=app.c
cc -O2 app.c -lm -o app
```

## Benchmarks

| Benchmark | CRuby 3.2 | mruby | **Spinel AOT** | Speedup |
|-----------|-----------|-------|----------------|---------|
| mandelbrot (600x600 PBM) | 1.14s | 3.18s | **0.02s** | **57x** |
| ao_render (64x64 AO raytracer) | 3.55s | 13.69s | **0.06s** | **59x** |

Binary sizes: mandelbrot 16KB, ao_render 21KB (stripped).

```bash
make test   # compile mandelbrot, run, verify output matches CRuby
```

## How It Works

```
Ruby Source (.rb)
    |
    v
Prism (libprism)             -- parse to AST
    |
    v
Pass 1: Class Analysis       -- find classes, methods, instance variables
    |
    v
Pass 2: Type Inference        -- infer types for all variables, ivars, params
    |                            (Integer, Float, Boolean, Object types)
    v
Pass 3: Struct/Method Emit    -- classes -> C structs
    |                            methods -> C functions (direct calls)
    |                            getters/setters -> inline field access
    v
Pass 4: Main Codegen          -- top-level code -> main()
    |                            while/for/times -> C loops
    |                            arithmetic -> C operators
    |                            puts/print/printf -> stdio
    v
Standalone C file
    |
    v
cc -O2 -lm -> native binary   -- no mruby, no GC, just libc
```

For `bm_ao_render.rb`, the compiler:
- Converts 6 Ruby classes (Vec, Sphere, Plane, Ray, Isect, Scene) into C structs
- Vec (3 floats) is passed/returned by value — no heap allocation
- All method calls are devirtualized to direct C function calls
- `Integer#times` blocks become C for loops
- `Math.sqrt/cos/sin` map directly to C math functions
- The Rand module's xorshift PRNG compiles to inline integer arithmetic

## Supported Language Features

| Feature | Example |
|---------|---------|
| **Classes & OOP** | |
| Classes with instance variables | `class Vec; def initialize(x,y,z); @x=x; end; end` |
| Inheritance | `class Dog < Animal` |
| `super` | `super(name)` in child initialize/methods |
| Method definitions | `def vadd(b); Vec.new(@x+b.x, ...); end` |
| Getters/setters (inlined) | `def x; @x; end` / `def x=(v); @x=v; end` |
| Object construction | `Vec.new(1.0, 2.0, 3.0)` |
| Method calls on typed objects | `ray.org.vsub(@center)` |
| Modules with state | `module Rand; @x = 123; def self.rand; ...; end; end` |
| **Blocks & Closures** | |
| `yield` | `def my_iter(n); yield i; end` |
| Blocks at call sites | `my_iter(10) do \|i\| total += i end` |
| `Array#each/map/select` | `arr.each { \|x\| puts x }` (inlined) |
| `Integer#times` with block | `n.times do \|i\| ... end` |
| Lambda/closures | `-> x { x + 1 }` with capture analysis |
| **Control Flow** | |
| `if` / `elsif` / `else` | conditional branching |
| `case` / `when` / `else` | pattern matching with values, ranges |
| `unless` | `unless x > 20` |
| `while`, `until` | loops |
| Ternary operator | `escape ? 0 : 1` |
| `and` / `or` / `not` | boolean operators |
| `break`, `next`, `return` | loop/method exit, continue |
| **Exception Handling** | |
| `begin`/`rescue`/`ensure` | exception handling (setjmp/longjmp) |
| `raise` | `raise "error message"` |
| `rescue => e` | capture exception message |
| `retry` | restart begin block |
| **Types & Literals** | |
| Integer, Float, Boolean, String, nil | unboxed C types |
| Symbol | `:hello` → string constant |
| Integer arrays | push/pop/shift/dup/reverse!/each/map/select |
| Hash (string→integer) | `h = {}; h["key"] = val; h.each { \|k,v\| }` |
| Default parameter values | `def greet(name, greeting = "Hello")` |
| **Arithmetic & Operators** | |
| Arithmetic, comparison, bitwise | `+`, `-`, `*`, `/`, `%`, `<`, `>`, `==`, `<<`, `\|`, `^`, `**` |
| Unary minus | `-b`, `-(expr)` |
| `Math.sqrt`, `Math.cos`, `Math.sin` | C math functions |
| Integer methods | `abs`, `even?`, `odd?`, `zero?` |
| Float methods | `abs`, `ceil`, `floor`, `round` |
| **Strings** | |
| String literals, interpolation | `"hello #{name}"` → printf |
| String methods | `length`, `upcase`, `downcase`, `include?`, `+` |
| `Integer#to_s`, `Integer#chr` | number to string conversion |
| **I/O** | |
| `puts`, `print`, `printf`, `putc`, `p` | stdio calls (Int, Float, Bool, String) |
| **Other** | |
| Parallel/chained assignment | `zr, zi = tr, ti` / `a = b = 0` |
| Local variables, constants | `size = 600`, `ITER = 49` |
| Array indexing | `basis[0].x`, `@spheres[1].intersect(...)` |
| Mark-and-sweep GC | automatic for heap-allocated objects |

## Benchmarks

| Benchmark | CRuby 3.2 | mruby | **Spinel AOT** | Speedup |
|-----------|-----------|-------|----------------|---------|
| mandelbrot (600x600 PBM) | 1.14s | 3.18s | **0.02s** | **57x** |
| ao_render (64x64 AO raytracer) | 3.55s | 13.69s | **0.07s** | **51x** |
| so_lists (300x10K arrays) | 0.44s | 2.01s | **0.02s** | **22x** |
| fib(34) recursive | 0.55s | 2.78s | **0.01s** | **55x** |
| lc_fizzbuzz (Church encoding) | 28.96s | — | **1.55s** | **19x** |
| mandel_term (terminal art) | 0.05s | 0.05s | **~0s** | **50x+** |

## Project Structure

```
spinel/
├── src/
│   ├── main.c          # CLI, file reading, Prism parsing
│   ├── codegen.h       # Type system, class/method/module info structs
│   └── codegen.c       # Multi-pass code generator (~5000 lines)
├── examples/
│   ├── bm_so_mandelbrot.rb   # Mandelbrot set (while loops, bitwise)
│   ├── bm_ao_render.rb       # AO raytracer (6 classes, modules)
│   ├── bm_so_lists.rb        # Array operations (push/pop/shift)
│   ├── bm_fib.rb             # Recursive fibonacci
│   ├── bm_app_lc_fizzbuzz.rb # Lambda calculus FizzBuzz (1201 closures)
│   ├── bm_mandel_term.rb     # Terminal Mandelbrot (cross-function calls)
│   ├── bm_yield.rb           # yield/blocks (iterators, each/map/select)
│   ├── bm_case.rb            # case/when, unless, next, default args
│   └── bm_inherit.rb         # Inheritance, super
├── prototype/
│   └── tools/          # Step 0 prototype (RBS extraction, LumiTrace, etc.)
├── Makefile
├── PLAN.md             # Implementation roadmap
└── ruby_aot_compiler_design.md  # Detailed design document
```

## Dependencies

- **Build time**: [Prism](https://github.com/ruby/prism) (fetched automatically by `make deps`)
- **Run time**: None. Generated binaries are standalone (libc + libm only).

## License

Spinel is released under the [MIT License](LICENSE).

### Note on License

mruby has chosen a MIT License due to its permissive license allowing
developers to target various environments such as embedded systems.
However, the license requires the display of the copyright notice and license
information in manuals for instance. Doing so for big projects can be
complicated or troublesome. This is why mruby has decided to display "mruby
developers" as the copyright name to make it simple conventionally.
In the future, mruby might ask you to distribute your new code
(that you will commit,) under the MIT License as a member of
"mruby developers" but contributors will keep their copyright.
(We did not intend for contributors to transfer or waive their copyrights,
actual copyright holder name (contributors) will be listed in the [AUTHORS](AUTHORS)
file.)

Please ask us if you want to distribute your code under another license.
