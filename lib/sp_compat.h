/* sp_compat.h -- the GNU C extensions the runtime and the generated C use,
 * behind macros that fall back to standard C where a compiler lacks them.
 *
 * A compiler with the extension keeps the code it compiled before: every
 * macro expands to the same builtin or attribute. One without it (an old or
 * non-GNU toolchain for a small target) gets the fallback: a hint becomes
 * nothing, a builtin becomes the standard (C23 <stdckdint.h>) or hand-written
 * equivalent.
 *
 * Detection asks __has_builtin / __has_attribute / __has_include first; the
 * GCC version is the last resort, for the GCCs that predate the __has_ forms.
 *
 * Defining SP_PORTABLE takes every fallback even where the extension exists,
 * so a GCC or clang build exercises the code a plain C compiler would get.
 * What no macro can express stays an extension under SP_PORTABLE too (the
 * statement expressions and cleanup scopes the generated C is built from, and
 * the rest listed at the end of this file).
 *
 * Included first by sp_types.h, the base header every runtime source and the
 * generated translation unit see; it depends on nothing but <stdint.h>.
 */
#ifndef SP_COMPAT_H
#define SP_COMPAT_H

#include <stdint.h>
#include <stddef.h>

/* ---- detection ---- */

#if defined(__GNUC__) && !defined(__clang__)
# define SP_GNUC_PREREQ(maj, min) (__GNUC__ > (maj) || (__GNUC__ == (maj) && __GNUC_MINOR__ >= (min)))
#else
# define SP_GNUC_PREREQ(maj, min) 0
#endif

#if defined(SP_PORTABLE)
# define SP_HAS_BUILTIN(x)   0
# define SP_HAS_ATTRIBUTE(x) 0
# define SP_GNU_OK           0
#else
# if defined(__has_builtin)
#  define SP_HAS_BUILTIN(x) __has_builtin(x)
# else
#  define SP_HAS_BUILTIN(x) 0
# endif
# if defined(__has_attribute)
#  define SP_HAS_ATTRIBUTE(x) __has_attribute(x)
# else
#  define SP_HAS_ATTRIBUTE(x) 0
# endif
/* the version fallback: a GCC before __has_builtin (10) / __has_attribute (5) */
# if defined(__GNUC__) || defined(__clang__)
#  define SP_GNU_OK 1
# else
#  define SP_GNU_OK 0
# endif
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
# define SP_C11 1
#else
# define SP_C11 0
#endif

/* ---- hints: nothing when absent ---- */

#if SP_HAS_BUILTIN(__builtin_expect) || (!defined(SP_PORTABLE) && SP_GNUC_PREREQ(3, 0))
# define SP_LIKELY(x)   __builtin_expect(!!(x), 1)
# define SP_UNLIKELY(x) __builtin_expect(!!(x), 0)
/* the value itself, unnormalized: what a site wrote as __builtin_expect(x, v) */
# define SP_EXPECT(x, v) __builtin_expect((x), (v))
#else
# define SP_LIKELY(x)   (x)
# define SP_UNLIKELY(x) (x)
# define SP_EXPECT(x, v) (x)
#endif

#if SP_HAS_ATTRIBUTE(cold) || (!defined(SP_PORTABLE) && SP_GNUC_PREREQ(4, 3))
# define SP_COLD __attribute__((cold))
#else
# define SP_COLD
#endif

#if SP_HAS_ATTRIBUTE(noinline) || (!defined(SP_PORTABLE) && SP_GNUC_PREREQ(3, 1))
# define SP_NOINLINE __attribute__((noinline))
#else
# define SP_NOINLINE
#endif

/* `inline` stays: only the forcing is a hint. The generated C spells it
   `inline SP_ALWAYS_INLINE`, so src/csplit.c still sees the `inline` word. */
#if SP_HAS_ATTRIBUTE(always_inline) || (!defined(SP_PORTABLE) && SP_GNUC_PREREQ(3, 1))
# define SP_ALWAYS_INLINE __attribute__((always_inline))
#else
# define SP_ALWAYS_INLINE
#endif
#define SP_INLINE inline SP_ALWAYS_INLINE

#if SP_HAS_ATTRIBUTE(unused) || (!defined(SP_PORTABLE) && SP_GNUC_PREREQ(3, 0))
# define SP_UNUSED __attribute__((unused))
#else
# define SP_UNUSED
#endif

#if SP_HAS_ATTRIBUTE(format) || (!defined(SP_PORTABLE) && SP_GNUC_PREREQ(3, 0))
# define SP_PRINTF_FORMAT(fmt, first) __attribute__((format(printf, fmt, first)))
#else
# define SP_PRINTF_FORMAT(fmt, first)
#endif

