# Portability: the GNU C extensions and their fallbacks

The C that Spinel generates, and the runtime it links against, use a number of
GCC/clang extensions: builtins, attributes, 128-bit integers, atomics. Each of
them goes through a macro in `lib/sp_compat.h`, which expands to the extension
where the compiler has it and to standard C where it does not. A GCC or clang
build compiles to the same machine code it did before the macros existed.

What the macros cannot express is listed at the end: the generated C still
needs a GNU-compatible compiler for those.

## Detection

`sp_compat.h` asks `__has_builtin`, `__has_attribute` and `__has_include`
first. The GCC version is the last resort, for the GCCs that predate the
`__has_` forms. It is included first by `lib/sp_types.h`, so every runtime
source and every generated program sees it.

## The fallbacks

| Macro | With the extension | Fallback |
|---|---|---|
| `SP_LIKELY`, `SP_UNLIKELY`, `SP_EXPECT` | `__builtin_expect` | the condition itself |
| `SP_COLD`, `SP_NOINLINE`, `SP_ALWAYS_INLINE`, `SP_UNUSED`, `SP_PRINTF_FORMAT` | the attribute | nothing |
| `SP_INLINE` | `inline __attribute__((always_inline))` | `inline` |
| `SP_NORETURN` | `__attribute__((noreturn))` | C11 `_Noreturn`, else nothing |
| `SP_MUSTTAIL` (before `return`) | `__attribute__((musttail))` | a plain call; Ruby does not promise tail calls |
| `SP_UNREACHABLE()` | `__builtin_unreachable()` | `abort()` |
| `SP_PREFETCH`, `SP_CPU_RELAX` | `__builtin_prefetch`, the pause instruction | nothing |
| `SP_CONSTANT_P` | `__builtin_constant_p` | 0: the general path, always correct |
| `sp_ckd_add_iptr` and the other checked operations | `__builtin_*_overflow` | C23 `<stdckdint.h>`, else a range check by hand |
| `SP_CLZ64`, `SP_CTZ64`, `SP_POPCOUNT64` | `__builtin_clzll` etc. | portable bit counting |
| `SP_HAVE_INT128`, `sp_int128` | `__int128` | `long double` in `Time`; checked `long long` in `Rational`, which raises `RangeError` when an intermediate product leaves 64 bits even if the reduced result would fit |
| `SP_ATOMIC_*` | the `__atomic` builtins | the plain operation: only the single-threaded runtime can use it |
| `SP_THREAD_LOCAL` | `__thread` | C11 `_Thread_local` |
| `SP_WEAK` | `__attribute__((weak))` | none; the jemalloc probe that uses it is compiled out |
| `SP_HAVE_X86_SHA`, `SP_TARGET_X86_SHA` | on x86-64, `__attribute__((target("sha,sse4.1")))` for a function its caller reaches only after asking the CPU | 0: the function is not compiled, and SHA-256 stays the portable C |

## `PORTABLE=1`: take every fallback

Defining `SP_PORTABLE` makes `sp_compat.h` take every fallback even where the
extension exists, so a GCC or clang build runs the code a plain C compiler
would get:

```sh
make PORTABLE=1 test      # the runtime and the test programs with -DSP_PORTABLE
```

Use a tree of its own for it: the objects differ, and make does not rebuild on
a change of flags. For a program of your own, compile the generated C and the
runtime with `-DSP_PORTABLE`.

One thing keeps the extension under `SP_PORTABLE`, because the program needs
it to be correct rather than fast: the threaded runtime (`SP_THREADS`) keeps
the `__atomic` builtins; C11 atomics would need the shared variables declared
`_Atomic`.

`SP_PORTABLE` costs speed. optcarrot runs about 25% slower, 21 points of it
from losing the forced inlining of the generated C; the benchmarks that run
long enough to measure average about 5%.

## What stays a GNU extension

These have no macro spelling. A compiler without them cannot build the
generated C until the generator writes them differently:

- statement expressions `({ ... })`, the shape of most generated expressions
  (and `SP_WBO`, `SP_POOL_NEW` in the runtime headers);
- `__attribute__((cleanup))`, which pops the GC roots (`SP_GC_SAVE`,
  `SP_GC_ROOT*`) and the regexp frame on every scope exit;
- `__COUNTER__` in the names of those guards, and `__typeof__` in a few
  temporaries the generator splices;
- `__attribute__((constructor))`, the runtime's start-up hooks;
- the `__asm__` label on `--ffi` declarations, and the fiber context switch,
  written in assembly per architecture;
- `SP_FLOAT_NIL_CONST`, above.

With `-D_DEFAULT_SOURCE` for the POSIX declarations, the runtime compiles under
`-std=c11 -pedantic` with only one kind of diagnostic: a function pointer kept
in a `void *` (the scan and hook pointers), which ISO C does not define and
POSIX does. The generated C adds the statement expressions to that. C99 is not
reachable: the GC root macros choose their tag with `_Generic`.

The code from mruby (`lib/regexp`, `lib/sp_bigint.c`, `lib/sp_dtoa.c`) keeps
its own spelling, to stay in step with mruby. It carries its own detection and
fallbacks (`__int128`, clz/ctz/popcount in `sp_bigint.c` and `sp_dtoa.c`;
`lib/regexp` uses atomics only in the threaded build), which `SP_PORTABLE`
does not reach.