/* A tail call the compiler must make. Ruby does not promise one, so the plain
   call a compiler without it makes means the same. Written before `return`. */
#if SP_HAS_ATTRIBUTE(musttail)
# define SP_MUSTTAIL __attribute__((musttail))
#else
# define SP_MUSTTAIL
#endif

/* noreturn is a hint to the optimizer and the warnings; C11 has its own */
#if SP_HAS_ATTRIBUTE(noreturn) || (!defined(SP_PORTABLE) && SP_GNUC_PREREQ(2, 5))
# define SP_NORETURN __attribute__((noreturn))
#elif SP_C11
# define SP_NORETURN _Noreturn
#else
# define SP_NORETURN
#endif

#if SP_HAS_BUILTIN(__builtin_unreachable) || (!defined(SP_PORTABLE) && SP_GNUC_PREREQ(4, 5))
# define SP_UNREACHABLE() __builtin_unreachable()
#else
# include <stdlib.h>
# define SP_UNREACHABLE() abort()
#endif

#if SP_HAS_BUILTIN(__builtin_prefetch) || (!defined(SP_PORTABLE) && SP_GNUC_PREREQ(3, 1))
# define SP_PREFETCH(p) __builtin_prefetch(p)
#else
# define SP_PREFETCH(p) ((void)(p))
#endif

/* Is `x` a constant the compiler can see? No means the general path, which
   is always correct. */
#if SP_HAS_BUILTIN(__builtin_constant_p) || (!defined(SP_PORTABLE) && SP_GNUC_PREREQ(3, 1))
# define SP_CONSTANT_P(x) __builtin_constant_p(x)
#else
# define SP_CONSTANT_P(x) 0
#endif

/* The pause a spin-wait loop takes between polls. */
#if !defined(SP_PORTABLE) && (defined(__x86_64__) || defined(__i386__)) && \
    (SP_HAS_BUILTIN(__builtin_ia32_pause) || SP_GNU_OK)
# define SP_CPU_RELAX() __builtin_ia32_pause()
#elif !defined(SP_PORTABLE) && (defined(__aarch64__) || defined(__arm__)) && SP_GNU_OK
# define SP_CPU_RELAX() __asm__ __volatile__("yield" ::: "memory")
#else
# define SP_CPU_RELAX() ((void)0)
#endif

/* ---- instruction-set extensions ----
   A function compiled for instructions the rest of the build does not assume
   carries the target attribute, and its caller asks the CPU before calling
   it. Where the macro is 0 the function is not compiled and the caller keeps
   its portable C; SP_PORTABLE takes that side everywhere. */

/* The x86-64 SHA extensions, with the SSSE3 and SSE4.1 their block function
   shuffles and blends with. <immintrin.h> has the intrinsics, <cpuid.h> the
   question. */
#if !defined(SP_PORTABLE) && defined(__x86_64__) && SP_HAS_ATTRIBUTE(target) && defined(__has_include)
# if __has_include(<immintrin.h>) && __has_include(<cpuid.h>)
#  define SP_HAVE_X86_SHA 1
#  define SP_TARGET_X86_SHA __attribute__((target("sha,sse4.1")))
# endif
#endif
#if !defined(SP_HAVE_X86_SHA)
# define SP_HAVE_X86_SHA 0
#endif

/* ---- attributes with a meaning: no silent fallback ---- */

/* Run before main. There is no standard spelling; a compiler without it
   leaves the hook unrun, so it is kept even under SP_PORTABLE (the
   generated program would otherwise start with its hooks missing). */
#if SP_HAS_ATTRIBUTE(constructor) || defined(__GNUC__) || defined(__clang__)
# define SP_CONSTRUCTOR __attribute__((constructor))
# define SP_DESTRUCTOR  __attribute__((destructor))
#else
# error "spinel: the runtime needs __attribute__((constructor)) (see sp_compat.h)"
#endif

/* An optional symbol that is NULL when nothing defines it. Without it the
   feature that probes the symbol is compiled out (SP_HAVE_WEAK is 0). */
#if SP_HAS_ATTRIBUTE(weak) || (!defined(SP_PORTABLE) && SP_GNUC_PREREQ(3, 0))
# define SP_WEAK __attribute__((weak))
# define SP_HAVE_WEAK 1
#else
# define SP_WEAK
# define SP_HAVE_WEAK 0
#endif

/* Run `fn(&var)` when `var` goes out of scope, by any exit. The GC root and
   regexp frame guards pop themselves this way; nothing standard does it, so
   it is kept under SP_PORTABLE and a compiler without it cannot build the
   generated C (the generator would have to emit every pop itself). */
#if SP_HAS_ATTRIBUTE(cleanup) || defined(__GNUC__) || defined(__clang__)
# define SP_CLEANUP(fn) __attribute__((cleanup(fn)))
#else
# error "spinel: the runtime needs __attribute__((cleanup)) (see sp_compat.h)"
#endif

/* ---- thread-local storage ---- */

#if !defined(SP_PORTABLE) && (defined(__GNUC__) || defined(__clang__))
# define SP_THREAD_LOCAL __thread
#elif SP_C11 && !defined(__STDC_NO_THREADS__)
# define SP_THREAD_LOCAL _Thread_local
#else
# define SP_THREAD_LOCAL /* no TLS: only the single-threaded build works */
# define SP_NO_THREAD_LOCAL 1
#endif

/* ---- overflow-checked arithmetic ----
   The builtin first, then C23's <stdckdint.h>, then a range check by hand.
   Named by operand width (intptr_t, which sp_int is, and uint64_t): the
   builtins are type-generic, the hand-written checks cannot be. Each
   answers nonzero on overflow and stores the wrapped result. */

#if (SP_HAS_BUILTIN(__builtin_add_overflow) && SP_HAS_BUILTIN(__builtin_sub_overflow) && \
     SP_HAS_BUILTIN(__builtin_mul_overflow)) || (!defined(SP_PORTABLE) && SP_GNUC_PREREQ(5, 0))
# define SP_CKD_BUILTIN 1
#elif !defined(SP_PORTABLE) && defined(__has_include)
# if __has_include(<stdckdint.h>)
#  include <stdckdint.h>
#  define SP_CKD_STDC 1
# endif
#endif

#if defined(SP_CKD_BUILTIN)
/* the builtin in place, as the call sites wrote it before */
# define sp_ckd_add_iptr(a, b, r) __builtin_add_overflow(a, b, r)
# define sp_ckd_sub_iptr(a, b, r) __builtin_sub_overflow(a, b, r)
# define sp_ckd_mul_iptr(a, b, r) __builtin_mul_overflow(a, b, r)
# define sp_ckd_add_u64(a, b, r)  __builtin_add_overflow(a, b, r)
# define sp_ckd_mul_u64(a, b, r)  __builtin_mul_overflow(a, b, r)
#elif defined(SP_CKD_STDC)
# define sp_ckd_add_iptr(a, b, r) ckd_add(r, (intptr_t)(a), (intptr_t)(b))
# define sp_ckd_sub_iptr(a, b, r) ckd_sub(r, (intptr_t)(a), (intptr_t)(b))
# define sp_ckd_mul_iptr(a, b, r) ckd_mul(r, (intptr_t)(a), (intptr_t)(b))
# define sp_ckd_add_u64(a, b, r)  ckd_add(r, (uint64_t)(a), (uint64_t)(b))
# define sp_ckd_mul_u64(a, b, r)  ckd_mul(r, (uint64_t)(a), (uint64_t)(b))
#else
/* unsigned arithmetic wraps, and the sign bits of the operands against the
   result say whether the signed sum did */
static inline int sp_ckd_add_iptr(intptr_t a, intptr_t b, intptr_t *r) {
  uintptr_t x = (uintptr_t)a, y = (uintptr_t)b, z = x + y;
  *r = (intptr_t)z;
  return (((x ^ z) & (y ^ z)) >> (sizeof(intptr_t) * 8 - 1)) != 0;
}
static inline int sp_ckd_sub_iptr(intptr_t a, intptr_t b, intptr_t *r) {
  uintptr_t x = (uintptr_t)a, y = (uintptr_t)b, z = x - y;
  *r = (intptr_t)z;
  return (((x ^ z) & (~y ^ z)) >> (sizeof(intptr_t) * 8 - 1)) != 0;
}
/* the bounds are checked before multiplying: no wider type is portable */
static inline int sp_ckd_mul_iptr(intptr_t a, intptr_t b, intptr_t *r) {
  int ovf;
  if (a > 0) ovf = b > 0 ? a > INTPTR_MAX / b : b < INTPTR_MIN / a;
  else if (a < 0) ovf = b > 0 ? a < INTPTR_MIN / b : (b < 0 && (a == INTPTR_MIN || b == INTPTR_MIN || -a > INTPTR_MAX / -b));
  else ovf = 0;
  *r = (intptr_t)((uintptr_t)a * (uintptr_t)b);
  return ovf;
}
static inline int sp_ckd_add_u64(uint64_t a, uint64_t b, uint64_t *r) {
  *r = a + b;
  return *r < a;
}
static inline int sp_ckd_mul_u64(uint64_t a, uint64_t b, uint64_t *r) {
  *r = a * b;
  return a != 0 && *r / a != b;
}
#endif

/* ---- bit counting (64-bit operands; clz/ctz of 0 is undefined, as the
   builtins leave it) ---- */

#if SP_HAS_BUILTIN(__builtin_clzll) || (!defined(SP_PORTABLE) && SP_GNUC_PREREQ(3, 4))
# define SP_CLZ64(x) __builtin_clzll((unsigned long long)(x))
#else
static inline int sp_clz64_fallback(uint64_t x) {
  int n = 0;
  if (!(x >> 32)) { n += 32; x <<= 32; }
  if (!(x >> 48)) { n += 16; x <<= 16; }
  if (!(x >> 56)) { n += 8; x <<= 8; }
  if (!(x >> 60)) { n += 4; x <<= 4; }
  if (!(x >> 62)) { n += 2; x <<= 2; }
  if (!(x >> 63)) { n += 1; }
  return n;
}
# define SP_CLZ64(x) sp_clz64_fallback((uint64_t)(x))
#endif

#if SP_HAS_BUILTIN(__builtin_ctzll) || (!defined(SP_PORTABLE) && SP_GNUC_PREREQ(3, 4))
# define SP_CTZ64(x) __builtin_ctzll((unsigned long long)(x))
#else
static inline int sp_ctz64_fallback(uint64_t x) {
  int n = 0;
  if (!(x & 0xffffffffu)) { n += 32; x >>= 32; }
  if (!(x & 0xffffu)) { n += 16; x >>= 16; }
  if (!(x & 0xffu)) { n += 8; x >>= 8; }
  if (!(x & 0xfu)) { n += 4; x >>= 4; }
  if (!(x & 0x3u)) { n += 2; x >>= 2; }
  if (!(x & 0x1u)) { n += 1; }
  return n;
}
# define SP_CTZ64(x) sp_ctz64_fallback((uint64_t)(x))
#endif

#if SP_HAS_BUILTIN(__builtin_popcountll) || (!defined(SP_PORTABLE) && SP_GNUC_PREREQ(3, 4))
# define SP_POPCOUNT64(x) __builtin_popcountll((unsigned long long)(x))
#else
static inline int sp_popcount64_fallback(uint64_t x) {
  x = x - ((x >> 1) & 0x5555555555555555ULL);
  x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
  x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
  return (int)((x * 0x0101010101010101ULL) >> 56);
}
# define SP_POPCOUNT64(x) sp_popcount64_fallback((uint64_t)(x))
#endif

/* ---- 128-bit integers ---- */

#if !defined(SP_PORTABLE) && defined(__SIZEOF_INT128__)
# define SP_HAVE_INT128 1
__extension__ typedef __int128 sp_int128;
__extension__ typedef unsigned __int128 sp_uint128;
#else
# define SP_HAVE_INT128 0
#endif

/* ---- atomics ----
   The __atomic builtins take a plain object, which C11's <stdatomic.h> does
   not (it wants an _Atomic one), so the fallback is the plain operation:
   right for the single-threaded runtime, which is all a compiler without
   them can build. The memory-order arguments are dropped with the builtin,
   so the __ATOMIC_* names need not exist there. */

/* The threaded runtime needs them for its correctness, not its speed, so
   SP_PORTABLE keeps them there: its fallback is the single-threaded one. */
#if defined(SP_PORTABLE) && !defined(SP_THREADS)
# define SP_ATOMICS_BUILTIN 0
#elif SP_HAS_BUILTIN(__atomic_load_n) || SP_GNUC_PREREQ(4, 7) || defined(__clang__) || \
      (defined(SP_THREADS) && defined(__GNUC__))
# define SP_ATOMICS_BUILTIN 1
#else
# define SP_ATOMICS_BUILTIN 0
#endif

#if SP_ATOMICS_BUILTIN
# define SP_HAVE_ATOMICS 1
# define SP_ATOMIC_LOAD(p, mo)            __atomic_load_n(p, mo)
# define SP_ATOMIC_STORE(p, v, mo)        __atomic_store_n(p, v, mo)
# define SP_ATOMIC_EXCHANGE(p, v, mo)     __atomic_exchange_n(p, v, mo)
# define SP_ATOMIC_FETCH_ADD(p, v, mo)    __atomic_fetch_add(p, v, mo)
# define SP_ATOMIC_FETCH_SUB(p, v, mo)    __atomic_fetch_sub(p, v, mo)
# define SP_ATOMIC_FETCH_OR(p, v, mo)     __atomic_fetch_or(p, v, mo)
# define SP_ATOMIC_FETCH_AND(p, v, mo)    __atomic_fetch_and(p, v, mo)
# define SP_ATOMIC_ADD_FETCH(p, v, mo)    __atomic_add_fetch(p, v, mo)
# define SP_ATOMIC_SUB_FETCH(p, v, mo)    __atomic_sub_fetch(p, v, mo)
# define SP_ATOMIC_CAS(p, exp, des, weak, smo, fmo) __atomic_compare_exchange_n(p, exp, des, weak, smo, fmo)
# define SP_ATOMIC_FENCE(mo)              __atomic_thread_fence(mo)
#else
# define SP_HAVE_ATOMICS 0
# if defined(SP_THREADS)
#  error "spinel: the threaded runtime (SP_THREADS) needs the __atomic builtins"
# endif
# define SP_ATOMIC_LOAD(p, mo)            (*(p))
# define SP_ATOMIC_STORE(p, v, mo)        ((void)(*(p) = (v)))
/* the old value, without a temporary of the object's type: swap through
   the arithmetic that restores it (integers and pointers alike) */
# define SP_ATOMIC_EXCHANGE(p, v, mo)     sp_atomic_xchg_fallback((void *)(p), sizeof *(p), (uint64_t)(uintptr_t)(v))
# define SP_ATOMIC_FETCH_ADD(p, v, mo)    ((*(p) += (v)) - (v))
# define SP_ATOMIC_FETCH_SUB(p, v, mo)    ((*(p) -= (v)) + (v))
# define SP_ATOMIC_FETCH_OR(p, v, mo)     sp_atomic_or_fallback((uint64_t *)(p), (uint64_t)(v))
# define SP_ATOMIC_FETCH_AND(p, v, mo)    sp_atomic_and_fallback((uint64_t *)(p), (uint64_t)(v))
# define SP_ATOMIC_ADD_FETCH(p, v, mo)    (*(p) += (v))
# define SP_ATOMIC_SUB_FETCH(p, v, mo)    (*(p) -= (v))
# define SP_ATOMIC_CAS(p, exp, des, weak, smo, fmo) \
  (*(p) == *(exp) ? (*(p) = (des), 1) : (*(exp) = *(p), 0))
# define SP_ATOMIC_FENCE(mo)              ((void)0)
#endif

#if !SP_HAVE_ATOMICS
#include <string.h>
/* exchange on an object of 1, 2, 4 or 8 bytes, answering the old value in
   the low bytes of a uint64_t that the caller's cast narrows back */
static inline uint64_t sp_atomic_xchg_fallback(void *p, size_t n, uint64_t v) {
  uint64_t old = 0;
  switch (n) {
    case 1: { uint8_t o = *(uint8_t *)p; *(uint8_t *)p = (uint8_t)v; old = o; break; }
    case 2: { uint16_t o = *(uint16_t *)p; *(uint16_t *)p = (uint16_t)v; old = o; break; }
    case 4: { uint32_t o = *(uint32_t *)p; *(uint32_t *)p = (uint32_t)v; old = o; break; }
    default: { uint64_t o; memcpy(&o, p, 8); memcpy(p, &v, 8); old = o; break; }
  }
  return old;
}
static inline uint64_t sp_atomic_or_fallback(uint64_t *p, uint64_t v) { uint64_t o = *p; *p = o | v; return o; }
static inline uint64_t sp_atomic_and_fallback(uint64_t *p, uint64_t v) { uint64_t o = *p; *p = o & v; return o; }
#endif

/* ---- what stays an extension under SP_PORTABLE ----
   No macro can stand in for these; a plain C compiler needs the generator
   (or the runtime) to write them differently:
   - statement expressions `({ ... })`, the shape of most generated C;
   - __attribute__((cleanup)) on the GC root and regexp frame guards
     (SP_GC_SAVE / SP_GC_ROOT*, sp_re_frame), which pop at every scope exit;
   - __COUNTER__ in the guard names, and __typeof__ in a few spliced temps;
   - __attribute__((constructor)) hooks (SP_CONSTRUCTOR above);
   - an asm label (`__asm__("sym")`) on --ffi declarations;
   - the fiber context switch, written in asm per architecture. */

#endif /* SP_COMPAT_H */
