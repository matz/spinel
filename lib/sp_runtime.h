/* Spinel Runtime Library */
#ifndef SP_RUNTIME_H
#define SP_RUNTIME_H

/* Platform feature-test macros (_XOPEN_SOURCE for Darwin's ucontext, etc.)
   live at the top of sp_types.h so every translation unit that includes it --
   the generated TU here, and the standalone libspinel_rt.a units like
   sp_fiber.c that reach <ucontext.h> via sp_fiber.h -> sp_gc.h -> sp_types.h --
   define them before the first system header. Must precede <stdio.h>. */
#include "sp_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#include <setjmp.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

/* Opt-in native backtrace (spinel --debug). In a -g, non-inlined build the
   sp_<method> symbols are present, so sp_raise_cls can snapshot the live C
   stack at raise time and Exception#backtrace / caller format it into a
   Ruby-style backtrace — no per-method shadow frames needed. Off unless the
   generated main() sets sp_bt_enabled (debug builds), so non-debug behaviour
   and cost are unchanged. execinfo is POSIX-ish; absent on Windows. */
#if !defined(_WIN32)
#include <execinfo.h>
#define SP_BT_AVAILABLE 1
#else
#define SP_BT_AVAILABLE 0
#endif
static int sp_bt_enabled = 0;          /* set to 1 by debug-build main() */
static const char *sp_bt_srcfile = ""; /* toplevel .rb path, set by debug main() */
#if SP_BT_AVAILABLE
static void *sp_bt_buf[256];       /* frames captured at the last raise */
static int sp_bt_n = 0;
#endif
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <direct.h>
#include <io.h>      /* _pipe (POSIX pipe(2) is absent on MinGW) */
#include <fcntl.h>   /* _O_BINARY */
/* POSIX compat shims for MinGW */
#define mmap(a,l,p,f,fd,off) VirtualAlloc(NULL,(l),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)
#define munmap(a,l) (VirtualFree((a),0,MEM_RELEASE)?0:-1)
#define MAP_FAILED NULL
#define PROT_READ 0
#define PROT_WRITE 0
#define MAP_PRIVATE 0
#define MAP_ANONYMOUS 0
#define MAP_NORESERVE 0
#else
#include <ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#endif
#if !defined(__APPLE__) && !defined(_WIN32) && !defined(__FreeBSD__)
#include <malloc.h>
#else
/* Darwin's libc has no malloc_trim; make it a no-op so call sites stay portable. */
#define malloc_trim(x) ((void)0)
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

/* Core value-type definitions (primitives, leaf structs, GC headers,
   typed arrays, non-poly hashes) live in sp_types.h so libspinel_rt.a
   sources can share them without the function bodies below. */
#include "sp_types.h"
/* sp_RbVal, the collector globals/entry points, and the hot inline mark
   helpers; the collector body lives in libspinel_rt.a (lib/sp_gc.c). */
#include "sp_gc.h"
/* sp_Fiber + the Fiber API; the bodies live in libspinel_rt.a (lib/sp_fiber.c). */
#include "sp_fiber.h"
static const char *sp_sym_to_s(sp_sym id);

/* sp_raise_cls forward decl — defined later in this header (line ~1017).
   Used by the integer-division helpers below to match CRuby semantics:
   `a / 0`, `a % 0`, `a.divmod(0)`, `a.ceildiv(0)`, and `a.pow(e, 0)` all
   raise ZeroDivisionError instead of triggering C undefined behaviour
   (SIGFPE on x86) or silently returning 0. */
SP_NORETURN SP_COLD void sp_raise_cls(const char *cls, const char *msg);

static inline mrb_int sp_idiv(mrb_int a, mrb_int b) {
  if (b == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  mrb_int q = a / b; mrb_int r = a % b;
  if ((r != 0) && ((r ^ b) < 0)) q--;
  return q;
}
static inline mrb_int sp_imod(mrb_int a, mrb_int b) {
  if (b == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  mrb_int r = a % b;
  if ((r != 0) && ((r ^ b) < 0)) r += b;
  return r;
}
/* Overflow-checked integer arithmetic (BIGINT.md option β: raise on
   overflow, keep locals at native mrb_int width).

   Build modes (preprocessor toggles):
     default                 : overflow raises RangeError
     SP_NO_OVERFLOW_CHECK    : bare `+ - *`, silent wrap (UB on signed
                               overflow, but matches spinel's prior
                               behaviour where the user asks for
                               max speed and accepts the risk)

   The actual overflow check is wrapped through
   `sp_int_*_overflow_p` helpers so non-gcc/clang compilers fall back
   to a portable unsigned-arithmetic implementation. The shape
   mirrors mruby's mrb_int_*_overflow in include/mruby/numeric.h.

   The outer sp_int_add/sub/mul/neg are GCC statement-expression
   macros (`({ ... })`) rather than functions: an earlier inline-
   function variant produced wrong optcarrot output (checksum
   diverged 59662→4096) even with __attribute__((always_inline)),
   apparently from a UB-assumption interaction between the
   optimizer and the function-call-shaped expression that the
   macro form bypasses by keeping every operand in its surrounding
   expression context. The macros also let the compiler fold
   constant-operand cases at every call site. */
#ifndef __has_builtin
#  define __has_builtin(x) 0
#endif
#if (defined(__GNUC__) && __GNUC__ >= 5) || \
    (__has_builtin(__builtin_add_overflow) && \
     __has_builtin(__builtin_sub_overflow) && \
     __has_builtin(__builtin_mul_overflow))
#  define SP_HAVE_OVERFLOW_BUILTINS 1
#endif

#ifdef SP_HAVE_OVERFLOW_BUILTINS
static inline mrb_bool sp_int_add_overflow_p(mrb_int a, mrb_int b, mrb_int *r) {
  return __builtin_add_overflow(a, b, r);
}
static inline mrb_bool sp_int_sub_overflow_p(mrb_int a, mrb_int b, mrb_int *r) {
  return __builtin_sub_overflow(a, b, r);
}
static inline mrb_bool sp_int_mul_overflow_p(mrb_int a, mrb_int b, mrb_int *r) {
  return __builtin_mul_overflow(a, b, r);
}
#else
/* Portable fallback for compilers lacking __builtin_*_overflow.
   mrb_int is pointer-width (intptr_t), so compute in uintptr_t --
   unsigned overflow is well-defined wrap-around in C -- and detect
   signed overflow via the sign-bit XOR trick at the *correct* width
   (the sign bit is mrb_int's top bit: 63 on 64-bit, 31 on 32-bit).
   Bounds use INTPTR_MAX/MIN, not the int64 MRB_INT_* macros, so this
   path is self-contained and width-correct. Mul checks bounds before
   multiplying because a 2x-width intermediate isn't portable. */
#define SP_INT_OVF_SIGN ((uintptr_t)1 << (sizeof(mrb_int) * 8 - 1))
static inline mrb_bool sp_int_add_overflow_p(mrb_int a, mrb_int b, mrb_int *r) {
  uintptr_t x = (uintptr_t)a, y = (uintptr_t)b, z = x + y;
  *r = (mrb_int)z;
  return !!(((x ^ z) & (y ^ z)) & SP_INT_OVF_SIGN);
}
static inline mrb_bool sp_int_sub_overflow_p(mrb_int a, mrb_int b, mrb_int *r) {
  uintptr_t x = (uintptr_t)a, y = (uintptr_t)b, z = x - y;
  *r = (mrb_int)z;
  return !!(((x ^ z) & (~y ^ z)) & SP_INT_OVF_SIGN);
}
static inline mrb_bool sp_int_mul_overflow_p(mrb_int a, mrb_int b, mrb_int *r) {
  if (a > 0 && b > 0 && a > INTPTR_MAX / b) { *r = a * b; return TRUE; }
  if (a < 0 && b > 0 && a < INTPTR_MIN / b) { *r = a * b; return TRUE; }
  if (a > 0 && b < 0 && b < INTPTR_MIN / a) { *r = a * b; return TRUE; }
  if (a < 0 && b < 0 && (a <= INTPTR_MIN || b <= INTPTR_MIN || -a > INTPTR_MAX / -b)) {
    *r = a * b; return TRUE;
  }
  *r = a * b;
  return FALSE;
}
#undef SP_INT_OVF_SIGN
#endif

/* Three modes selected by `--int-overflow=raise|wrap|promote` on the
   spinel wrapper, which passes -DSP_INT_OVERFLOW_MODE_{RAISE,WRAP,
   PROMOTE}. WRAP skips the check entirely. PROMOTE still raises at
   this layer -- the actual promotion semantics is implemented in the
   analyzer by rewriting every int local as bigint, so the helpers
   below are only reached on the few residual sites that stay int
   even in promote mode (FFI argument coercion, etc.). */
#ifdef SP_INT_OVERFLOW_MODE_WRAP
#  define sp_int_add(a, b) ((a) + (b))
#  define sp_int_sub(a, b) ((a) - (b))
#  define sp_int_mul(a, b) ((a) * (b))
#  define sp_int_neg(a)    (-(a))
#else
#  define sp_int_add(a, b) ({ mrb_int _sp_a = (a), _sp_b = (b), _sp_r; \
    if (sp_int_add_overflow_p(_sp_a, _sp_b, &_sp_r)) sp_raise_cls("RangeError", "integer overflow in +"); \
    _sp_r; })
#  define sp_int_sub(a, b) ({ mrb_int _sp_a = (a), _sp_b = (b), _sp_r; \
    if (sp_int_sub_overflow_p(_sp_a, _sp_b, &_sp_r)) sp_raise_cls("RangeError", "integer overflow in -"); \
    _sp_r; })
#  define sp_int_mul(a, b) ({ mrb_int _sp_a = (a), _sp_b = (b), _sp_r; \
    if (sp_int_mul_overflow_p(_sp_a, _sp_b, &_sp_r)) sp_raise_cls("RangeError", "integer overflow in *"); \
    _sp_r; })
#  define sp_int_neg(a)    ({ mrb_int _sp_a = (a), _sp_r; \
    if (sp_int_sub_overflow_p((mrb_int)0, _sp_a, &_sp_r)) sp_raise_cls("RangeError", "integer overflow in -@"); \
    _sp_r; })
#endif

/* sp_gcd / sp_lcm / sp_powmod / sp_ceildiv / sp_int_clamp / sp_int_sqrt
   now live in libspinel_rt.a (lib/sp_core.c); declared via sp_core.h. */
static inline char *sp_str_alloc_raw(size_t total_with_null);  /* fwd decl */
static const char*sp_int_chr(mrb_int n){char*s=sp_str_alloc_raw(2);s[0]=(char)n;s[1]=0;return s;}
/* sp_ipow10 / sp_int_round / sp_int_ceil / sp_int_floor /
   sp_int_truncate / sp_str_oct now live in libspinel_rt.a
   (lib/sp_core.c); declared via sp_core.h. */
/* Narrow a foreign 64-bit value (time_t / off_t, etc.) to a Ruby
   Integer. On 64-bit mrb_int this is the identity. On 32-bit, a value
   outside the mrb_int range can't be represented; rather than silently
   truncating a clock/size value the program never computed (which
   `int-overflow=wrap` is NOT meant to license -- wrap is about the
   user's own arithmetic), raise RangeError. promote-mode bigint
   promotion of these boundary values is a follow-up. */
static inline mrb_int sp_i64_to_int(int64_t v){
#if INTPTR_MAX == INT64_MAX
  return (mrb_int)v;
#else
  if(v < (int64_t)INTPTR_MIN || v > (int64_t)INTPTR_MAX)
    sp_raise_cls("RangeError","value out of range for 32-bit Integer");
  return (mrb_int)v;
#endif
}

/* Forward decls for helpers used across this header (and by the
   string->number parsers that now live in libspinel_rt.a). */
SP_NORETURN SP_COLD void sp_raise_cls(const char *cls, const char *msg);
const char *sp_sprintf(const char *fmt, ...);

/* String -> number parsers now live in libspinel_rt.a (lib/sp_core.c). */
#include "sp_core.h"
/* system()/backtick ($?) support now lives in libspinel_rt.a (lib/sp_system.c). */
#include "sp_system.h"

static sp_Range sp_range_new(mrb_int f,mrb_int l,mrb_int e){sp_Range r;r.first=f;r.last=l;r.excl=e;return r;}
static mrb_bool sp_range_eq(sp_Range a,sp_Range b){return a.first==b.first&&a.last==b.last&&a.excl==b.excl;}
/* `Range#include?`/`#cover?` on the boxed (SP_TAG_OBJ cls_id
   SP_BUILTIN_RANGE) Range value. The direct sp_Range typed path
   inlines this same check via compile_range_method_expr; poly-recv
   dispatch needs the wrapper so the cls_id arm in
   emit_poly_builtin_dispatch can land on a single C expression. An
   exclusive range stops one short of `last`, so the upper bound is
   `last - excl` (excl is 0 or 1). */
static mrb_bool sp_range_include(sp_Range *r, mrb_int x){return r->first<=x && x<=r->last-r->excl;}

/* ---- Class object ----
   Value-type Class reference: a single class id that indexes into
   the per-program sp_class_names[] table emitted by codegen. Lets
   `c = Foo` produce a runtime value (`(sp_Class){<id>}`) instead of
   a bare C identifier, and `c.to_s` lower to a names-table lookup.
   Other Class methods (`.name`, `.inspect`, `.==`, `.!=`,
   `.superclass`, `.ancestors`, dynamic `is_a?(c)` against a
   variable, etc.) are not yet supported. */

/* ---- Complex runtime ---- */
/* Value-type Cartesian Complex: 16 bytes, passed by value. Used by
   optcarrot's nestopia palette generator; the palette is precomputed
   in the default code path so this is exercised only with
   `--nestopia-palette`. */
static inline sp_Complex sp_complex_polar(mrb_float m,mrb_float a){sp_Complex c;c.re=m*cos(a);c.im=m*sin(a);return c;}
static inline sp_Complex sp_complex_add(sp_Complex a,sp_Complex b){sp_Complex c;c.re=a.re+b.re;c.im=a.im+b.im;return c;}
static inline sp_Complex sp_complex_mul(sp_Complex a,sp_Complex b){sp_Complex c;c.re=a.re*b.re-a.im*b.im;c.im=a.re*b.im+a.im*b.re;return c;}
static inline sp_Complex sp_complex_conjugate(sp_Complex a){sp_Complex c;c.re=a.re;c.im=-a.im;return c;}
/* Inspect renders Complex per Ruby: `(re+imi)` or `(re-imi)` for
   negative imaginary. Integer-valued components render without
   decimals; fractional render via %g. Issue #840. */
static const char *sp_complex_inspect(sp_Complex c) {
  char buf[128];
  int n = 0;
  /* Real part: keep integer-looking values short. */
  if (c.re == (mrb_int)c.re) n += snprintf(buf + n, sizeof(buf) - n, "(%lld", (long long)c.re);
  else n += snprintf(buf + n, sizeof(buf) - n, "(%g", c.re);
  /* Imaginary sign + value. */
  if (c.im < 0) {
    if (c.im == (mrb_int)c.im) n += snprintf(buf + n, sizeof(buf) - n, "-%lldi)", -(long long)c.im);
    else n += snprintf(buf + n, sizeof(buf) - n, "%gi)", c.im);
  } else {
    if (c.im == (mrb_int)c.im) n += snprintf(buf + n, sizeof(buf) - n, "+%lldi)", (long long)c.im);
    else n += snprintf(buf + n, sizeof(buf) - n, "+%gi)", c.im);
  }
  if (n < 0) n = 0;
  char *r = sp_str_alloc_raw(n + 1);
  memcpy(r, buf, n);
  r[n] = 0;
  return r;
}

/* ---- Rational runtime ---- */
/* Value-type Rational: 16 bytes (two mrb_ints), passed by value.
   Stored in reduced form -- the parser hands us the already-reduced
   numerator/denominator from the literal; Integer#quo / arithmetic
   normalizes via sp_rational_reduce. Issue #841. */
static inline mrb_int sp_rational_gcd_i(mrb_int a, mrb_int b) {
  if (a < 0) a = -a;
  if (b < 0) b = -b;
  while (b) { mrb_int t = b; b = a % b; a = t; }
  return a;
}
static inline sp_Rational sp_rational_new(mrb_int n, mrb_int d) {
  sp_Rational r;
  if (d == 0) { r.num = n; r.den = 0; return r; }
  if (d < 0) { n = -n; d = -d; }
  mrb_int g = sp_rational_gcd_i(n, d);
  if (g <= 0) g = 1;
  r.num = n / g;
  r.den = d / g;
  return r;
}
static const char *sp_rational_inspect(sp_Rational r) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "(%lld/%lld)", (long long)r.num, (long long)r.den);
  if (n < 0) n = 0;
  char *o = sp_str_alloc_raw(n + 1);
  memcpy(o, buf, n);
  o[n] = 0;
  return o;
}

/* ---- Time runtime ---- */
/* sp_Time and the libc-backed accessors / formatters live in
   lib/sp_time.{c,h} (compiled into libspinel_rt.a). What stays here
   are the GC-aware wrappers — sp_box_time copies the value onto the
   GC heap, and the *_gc string forwarders allocate a small stack buf,
   call the libspinel_rt format helper, then sp_str_dup_external the
   result into the GC string heap. is_utc distinguishes UTC-coerced
   times from local-zone times; the underlying epoch is the same. */
#include "sp_time.h"


/* `recycle`: optional sweep hook. If non-NULL, sp_gc_collect calls
   recycle(h) on the unmarked object instead of finalize+free. The
   hook is responsible for deciding whether to keep the storage
   (pool push) or free it. Used by class-instance free-list pools. */
/* sp_gc_heap / sp_gc_bytes are defined in lib/sp_gc.c (extern via sp_gc.h). */
static size_t sp_gc_threshold = 256*1024;

/* ---- GC verify: opt-in mark-path validation (release-neutral) ------------
 * SPINEL_GC_VERIFY=1  before the collector invokes an object's scan hook,
 *   check that the object is a currently-registered heap allocation. If a
 *   raw/aliased pointer (e.g. into a string or builder buffer) has been put
 *   on the mark path, abort with a diagnostic at that point instead of
 *   calling through a bogus scan-function pointer (which otherwise faults at
 *   a garbage address and is near-impossible to attribute).
 * Default OFF; with it unset, behavior and codegen are unchanged. */
/* GC verify state + helpers live in lib/sp_gc.c. */

/* ---- String GC ---- */
static sp_str_hdr *sp_str_heap = NULL;
#define SPL(s) (&("\xff" s)[1])
static const char sp_str_empty_data[] = "\xff";
#define sp_str_empty (sp_str_empty_data + 1)

/* RUBY_PLATFORM string -- host arch + OS. Detected at C compile time
   so cross-builds report the target platform. Issue #890. */
#if defined(__x86_64__) || defined(_M_X64)
#  define SP_RUBY_ARCH "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define SP_RUBY_ARCH "aarch64"
#elif defined(__i386__) || defined(_M_IX86)
#  define SP_RUBY_ARCH "i686"
#elif defined(__arm__)
#  define SP_RUBY_ARCH "arm"
#else
#  define SP_RUBY_ARCH "unknown"
#endif
#if defined(__linux__)
#  define SP_RUBY_OS "linux"
#elif defined(__APPLE__)
#  define SP_RUBY_OS "darwin"
#elif defined(_WIN32) || defined(_WIN64)
#  define SP_RUBY_OS "mingw32"
#elif defined(__FreeBSD__)
#  define SP_RUBY_OS "freebsd"
#else
#  define SP_RUBY_OS "unknown"
#endif
static const char sp_ruby_platform_data[] = "\xff" SP_RUBY_ARCH "-" SP_RUBY_OS;
static inline const char *sp_ruby_platform_str(void) { return sp_ruby_platform_data + 1; }

/* Process.ppid wrapper. MinGW's unistd.h doesn't expose getppid;
   return 0 there. Issue #893. */
static inline mrb_int sp_process_ppid(void) {
#ifdef _WIN32
  return 0;
#else
  return (mrb_int)getppid();
#endif
}

/* ---- Encoding runtime ----
   Spinel currently assumes UTF-8 source and string data. Keep Encoding
   as a tiny value type so `__ENCODING__` and `String#encoding` can
   answer Ruby's Encoding-shaped protocol without carrying full
   transcoding state. */
static inline sp_Encoding sp_encoding_utf8(void){return(sp_Encoding){"UTF-8"};}
static inline const char*sp_encoding_name(sp_Encoding e){return e.name?e.name:sp_str_empty;}
static inline const char*sp_encoding_inspect(sp_Encoding e){return sp_sprintf("#<Encoding:%s>",sp_encoding_name(e));}
static inline mrb_bool sp_encoding_eq(sp_Encoding a,sp_Encoding b){const char*an=sp_encoding_name(a);const char*bn=sp_encoding_name(b);return strcmp(an,bn)==0;}

static char *sp_str_alloc(size_t len) {
  size_t total = sizeof(sp_str_hdr) + 1 + len + 1;
  sp_str_hdr *h = (sp_str_hdr *)malloc(total);
  if (!h) sp_oom_die();
  h->next = sp_str_heap;
  h->size = total;
  h->len = len;
  sp_str_heap = h;
  /* Don't fold string-heap pressure into sp_gc_bytes : the
     threshold heuristic in sp_gc_alloc is keyed on heap survivors, and
     the str-heap mark-sweep that runs alongside (sp_str_sweep,
     called from sp_gc_collect) doesn't add surviving strings back into
     sp_gc_bytes. Each string alloc increments sp_gc_bytes by its full
     size, but a sweep that reaps a string subtracts its size — and a
     surviving string's size isn't re-added on the way out of
     sp_gc_collect. Net effect on a workload that mixes heap allocs
     with frequent small string allocs (e.g. `puts <float>` in a tight
     loop): sp_gc_bytes drifts low after each collect, freed/before
     looks artificially large, the threshold-recompute branch in
     sp_gc_alloc takes the `sp_gc_bytes*4` path with a too-small base,
     and the GC starts firing on every allocation. Strings are still
     reaped via sp_str_sweep on every gc cycle, so dropping the
     accounting only removes the threshold noise. */
  char *body = (char *)(h + 1);
  body[0] = (char)0xfe;
  body[1 + len] = 0;
  return body + 1;
}

static inline char *sp_str_alloc_raw(size_t total_with_null) {
  return sp_str_alloc(total_with_null > 0 ? total_with_null - 1 : 0);
}

static inline size_t sp_str_byte_len(const char *s) {
  if (!s) return 0;
  unsigned char marker = ((const unsigned char *)s)[-1];
  if (marker == 0xfe || marker == 0xfc) {
    return (((const sp_str_hdr *)(s - 1)) - 1)->len;
  }
  return strlen(s);
}

static inline void sp_str_set_len(char *s, size_t len) {
  if (!s) return;
  unsigned char marker = ((unsigned char *)s)[-1];
  if (marker == 0xfe || marker == 0xfc) {
    (((sp_str_hdr *)(s - 1)) - 1)->len = len;
  }
}

static const char *sp_str_dup_external(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s);
  char *r = sp_str_alloc(n);
  memcpy(r, s, n);
  return r;
}

/* ---- UTF-8 helpers (used throughout the string runtime below) ---- */
static inline int sp_utf8_char_len(unsigned char c){if(c<0x80)return 1;if(c<0xC0)return 1;if(c<0xE0)return 2;if(c<0xF0)return 3;return 4;}
/* Bytes to advance past the codepoint at p (caller guarantees *p != 0).
   Caps at NUL and validates the continuation-byte pattern, so malformed or
   truncated UTF-8 never advances past the terminator. */
static inline int sp_utf8_advance(const char*p){int cn=sp_utf8_char_len((unsigned char)*p);int i=1;while(i<cn&&((unsigned char)p[i]&0xC0)==0x80)i++;return i;}
static inline int sp_utf8_decode(const char*p,uint32_t*out){unsigned char c=(unsigned char)p[0];if(c<0x80){*out=c;return 1;}if(c<0xC0){*out=c;return 1;}unsigned char c1=(unsigned char)p[1];if((c1&0xC0)!=0x80){*out=c;return 1;}if(c<0xE0){*out=((uint32_t)(c&0x1F)<<6)|(c1&0x3F);return 2;}unsigned char c2=(unsigned char)p[2];if((c2&0xC0)!=0x80){*out=c;return 1;}if(c<0xF0){*out=((uint32_t)(c&0x0F)<<12)|((uint32_t)(c1&0x3F)<<6)|(c2&0x3F);return 3;}unsigned char c3=(unsigned char)p[3];if((c3&0xC0)!=0x80){*out=c;return 1;}*out=((uint32_t)(c&0x07)<<18)|((uint32_t)(c1&0x3F)<<12)|((uint32_t)(c2&0x3F)<<6)|(c3&0x3F);return 4;}
static inline int sp_utf8_encode(uint32_t cp,char*out){if(cp<0x80){out[0]=(char)cp;return 1;}if(cp<0x800){out[0]=(char)(0xC0|(cp>>6));out[1]=(char)(0x80|(cp&0x3F));return 2;}if(cp<0x10000){out[0]=(char)(0xE0|(cp>>12));out[1]=(char)(0x80|((cp>>6)&0x3F));out[2]=(char)(0x80|(cp&0x3F));return 3;}out[0]=(char)(0xF0|(cp>>18));out[1]=(char)(0x80|((cp>>12)&0x3F));out[2]=(char)(0x80|((cp>>6)&0x3F));out[3]=(char)(0x80|(cp&0x3F));return 4;}
/* Issue #882: `"hello" << 33` should append the character with
   that codepoint, not the decimal digits. UTF-8 encode (1..4 bytes)
   and return a NUL-terminated string. */
static const char *sp_int_codepoint_to_str(mrb_int n) {
  char *s = sp_str_alloc_raw(5);
  if (n < 0 || n > 0x10FFFF) { s[0] = 0; return s; }
  int len = sp_utf8_encode((uint32_t)n, s);
  s[len] = 0;
  return s;
}
/* Direct-mapped pointer-keyed cache for (byte_len, char_len). Populated lazily
   by sp_str_length / sp_utf8_byte_offset; the same entries unlock both calls
   so iterating a single string with `s.length` + `s[i]` walks UTF-8 once
   instead of per access. ASCII strings (char_len == byte_len) take an O(1)
   byte_offset fast path. Cleared by sp_str_sweep so freed-and-reused heap
   addresses can't leak stale entries. Skipped for sp_String wrappers (marker
   0xfd) whose buffers can move on realloc. */
#define SP_STR_LCACHE_BITS 5
#define SP_STR_LCACHE_SIZE (1u << SP_STR_LCACHE_BITS)
static struct sp_str_lcache_entry {
  const char *s;
  size_t byte_len;
  mrb_int char_len;
} sp_str_lcache[SP_STR_LCACHE_SIZE];

static inline unsigned sp_str_lcache_hash(const char *s) {
  uintptr_t k = (uintptr_t)s;
  return (unsigned)((k ^ (k >> 4) ^ (k >> 12)) & (SP_STR_LCACHE_SIZE - 1));
}

static inline void sp_str_lcache_clear(void) {
  for (unsigned i = 0; i < SP_STR_LCACHE_SIZE; i++) sp_str_lcache[i].s = NULL;
}

/* Count UTF-8 code points in s[0..bl). The 8-byte ASCII-detect prologue skips
   bytes 8 at a time while the high bit stays clear, so pure-ASCII strings (the
   common case in the JSON / CSV / template benchmarks) run vastly faster than
   the per-byte advance loop they used to fall through. */
static mrb_int sp_str_count_chars(const char *s, size_t bl) {
  const char *p = s, *end = s + bl;
  mrb_int n = 0;
  while (p + 8 <= end) {
    uint64_t w;
    memcpy(&w, p, sizeof(w));
    if (w & 0x8080808080808080ULL) break;
    p += 8; n += 8;
  }
  while (p < end) {
    if ((unsigned char)*p < 0x80) { p++; n++; }
    else { p += sp_utf8_advance(p); n++; }
  }
  return n;
}

/* True when `s` carries one of spinel's own string markers in the
   preceding byte (0xfe / 0xfc heap, 0xff rodata literal). FFI returns
   a bare `const char *` whose preceding byte is whatever C variable
   sits before the buffer in memory — using the pointer as a cache
   key without this gate aliased subsequent FFI calls into the prior
   call's cached length (#611). 0xfd (sp_String wrapper) is excluded
   too because its buffer can move on append. */
static inline int sp_str_cacheable(const char *s) {
  unsigned char m = ((const unsigned char *)s)[-1];
  return m == 0xfe || m == 0xfc || m == 0xff;
}

static mrb_int sp_str_length(const char*s){
  if (!s) return 0;
  if (!sp_str_cacheable(s)) return sp_str_count_chars(s, sp_str_byte_len(s));
  unsigned h = sp_str_lcache_hash(s);
  if (sp_str_lcache[h].s == s) return sp_str_lcache[h].char_len;
  size_t bl = sp_str_byte_len(s);
  mrb_int n = sp_str_count_chars(s, bl);
  sp_str_lcache[h].s = s;
  sp_str_lcache[h].byte_len = bl;
  sp_str_lcache[h].char_len = n;
  return n;
}
static mrb_int sp_str_ord(const char*s){if(!s)sp_raise_cls("ArgumentError","empty string");unsigned char m=((const unsigned char*)s)[-1];size_t blen;if(m==0xfe||m==0xfc){blen=(((const sp_str_hdr*)(s-1))-1)->len;if(blen==0)sp_raise_cls("ArgumentError","empty string");}else{blen=strlen(s);if(blen==0)sp_raise_cls("ArgumentError","empty string");}uint32_t cp;sp_utf8_decode(s,&cp);return(mrb_int)cp;}
/* NULL-safe string equality. ENV[] returns NULL for unset vars
   (the dispatch is `sp_str_dup_external(getenv(...))`, which propagates
   NULL), so emitted strcmp(...) on the result of `ENV["X"] == "1"` would
   dereference NULL on either side. nil-vs-string equality is false in
   Ruby; nil == nil is true, so falling back to pointer equality on the
   NULL path covers both. */
static inline int sp_str_eq(const char*a,const char*b){if(!a||!b)return a==b;return strcmp(a,b)==0;}
static size_t sp_utf8_byte_offset(const char*s,mrb_int char_idx){
  if (!s || char_idx <= 0) return 0;
  if (sp_str_cacheable(s)) {
    unsigned h = sp_str_lcache_hash(s);
    if (sp_str_lcache[h].s == s
        && (size_t)sp_str_lcache[h].char_len == sp_str_lcache[h].byte_len) {
      size_t off = (size_t)char_idx;
      return off > sp_str_lcache[h].byte_len ? sp_str_lcache[h].byte_len : off;
    }
  }
  /* Walk char_idx code points or stop at byte_len, whichever comes first.
     Bounding on byte_len (instead of "*p != 0") keeps the walk correct
     past an embedded NUL byte -- a heap string with the 0xfe/0xfc marker
     carries its real length in the sp_str_hdr, so a NUL byte inside the
     payload no longer terminates the walk prematurely. For an external
     0xff literal sp_str_byte_len falls back to strlen which legitimately
     stops at NUL, matching the prior behaviour. */
  size_t blen = sp_str_byte_len(s);
  const char *p = s;
  const char *end = s + blen;
  while (char_idx > 0 && p < end) {
    p += sp_utf8_advance(p);
    char_idx--;
  }
  if (p > end) p = end;
  return (size_t)(p - s);
}
/* Issue #762: check malloc/realloc returns. On OOM, return an empty
   array rather than dereferencing NULL. */
static uint32_t*sp_utf8_decode_all(const char*s,size_t*out_n){size_t cap=8,n=0;uint32_t*cps=(uint32_t*)malloc(cap*sizeof(uint32_t));if(!cps){*out_n=0;return NULL;}const char*p=s;while(s&&*p){if(n>=cap){size_t nc=cap*2;uint32_t*nx=(uint32_t*)realloc(cps,nc*sizeof(uint32_t));if(!nx){free(cps);*out_n=0;return NULL;}cps=nx;cap=nc;}uint32_t cp;p+=sp_utf8_decode(p,&cp);cps[n++]=cp;}*out_n=n;return cps;}

/* Issue #858: expand `a-z` range notation in a String#delete /
   String#tr / String#count character set. `^abc` negation is
   NOT handled (separate v1 scope). Result is a malloc'd flat
   codepoint array — caller frees. */
static uint32_t*sp_utf8_decode_charset(const char*s,size_t*out_n){
  size_t cap=16,n=0;
  uint32_t*cps=(uint32_t*)malloc(cap*sizeof(uint32_t));
  if(!cps){*out_n=0;return NULL;}
  const char*p=s;
  uint32_t prev=0; int has_prev=0;
  while(s&&*p){
    uint32_t cp;
    int len=sp_utf8_decode(p,&cp);
    p+=len;
    /* Detect range: prev '-' next  (but leading or trailing '-'
       is literal). When current char is '-' and there's a next
       non-'-' char and we have a prev, expand. */
    if(cp=='-' && has_prev && *p){
      uint32_t hi;
      int hi_len=sp_utf8_decode(p,&hi);
      p+=hi_len;
      if(hi>=prev){
        /* Drop the prev we already wrote, re-emit the whole range. */
        n--;  /* undo prev */
        for(uint32_t c=prev;c<=hi;c++){
          if(n>=cap){cap*=2;cps=(uint32_t*)realloc(cps,cap*sizeof(uint32_t));}
          cps[n++]=c;
        }
        has_prev=0;
        continue;
      }
      /* Bad range (hi<prev): fall through, push '-' literally. */
      cp='-';
    }
    if(n>=cap){cap*=2;cps=(uint32_t*)realloc(cps,cap*sizeof(uint32_t));}
    cps[n++]=cp;
    prev=cp; has_prev=1;
  }
  *out_n=n;
  return cps;
}
static int sp_utf8_set_has(const uint32_t*cps,size_t n,uint32_t cp){for(size_t i=0;i<n;i++)if(cps[i]==cp)return 1;return 0;}

/* sp_mark_string is an inline helper in sp_gc.h. */
static void sp_str_sweep(void) {
  sp_str_hdr **pp = &sp_str_heap;
  while (*pp) {
    sp_str_hdr *h = *pp;
    char *body = (char *)(h + 1);
    if ((unsigned char)body[0] == 0xfc) {
      body[0] = (char)0xfe;
      pp = &h->next;
    } else {
      *pp = h->next;
      /* String allocs no longer fold into sp_gc_bytes (see
         sp_str_alloc); the matching subtract here drops too. */
      free(h);
    }
  }
  sp_str_lcache_clear();
}

/* GC-aware Time trampolines. The libspinel_rt format helpers write
   into a local buffer; we sp_str_dup_external the result so the GC
   tracks the lifetime. strftime returns 0 -- never overruns the buffer
   -- when the formatted result would exceed it, which we surface as "".
   The 4 KB buffer covers any realistic format (CRuby's built-ins are
   ~25 bytes; this leaves room for long literal text or wide fields). A
   pathological field width (`"%1000000000F"`, which CRuby rejects with
   ERANGE) does not fit and yields "" -- a graceful empty string rather
   than a crash. */
static const char *sp_time_strftime(sp_Time t, const char *fmt) {
  char buf[4096];
  size_t n = sp_time_strftime_to(t, fmt, buf, sizeof(buf));
  if (n == 0) return SPL("");
  return sp_str_dup_external(buf);
}
static const char *sp_time_iso8601(sp_Time t) {
  char buf[64];
  size_t n = sp_time_iso8601_to(t, buf, sizeof(buf));
  if (n == 0) return SPL("");
  return sp_str_dup_external(buf);
}
static const char *sp_time_zone(sp_Time t) {
  char buf[8];
  sp_time_zone_to(t, buf, sizeof(buf));
  return sp_str_dup_external(buf);
}
/* Scalar Time inspect. CRuby form: local "YYYY-MM-DD HH:MM:SS +0900",
   UTC "YYYY-MM-DD HH:MM:SS UTC". The poly-box path keeps its own
   sp_Time_inspect; this value-taking variant is for the scalar
   p/puts/to_s codegen path. */
static const char *sp_time_inspect_v(sp_Time t) {
  char buf[40];
  sp_time_inspect_to(t, buf, sizeof(buf));
  return sp_str_dup_external(buf);
}

/* SP_GC_STACK_MAX, sp_gc_roots, sp_gc_nroots come from sp_gc.h / lib/sp_gc.c. */
/* Cooperative-fiber GC root storage (issue #636).
   sp_gc_roots[] holds the CURRENT fiber's active roots. When a fiber
   yields, its roots get copied out to the fiber's saved_roots buffer
   and the resuming fiber's saved_roots are copied back in — so the
   per-fiber stacks never clobber each other through interleaved
   pushes the way they did when a single global stack was shared by
   every fiber. sp_gc_mark_all calls the hook below (installed once
   the Fiber section's setup runs) to walk every live fiber's
   saved_roots in addition to the current view. */
/* GC root tracking. SP_GC_ROOT registers a stack-resident root with a
   cleanup-attribute sentinel so it's auto-popped when its declaring
   scope ends — matches the variable's actual lifetime, including for
   temporaries declared inside nested if/while/for blocks. The previous
   form (push to global array, paired with SP_GC_SAVE/RESTORE at
   function entry) leaked roots whose stack memory was reclaimed when
   inner blocks returned, which clang's stricter stack layout exposed
   as use-after-scope on inputs that nest scan_locals deeply (issue
   surfaced on test/block2.rb under clang-built spinel_codegen). */
static inline int _sp_gc_root_push(void **p) {
  if (sp_gc_nroots < SP_GC_STACK_MAX) { sp_gc_roots[sp_gc_nroots++] = p; return 1; }
  return 0;
}
static inline void _sp_gc_root_pop(int *added) { if (*added) sp_gc_nroots--; }
#define _SP_GC_CONCAT2(a,b) a##b
#define _SP_GC_CONCAT(a,b) _SP_GC_CONCAT2(a,b)
#define SP_GC_SAVE() int __attribute__((cleanup(sp_gc_cleanup))) _gc_saved = sp_gc_nroots
#define SP_GC_ROOT(v) int __attribute__((cleanup(_sp_gc_root_pop))) _SP_GC_CONCAT(_sp_gcr_, __COUNTER__) = _sp_gc_root_push((void**)&(v))
/* Root a poly (sp_RbVal) local/global. The root stack stores void**
   and marks *slot as a single GC pointer, but an sp_RbVal carries its
   object pointer inside a union (offset != 0) and only when its tag is
   STR/OBJ -- a blind *slot deref would read the tag word as a pointer.
   So we tag the stored slot with the low bit (sp_RbVal addresses are
   >=4-byte aligned, so the bit is free) and the mark walker, seeing the
   tag, routes the entry through sp_mark_rbval instead. Same auto-pop
   cleanup as SP_GC_ROOT, so SP_GC_SAVE/RESTORE and the fiber root
   save/restore (which treat entries as opaque) need no changes. */
#define SP_GC_ROOT_RBVAL(v) int __attribute__((cleanup(_sp_gc_root_pop))) _SP_GC_CONCAT(_sp_gcr_, __COUNTER__) = _sp_gc_root_push((void**)((uintptr_t)&(v) | (uintptr_t)1))
#define SP_GC_RESTORE() sp_gc_nroots = _gc_saved
#define SP_GC_MARK_STACK_MAX (1024*64)
/* sp_gc_mark / sp_gc_mark_all + the suspended-fiber hook live in
   lib/sp_gc.c (declared in sp_gc.h). sp_re_mark_globals is defined below
   (with the regex globals it marks) and carries external linkage so the
   collector body can reach it. */
static void sp_gc_cleanup(int*p){sp_gc_nroots=*p;}
#define SP_GC_NBUCKETS 32
static sp_gc_hdr*sp_gc_buckets[SP_GC_NBUCKETS];
static inline int sp_gc_bucket(size_t sz){int b=(int)(sz/16);return b<SP_GC_NBUCKETS?b:SP_GC_NBUCKETS-1;}
/* sp_gc_cycle / sp_gc_old_bytes are in lib/sp_gc.c (extern via sp_gc.h);
   sp_gc_old_heap is collector-private to lib/sp_gc.c. */

/* GC verify support + sp_gc_collect live in lib/sp_gc.c. */
static size_t sp_gc_threshold_init=256*1024;
/* sp_oom_die + the SPINEL_MAX_HEAP_MB governor (sp_gc_enforce_mem_limit)
   live in lib/sp_gc.c. */
void*sp_gc_alloc(size_t sz,void(*fin)(void*),void(*scn)(void*)){if(sp_gc_bytes>sp_gc_threshold){size_t before=sp_gc_bytes;sp_gc_collect();size_t freed=before-sp_gc_bytes;if(freed<before/4){sp_gc_threshold=before*2;}else if(sp_gc_bytes>0){sp_gc_threshold=sp_gc_bytes*4;if(sp_gc_threshold<sp_gc_threshold_init)sp_gc_threshold=sp_gc_threshold_init;}else{sp_gc_threshold=sp_gc_threshold_init;}sp_gc_enforce_mem_limit();}size_t need=sizeof(sp_gc_hdr)+sz;sp_gc_hdr*h=(sp_gc_hdr*)calloc(1,need);if(!h)sp_oom_die();h->finalize=fin;h->scan=scn;h->size=need;h->marked=0;h->next=sp_gc_heap;sp_gc_heap=h;sp_gc_bytes+=need;return(char*)h+sizeof(sp_gc_hdr);}
void*sp_gc_alloc_nogc(size_t sz,void(*fin)(void*),void(*scn)(void*)){size_t need=sizeof(sp_gc_hdr)+sz;sp_gc_hdr*h=(sp_gc_hdr*)calloc(1,need);if(!h)sp_oom_die();h->finalize=fin;h->scan=scn;h->size=need;h->marked=0;h->next=sp_gc_heap;sp_gc_heap=h;sp_gc_bytes+=need;return(char*)h+sizeof(sp_gc_hdr);}
/* GC-header frozen bit — used for containers whose mutators are NOT
   on a hot path (hashes), so the extra cache line vs. a struct field
   doesn't matter. Arrays co-locate `frozen` in the struct instead
   (see sp_IntArray); strings use the 0xff marker / wrapper bit. */
static inline mrb_bool sp_gc_is_frozen(void *p) { if (!p) return FALSE; return ((sp_gc_hdr *)((char *)p - sizeof(sp_gc_hdr)))->frozen; }
static inline void *sp_gc_freeze(void *p) { if (p) ((sp_gc_hdr *)((char *)p - sizeof(sp_gc_hdr)))->frozen = 1; return p; }
static void __attribute__((noinline,cold)) sp_raise_frozen_hash(void){sp_raise_cls("FrozenError","can't modify frozen Hash");}
/* Pool-aware alloc. The recycle hook is stored in the gc_hdr; sweep
   calls it on unmarked objects instead of finalize+free. The hook
   decides whether to push the storage onto a per-class free-list or
   actually free it. */
static void *sp_gc_alloc_pool(size_t sz, void(*scn)(void*), void(*recycle)(sp_gc_hdr*)) {
  void *p = sp_gc_alloc(sz, NULL, scn);
  sp_gc_hdr *h = (sp_gc_hdr *)((char *)p - sizeof(sp_gc_hdr));
  h->recycle = recycle;
  return p;
}
/* Re-link a previously-pooled slot back into sp_gc_heap so the next
   GC cycle visits it. Called by class _new functions when reusing
   a pooled instance. The storage was kept alive by the pool
   free-list since the last sweep unlinked it from sp_gc_heap. */
static void sp_gc_pool_relink(sp_gc_hdr *h) {
  h->marked = 0;
  h->next = sp_gc_heap;
  sp_gc_heap = h;
  sp_gc_bytes += h->size;
}

/* Per-class free-list pool boilerplate. SP_POOL_DEFINE(CLS) goes at
   file scope, near the class _new function. SP_POOL_NEW(CLS, scan)
   replaces the body of an `sp_gc_alloc(sizeof(sp_CLS), NULL, scan)`
   call, popping from the per-class free-list if non-empty.
   Default cap can be overridden at runtime via SP_POOL_MAX envvar
   (uniform across classes). SP_POOL_REPORT=1 dumps per-class stats
   at exit. */
#define SP_POOL_DEFAULT_MAX 1048576L
#define SP_POOL_DEFINE(CLS) \
  static sp_gc_hdr *sp_##CLS##_pool_head = NULL; \
  static long sp_##CLS##_pool_count = 0; \
  static long sp_##CLS##_pool_max = SP_POOL_DEFAULT_MAX; \
  static long sp_##CLS##_pool_pushes = 0; \
  static long sp_##CLS##_pool_pops = 0; \
  static long sp_##CLS##_pool_freed = 0; \
  static long sp_##CLS##_pool_hwm = 0; \
  __attribute__((constructor)) static void sp_##CLS##_pool_init(void) { \
    const char *m = getenv("SP_POOL_MAX"); \
    if (m && *m) { long v = atol(m); if (v >= 0) sp_##CLS##_pool_max = v; } \
  } \
  static void sp_##CLS##_pool_recycle(sp_gc_hdr *h) { \
    if (sp_##CLS##_pool_count >= sp_##CLS##_pool_max) { \
      free(h); sp_##CLS##_pool_freed++; return; \
    } \
    h->next = sp_##CLS##_pool_head; \
    sp_##CLS##_pool_head = h; \
    sp_##CLS##_pool_count++; \
    sp_##CLS##_pool_pushes++; \
    if (sp_##CLS##_pool_count > sp_##CLS##_pool_hwm) sp_##CLS##_pool_hwm = sp_##CLS##_pool_count; \
  } \
  __attribute__((destructor)) static void sp_##CLS##_pool_report(void) { \
    const char *e = getenv("SP_POOL_REPORT"); \
    if (!e || !e[0] || e[0] == '0') return; \
    fprintf(stderr, #CLS " pool: pops=%ld pushes=%ld over_cap_freed=%ld hwm=%ld retained=%ld cap=%ld\n", \
      sp_##CLS##_pool_pops, sp_##CLS##_pool_pushes, sp_##CLS##_pool_freed, \
      sp_##CLS##_pool_hwm, sp_##CLS##_pool_count, sp_##CLS##_pool_max); \
  }

#define SP_POOL_NEW(CLS, SCAN) (__extension__ ({ \
  sp_##CLS *_p; \
  if (sp_##CLS##_pool_head) { \
    sp_gc_hdr *_h = sp_##CLS##_pool_head; \
    sp_##CLS##_pool_head = _h->next; \
    sp_##CLS##_pool_count--; \
    sp_##CLS##_pool_pops++; \
    sp_gc_pool_relink(_h); \
    _h->recycle = sp_##CLS##_pool_recycle; \
    _p = (sp_##CLS *)((char *)_h + sizeof(sp_gc_hdr)); \
  } else { \
    _p = (sp_##CLS *)sp_gc_alloc_pool(sizeof(sp_##CLS), SCAN, sp_##CLS##_pool_recycle); \
  } \
  _p; \
}))

/* `Object.new` — a sentinel object whose only meaningful property is
   identity. Each call returns a fresh GC-managed allocation, so two
   `Object.new` results compare as `!=` via their pointer addresses. */
typedef struct sp_Object_s { uint8_t _pad; } sp_Object;
static sp_Object *sp_Object_new(void){return(sp_Object*)sp_gc_alloc(sizeof(sp_Object),NULL,NULL);}

/* Cold out-of-line raise for frozen-container mutation; keeps the
   hot mutators (push / []=) small enough to stay inlined. */
static void __attribute__((noinline,cold)) sp_raise_frozen_array(void){sp_raise_cls("FrozenError","can't modify frozen Array");}

/* `frozen` rides in the struct (not the GC header) so the hot push /
   []= paths read it from the same cache line as len/cap — no extra
   cache miss vs. the GC-header bit. calloc in sp_gc_alloc zero-inits
   it, so constructors need no change. Issue #918. */
static void sp_IntArray_fin(void*p){free(((sp_IntArray*)p)->data);}
static sp_IntArray*sp_IntArray_new(void){sp_IntArray*a=(sp_IntArray*)sp_gc_alloc(sizeof(sp_IntArray),sp_IntArray_fin,NULL);a->cap=16;a->data=(mrb_int*)malloc(sizeof(mrb_int)*a->cap);if(!a->data)sp_oom_die();a->start=0;a->len=0;{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}return a;}
/* Issue #799: clamp e-s+1 against size_t overflow + an arbitrary
   sanity cap (1 << 30 elements; ~8 GB at 8 bytes/elem). Without
   the cap, `(1..MRB_INT_MAX).to_a` overflows the realloc size_t
   to a tiny number, then writes past the allocation. */
static sp_IntArray*sp_IntArray_from_range(mrb_int s,mrb_int e){sp_IntArray*a=sp_IntArray_new();mrb_int n=e-s+1;if(n<0)n=0;if(n>(mrb_int)(1LL<<30))n=(mrb_int)(1LL<<30);if(n>a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*a->cap;h->size-=sizeof(mrb_int)*a->cap;a->cap=n;a->data=(mrb_int*)realloc(a->data,sizeof(mrb_int)*a->cap);h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}for(mrb_int i=0;i<n;i++)a->data[i]=s+i;a->len=n;return a;}
/* `(a..b).step(k)` -- emit a, a+k, a+2k, ... up to b. k must be > 0;
   k <= 0 returns empty (matches CRuby's ArgumentError for k <= 0 in
   spirit; we soft-fail instead). Forward-declared since
   sp_IntArray_push is defined below. Issue #731. */
static void sp_IntArray_push(sp_IntArray*a,mrb_int v);
static sp_IntArray*sp_IntArray_from_range_step(mrb_int s,mrb_int e,mrb_int k){sp_IntArray*a=sp_IntArray_new();if(k<=0)return a;mrb_int v=s;while(v<=e){sp_IntArray_push(a,v);v+=k;}return a;}
static sp_IntArray*sp_IntArray_dup(sp_IntArray*a){sp_IntArray*b=sp_IntArray_new();if(a->len>b->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)b-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*b->cap;h->size-=sizeof(mrb_int)*b->cap;b->cap=a->len;b->data=(mrb_int*)realloc(b->data,sizeof(mrb_int)*b->cap);h->size+=sizeof(mrb_int)*b->cap;sp_gc_bytes+=sizeof(mrb_int)*b->cap;}memcpy(b->data,a->data+a->start,sizeof(mrb_int)*a->len);b->len=a->len;return b;}
/* a[start, len] / a[start..end] for IntArray. Negative start counts from
 * the end. start past the array length yields an empty result; len is
 * clamped so we never read past the source. CRuby returns nil for
 * out-of-bounds start; we return an empty IntArray since this typed
 * collection has no nullable form. */
static sp_IntArray*sp_IntArray_slice(sp_IntArray*a,mrb_int start,mrb_int len){if(start<0)start+=a->len;if(start<0)start=0;sp_IntArray*b=sp_IntArray_new();if(start>=a->len||len<=0)return b;if(start+len>a->len)len=a->len-start;if(len>b->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)b-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*b->cap;h->size-=sizeof(mrb_int)*b->cap;b->cap=len;b->data=(mrb_int*)realloc(b->data,sizeof(mrb_int)*b->cap);h->size+=sizeof(mrb_int)*b->cap;sp_gc_bytes+=sizeof(mrb_int)*b->cap;}memcpy(b->data,a->data+a->start+start,sizeof(mrb_int)*len);b->len=len;return b;}
/* a[start..end] / a[start...end] with possibly negative endpoints.
   Codegen used to compute (right - left + adj) for the length, which
   silently produced a negative count for `a[1..-2]` and the runtime
   then returned an empty array (issue #496). Normalize end against
   a->len first; the bare _slice already handles negative start. */
static sp_IntArray*sp_IntArray_slice_range(sp_IntArray*a,mrb_int start,mrb_int end_,mrb_int excl){if(end_<0)end_+=a->len;if(start<0)start+=a->len;mrb_int n=end_-start+(excl?0:1);if(n<0||start<0)n=0;return sp_IntArray_slice(a,start,n);}
static void sp_IntArray_replace(sp_IntArray*dst,sp_IntArray*src){dst->len=0;dst->start=0;if(src->len>dst->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)dst-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*dst->cap;h->size-=sizeof(mrb_int)*dst->cap;void*nd=realloc(dst->data,sizeof(mrb_int)*src->len);if(!nd){perror("realloc");exit(1);}dst->data=(mrb_int*)nd;dst->cap=src->len;h->size+=sizeof(mrb_int)*dst->cap;sp_gc_bytes+=sizeof(mrb_int)*dst->cap;}memcpy(dst->data,src->data+src->start,sizeof(mrb_int)*src->len);dst->len=src->len;}
static void __attribute__((noinline)) sp_IntArray_push_grow(sp_IntArray*a){if(a->start>0){memmove(a->data,a->data+a->start,sizeof(mrb_int)*a->len);a->start=0;if(a->len<a->cap)return;}{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*a->cap;h->size-=sizeof(mrb_int)*a->cap;a->cap=a->cap*2+1;void*nd=realloc(a->data,sizeof(mrb_int)*a->cap);if(!nd)sp_oom_die();a->data=(mrb_int*)nd;h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}}
static inline void sp_IntArray_push(sp_IntArray*a,mrb_int v){if(a->frozen){sp_raise_frozen_array();return;}if(a->start+a->len>=a->cap)sp_IntArray_push_grow(a);a->data[a->start+a->len]=v;a->len++;}
/* Issue #826: guard empty arrays. CRuby returns nil; spinel's int slot
   collapses nil to 0. Without the guard, `--a->len` wraps to -1 and
   reads past the buffer start. */
/* Issue #832: empty pop/shift return SP_INT_NIL (nullable int sentinel)
   to match MRI's nil semantics; callers must treat as int? */
static inline mrb_int sp_IntArray_pop(sp_IntArray*a){if(!a||a->len<=0)return SP_INT_NIL;if(a->frozen){sp_raise_frozen_array();return SP_INT_NIL;}return a->data[a->start+--a->len];}
static inline mrb_int sp_IntArray_shift(sp_IntArray*a){if(!a||a->len<=0)return SP_INT_NIL;if(a->frozen){sp_raise_frozen_array();return SP_INT_NIL;}mrb_int v=a->data[a->start];a->start++;a->len--;return v;}
static inline mrb_int sp_IntArray_length(sp_IntArray*a){return a->len;}
static inline mrb_bool sp_IntArray_empty(sp_IntArray*a){return a->len==0;}
static inline mrb_int sp_IntArray_get(sp_IntArray*a,mrb_int i){if(!a)return SP_INT_NIL;if(i<0)i+=a->len;if(i<0||i>=a->len)return SP_INT_NIL;return a->data[a->start+i];}
/* Issue #769: a very-negative i (e.g. `a[-999] = 42` on a 3-elt
   array) leaves i negative after the `i += a->len` adjustment.
   CRuby raises IndexError; spinel no-ops as the safest fallback
   (raising from a typed-array set would need setjmp plumbing
   throughout the call chain). */
static void sp_IntArray_set_slow(sp_IntArray*a,mrb_int i,mrb_int v){if(i<0)return;while(a->start+i>=a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*a->cap;h->size-=sizeof(mrb_int)*a->cap;a->cap=a->cap*2+1;a->data=(mrb_int*)realloc(a->data,sizeof(mrb_int)*a->cap);h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}while(i>=a->len){a->data[a->start+a->len]=0;a->len++;}a->data[a->start+i]=v;}
/* Issue #839: an extreme negative index (still negative after
   `i += len`) raises IndexError per MRI. */
static inline void sp_IntArray_set(sp_IntArray*a,mrb_int i,mrb_int v){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}mrb_int orig=i;if(i<0)i+=a->len;if(i<0)sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)-a->len));if(i<a->len){a->data[a->start+i]=v;return;}sp_IntArray_set_slow(a,i,v);}
static void sp_IntArray_reverse_bang(sp_IntArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}for(mrb_int i=0,j=a->len-1;i<j;i++,j--){mrb_int t=a->data[a->start+i];a->data[a->start+i]=a->data[a->start+j];a->data[a->start+j]=t;}}
static void sp_IntArray_rotate_bang(sp_IntArray*a,mrb_int n){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}if(a->len<=0)return;n=((n%a->len)+a->len)%a->len;if(n==0)return;mrb_int*d=a->data+a->start;mrb_int lo=0,hi=n-1;while(lo<hi){mrb_int t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=n;hi=a->len-1;while(lo<hi){mrb_int t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=0;hi=a->len-1;while(lo<hi){mrb_int t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}}
static int _sp_int_cmp(const void*a,const void*b){mrb_int va=*(const mrb_int*)a,vb=*(const mrb_int*)b;return(va>vb)-(va<vb);}
static sp_IntArray*sp_IntArray_sort(sp_IntArray*a){sp_IntArray*b=sp_IntArray_dup(a);qsort(b->data+b->start,b->len,sizeof(mrb_int),_sp_int_cmp);return b;}
static void sp_IntArray_sort_bang(sp_IntArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}qsort(a->data+a->start,a->len,sizeof(mrb_int),_sp_int_cmp);}
static void sp_IntArray_shuffle_bang(sp_IntArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}for(mrb_int i=a->len-1;i>0;i--){mrb_int j=(mrb_int)(rand()%(i+1));mrb_int t=a->data[a->start+i];a->data[a->start+i]=a->data[a->start+j];a->data[a->start+j]=t;}}
static sp_IntArray*sp_IntArray_shuffle(sp_IntArray*a){sp_IntArray*b=sp_IntArray_dup(a);sp_IntArray_shuffle_bang(b);return b;}
/* Array#sample helpers. CRuby returns nil for `[].sample`; in
   spinel's typed-array slots nil collapses to the C zero value
   (0 / 0.0 / NULL / sp_box_nil for poly). Guards rand()%0, which
   raises SIGFPE under -O0 and silently UBs under -O2+. Issue #536. */
static mrb_int sp_IntArray_sample(sp_IntArray*a){if(a->len<=0)return 0;return a->data[a->start+(mrb_int)(rand()%a->len)];}
/* Issue #745: guard the initial read on empty arrays. CRuby's [].min /
   .max return nil; spinel's int-typed slot collapses nil to 0. Without
   the guard, the first read is uninitialized memory. */
/* Issue #832: empty min/max return SP_INT_NIL (caller treats as int?). */
static mrb_int sp_IntArray_min(sp_IntArray*a){if(!a||a->len<=0)return SP_INT_NIL;mrb_int m=a->data[a->start];for(mrb_int i=1;i<a->len;i++)if(a->data[a->start+i]<m)m=a->data[a->start+i];return m;}
static mrb_int sp_IntArray_max(sp_IntArray*a){if(!a||a->len<=0)return SP_INT_NIL;mrb_int m=a->data[a->start];for(mrb_int i=1;i<a->len;i++)if(a->data[a->start+i]>m)m=a->data[a->start+i];return m;}
static mrb_int sp_IntArray_sum(sp_IntArray*a,mrb_int init){mrb_int s=init;for(mrb_int i=0;i<a->len;i++)s+=a->data[a->start+i];return s;}
static mrb_bool sp_IntArray_include(sp_IntArray*a,mrb_int v){for(mrb_int i=0;i<a->len;i++)if(a->data[a->start+i]==v)return TRUE;return FALSE;}
static mrb_int sp_IntArray_index(sp_IntArray*a,mrb_int v){for(mrb_int i=0;i<a->len;i++)if(a->data[a->start+i]==v)return i;return -1;}
static mrb_int sp_IntArray_rindex(sp_IntArray*a,mrb_int v){for(mrb_int i=a->len-1;i>=0;i--)if(a->data[a->start+i]==v)return i;return -1;}
static mrb_int sp_IntArray_delete_at(sp_IntArray*a,mrb_int i){if(a&&a->frozen){sp_raise_frozen_array();return 0;}if(i<0)i+=a->len;if(i<0||i>=a->len)return 0;mrb_int v=a->data[a->start+i];for(mrb_int j=i;j<a->len-1;j++)a->data[a->start+j]=a->data[a->start+j+1];a->len--;return v;}
static mrb_int sp_IntArray_delete(sp_IntArray*a,mrb_int v){if(a&&a->frozen){sp_raise_frozen_array();return 0;}mrb_int w=0;for(mrb_int i=0;i<a->len;i++){if(a->data[a->start+i]!=v){a->data[a->start+w]=a->data[a->start+i];w++;}}mrb_int d=a->len-w;a->len=w;return d>0?v:0;}
/* Issue #788: clamp i so a very-negative index doesn't underflow past
   a->start and write into the array's GC header. */
static void sp_IntArray_insert(sp_IntArray*a,mrb_int i,mrb_int v){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}if(i<0)i+=a->len+1;if(i<0)i=0;if(i>a->len)i=a->len;sp_IntArray_push(a,0);for(mrb_int j=a->len-1;j>i;j--)a->data[a->start+j]=a->data[a->start+j-1];a->data[a->start+i]=v;}
static sp_IntArray*sp_int_digits(mrb_int n,mrb_int base){sp_IntArray*a=sp_IntArray_new();if(base<2)base=10;if(n==0){sp_IntArray_push(a,0);return a;}if(n<0)n=-n;while(n>0){sp_IntArray_push(a,n%base);n/=base;}return a;}
static sp_IntArray*sp_IntArray_uniq(sp_IntArray*a){sp_IntArray*b=sp_IntArray_new();for(mrb_int i=0;i<a->len;i++){int found=0;for(mrb_int j=0;j<b->len;j++){if(b->data[b->start+j]==a->data[a->start+i]){found=1;break;}}if(!found)sp_IntArray_push(b,a->data[a->start+i]);}return b;}
static sp_IntArray*sp_IntArray_intersect(sp_IntArray*a,sp_IntArray*b){sp_IntArray*r=sp_IntArray_new();for(mrb_int i=0;i<a->len;i++){mrb_int v=a->data[a->start+i];if(sp_IntArray_include(b,v)&&!sp_IntArray_include(r,v))sp_IntArray_push(r,v);}return r;}
static sp_IntArray*sp_IntArray_union(sp_IntArray*a,sp_IntArray*b){sp_IntArray*r=sp_IntArray_new();for(mrb_int i=0;i<a->len;i++){mrb_int v=a->data[a->start+i];if(!sp_IntArray_include(r,v))sp_IntArray_push(r,v);}if(b){for(mrb_int i=0;i<b->len;i++){mrb_int v=b->data[b->start+i];if(!sp_IntArray_include(r,v))sp_IntArray_push(r,v);}}return r;}
/* Array#- / Array#difference: keep every LHS element NOT in RHS,
   preserving the LHS's duplicates. CRuby's Array#- is not a set
   subtraction — `[1,1,2,3] - [3]` is `[1,1,2]`, not `[1,2]`. */
static sp_IntArray*sp_IntArray_difference(sp_IntArray*a,sp_IntArray*b){sp_IntArray*r=sp_IntArray_new();for(mrb_int i=0;i<a->len;i++){mrb_int v=a->data[a->start+i];if(!sp_IntArray_include(b,v))sp_IntArray_push(r,v);}return r;}
static void sp_IntArray_unshift(sp_IntArray*a,mrb_int v){if(a->frozen){sp_raise_frozen_array();return;}if(a->start>0){a->start--;a->data[a->start]=v;a->len++;}else{mrb_int e=a->len+1;if(e>a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*a->cap;h->size-=sizeof(mrb_int)*a->cap;a->cap=a->cap*2+1;a->data=(mrb_int*)realloc(a->data,sizeof(mrb_int)*a->cap);h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}memmove(a->data+1,a->data,sizeof(mrb_int)*a->len);a->data[0]=v;a->len++;}}
static const char*sp_IntArray_join(sp_IntArray*a,const char*sep){size_t sl=strlen(sep),cap=256;char*buf=(char*)malloc(cap);size_t len=0;for(mrb_int i=0;i<a->len;i++){if(i>0){if(len+sl>=cap){cap*=2;buf=(char*)realloc(buf,cap);}memcpy(buf+len,sep,sl);len+=sl;}char tmp[32];int n=snprintf(tmp,32,"%lld",(long long)a->data[a->start+i]);if(len+n>=cap){cap*=2;buf=(char*)realloc(buf,cap);}memcpy(buf+len,tmp,n);len+=n;}buf[len]=0;char*r=sp_str_alloc(len);memcpy(r,buf,len);free(buf);return r;}
static mrb_bool sp_IntArray_eq(sp_IntArray*a,sp_IntArray*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++)if(a->data[a->start+i]!=b->data[b->start+i])return FALSE;return TRUE;}
/* Array#<=> for IntArray. Lexicographic: per-element compare,
   shorter array sorts before longer if all common elements match
   (matches CRuby `[1,2] <=> [1,2,3] == -1`). Used by `.max` /
   `.min` on `int_array_ptr_array` to dispatch the standard
   sort/compare protocol over arrays of arrays. NULL recv compares
   equal to NULL, lower than any non-NULL (matches the NULL-safe
   pattern other compare helpers use). */
static mrb_int sp_IntArray_cmp(sp_IntArray*a,sp_IntArray*b){if(!a||!b)return a==b?0:(a?1:-1);mrb_int n=a->len<b->len?a->len:b->len;for(mrb_int i=0;i<n;i++){mrb_int av=a->data[a->start+i],bv=b->data[b->start+i];if(av<bv)return -1;if(av>bv)return 1;}if(a->len<b->len)return -1;if(a->len>b->len)return 1;return 0;}

static void sp_FloatArray_fin(void*p){sp_FloatArray*a=(sp_FloatArray*)p;sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_float)*a->cap;h->size-=sizeof(mrb_float)*a->cap;free(a->data);}
static sp_FloatArray*sp_FloatArray_new(void){sp_FloatArray*a=(sp_FloatArray*)sp_gc_alloc(sizeof(sp_FloatArray),sp_FloatArray_fin,NULL);a->cap=16;a->data=(mrb_float*)malloc(sizeof(mrb_float)*a->cap);if(!a->data)sp_oom_die();a->len=0;{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));h->size+=sizeof(mrb_float)*a->cap;sp_gc_bytes+=sizeof(mrb_float)*a->cap;}return a;}
static inline void sp_FloatArray_push(sp_FloatArray*a,mrb_float v){if(a->frozen){sp_raise_frozen_array();return;}if(a->len>=a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_float)*a->cap;h->size-=sizeof(mrb_float)*a->cap;a->cap=a->cap*2+1;a->data=(mrb_float*)realloc(a->data,sizeof(mrb_float)*a->cap);h->size+=sizeof(mrb_float)*a->cap;sp_gc_bytes+=sizeof(mrb_float)*a->cap;}a->data[a->len++]=v;}
/* Float#step materialised as a FloatArray. Direction follows the
   sign of k; k==0 yields an empty array to avoid an infinite loop. */
static sp_FloatArray*sp_FloatArray_from_step(mrb_float s,mrb_float e,mrb_float k){sp_FloatArray*a=sp_FloatArray_new();if(k==0.0)return a;mrb_float v=s;if(k>0){while(v<=e){sp_FloatArray_push(a,v);v+=k;}}else{while(v>=e){sp_FloatArray_push(a,v);v+=k;}}return a;}
static mrb_float sp_FloatArray_min(sp_FloatArray*a){if(a->len==0)return 0;mrb_float m=a->data[0];for(mrb_int i=1;i<a->len;i++)if(a->data[i]<m)m=a->data[i];return m;}
static mrb_float sp_FloatArray_max(sp_FloatArray*a){if(a->len==0)return 0;mrb_float m=a->data[0];for(mrb_int i=1;i<a->len;i++)if(a->data[i]>m)m=a->data[i];return m;}
static mrb_float sp_FloatArray_sum(sp_FloatArray*a,mrb_float init){mrb_float s=init;for(mrb_int i=0;i<a->len;i++)s+=a->data[i];return s;}
static void sp_FloatArray_replace(sp_FloatArray*dst,sp_FloatArray*src){dst->len=0;if(src->len>dst->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)dst-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_float)*dst->cap;h->size-=sizeof(mrb_float)*dst->cap;void*nd=realloc(dst->data,sizeof(mrb_float)*src->len);if(!nd){perror("realloc");exit(1);}dst->data=(mrb_float*)nd;dst->cap=src->len;h->size+=sizeof(mrb_float)*dst->cap;sp_gc_bytes+=sizeof(mrb_float)*dst->cap;}memcpy(dst->data,src->data,sizeof(mrb_float)*src->len);dst->len=src->len;}
static inline mrb_float sp_FloatArray_pop(sp_FloatArray*a){if(!a||a->len<=0)return 0.0;if(a->frozen){sp_raise_frozen_array();return 0.0;}return a->data[--a->len];}
static inline mrb_float sp_FloatArray_shift(sp_FloatArray*a){if(!a||a->len==0)return 0.0;if(a->frozen){sp_raise_frozen_array();return 0.0;}mrb_float v=a->data[0];for(mrb_int i=0;i+1<a->len;i++)a->data[i]=a->data[i+1];a->len--;return v;}
/* delete_at for float arrays -- the IntArray/StrArray peers existed but
   FloatArray lacked it, so `[1.0].delete_at(0)` raised NoMethodError at runtime
   (surfaced by the tep openai_server embeddings path). FloatArray is 0-based (no
   `start` offset, unlike IntArray). Returns 0.0 on out-of-range (delete_at's nil
   there). (join lives further down, where sp_float_to_s / sp_String are in scope.) */
static inline mrb_float sp_FloatArray_delete_at(sp_FloatArray*a,mrb_int i){if(!a)return 0.0;if(a->frozen){sp_raise_frozen_array();return 0.0;}if(i<0)i+=a->len;if(i<0||i>=a->len)return 0.0;mrb_float v=a->data[i];for(mrb_int j=i;j+1<a->len;j++)a->data[j]=a->data[j+1];a->len--;return v;}
static inline mrb_int sp_FloatArray_length(sp_FloatArray*a){return a->len;}
static inline mrb_bool sp_FloatArray_empty(sp_FloatArray*a){return a->len==0;}
static inline mrb_float sp_FloatArray_get(sp_FloatArray*a,mrb_int i){if(!a)return sp_float_nil();if(i<0)i+=a->len;if(i<0||i>=a->len)return sp_float_nil();return a->data[i];}
/* first/last as float? : nil (sentinel) when empty, else the element.
   `[i]` stays non-nullable (0.0 for OOB) -- only first/last produce nil,
   matching CRuby where Array#first/#last on an empty array return nil. */
static inline mrb_float sp_FloatArray_first_opt(sp_FloatArray*a){return (!a||a->len<=0)?sp_float_nil():sp_FloatArray_get(a,0);}
static inline mrb_float sp_FloatArray_last_opt(sp_FloatArray*a){return (!a||a->len<=0)?sp_float_nil():sp_FloatArray_get(a,a->len-1);}
/* a[start, len] / a[start..end] for FloatArray. Same negative-start
 * and length-clamping semantics as sp_IntArray_slice. */
static sp_FloatArray*sp_FloatArray_slice(sp_FloatArray*a,mrb_int start,mrb_int len){if(start<0)start+=a->len;if(start<0)start=0;sp_FloatArray*b=sp_FloatArray_new();if(start>=a->len||len<=0)return b;if(start+len>a->len)len=a->len-start;if(len>b->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)b-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_float)*b->cap;h->size-=sizeof(mrb_float)*b->cap;b->cap=len;b->data=(mrb_float*)realloc(b->data,sizeof(mrb_float)*b->cap);h->size+=sizeof(mrb_float)*b->cap;sp_gc_bytes+=sizeof(mrb_float)*b->cap;}memcpy(b->data,a->data+start,sizeof(mrb_float)*len);b->len=len;return b;}
/* See sp_IntArray_slice_range -- same shape, issue #496. */
static sp_FloatArray*sp_FloatArray_slice_range(sp_FloatArray*a,mrb_int start,mrb_int end_,mrb_int excl){if(end_<0)end_+=a->len;if(start<0)start+=a->len;mrb_int n=end_-start+(excl?0:1);if(n<0||start<0)n=0;return sp_FloatArray_slice(a,start,n);}
/* Issue #769: no-op for negative index after adjustment. */
static inline void sp_FloatArray_set(sp_FloatArray*a,mrb_int i,mrb_float v){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}mrb_int orig=i;if(i<0)i+=a->len;if(i<0)sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)-a->len));while(i>=a->cap){a->cap=a->cap*2+1;a->data=(mrb_float*)realloc(a->data,sizeof(mrb_float)*a->cap);}while(i>=a->len){a->data[a->len]=0.0;a->len++;}a->data[i]=v;}
static void sp_FloatArray_reverse_bang(sp_FloatArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}for(mrb_int i=0,j=a->len-1;i<j;i++,j--){mrb_float t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
static void sp_FloatArray_rotate_bang(sp_FloatArray*a,mrb_int n){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}if(a->len<=0)return;n=((n%a->len)+a->len)%a->len;if(n==0)return;mrb_float*d=a->data;mrb_int lo=0,hi=n-1;while(lo<hi){mrb_float t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=n;hi=a->len-1;while(lo<hi){mrb_float t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=0;hi=a->len-1;while(lo<hi){mrb_float t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}}
static int _sp_float_cmp(const void*a,const void*b){mrb_float va=*(const mrb_float*)a,vb=*(const mrb_float*)b;return(va>vb)-(va<vb);}
static void sp_FloatArray_sort_bang(sp_FloatArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}qsort(a->data,a->len,sizeof(mrb_float),_sp_float_cmp);}
static void sp_FloatArray_shuffle_bang(sp_FloatArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}for(mrb_int i=a->len-1;i>0;i--){mrb_int j=(mrb_int)(rand()%(i+1));mrb_float t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
static sp_FloatArray*sp_FloatArray_shuffle(sp_FloatArray*a){sp_FloatArray*r=sp_FloatArray_new();sp_FloatArray_replace(r,a);sp_FloatArray_shuffle_bang(r);return r;}
static mrb_float sp_FloatArray_sample(sp_FloatArray*a){if(a->len<=0)return 0.0;return a->data[(mrb_int)(rand()%a->len)];}
/* IEEE 754 == on mrb_float: NaN never matches; +0.0 == -0.0 (diverges from Float#eql?). */
static mrb_bool sp_FloatArray_include(sp_FloatArray*a,mrb_float v){for(mrb_int i=0;i<a->len;i++)if(a->data[i]==v)return TRUE;return FALSE;}
static sp_FloatArray*sp_FloatArray_intersect(sp_FloatArray*a,sp_FloatArray*b){sp_FloatArray*r=sp_FloatArray_new();for(mrb_int i=0;i<a->len;i++){mrb_float v=a->data[i];if(sp_FloatArray_include(b,v)&&!sp_FloatArray_include(r,v))sp_FloatArray_push(r,v);}return r;}
static sp_FloatArray*sp_FloatArray_union(sp_FloatArray*a,sp_FloatArray*b){sp_FloatArray*r=sp_FloatArray_new();for(mrb_int i=0;i<a->len;i++){mrb_float v=a->data[i];if(!sp_FloatArray_include(r,v))sp_FloatArray_push(r,v);}if(b){for(mrb_int i=0;i<b->len;i++){mrb_float v=b->data[i];if(!sp_FloatArray_include(r,v))sp_FloatArray_push(r,v);}}return r;}
static sp_FloatArray*sp_FloatArray_difference(sp_FloatArray*a,sp_FloatArray*b){sp_FloatArray*r=sp_FloatArray_new();for(mrb_int i=0;i<a->len;i++){mrb_float v=a->data[i];if(!sp_FloatArray_include(b,v))sp_FloatArray_push(r,v);}return r;}

/* ---- PtrArray: array of void* pointers ---- */
static void sp_PtrArray_fin(void*p){sp_PtrArray*a=(sp_PtrArray*)p;sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(void*)*a->cap;h->size-=sizeof(void*)*a->cap;free(a->data);}
static void sp_PtrArray_gc_scan(void*p){sp_PtrArray*a=(sp_PtrArray*)p;if(!a->scan_elem)return;for(mrb_int i=0;i<a->len;i++){if(a->data[i])a->scan_elem(a->data[i]);}}
static sp_PtrArray*sp_PtrArray_new_scan(void(*scan_elem)(void*)){sp_PtrArray*a=(sp_PtrArray*)sp_gc_alloc(sizeof(sp_PtrArray),sp_PtrArray_fin,scan_elem?sp_PtrArray_gc_scan:NULL);a->cap=16;a->data=(void**)malloc(sizeof(void*)*a->cap);if(!a->data)sp_oom_die();a->len=0;a->scan_elem=scan_elem;{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));h->size+=sizeof(void*)*a->cap;sp_gc_bytes+=sizeof(void*)*a->cap;}return a;}
static sp_PtrArray*sp_PtrArray_new(void){return sp_PtrArray_new_scan(sp_gc_mark);}
/* PtrArray for raw external pointers (FFI `:ptr` returns, dlopen
   handles, ...). These don't carry sp_gc_hdr -- the default
   sp_gc_mark element scan would read undefined bytes and likely
   crash at collection time. Skip per-element scanning; the array
   header itself is still GC-tracked. */
static sp_PtrArray*sp_PtrArray_new_noscan(void){return sp_PtrArray_new_scan(NULL);}
static inline void sp_PtrArray_push(sp_PtrArray*a,void*v){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}if(a->len>=a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(void*)*a->cap;h->size-=sizeof(void*)*a->cap;a->cap=a->cap*2+1;void*nd=realloc(a->data,sizeof(void*)*a->cap);if(!nd)sp_oom_die();a->data=(void**)nd;h->size+=sizeof(void*)*a->cap;sp_gc_bytes+=sizeof(void*)*a->cap;}a->data[a->len++]=v;}
/* Array#pop on a `<X>_ptr_array`. Returns NULL when empty
   (matches CRuby's nil for typed-element arrays since the slot
   can't carry nil). Issue #520: previously the dispatch on
   `int_array_ptr_array.pop` warned "cannot resolve call to
   'pop'" and emitted 0, silently leaving the array intact. */
static inline void *sp_PtrArray_pop(sp_PtrArray*a){if(!a||a->len==0)return NULL;return a->data[--a->len];}
static inline void*sp_PtrArray_get(sp_PtrArray*a,mrb_int i){if(!a)return NULL;if(i<0)i+=a->len;if(i<0||i>=a->len)return NULL;return a->data[i];}
/* Issue #770: bounds-check the final index. CRuby grows the array on
   out-of-range set; spinel's typed-array slots have a fixed capacity
   shape per element type, so the safer fallback is to no-op rather
   than write into adjacent memory. */
static inline void sp_PtrArray_set(sp_PtrArray*a,mrb_int i,void*v){if(!a)return;if(i<0)i+=a->len;if(i<0||i>=a->len)return;a->data[i]=v;}
static inline mrb_int sp_PtrArray_length(sp_PtrArray*a){if(!a)return 0;return a->len;}
static inline mrb_bool sp_PtrArray_empty(sp_PtrArray*a){if(!a)return TRUE;return a->len==0;}
/* `Array#delete_at(i)` -- remove and return the element at index i.
   Negative indices count from the end. Returns NULL when i is out
   of range (matches CRuby's nil for typed-element arrays). Mirrors
   sp_IntArray_delete_at / sp_StrArray_delete_at; without this peer
   user-class arrays can't shrink. */
static void*sp_PtrArray_delete_at(sp_PtrArray*a,mrb_int i){if(i<0)i+=a->len;if(i<0||i>=a->len)return NULL;void*v=a->data[i];for(mrb_int j=i;j<a->len-1;j++)a->data[j]=a->data[j+1];a->len--;return v;}
static void sp_PtrArray_reverse_bang(sp_PtrArray*a){for(mrb_int i=0,j=a->len-1;i<j;i++,j--){void*t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
static void sp_PtrArray_rotate_bang(sp_PtrArray*a,mrb_int n){if(a->len<=0)return;n=((n%a->len)+a->len)%a->len;if(n==0)return;void**d=a->data;mrb_int lo=0,hi=n-1;while(lo<hi){void*t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=n;hi=a->len-1;while(lo<hi){void*t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=0;hi=a->len-1;while(lo<hi){void*t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}}
static sp_PtrArray*sp_PtrArray_dup(sp_PtrArray*a){sp_PtrArray*b=sp_PtrArray_new_scan(a->scan_elem);for(mrb_int i=0;i<a->len;i++)sp_PtrArray_push(b,a->data[i]);return b;}
static sp_PtrArray*sp_PtrArray_slice(sp_PtrArray*a,mrb_int start,mrb_int len){if(start<0)start+=a->len;if(start<0)start=0;sp_PtrArray*b=sp_PtrArray_new_scan(a->scan_elem);if(start>=a->len||len<=0)return b;if(start+len>a->len)len=a->len-start;for(mrb_int i=0;i<len;i++)sp_PtrArray_push(b,a->data[start+i]);return b;}
static void sp_PtrArray_shuffle_bang(sp_PtrArray*a){for(mrb_int i=a->len-1;i>0;i--){mrb_int j=(mrb_int)(rand()%(i+1));void*t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
static sp_PtrArray*sp_PtrArray_shuffle(sp_PtrArray*a){sp_PtrArray*b=sp_PtrArray_dup(a);sp_PtrArray_shuffle_bang(b);return b;}
static void *sp_PtrArray_sample(sp_PtrArray*a){if(a->len<=0)return NULL;return a->data[(mrb_int)(rand()%a->len)];}

/* Small-array optimization: keep the first SP_STRARR_INLINE elements
 * inside the struct so empty/short StrArrays skip the data malloc.
 * Common idiom "".split(",") and most class-metadata lists stay small.
 * data == inline_data is the discriminator for "still on inline storage". */
static void sp_StrArray_fin(void*p){sp_StrArray*a=(sp_StrArray*)p;if(a->data!=a->inline_data){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(const char*)*a->cap;h->size-=sizeof(const char*)*a->cap;free(a->data);}}
static void sp_StrArray_scan(void*p){sp_StrArray*a=(sp_StrArray*)p;for(mrb_int i=0;i<a->len;i++)sp_mark_string(a->data[i]);}
static sp_StrArray*sp_StrArray_new(void){sp_StrArray*a=(sp_StrArray*)sp_gc_alloc(sizeof(sp_StrArray),sp_StrArray_fin,sp_StrArray_scan);a->cap=SP_STRARR_INLINE;a->data=a->inline_data;a->len=0;return a;}
static inline void sp_StrArray_push(sp_StrArray*a,const char*v){if(a->frozen){sp_raise_frozen_array();return;}if(a->len>=a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));mrb_int nc=a->cap*2+1;if(a->data==a->inline_data){const char**nd=(const char**)malloc(sizeof(const char*)*nc);if(!nd)sp_oom_die();memcpy(nd,a->data,sizeof(const char*)*a->len);a->data=nd;}else{sp_gc_bytes-=sizeof(const char*)*a->cap;h->size-=sizeof(const char*)*a->cap;void*nd=realloc(a->data,sizeof(const char*)*nc);if(!nd)sp_oom_die();a->data=(const char**)nd;}a->cap=nc;h->size+=sizeof(const char*)*a->cap;sp_gc_bytes+=sizeof(const char*)*a->cap;}a->data[a->len++]=v;}
static void sp_StrArray_replace(sp_StrArray*dst,sp_StrArray*src){dst->len=0;if(src->len>dst->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)dst-sizeof(sp_gc_hdr));void*nd;if(dst->data==dst->inline_data){nd=malloc(sizeof(const char*)*src->len);if(!nd){perror("malloc");exit(1);}}else{sp_gc_bytes-=sizeof(const char*)*dst->cap;h->size-=sizeof(const char*)*dst->cap;nd=realloc(dst->data,sizeof(const char*)*src->len);if(!nd){perror("realloc");exit(1);}}dst->data=(const char**)nd;dst->cap=src->len;h->size+=sizeof(const char*)*dst->cap;sp_gc_bytes+=sizeof(const char*)*dst->cap;}memcpy(dst->data,src->data,sizeof(const char*)*src->len);dst->len=src->len;}
static const char*sp_StrArray_pop(sp_StrArray*a){if(!a||a->len<=0)return NULL;if(a->frozen){sp_raise_frozen_array();return NULL;}return a->data[--a->len];}
static inline mrb_int sp_StrArray_length(sp_StrArray*a){return a->len;}
static inline mrb_bool sp_StrArray_empty(sp_StrArray*a){return a->len==0;}
static inline const char*sp_StrArray_get(sp_StrArray*a,mrb_int i){if(!a)return NULL;if(i<0)i+=a->len;if(i<0||i>=a->len)return NULL;return a->data[i];}
/* a[start, len] / a[start..end] for StrArray. Same negative-start and
 * length-clamping semantics as sp_IntArray_slice. Out-of-bounds start
 * returns an empty StrArray (we don't have a nullable form). */
static sp_StrArray*sp_StrArray_slice(sp_StrArray*a,mrb_int start,mrb_int len){if(start<0)start+=a->len;if(start<0)start=0;sp_StrArray*b=sp_StrArray_new();if(start>=a->len||len<=0)return b;if(start+len>a->len)len=a->len-start;for(mrb_int i=0;i<len;i++)sp_StrArray_push(b,a->data[start+i]);return b;}
/* Forward decl — sp_str_succ is defined further down (it uses
   sp_utf8_decode); the StrArray_from_string_range loop below needs
   it visible early. */
static const char *sp_str_succ(const char *s);
/* String range to_a — single-char and multi-char ASCII ranges via
   sp_str_succ. The 4096-iteration cap stops a pathological prepend-
   style infinite loop before it eats memory. */
static sp_StrArray *sp_StrArray_from_string_range(const char *s, const char *e, mrb_int excl) {
  sp_StrArray *a = sp_StrArray_new();
  if (!s || !e) return a;
  const char *cur = s;
  int iters = 0;
  while (iters < 4096) {
    int cmp = strcmp(cur, e);
    if (cmp > 0) break;
    if (cmp == 0 && excl) break;
    char *copy = sp_str_alloc(strlen(cur));
    strcpy(copy, cur);
    sp_StrArray_push(a, copy);
    if (cmp == 0) break;
    cur = sp_str_succ(cur);
    iters++;
  }
  return a;
}
/* See sp_IntArray_slice_range -- same shape, issue #496. */
static sp_StrArray*sp_StrArray_slice_range(sp_StrArray*a,mrb_int start,mrb_int end_,mrb_int excl){if(end_<0)end_+=a->len;if(start<0)start+=a->len;mrb_int n=end_-start+(excl?0:1);if(n<0||start<0)n=0;return sp_StrArray_slice(a,start,n);}
static inline void sp_StrArray_set(sp_StrArray*a,mrb_int i,const char*v){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}mrb_int orig=i;if(i<0)i+=a->len;if(i<0)sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)-a->len));while(i>=a->len)sp_StrArray_push(a,sp_str_empty);a->data[i]=v;}
static void sp_StrArray_reverse_bang(sp_StrArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}for(mrb_int i=0,j=a->len-1;i<j;i++,j--){const char*t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
static void sp_StrArray_rotate_bang(sp_StrArray*a,mrb_int n){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}if(a->len<=0)return;n=((n%a->len)+a->len)%a->len;if(n==0)return;const char**d=a->data;mrb_int lo=0,hi=n-1;while(lo<hi){const char*t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=n;hi=a->len-1;while(lo<hi){const char*t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=0;hi=a->len-1;while(lo<hi){const char*t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}}
static int _sp_str_cmp(const void*a,const void*b){return strcmp(*(const char*const*)a,*(const char*const*)b);}
static void sp_StrArray_sort_bang(sp_StrArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}qsort(a->data,a->len,sizeof(const char*),_sp_str_cmp);}
/* Case-insensitive string compare. Portable across glibc / MinGW
   (avoids strcasecmp which lives in strings.h on POSIX and is named
   stricmp on Windows). Returns -1 / 0 / 1 like CRuby's String#casecmp. */
static mrb_int sp_str_casecmp(const char*a,const char*b){if(!a||!b)return a==b?0:(a?1:-1);for(;;){int ca=tolower((unsigned char)*a),cb=tolower((unsigned char)*b);if(ca!=cb)return ca<cb?-1:1;if(!*a)return 0;a++;b++;}}
/* String#valid_encoding? — walks the buffer and accepts pure ASCII
   or well-formed UTF-8 (RFC 3629 byte sequences with no overlong
   forms, no surrogate halves, code points <= U+10FFFF). */
static mrb_bool sp_str_valid_encoding(const char*s){if(!s)return TRUE;const unsigned char*p=(const unsigned char*)s;while(*p){unsigned c=*p;if(c<0x80){p++;continue;}int extra;unsigned cp;unsigned min;if((c&0xE0)==0xC0){extra=1;cp=c&0x1F;min=0x80;}else if((c&0xF0)==0xE0){extra=2;cp=c&0x0F;min=0x800;}else if((c&0xF8)==0xF0){extra=3;cp=c&0x07;min=0x10000;}else return FALSE;p++;for(int i=0;i<extra;i++){if((*p&0xC0)!=0x80)return FALSE;cp=(cp<<6)|(*p&0x3F);p++;}if(cp<min)return FALSE;if(cp>=0xD800&&cp<=0xDFFF)return FALSE;if(cp>0x10FFFF)return FALSE;}return TRUE;}
static const char*sp_StrArray_join(sp_StrArray*a,const char*sep){size_t sl=strlen(sep),cap=256;char*buf=(char*)malloc(cap);size_t len=0;for(mrb_int i=0;i<a->len;i++){if(i>0){if(len+sl>=cap){cap*=2;buf=(char*)realloc(buf,cap);}memcpy(buf+len,sep,sl);len+=sl;}size_t el=strlen(a->data[i]);if(len+el>=cap){cap=(len+el)*2+1;buf=(char*)realloc(buf,cap);}memcpy(buf+len,a->data[i],el);len+=el;}buf[len]=0;char*r=sp_str_alloc(len);memcpy(r,buf,len);free(buf);return r;}
static mrb_bool sp_StrArray_include(sp_StrArray*a,const char*v){for(mrb_int i=0;i<a->len;i++)if(strcmp(a->data[i],v)==0)return TRUE;return FALSE;}
static sp_StrArray*sp_StrArray_intersect(sp_StrArray*a,sp_StrArray*b){sp_StrArray*r=sp_StrArray_new();for(mrb_int i=0;i<a->len;i++){const char*v=a->data[i];if(sp_StrArray_include(b,v)&&!sp_StrArray_include(r,v))sp_StrArray_push(r,v);}return r;}
static sp_StrArray*sp_StrArray_union(sp_StrArray*a,sp_StrArray*b){sp_StrArray*r=sp_StrArray_new();for(mrb_int i=0;i<a->len;i++){const char*v=a->data[i];if(!sp_StrArray_include(r,v))sp_StrArray_push(r,v);}if(b){for(mrb_int i=0;i<b->len;i++){const char*v=b->data[i];if(!sp_StrArray_include(r,v))sp_StrArray_push(r,v);}}return r;}
static sp_StrArray*sp_StrArray_difference(sp_StrArray*a,sp_StrArray*b){sp_StrArray*r=sp_StrArray_new();for(mrb_int i=0;i<a->len;i++){const char*v=a->data[i];if(!sp_StrArray_include(b,v))sp_StrArray_push(r,v);}return r;}
static mrb_int sp_StrArray_index(sp_StrArray*a,const char*v){for(mrb_int i=0;i<a->len;i++)if(strcmp(a->data[i],v)==0)return i;return -1;}
static mrb_int sp_StrArray_rindex(sp_StrArray*a,const char*v){for(mrb_int i=a->len-1;i>=0;i--)if(strcmp(a->data[i],v)==0)return i;return -1;}
static sp_StrArray*sp_StrArray_compact(sp_StrArray*a){sp_StrArray*r=sp_StrArray_new();for(mrb_int i=0;i<a->len;i++)if(a->data[i]!=NULL)sp_StrArray_push(r,a->data[i]);return r;}
static const char*sp_StrArray_delete_at(sp_StrArray*a,mrb_int i){if(!a)return NULL;if(a->frozen){sp_raise_frozen_array();return NULL;}if(i<0)i+=a->len;if(i<0||i>=a->len)return NULL;const char*v=a->data[i];for(mrb_int j=i;j<a->len-1;j++)a->data[j]=a->data[j+1];a->len--;return v;}
static const char*sp_StrArray_delete(sp_StrArray*a,const char*v){if(!a)return NULL;if(a->frozen){sp_raise_frozen_array();return NULL;}mrb_int w=0;const char*found=NULL;for(mrb_int i=0;i<a->len;i++){if(strcmp(a->data[i],v)!=0){a->data[w]=a->data[i];w++;}else{found=a->data[i];}}a->len=w;return found;}
static void sp_StrArray_insert(sp_StrArray*a,mrb_int i,const char*v){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}if(i<0)i+=a->len+1;sp_StrArray_push(a,sp_str_empty);for(mrb_int j=a->len-1;j>i;j--)a->data[j]=a->data[j-1];a->data[i]=v;}
static void sp_StrArray_shuffle_bang(sp_StrArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}for(mrb_int i=a->len-1;i>0;i--){mrb_int j=(mrb_int)(rand()%(i+1));const char*t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
static sp_StrArray*sp_StrArray_shuffle(sp_StrArray*a){sp_StrArray*r=sp_StrArray_new();sp_StrArray_replace(r,a);sp_StrArray_shuffle_bang(r);return r;}
static const char *sp_StrArray_sample(sp_StrArray*a){if(a->len<=0)return sp_str_empty;return a->data[(mrb_int)(rand()%a->len)];}

static inline uint64_t sp_str_hash(const char*s){uint64_t h=14695981039346656037ULL;while(*s){h^=(unsigned char)*s++;h*=1099511628211ULL;}return h;}
static void sp_StrIntHash_fin(void*p){sp_StrIntHash*h=(sp_StrIntHash*)p;free(h->keys);free(h->vals);free(h->order);}
static void sp_StrIntHash_scan(void*p){sp_StrIntHash*h=(sp_StrIntHash*)p;for(mrb_int i=0;i<h->cap;i++){if(h->keys[i])sp_mark_string(h->keys[i]);}}
/* default_v is SP_INT_NIL for a hash with no explicit default ({} / {k=>v}),
   so a missing-key `[]` read surfaces Ruby nil (#801). Hash.new(N) sets it to
   N via _new_with_default. Proven-present internal reads use _get on present
   keys, so this only governs the miss path. */
static sp_StrIntHash*sp_StrIntHash_new(void){sp_StrIntHash*h=(sp_StrIntHash*)sp_gc_alloc(sizeof(sp_StrIntHash),sp_StrIntHash_fin,sp_StrIntHash_scan);h->cap=16;h->mask=15;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(const char**)malloc(sizeof(const char*)*h->cap);h->len=0;h->default_v=SP_INT_NIL;return h;}
static sp_StrIntHash*sp_StrIntHash_new_with_default(mrb_int d){sp_StrIntHash*h=sp_StrIntHash_new();h->default_v=d;return h;}
static void sp_StrIntHash_grow(sp_StrIntHash*h){mrb_int oc=h->cap;const char**ok=h->keys;mrb_int*ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(const char**)realloc(h->order,sizeof(const char*)*h->cap);mrb_int ol=h->len;h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]){mrb_int idx=(mrb_int)(sp_str_hash(ok[i])&h->mask);while(h->keys[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}
static mrb_int sp_StrIntHash_get(sp_StrIntHash*h,const char*k){if(!h)return 0;mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
/* Issue #801: maybe-missing public `[]` read. Returns default_v on a miss,
   which is SP_INT_NIL (Ruby nil at the value level) for a no-default hash and
   the explicit default for Hash.new(N). Proven-present reads keep using _get. */
static mrb_int sp_StrIntHash_get_opt(sp_StrIntHash*h,const char*k){if(!h)return SP_INT_NIL;mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
static void sp_StrIntHash_set(sp_StrIntHash*h,const char*k,mrb_int v){if(h->len*2>=h->cap)sp_StrIntHash_grow(h);mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}
static mrb_bool sp_StrIntHash_has_key(sp_StrIntHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
/* Hash#value? -- scan values in insertion order. Issue #738. */
static mrb_bool sp_StrIntHash_has_value(sp_StrIntHash*h,mrb_int v){if(!h)return FALSE;for(mrb_int i=0;i<h->len;i++)if(sp_StrIntHash_get(h,h->order[i])==v)return TRUE;return FALSE;}
static mrb_int sp_StrIntHash_length(sp_StrIntHash*h){return h->len;}
static void sp_StrIntHash_delete(sp_StrIntHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0){h->keys[idx]=NULL;h->vals[idx]=0;h->len--;mrb_int j=(idx+1)&h->mask;while(h->keys[j]){mrb_int nj=(mrb_int)(sp_str_hash(h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->keys[j]=NULL;h->vals[j]=0;idx=j;}j=(j+1)&h->mask;}{mrb_int oi=0;while(oi<=h->len){if(strcmp(h->order[oi],k)==0){while(oi<h->len){h->order[oi]=h->order[oi+1];oi++;}break;}oi++;}}return;}idx=(idx+1)&h->mask;}}
static sp_StrArray*sp_StrIntHash_keys(sp_StrIntHash*h){sp_StrArray*a=sp_StrArray_new();for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,h->order[i]);return a;}
static sp_IntArray*sp_StrIntHash_values(sp_StrIntHash*h){sp_IntArray*a=sp_IntArray_new();for(mrb_int i=0;i<h->len;i++)sp_IntArray_push(a,sp_StrIntHash_get(h,h->order[i]));return a;}
static sp_StrIntHash*sp_StrArray_tally(sp_StrArray*a){sp_StrIntHash*h=sp_StrIntHash_new();for(mrb_int i=0;i<a->len;i++){const char*k=a->data[i];mrb_int c=sp_StrIntHash_has_key(h,k)?sp_StrIntHash_get(h,k):0;sp_StrIntHash_set(h,k,c+1);}return h;}
static sp_StrIntHash*sp_StrIntHash_merge(sp_StrIntHash*a,sp_StrIntHash*b){sp_StrIntHash*r=sp_StrIntHash_new();r->default_v=a->default_v;for(mrb_int i=0;i<a->len;i++)sp_StrIntHash_set(r,a->order[i],sp_StrIntHash_get(a,a->order[i]));for(mrb_int i=0;i<b->len;i++)sp_StrIntHash_set(r,b->order[i],sp_StrIntHash_get(b,b->order[i]));return r;}
static void sp_StrIntHash_update(sp_StrIntHash*a,sp_StrIntHash*b){for(mrb_int i=0;i<b->len;i++)sp_StrIntHash_set(a,b->order[i],sp_StrIntHash_get(b,b->order[i]));}
static sp_StrIntHash*sp_StrIntHash_dup(sp_StrIntHash*h){sp_StrIntHash*r=sp_StrIntHash_new();r->default_v=h->default_v;for(mrb_int i=0;i<h->len;i++)sp_StrIntHash_set(r,h->order[i],sp_StrIntHash_get(h,h->order[i]));return r;}
static mrb_bool sp_StrIntHash_eq(sp_StrIntHash*a,sp_StrIntHash*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++){const char*k=a->order[i];if(!sp_StrIntHash_has_key(b,k))return FALSE;if(sp_StrIntHash_get(a,k)!=sp_StrIntHash_get(b,k))return FALSE;}return TRUE;}

/* GC.stat snapshot: String=>Integer hash over the collector globals.
   full_runs derives from sp_gc_cycle / SP_GC_FULL_INTERVAL (the major
   collection cadence). */
static sp_StrIntHash*sp_gc_stat(void){sp_StrIntHash*h=sp_StrIntHash_new();sp_StrIntHash_set(h,"bytes",(mrb_int)sp_gc_bytes);sp_StrIntHash_set(h,"old_bytes",(mrb_int)sp_gc_old_bytes);sp_StrIntHash_set(h,"threshold",(mrb_int)sp_gc_threshold);sp_StrIntHash_set(h,"cycle",(mrb_int)sp_gc_cycle);sp_StrIntHash_set(h,"full_runs",(mrb_int)(sp_gc_cycle/SP_GC_FULL_INTERVAL));return h;}

static void sp_StrStrHash_fin(void*p){sp_StrStrHash*h=(sp_StrStrHash*)p;free(h->keys);free(h->vals);free(h->order);}
static void sp_StrStrHash_scan(void*p){sp_StrStrHash*h=(sp_StrStrHash*)p;for(mrb_int i=0;i<h->cap;i++){if(h->keys[i]){sp_mark_string(h->keys[i]);sp_mark_string(h->vals[i]);}}if(h->default_v)sp_mark_string(h->default_v);}
static sp_StrStrHash*sp_StrStrHash_new(void){sp_StrStrHash*h=(sp_StrStrHash*)sp_gc_alloc(sizeof(sp_StrStrHash),sp_StrStrHash_fin,sp_StrStrHash_scan);h->cap=16;h->mask=15;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(const char**)calloc(h->cap,sizeof(const char*));h->order=(const char**)malloc(sizeof(const char*)*h->cap);h->len=0;h->default_v=NULL;return h;}
static sp_StrStrHash*sp_StrStrHash_new_with_default(const char*d){sp_StrStrHash*h=sp_StrStrHash_new();h->default_v=d;return h;}
static void sp_StrStrHash_grow(sp_StrStrHash*h){mrb_int oc=h->cap;const char**ok=h->keys;const char**ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(const char**)calloc(h->cap,sizeof(const char*));h->order=(const char**)realloc(h->order,sizeof(const char*)*h->cap);h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]){mrb_int idx=(mrb_int)(sp_str_hash(ok[i])&h->mask);while(h->keys[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}
static const char*sp_StrStrHash_get(sp_StrStrHash*h,const char*k){if(!h)return NULL;mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
static void sp_StrStrHash_set(sp_StrStrHash*h,const char*k,const char*v){if(h->len*2>=h->cap)sp_StrStrHash_grow(h);mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}
static mrb_bool sp_StrStrHash_has_key(sp_StrStrHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_bool sp_StrStrHash_has_value(sp_StrStrHash*h,const char*v){if(!h||!v)return FALSE;for(mrb_int i=0;i<h->len;i++){const char*x=sp_StrStrHash_get(h,h->order[i]);if(x&&strcmp(x,v)==0)return TRUE;}return FALSE;}
static mrb_int sp_StrStrHash_length(sp_StrStrHash*h){return h->len;}
static void sp_StrStrHash_delete(sp_StrStrHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0){h->keys[idx]=NULL;h->vals[idx]=NULL;h->len--;mrb_int j=(idx+1)&h->mask;while(h->keys[j]){mrb_int nj=(mrb_int)(sp_str_hash(h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->keys[j]=NULL;h->vals[j]=NULL;idx=j;}j=(j+1)&h->mask;}{mrb_int oi=0;while(oi<=h->len){if(strcmp(h->order[oi],k)==0){while(oi<h->len){h->order[oi]=h->order[oi+1];oi++;}break;}oi++;}}return;}idx=(idx+1)&h->mask;}}
static sp_StrArray*sp_StrStrHash_keys(sp_StrStrHash*h){sp_StrArray*a=sp_StrArray_new();for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,h->order[i]);return a;}
static sp_StrArray*sp_StrStrHash_values(sp_StrStrHash*h){sp_StrArray*a=sp_StrArray_new();for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,sp_StrStrHash_get(h,h->order[i]));return a;}
static sp_StrStrHash*sp_StrStrHash_invert(sp_StrStrHash*h){sp_StrStrHash*r=sp_StrStrHash_new();for(mrb_int i=0;i<h->len;i++){const char*k=h->order[i];sp_StrStrHash_set(r,sp_StrStrHash_get(h,k),k);}return r;}
static void sp_StrStrHash_update(sp_StrStrHash*a,sp_StrStrHash*b){for(mrb_int i=0;i<b->len;i++)sp_StrStrHash_set(a,b->order[i],sp_StrStrHash_get(b,b->order[i]));}
static sp_StrStrHash*sp_StrStrHash_dup(sp_StrStrHash*h){sp_StrStrHash*r=sp_StrStrHash_new();r->default_v=h->default_v;for(mrb_int i=0;i<h->len;i++)sp_StrStrHash_set(r,h->order[i],sp_StrStrHash_get(h,h->order[i]));return r;}
static mrb_bool sp_StrStrHash_eq(sp_StrStrHash*a,sp_StrStrHash*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++){const char*k=a->order[i];if(!sp_StrStrHash_has_key(b,k))return FALSE;if(!sp_str_eq(sp_StrStrHash_get(a,k),sp_StrStrHash_get(b,k)))return FALSE;}return TRUE;}

static void sp_IntStrHash_fin(void*p){sp_IntStrHash*h=(sp_IntStrHash*)p;free(h->keys);free(h->vals);free(h->order);free(h->used);}
static void sp_IntStrHash_scan(void*p){sp_IntStrHash*h=(sp_IntStrHash*)p;for(mrb_int i=0;i<h->cap;i++)if(h->used[i])sp_mark_string(h->vals[i]);if(h->default_v)sp_mark_string(h->default_v);}
static sp_IntStrHash*sp_IntStrHash_new(void){sp_IntStrHash*h=(sp_IntStrHash*)sp_gc_alloc(sizeof(sp_IntStrHash),sp_IntStrHash_fin,sp_IntStrHash_scan);h->cap=16;h->mask=15;h->keys=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->vals=(const char**)calloc(h->cap,sizeof(const char*));h->order=(mrb_int*)malloc(sizeof(mrb_int)*h->cap);h->used=(mrb_bool*)calloc(h->cap,sizeof(mrb_bool));h->len=0;h->default_v=NULL;return h;}
static sp_IntStrHash*sp_IntStrHash_new_with_default(const char*d){sp_IntStrHash*h=sp_IntStrHash_new();h->default_v=d;return h;}
static inline mrb_int _sp_istr_idx(mrb_int mask,mrb_int k){return(mrb_int)(((uint64_t)(unsigned long long)k*11400714819323198485ULL)&(uint64_t)mask);}
static void sp_IntStrHash_grow(sp_IntStrHash*h){mrb_int oc=h->cap,ol=h->len;mrb_int*ok=h->keys;const char**ov=h->vals;mrb_bool*ou=h->used;mrb_int*oo=h->order;h->cap*=2;h->mask=h->cap-1;h->keys=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->vals=(const char**)calloc(h->cap,sizeof(const char*));h->order=(mrb_int*)malloc(sizeof(mrb_int)*h->cap);h->used=(mrb_bool*)calloc(h->cap,sizeof(mrb_bool));h->len=ol;for(mrb_int i=0;i<oc;i++){if(!ou[i])continue;mrb_int k=ok[i];const char*v=ov[i];mrb_int di=_sp_istr_idx(h->mask,k);while(h->used[di])di=(di+1)&h->mask;h->used[di]=TRUE;h->keys[di]=k;h->vals[di]=v;}for(mrb_int i=0;i<ol;i++)h->order[i]=oo[i];free(ok);free(ov);free(ou);free(oo);}
static void sp_IntStrHash_set(sp_IntStrHash*h,mrb_int k,const char*v){if(h->len*2>=h->cap)sp_IntStrHash_grow(h);mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->used[idx]=TRUE;h->keys[idx]=k;h->vals[idx]=v;h->order[h->len++]=k;}
static const char*sp_IntStrHash_get(sp_IntStrHash*h,mrb_int k){if(!h)return NULL;mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
static mrb_bool sp_IntStrHash_has_key(sp_IntStrHash*h,mrb_int k){mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_bool sp_IntStrHash_has_value(sp_IntStrHash*h,const char*v){if(!h||!v)return FALSE;for(mrb_int i=0;i<h->len;i++){const char*x=sp_IntStrHash_get(h,h->order[i]);if(x&&strcmp(x,v)==0)return TRUE;}return FALSE;}
static mrb_int sp_IntStrHash_length(sp_IntStrHash*h){return h->len;}
static sp_IntArray*sp_IntStrHash_keys(sp_IntStrHash*h){sp_IntArray*a=sp_IntArray_new();for(mrb_int i=0;i<h->len;i++)sp_IntArray_push(a,h->order[i]);return a;}
static sp_StrArray*sp_IntStrHash_values(sp_IntStrHash*h){sp_StrArray*a=sp_StrArray_new();for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,sp_IntStrHash_get(h,h->order[i]));return a;}
static sp_IntStrHash*sp_IntStrHash_dup(sp_IntStrHash*h){sp_IntStrHash*r=sp_IntStrHash_new();r->default_v=h->default_v;for(mrb_int i=0;i<h->len;i++)sp_IntStrHash_set(r,h->order[i],sp_IntStrHash_get(h,h->order[i]));return r;}
static mrb_bool sp_IntStrHash_eq(sp_IntStrHash*a,sp_IntStrHash*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++){mrb_int k=a->order[i];if(!sp_IntStrHash_has_key(b,k))return FALSE;if(!sp_str_eq(sp_IntStrHash_get(a,k),sp_IntStrHash_get(b,k)))return FALSE;}return TRUE;}

/* Int → Int typed hash. Mirrors sp_IntStrHash's open-addressing
   layout (used[] bitmap so 0/-1 keys are distinguishable from
   empty), with int-valued slots. Used by Array#tally on int
   arrays — see #865. */
static void sp_IntIntHash_fin(void*p){sp_IntIntHash*h=(sp_IntIntHash*)p;free(h->keys);free(h->vals);free(h->order);free(h->used);}
/* default_v is SP_INT_NIL for a hash with no explicit default, so a
   missing-key `[]` read surfaces Ruby nil (#801). Hash.new(N) sets it via
   _new_with_default. */
static sp_IntIntHash*sp_IntIntHash_new(void){sp_IntIntHash*h=(sp_IntIntHash*)sp_gc_alloc(sizeof(sp_IntIntHash),sp_IntIntHash_fin,NULL);h->cap=16;h->mask=15;h->keys=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(mrb_int*)malloc(sizeof(mrb_int)*h->cap);h->used=(mrb_bool*)calloc(h->cap,sizeof(mrb_bool));h->len=0;h->default_v=SP_INT_NIL;return h;}
static sp_IntIntHash*sp_IntIntHash_new_with_default(mrb_int d){sp_IntIntHash*h=sp_IntIntHash_new();h->default_v=d;return h;}
static void sp_IntIntHash_grow(sp_IntIntHash*h){mrb_int oc=h->cap,ol=h->len;mrb_int*ok=h->keys;mrb_int*ov=h->vals;mrb_bool*ou=h->used;mrb_int*oo=h->order;h->cap*=2;h->mask=h->cap-1;h->keys=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(mrb_int*)malloc(sizeof(mrb_int)*h->cap);h->used=(mrb_bool*)calloc(h->cap,sizeof(mrb_bool));h->len=ol;for(mrb_int i=0;i<oc;i++){if(!ou[i])continue;mrb_int k=ok[i];mrb_int v=ov[i];mrb_int di=_sp_istr_idx(h->mask,k);while(h->used[di])di=(di+1)&h->mask;h->used[di]=TRUE;h->keys[di]=k;h->vals[di]=v;}for(mrb_int i=0;i<ol;i++)h->order[i]=oo[i];free(ok);free(ov);free(ou);free(oo);}
static void sp_IntIntHash_set(sp_IntIntHash*h,mrb_int k,mrb_int v){if(h->len*2>=h->cap)sp_IntIntHash_grow(h);mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->used[idx]=TRUE;h->keys[idx]=k;h->vals[idx]=v;h->order[h->len++]=k;}
static mrb_int sp_IntIntHash_get(sp_IntIntHash*h,mrb_int k){if(!h)return 0;mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
/* Issue #801: maybe-missing public `[]` read. Returns default_v on a miss
   (SP_INT_NIL for a no-default hash = Ruby nil; the explicit default for
   Hash.new(N)). Proven-present reads keep using _get. */
static mrb_int sp_IntIntHash_get_opt(sp_IntIntHash*h,mrb_int k){if(!h)return SP_INT_NIL;mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
static mrb_bool sp_IntIntHash_has_key(sp_IntIntHash*h,mrb_int k){mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_int sp_IntIntHash_length(sp_IntIntHash*h){return h?h->len:0;}
static sp_IntIntHash*sp_IntIntHash_dup(sp_IntIntHash*h){sp_IntIntHash*r=sp_IntIntHash_new();r->default_v=h->default_v;for(mrb_int i=0;i<h->len;i++)sp_IntIntHash_set(r,h->order[i],sp_IntIntHash_get(h,h->order[i]));return r;}
/* Array#tally on int_array. CRuby returns an Integer-keyed Hash
   mapping each distinct element to its occurrence count. */
static sp_IntIntHash*sp_IntArray_tally_int(sp_IntArray*a){sp_IntIntHash*h=sp_IntIntHash_new();if(!a)return h;for(mrb_int i=0;i<a->len;i++){mrb_int k=a->data[a->start+i];mrb_int c=sp_IntIntHash_has_key(h,k)?sp_IntIntHash_get(h,k):0;sp_IntIntHash_set(h,k,c+1);}return h;}

/* Reuse an existing StrArray for split, avoiding GC alloc.
   Clears a->len and refills.  Substring strings are still malloc'd. */
static inline void sp_str_split_push(sp_StrArray*a,const char*p,size_t n){
  char*r=sp_str_alloc_raw(n+1);
  memcpy(r,p,n);
  r[n]=0;
  sp_StrArray_push(a,r);
}
static void sp_str_split_into(sp_StrArray*a,const char*s,const char*sep){
  SP_GC_ROOT(a);
  SP_GC_ROOT(s);
  SP_GC_ROOT(sep);
  a->len=0;
  if(*s==0)return;
  size_t sl=strlen(sep);
  if(sl==0){
    const char*p=s;
    while(*p){
      int cn=sp_utf8_advance(p);
      sp_str_split_push(a,p,(size_t)cn);
      p+=cn;
    }
    return;
  }
  const char*p=s;
  while(1){
    const char*f=strstr(p,sep);
    if(!f){
      sp_str_split_push(a,p,strlen(p));
      break;
    }
    size_t n=f-p;
    sp_str_split_push(a,p,n);
    p=f+sl;
  }
}
/* Extract the n-th field (0-based) from s split by sep, without
   allocating a full StrArray.  Returns a newly allocated string.
   If the field doesn't exist, returns "". */
static const char*sp_str_field(const char*s,const char*sep,mrb_int n){
  size_t sl=strlen(sep);mrb_int cur=0;const char*p=s;
  if(sl==0)return sp_str_empty;
  while(cur<n){const char*f=strstr(p,sep);if(!f)return sp_str_empty;p=f+sl;cur++;}
  const char*end=strstr(p,sep);size_t len=end?((size_t)(end-p)):strlen(p);
  char*r=sp_str_alloc_raw(len+1);memcpy(r,p,len);r[len]=0;return r;}
/* Count fields in s split by sep (without allocating). */
static mrb_int sp_str_field_count(const char*s,const char*sep){
  if(*s==0)return 0;size_t sl=strlen(sep);if(sl==0)return(mrb_int)strlen(s);
  mrb_int c=1;const char*p=s;while((p=strstr(p,sep))!=NULL){c++;p+=sl;}return c;}
static const char*sp_str_concat(const char*a,const char*b){if(!a)a=sp_str_empty;if(!b)b=sp_str_empty;size_t la=sp_str_byte_len(a),lb=sp_str_byte_len(b);char*r=sp_str_alloc(la+lb);memcpy(r,a,la);memcpy(r+la,b,lb);return r;}
/* Issue #760: NULL src to memcpy is UB. Treat NULL as empty string. */
static const char*sp_str_concat3(const char*a,const char*b,const char*c){if(!a)a="";if(!b)b="";if(!c)c="";size_t la=sp_str_byte_len(a),lb=sp_str_byte_len(b),lc=sp_str_byte_len(c);char*r=sp_str_alloc(la+lb+lc);memcpy(r,a,la);memcpy(r+la,b,lb);memcpy(r+la+lb,c,lc);return r;}
static const char*sp_str_concat4(const char*a,const char*b,const char*c,const char*d){if(!a)a="";if(!b)b="";if(!c)c="";if(!d)d="";size_t la=sp_str_byte_len(a),lb=sp_str_byte_len(b),lc=sp_str_byte_len(c),ld=sp_str_byte_len(d);char*r=sp_str_alloc(la+lb+lc+ld);memcpy(r,a,la);memcpy(r+la,b,lb);memcpy(r+la+lb,c,lc);memcpy(r+la+lb+lc,d,ld);return r;}
/* Concatenate N strings into a single GC-managed buffer. */
/* Issue #760: NULL entries treated as empty strings. */
static const char*sp_str_concat_arr(const char *const *parts,int n){size_t total=0;for(int i=0;i<n;i++)total+=sp_str_byte_len(parts[i]?parts[i]:"");char*r=sp_str_alloc(total);char*p=r;for(int i=0;i<n;i++){const char*s=parts[i]?parts[i]:"";size_t sl=sp_str_byte_len(s);memcpy(p,s,sl);p+=sl;}return r;}
static const char*sp_int_to_s(mrb_int n){char*b=sp_str_alloc_raw(32);int len=snprintf(b,32,"%lld",(long long)n);if(len<0)len=0;sp_str_set_len(b,(size_t)len);return b;}
static const char*sp_int_to_s_base(mrb_int n,mrb_int base){if(base<2||base>36)base=10;char*b=sp_str_alloc_raw(72);char tmp[72];int i=0;int neg=0;uint64_t u;if(n<0){neg=1;u=(uint64_t)(-(n+1))+1;}else{u=(uint64_t)n;}if(u==0){tmp[i++]='0';}else{while(u>0){mrb_int d=u%base;tmp[i++]=d<10?'0'+d:'a'+d-10;u/=base;}}int j=0;if(neg)b[j++]='-';while(i>0)b[j++]=tmp[--i];b[j]=0;sp_str_set_len(b,(size_t)j);return b;}
/* Float#to_s (Ruby semantics): produce the shortest decimal that
   round-trips back to the same double, formatted per CRuby — fixed
   point when the decimal exponent is in [-4, 15], scientific
   (`d.ddde+NN`, two-digit zero-padded) otherwise. NaN, ±Infinity and
   -0.0 match CRuby's spelling. Float#inspect is aliased. */
static const char*sp_float_to_s(mrb_float f){
  if(f!=f){char*r=sp_str_alloc_raw(4);r[0]='N';r[1]='a';r[2]='N';r[3]=0;return r;}
  if(f==HUGE_VAL||f==-HUGE_VAL){if(f<0){char*r=sp_str_alloc_raw(10);memcpy(r,"-Infinity",10);return r;}char*r=sp_str_alloc_raw(9);memcpy(r,"Infinity",9);return r;}
  if(f==0.0){if(signbit(f)){char*r=sp_str_alloc_raw(5);memcpy(r,"-0.0",5);return r;}char*r=sp_str_alloc_raw(4);memcpy(r,"0.0",4);return r;}
  char tmp[64];int p;
  for(p=0;p<=17;p++){snprintf(tmp,sizeof(tmp),"%.*e",p,(double)f);if(strtod(tmp,NULL)==f)break;}
  int neg=(tmp[0]=='-')?1:0;const char*s=tmp+neg;char digits[32];int dlen=0;
  digits[dlen++]=*s++;
  if(*s=='.'){s++;while(*s&&*s!='e'&&*s!='E')digits[dlen++]=*s++;}
  while(*s&&*s!='e'&&*s!='E')s++;
  int exp_val=(*s)?atoi(s+1):0;int decpt=exp_val+1;
  char*out=sp_str_alloc_raw(64);int o=0;
  if(neg)out[o++]='-';
  if(decpt>0&&decpt<=15){
    if(decpt<dlen){memcpy(out+o,digits,decpt);o+=decpt;out[o++]='.';memcpy(out+o,digits+decpt,dlen-decpt);o+=(dlen-decpt);}
    else{memcpy(out+o,digits,dlen);o+=dlen;for(int i=dlen;i<decpt;i++)out[o++]='0';out[o++]='.';out[o++]='0';}
  }else if(decpt<=0&&decpt>-4){
    out[o++]='0';out[o++]='.';for(int i=decpt;i<0;i++)out[o++]='0';memcpy(out+o,digits,dlen);o+=dlen;
  }else{
    out[o++]=digits[0];out[o++]='.';
    if(dlen==1)out[o++]='0';else{memcpy(out+o,digits+1,dlen-1);o+=(dlen-1);}
    out[o++]='e';int e=decpt-1;
    if(e>=0)out[o++]='+';else{out[o++]='-';e=-e;}
    if(e<10){out[o++]='0';out[o++]=(char)('0'+e);}else o+=snprintf(out+o,16,"%d",e);
  }
  out[o]=0;sp_str_set_len(out,(size_t)o);return out;
}
#define sp_float_inspect sp_float_to_s
/* String#inspect: wrap in double quotes and escape \, ", \n, \t, \r,
   plus any non-printable byte as \xNN. Output is always ASCII-safe. */
static const char*sp_str_inspect(const char*s){if(!s){char*r=sp_str_alloc_raw(4);r[0]='n';r[1]='i';r[2]='l';r[3]=0;return r;}size_t sl=sp_str_byte_len(s);size_t cap=sl*4+3;char*r=sp_str_alloc_raw(cap);size_t o=0;r[o++]='"';for(size_t i=0;i<sl;i++){unsigned char c=(unsigned char)s[i];if(c=='\\'||c=='"'){r[o++]='\\';r[o++]=c;}else if(c=='\n'){r[o++]='\\';r[o++]='n';}else if(c=='\t'){r[o++]='\\';r[o++]='t';}else if(c=='\r'){r[o++]='\\';r[o++]='r';}else if(c<0x20||c==0x7f){snprintf(r+o,5,"\\x%02X",c);o+=4;}else{r[o++]=(char)c;}}r[o++]='"';r[o]=0;sp_str_set_len(r,o);return r;}
/* Issue #791: loop to `i < l` and write the NUL terminator explicitly.
   The original `<= l` form worked because sp_str_alloc_raw(l+1) makes
   index l valid, but it's brittle if allocation changes. Issue #797
   adds the NULL guard. */
static const char*sp_str_upcase(const char*s){if(!s)return sp_str_empty;size_t l=strlen(s);char*r=sp_str_alloc_raw(l+1);for(size_t i=0;i<l;i++)r[i]=toupper((unsigned char)s[i]);r[l]=0;return r;}
static const char*sp_str_downcase(const char*s){if(!s)return sp_str_empty;size_t l=strlen(s);char*r=sp_str_alloc_raw(l+1);for(size_t i=0;i<l;i++)r[i]=tolower((unsigned char)s[i]);r[l]=0;return r;}
static const char*sp_str_swapcase(const char*s){if(!s)return sp_str_empty;size_t l=strlen(s);char*r=sp_str_alloc_raw(l+1);for(size_t i=0;i<l;i++){unsigned char c=(unsigned char)s[i];if(isupper(c))r[i]=tolower(c);else if(islower(c))r[i]=toupper(c);else r[i]=s[i];}r[l]=0;return r;}
/* String#undump: reverse of String#dump. The argument must be wrapped in
   double quotes; the escapes dump can emit (\n \t \r \f \v \a \b \e \s \0
   \" \\ \# \xHH \uHHHH \u{...}) are decoded back to bytes. The decoded
   string is never longer than the dumped form, so one buffer suffices. */
static int _sp_hexval(unsigned char d){return (d<='9')?(d-'0'):(tolower(d)-'a'+10);}
static const char*sp_str_undump(const char*s){
  if(!s)return sp_str_empty;
  size_t n=strlen(s);
  if(n<2||s[0]!='"'||s[n-1]!='"'){sp_raise_cls("RuntimeError","invalid dumped string");return sp_str_empty;}
  const char*p=s+1;const char*pe=s+n-1;
  char*out=sp_str_alloc_raw(n+1);size_t oi=0;
  while(p<pe){
    if(*p!='\\'){out[oi++]=*p++;continue;}
    p++;if(p>=pe)break;
    char c=*p++;
    if(c=='n')out[oi++]='\n';else if(c=='t')out[oi++]='\t';else if(c=='r')out[oi++]='\r';
    else if(c=='f')out[oi++]='\f';else if(c=='v')out[oi++]='\v';else if(c=='a')out[oi++]='\a';
    else if(c=='b')out[oi++]='\b';else if(c=='e')out[oi++]='\033';else if(c=='s')out[oi++]=' ';
    else if(c=='0')out[oi++]='\0';else if(c=='\\')out[oi++]='\\';else if(c=='"')out[oi++]='"';
    else if(c=='#')out[oi++]='#';
    else if(c=='x'){int v=0,k=0;while(k<2&&p<pe&&isxdigit((unsigned char)*p)){v=v*16+_sp_hexval((unsigned char)*p);p++;k++;}out[oi++]=(char)v;}
    else if(c=='u'){
      if(p<pe&&*p=='{'){p++;while(p<pe&&*p!='}'){while(p<pe&&*p==' ')p++;uint32_t cp=0;int k=0;while(k<8&&p<pe&&isxdigit((unsigned char)*p)){cp=cp*16+(uint32_t)_sp_hexval((unsigned char)*p);p++;k++;}char enc[4];int el=sp_utf8_encode(cp,enc);for(int j=0;j<el;j++)out[oi++]=enc[j];while(p<pe&&*p==' ')p++;}if(p<pe&&*p=='}')p++;}
      else{uint32_t cp=0;int k=0;while(k<4&&p<pe&&isxdigit((unsigned char)*p)){cp=cp*16+(uint32_t)_sp_hexval((unsigned char)*p);p++;k++;}char enc[4];int el=sp_utf8_encode(cp,enc);for(int j=0;j<el;j++)out[oi++]=enc[j];}
    }
    else out[oi++]=c;
  }
  out[oi]=0;return out;
}
/* Issue #797: NULL guards on receiver + needle for the chunk of
   string functions that read directly into a non-checked strlen. */
static const char*sp_str_delete_prefix(const char*s,const char*p){if(!s)return sp_str_empty;if(!p)return s;size_t sl=strlen(s),pl=strlen(p);if(pl<=sl&&memcmp(s,p,pl)==0){char*r=sp_str_alloc_raw(sl-pl+1);memcpy(r,s+pl,sl-pl+1);return r;}char*r=sp_str_alloc_raw(sl+1);memcpy(r,s,sl+1);return r;}
/* Issue #758: NULL guard + bound the start so a negative result from
   sp_str_index doesn't underflow the source pointer. */
static const char*sp_str_substr(const char*s,mrb_int start,mrb_int len){if(!s||len<=0){char*r=sp_str_alloc_raw(1);r[0]=0;return r;}if(start<0)start=0;char*r=sp_str_alloc_raw(len+1);memcpy(r,s+start,len);r[len]=0;return r;}
static const char*sp_str_delete_suffix(const char*s,const char*p){if(!s)return sp_str_empty;if(!p)return s;size_t sl=strlen(s),pl=strlen(p);if(pl<=sl&&memcmp(s+sl-pl,p,pl)==0){char*r=sp_str_alloc_raw(sl-pl+1);memcpy(r,s,sl-pl);r[sl-pl]=0;return r;}char*r=sp_str_alloc_raw(sl+1);memcpy(r,s,sl+1);return r;}
static const char*sp_str_succ(const char*s){if(!s)return sp_str_empty;size_t l=strlen(s);if(l==0){char*r=sp_str_alloc_raw(1);r[0]=0;return r;}/* Find start of last codepoint */size_t lc=l-1;while(lc>0&&((unsigned char)s[lc]&0xC0)==0x80)lc--;if((unsigned char)s[lc]>=0x80){/* Multibyte tail: increment its codepoint */uint32_t cp;sp_utf8_decode(s+lc,&cp);cp++;char enc[4];int el=sp_utf8_encode(cp,enc);char*r=sp_str_alloc_raw(lc+el+1);memcpy(r,s,lc);memcpy(r+lc,enc,el);r[lc+el]=0;return r;}/* ASCII tail: existing carry logic */char*r=sp_str_alloc_raw(l+2);memcpy(r,s,l+1);mrb_int i=(mrb_int)l-1;while(i>=0){unsigned char c=(unsigned char)r[i];if(c>='0'&&c<'9'){r[i]=c+1;return r;}if(c=='9'){r[i]='0';i--;continue;}if(c>='a'&&c<'z'){r[i]=c+1;return r;}if(c=='z'){r[i]='a';i--;continue;}if(c>='A'&&c<'Z'){r[i]=c+1;return r;}if(c=='Z'){r[i]='A';i--;continue;}r[i]=c+1;return r;}memmove(r+1,r,l+1);if(r[1]=='0')r[0]='1';else if(r[1]=='a')r[0]='a';else if(r[1]=='A')r[0]='A';else r[0]=r[1];return r;}
static const char*sp_gets(void){char buf[4096];if(!fgets(buf,sizeof(buf),stdin))return NULL;size_t l=strlen(buf);char*r=sp_str_alloc_raw(l+1);memcpy(r,buf,l+1);return r;}
static sp_StrArray*sp_readlines(void){sp_StrArray*a=sp_StrArray_new();char buf[4096];while(fgets(buf,sizeof(buf),stdin)){size_t l=strlen(buf);char*r=sp_str_alloc_raw(l+1);memcpy(r,buf,l+1);sp_StrArray_push(a,r);}return a;}
/* strip / lstrip / rstrip. CRuby strips the set "\0\t\n\v\f\r " from the
   ends -- i.e. isspace() plus the NUL byte. Use sp_str_byte_len (not
   strlen) so a heap string carrying an embedded NUL (e.g. from pack /
   concat) is measured and stripped correctly; the result is a
   length-tracked heap string so any interior NUL survives. (A frozen
   literal with an embedded NUL is still truncated at the C level -- that
   needs length-tracked literals, out of scope.) */
static const char*sp_str_strip(const char*s){if(!s)return sp_str_empty;size_t len=sp_str_byte_len(s);size_t a=0;while(a<len&&(isspace((unsigned char)s[a])||s[a]=='\0'))a++;size_t b=len;while(b>a&&(isspace((unsigned char)s[b-1])||s[b-1]=='\0'))b--;size_t n=b-a;char*r=sp_str_alloc(n);memcpy(r,s+a,n);r[n]=0;return r;}
static const char*sp_str_chomp(const char*s){if(!s)return sp_str_empty;size_t l=strlen(s);if(l>=2&&s[l-2]=='\r'&&s[l-1]=='\n')l-=2;else if(l>0&&s[l-1]=='\n')l--;else if(l>0&&s[l-1]=='\r')l--;char*r=sp_str_alloc_raw(l+1);memcpy(r,s,l);r[l]=0;return r;}

/* Issue #881: `"hello!".chomp("!")` strips the explicit separator.
   Empty sep strips any trailing newlines (CRuby paragraph mode).
   NULL sep is caller's responsibility (codegen routes nil to a
   no-op before calling). */
static const char *sp_str_chomp_sep(const char *s, const char *sep) {
  if (!s) return sp_str_empty;
  size_t l = strlen(s);
  if (!sep || !*sep) {
    /* Empty sep = paragraph mode: strip trailing \r\n pairs and
       standalone \n's, but NOT standalone \r's. A trailing \r that
       is not part of a \r\n pair stops the stripping. */
    while (l > 0) {
      if (l >= 2 && s[l-2] == '\r' && s[l-1] == '\n') { l -= 2; continue; }
      if (s[l-1] == '\n') { l--; continue; }
      break;
    }
  } else {
    size_t sl = strlen(sep);
    if (sl <= l && memcmp(s + l - sl, sep, sl) == 0) l -= sl;
  }
  char *r = sp_str_alloc_raw(l + 1);
  memcpy(r, s, l);
  r[l] = 0;
  return r;
}
static const char*sp_str_chop(const char*s){if(!s)return sp_str_empty;size_t l=strlen(s);if(l>0){if(l>=2&&s[l-2]=='\r'&&s[l-1]=='\n')l-=2;else l--;}char*r=sp_str_alloc_raw(l+1);memcpy(r,s,l);r[l]=0;return r;}
static mrb_bool sp_str_include(const char*s,const char*sub){if(!sub)sp_raise_cls("TypeError","no implicit conversion of nil into String");if(!s)return FALSE;return strstr(s,sub)!=NULL;}
static mrb_bool sp_str_start_with(const char*s,const char*p){if(!p)sp_raise_cls("TypeError","no implicit conversion of nil into String");if(!s)return FALSE;return strncmp(s,p,strlen(p))==0;}
static mrb_bool sp_str_end_with(const char*s,const char*suf){if(!suf)sp_raise_cls("TypeError","no implicit conversion of nil into String");if(!s)return FALSE;size_t ls=strlen(s),lsuf=strlen(suf);if(lsuf>ls)return FALSE;return strcmp(s+ls-lsuf,suf)==0;}
static sp_StrArray*sp_str_split(const char*s,const char*sep){
  SP_GC_ROOT(s);
  SP_GC_ROOT(sep);
  sp_StrArray*a=sp_StrArray_new();
  sp_str_split_into(a,s,sep);
  return a;
}
/* Same as sp_str_split but removes trailing empty strings
   (CRuby default limit behavior: split without limit drops
   trailing empties; split(sep, -1) keeps them). */
static sp_StrArray*sp_str_split_drop_trailing(const char*s,const char*sep){sp_StrArray*a=sp_str_split(s,sep);while(a->len>0&&a->data[a->len-1][0]==0)a->len--;return a;}
/* `s.split(sep, n)` with explicit limit. Positive n caps the result
   at n elements: the last element holds the unsplit remainder.
   n == 0 means "no limit" and drops trailing empty strings (same as
   the no-arg default); n < 0 means "no limit" but keeps trailing
   empties. Empty separator works the same as the no-limit path --
   splits into Unicode characters; the limit caps the array.
   Issue #619 puzzle 2. */
static sp_StrArray*sp_str_split_limit(const char*s,const char*sep,mrb_int n){
  if(n==0)return sp_str_split_drop_trailing(s,sep);
  if(n<0)return sp_str_split(s,sep);
  SP_GC_ROOT(s);
  SP_GC_ROOT(sep);
  sp_StrArray*a=sp_StrArray_new();
  SP_GC_ROOT(a);
  if(*s==0)return a;
  size_t sl=strlen(sep);
  if(sl==0){
    const char*p=s;
    mrb_int k=0;
    while(*p&&k<n-1){
      int cn=sp_utf8_advance(p);
      sp_str_split_push(a,p,(size_t)cn);
      p+=cn;
      k++;
    }
    if(*p){
      sp_str_split_push(a,p,strlen(p));
    }
    return a;
  }
  const char*p=s;
  mrb_int k=0;
  while(k<n-1){
    const char*f=strstr(p,sep);
    if(!f)break;
    size_t m=f-p;
    sp_str_split_push(a,p,m);
    p=f+sl;
    k++;
  }
  sp_str_split_push(a,p,strlen(p));
  return a;
}
/* `s.split` / `s.split(nil)` -- whitespace mode: split on runs of
   ASCII whitespace, skip leading whitespace. Issue #507: the no-arg
   form previously emitted `sp_str_split(s, 0)` and segfaulted at
   strlen(NULL). */
static sp_StrArray*sp_str_split_ws(const char*s){
  SP_GC_ROOT(s);
  sp_StrArray*a=sp_StrArray_new();
  SP_GC_ROOT(a);
  const char*p=s;
  while(*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p=='\f'||*p=='\v')p++;
  while(*p){
    const char*start=p;
    while(*p&&!(*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p=='\f'||*p=='\v'))p++;
    size_t n=p-start;
    sp_str_split_push(a,start,n);
    while(*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p=='\f'||*p=='\v')p++;
  }
  return a;
}
/* String#lines: split on \n but PRESERVE the trailing newline on each
   line (CRuby semantics). The last line keeps its terminator if present;
   if absent, it just stops there. Empty string returns an empty array.
   `end` is computed once at entry so a string with no newlines avoids
   a redundant strlen call on the trailing piece. */
static sp_StrArray*sp_str_lines(const char*s){sp_StrArray*a=sp_StrArray_new();if(*s==0)return a;const char*end=s+strlen(s);const char*p=s;while(p<end){const char*nl=strchr(p,'\n');size_t n=nl?(size_t)(nl-p+1):(size_t)(end-p);char*r=sp_str_alloc_raw(n+1);memcpy(r,p,n);r[n]=0;sp_StrArray_push(a,r);if(!nl)break;p=nl+1;}return a;}
static sp_StrArray*sp_str_lines_chomp(const char*s){sp_StrArray*a=sp_StrArray_new();if(*s==0)return a;const char*end=s+strlen(s);const char*p=s;while(p<end){const char*nl=strchr(p,'\n');size_t n=nl?(size_t)(nl-p):(size_t)(end-p);if(nl&&nl>s&&nl[-1]=='\r')n--;char*r=sp_str_alloc_raw(n+1);memcpy(r,p,n);r[n]=0;sp_StrArray_push(a,r);if(!nl)break;p=nl+1;}return a;}
/* Issue #827: gsub previously returned a raw malloc buffer. The GC's
   sp_mark_string writes byte[-1] = 0xfc, which on a raw malloc buffer
   clobbers malloc metadata. Build into a scratch buffer, then copy
   into a sp_str_alloc'd buffer that has the GC marker byte. */
/* Issue #850: empty pattern inserts the replacement between every
   character (and at the start). CRuby walks codepoint-by-codepoint;
   spinel's gsub is byte-oriented so we mirror that with UTF-8 stride
   to keep multi-byte characters intact. */
static const char*sp_str_gsub(const char*s,const char*pat,const char*rep){
  if(!s)return sp_str_empty;
  if(!pat||!rep)return s;
  size_t pl=strlen(pat),rl=strlen(rep),sl=strlen(s);
  if(pl==0){
    /* Empty pattern: insert rep between every codepoint + at start/end.
       Result size: (chars+1) * rl + sl. */
    size_t cap=sl+rl*(sl+1)+1;
    char*out=(char*)malloc(cap);
    size_t ol=0;
    memcpy(out+ol,rep,rl); ol+=rl;
    const char*p=s;
    while(*p){
      int n=sp_utf8_advance(p);
      memcpy(out+ol,p,n); ol+=n;
      memcpy(out+ol,rep,rl); ol+=rl;
      p+=n;
    }
    out[ol]=0;
    char*r=sp_str_alloc(ol); memcpy(r,out,ol+1); free(out); return r;
  }
  size_t cap=sl*2+1;
  char*out=(char*)malloc(cap);
  size_t ol=0;
  const char*p=s;
  while(*p){
    const char*f=strstr(p,pat);
    if(!f){size_t n=strlen(p);if(ol+n>=cap){cap=(ol+n)*2+1;out=(char*)realloc(out,cap);}memcpy(out+ol,p,n);ol+=n;break;}
    size_t n=f-p;
    if(ol+n+rl>=cap){cap=(ol+n+rl)*2+1;out=(char*)realloc(out,cap);}
    memcpy(out+ol,p,n);ol+=n;
    memcpy(out+ol,rep,rl);ol+=rl;
    p=f+pl;
  }
  out[ol]=0;char*r=sp_str_alloc(ol);memcpy(r,out,ol+1);free(out);return r;
}
/* Returns a *character* offset (codepoint index), not a byte offset. */
/* Issue #759: NULL sub to strstr is UB. Issue #847: nil sub raises
   TypeError per MRI rather than silently returning -1 (would conflate
   "nil arg" with "not found"). Self NULL stays -1 -- shouldn't happen
   but keep defensive. */
static mrb_int sp_str_index(const char*s,const char*sub){if(!s)return -1;if(!sub)sp_raise_cls("TypeError","no implicit conversion of nil into String");const char*f=strstr(s,sub);if(!f)return -1;mrb_int n=0;const char*p=s;while(p<f){p+=sp_utf8_advance(p);n++;}return n;}
/* `s.index(sub, start)` -- search starts at codepoint index `start`.
   Negative start counts back from the end of the string. Returns the
   absolute codepoint offset of the match (not relative to start), or
   -1 when no match exists at or after start. Out-of-range starts
   (start > length) return -1, matching CRuby. The codepoint counter
   begins at `start` and walks from `s + boff`, so the "walk all
   matches" loop is O(N) total rather than O(N^2). */
static mrb_int sp_str_index_from(const char*s,const char*sub,mrb_int start){mrb_int cl=sp_str_length(s);if(start<0)start+=cl;if(start<0)start=0;if(start>cl)return -1;size_t boff=sp_utf8_byte_offset(s,start);const char*f=strstr(s+boff,sub);if(!f)return -1;mrb_int n=start;const char*p=s+boff;while(p<f){p+=sp_utf8_advance(p);n++;}return n;}
static mrb_int sp_str_rindex(const char*s,const char*sub){if(!sub)sp_raise_cls("TypeError","no implicit conversion of nil into String");size_t sl=strlen(sub);if(sl==0)return sp_str_length(s);const char*last=NULL;const char*p=s;while((p=strstr(p,sub))){last=p;p++;}if(!last)return -1;mrb_int n=0;const char*q=s;while(q<last){q+=sp_utf8_advance(q);n++;}return n;}
/* `s.rindex(sub, pos)` — find the rightmost occurrence of sub at or
   before codepoint index pos. Walks all matches and keeps the last
   one whose codepoint index <= pos. */
static mrb_int sp_str_rindex_from(const char*s,const char*sub,mrb_int pos){if(!s)return SP_INT_NIL;if(!sub)sp_raise_cls("TypeError","no implicit conversion of nil into String");mrb_int cl=sp_str_length(s);if(pos<0)pos=cl+pos;if(pos<0)return SP_INT_NIL;size_t sl=strlen(sub);if(sl==0){if(pos>=cl)return cl;return pos;}const char*p=s;mrb_int best=-1;const char*r=s;mrb_int cur_n=0;while((p=strstr(p,sub))!=NULL){while(r<p){r+=sp_utf8_advance(r);cur_n++;}if(cur_n>pos)break;best=cur_n;p++;}return best<0?SP_INT_NIL:best;}

static char sp_char_cache[256][3];
static int sp_char_cache_init = 0;
/* start/len are codepoint indices/counts. */
static const char*sp_str_sub_range(const char*s,mrb_int start,mrb_int len){mrb_int cl=sp_str_length(s);if(start<0)start+=cl;if(start<0)start=0;if(start>=cl||len<=0){return &("\xff" "")[1];}if(start+len>cl)len=cl-start;size_t boff=sp_utf8_byte_offset(s,start);size_t blen_total=sp_str_byte_len(s);size_t bp=boff;mrb_int rem=len;while(rem>0&&bp<blen_total){bp+=sp_utf8_advance(s+bp);rem--;}if(bp>blen_total)bp=blen_total;size_t bend=bp;size_t blen=bend-boff;if(len==1&&blen==1){unsigned char c=(unsigned char)s[boff];if(c!=0){if(!sp_char_cache_init){for(int i=0;i<256;i++){sp_char_cache[i][0]=(char)0xff;sp_char_cache[i][1]=(char)i;sp_char_cache[i][2]=0;}sp_char_cache_init=1;}return &sp_char_cache[c][1];}}char*r=sp_str_alloc_raw(blen+1);memcpy(r,s+boff,blen);r[blen]=0;return r;}
/* Single-character form of `s[i]`. Returns NULL on out-of-bounds to
   match CRuby's `"hello"[20] -> nil`. The two-arg `s[i, len]` /
   range forms keep returning "" on OOB via sp_str_sub_range; only
   the bare single-int index aliases here. Issue #619 puzzle 3. */
static const char*sp_str_char_at_or_nil(const char*s,mrb_int i){mrb_int cl=sp_str_length(s);if(i<0)i+=cl;if(i<0||i>=cl)return NULL;return sp_str_sub_range(s,i,1);}
/* String#byteslice(start,len): byte-indexed (unlike the char-indexed
   sp_str_sub_range). Negative start counts from the byte length. Out of
   range yields the empty string, matching slice's "" rather than CRuby nil. */
static const char*sp_str_byteslice(const char*s,mrb_int start,mrb_int len){mrb_int bl=(mrb_int)sp_str_byte_len(s);if(start<0)start+=bl;if(start<0||start>bl||len<0){return &("\xff" "")[1];}if(start+len>bl)len=bl-start;if(len<=0){return &("\xff" "")[1];}char*r=sp_str_alloc_raw(len+1);memcpy(r,s+start,len);r[len]=0;return r;}
/* String#ascii_only?: 1 iff every byte is in the 7-bit ASCII range. */
static int sp_str_ascii_only(const char*s){mrb_int bl=(mrb_int)sp_str_byte_len(s);for(mrb_int i=0;i<bl;i++){if((unsigned char)s[i]>=0x80)return 0;}return 1;}
/* Char-indexed variant; the second arg used to be a hoisted byte length, now a
   hoisted codepoint count.  We don't need it for correctness, but keeping the
   ABI lets callers pass it without a wrapper. */
static const char*sp_str_sub_range_len(const char*s,mrb_int cl,mrb_int start,mrb_int len){if(start<0)start+=cl;if(start<0)start=0;if(start>=cl||len<=0){return &("\xff" "")[1];}if(start+len>cl)len=cl-start;size_t boff=sp_utf8_byte_offset(s,start);size_t blen_total=sp_str_byte_len(s);size_t bp=boff;mrb_int rem=len;while(rem>0&&bp<blen_total){bp+=sp_utf8_advance(s+bp);rem--;}if(bp>blen_total)bp=blen_total;size_t bend=bp;size_t blen=bend-boff;if(len==1&&blen==1){unsigned char c=(unsigned char)s[boff];if(c!=0){if(!sp_char_cache_init){for(int i=0;i<256;i++){sp_char_cache[i][0]=(char)0xff;sp_char_cache[i][1]=(char)i;sp_char_cache[i][2]=0;}sp_char_cache_init=1;}return &sp_char_cache[c][1];}}char*r=sp_str_alloc_raw(blen+1);memcpy(r,s+boff,blen);r[blen]=0;return r;}
/* String s[start..end] / s[start...end] with possibly negative
   endpoints. Mirrors sp_IntArray_slice_range; issue #496. */
static const char*sp_str_sub_range_r(const char*s,mrb_int start,mrb_int end_,mrb_int excl){mrb_int cl=sp_str_length(s);if(end_<0)end_+=cl;if(start<0)start+=cl;mrb_int n=end_-start+(excl?0:1);if(n<0||start<0)n=0;return sp_str_sub_range_len(s,cl,start,n);}
static const char*sp_str_sub_range_len_r(const char*s,mrb_int cl,mrb_int start,mrb_int end_,mrb_int excl){if(end_<0)end_+=cl;if(start<0)start+=cl;mrb_int n=end_-start+(excl?0:1);if(n<0||start<0)n=0;return sp_str_sub_range_len(s,cl,start,n);}
const char*sp_sprintf(const char*fmt,...){char _sp_tmp[4096];va_list ap;va_start(ap,fmt);int _sp_n=vsnprintf(_sp_tmp,sizeof(_sp_tmp),fmt,ap);va_end(ap);if(_sp_n<0)_sp_n=0;char*b=sp_str_alloc((size_t)_sp_n);if(_sp_n<(int)sizeof(_sp_tmp)){memcpy(b,_sp_tmp,(size_t)_sp_n);}else{/* result didn't fit the stack temp; re-render at full width (sp_str_alloc gives _sp_n bytes + NUL) so long string interpolations aren't truncated. re-arm the va_list rather than va_copy so the common fast path pays nothing */va_start(ap,fmt);vsnprintf(b,(size_t)_sp_n+1,fmt,ap);va_end(ap);}return b;}
/* Use a temp pointer for realloc so the original buffer is not leaked
   on allocation failure. Match the perror+exit pattern used elsewhere
   (see sp_IntArray_replace) instead of returning a partial result. */
static const char*sp_str_format_strarr(const char*fmt,sp_StrArray*a){size_t cap=strlen(fmt)+64;char*buf=(char*)malloc(cap);if(!buf){perror("malloc");exit(1);}size_t out=0;mrb_int idx=0;const char*p=fmt;while(*p){if(*p=='%'){if(p[1]=='s'){const char*s=(idx<a->len)?a->data[idx]:"";size_t sl=strlen(s);if(out+sl>=cap){size_t nc=(out+sl)*2+1;char*nb=(char*)realloc(buf,nc);if(!nb){free(buf);perror("realloc");exit(1);}buf=nb;cap=nc;}memcpy(buf+out,s,sl);out+=sl;idx++;p+=2;}else if(p[1]=='%'){if(out+1>=cap){size_t nc=cap*2;char*nb=(char*)realloc(buf,nc);if(!nb){free(buf);perror("realloc");exit(1);}buf=nb;cap=nc;}buf[out++]='%';p+=2;}else{if(out+1>=cap){size_t nc=cap*2;char*nb=(char*)realloc(buf,nc);if(!nb){free(buf);perror("realloc");exit(1);}buf=nb;cap=nc;}buf[out++]=*p++;}}else{if(out+1>=cap){size_t nc=cap*2;char*nb=(char*)realloc(buf,nc);if(!nb){free(buf);perror("realloc");exit(1);}buf=nb;cap=nc;}buf[out++]=*p++;}}buf[out]=0;char*r=sp_str_alloc(out);memcpy(r,buf,out);free(buf);return r;}

static const char*sp_str_reverse(const char*s){if(!s)return sp_str_empty;size_t bl=strlen(s);char*r=sp_str_alloc_raw(bl+1);size_t end=bl;const char*p=s;while(*p){int cn=sp_utf8_advance(p);end-=cn;memcpy(r+end,p,cn);p+=cn;}r[bl]=0;return r;}
static const char*sp_str_sub(const char*s,const char*pat,const char*rep){if(!s)return sp_str_empty;if(!pat||!rep)return s;const char*f=strstr(s,pat);if(!f)return s;size_t pl=strlen(pat),rl=strlen(rep),sl=strlen(s);char*r=sp_str_alloc_raw(sl-pl+rl+1);size_t n=f-s;memcpy(r,s,n);memcpy(r+n,rep,rl);memcpy(r+n+rl,f+pl,sl-n-pl+1);return r;}
static const char*sp_str_capitalize(const char*s){if(!s)return sp_str_empty;size_t l=strlen(s);char*r=sp_str_alloc_raw(l+1);for(size_t i=0;i<=l;i++)r[i]=tolower((unsigned char)s[i]);if(l>0)r[0]=toupper((unsigned char)r[0]);return r;}
static mrb_int sp_str_count(const char*s,const char*chars){if(!chars)sp_raise_cls("TypeError","no implicit conversion of nil into String");int negate=0;const char*csp=chars;if(*csp=='^'&&*(csp+1)){negate=1;csp++;}size_t setn;uint32_t*set=sp_utf8_decode_charset(csp,&setn);mrb_int c=0;const char*p=s;while(*p){uint32_t cp;p+=sp_utf8_decode(p,&cp);int in_set=sp_utf8_set_has(set,setn,cp);if(negate)in_set=!in_set;if(in_set)c++;}free(set);return c;}
/* String#count with multiple args: intersection of charsets.
   Each additional arg further restricts which chars to count.
   sp_str_count_n(s, args[], n) computes the intersection. */
static mrb_int sp_str_count_n(const char*s,const char**chars,mrb_int n){if(n<=0)return 0;size_t*setns=(size_t*)malloc(n*sizeof(size_t));uint32_t**sets=(uint32_t**)malloc(n*sizeof(uint32_t*));int*negs=(int*)malloc(n*sizeof(int));for(mrb_int i=0;i<n;i++){if(!chars[i])sp_raise_cls("TypeError","no implicit conversion of nil into String");const char*cs=chars[i];negs[i]=0;if(*cs=='^'&&*(cs+1)){negs[i]=1;cs++;}sets[i]=sp_utf8_decode_charset(cs,&setns[i]);}mrb_int c=0;const char*p=s;while(*p){uint32_t cp;p+=sp_utf8_decode(p,&cp);int all=1;for(mrb_int i=0;i<n;i++){int in_set=sp_utf8_set_has(sets[i],setns[i],cp);if(negs[i])in_set=!in_set;if(!in_set){all=0;break;}}if(all)c++;}for(mrb_int i=0;i<n;i++)free(sets[i]);free(sets);free(setns);free(negs);return c;}
/* Issue #800: clamp l*n so a malicious input can't allocate a tiny
   buffer through size_t overflow. */
/* Issue #836: bound the multiplier so a wildly oversized request
   raises ArgumentError rather than segfaulting when malloc returns
   NULL and memcpy walks it. 1 GiB cap covers realistic use. */
static const char*sp_str_repeat(const char*s,mrb_int n){
  if(n<0) sp_raise_cls("ArgumentError","negative argument");
  if(!s||n<=0)return sp_str_empty;
  size_t l=strlen(s);
  if(l==0) return sp_str_empty;
  if((size_t)n>SIZE_MAX/l) sp_raise_cls("ArgumentError","string size too big");
  size_t total=(size_t)n*l;
  if(total>(size_t)(1u<<30)) sp_raise_cls("ArgumentError","string size too big");
  char*r=sp_str_alloc_raw(total+1);
  for(mrb_int i=0;i<n;i++)memcpy(r+l*i,s,l);
  r[total]=0;
  return r;
}
static sp_IntArray*sp_str_bytes(const char*s){sp_IntArray*a=sp_IntArray_new();if(!s)return a;size_t n=sp_str_byte_len(s);for(size_t i=0;i<n;i++)sp_IntArray_push(a,(mrb_int)(unsigned char)s[i]);return a;}
/* Issue #903: String#codepoints -- one IntArray entry per UTF-8
   codepoint (not byte). Replacement-character behaviour mirrors
   sp_utf8_decode (returns the leading byte for malformed seqs). */
static sp_IntArray*sp_str_codepoints(const char*s){sp_IntArray*a=sp_IntArray_new();if(!s)return a;const char*p=s;while(*p){uint32_t cp;int n=sp_utf8_decode(p,&cp);sp_IntArray_push(a,(mrb_int)cp);p+=n;}return a;}
/* Issue #798: guard NULL inputs (CRuby treats nil/no-op gracefully). */
static const char*sp_str_tr(const char*s,const char*from,const char*to){if(!s)return sp_str_empty;if(!from||!to)return s;int negate=0;const char*fp=from;if(*fp=='^'&&*(fp+1)){negate=1;fp++;}size_t fn,tn;uint32_t*fcps=sp_utf8_decode_charset(fp,&fn);uint32_t*tcps=sp_utf8_decode_charset(to,&tn);size_t bl=strlen(s);size_t cap=bl*4+1;char*buf=(char*)malloc(cap);size_t n=0;const char*p=s;while(*p){uint32_t cp;int cn=sp_utf8_decode(p,&cp);size_t mi=fn;for(size_t j=0;j<fn;j++)if(fcps[j]==cp){mi=j;break;}int in_set=(mi<fn);if(negate)in_set=!in_set;if(in_set&&tn>0){uint32_t rep=negate?tcps[tn-1]:(mi<tn?tcps[mi]:tcps[tn-1]);n+=sp_utf8_encode(rep,buf+n);}else if(in_set){}else{memcpy(buf+n,p,cn);n+=cn;}p+=cn;}buf[n]=0;char*r=sp_str_alloc(n);memcpy(r,buf,n+1);free(buf);free(fcps);free(tcps);return r;}
/* Issue #902: String#tr_s -- translate AND squeeze consecutive
   identical results into one. Walks codepoint-by-codepoint and
   collapses adjacent duplicates only among the translated bytes
   (untranslated runs keep their original characters). */
static const char*sp_str_tr_s(const char*s,const char*from,const char*to){
  if(!s)return sp_str_empty;
  if(!from||!to)return s;
  int negate=0;const char*fp=from;
  if(*fp=='^'&&*(fp+1)){negate=1;fp++;}
  size_t fn,tn;
  uint32_t*fcps=sp_utf8_decode_charset(fp,&fn);
  uint32_t*tcps=sp_utf8_decode_charset(to,&tn);
  size_t bl=strlen(s);
  size_t cap=bl*4+1;
  char*buf=(char*)malloc(cap);
  size_t n=0;
  const char*p=s;
  uint32_t last_emit=0; int has_last=0; int last_was_translated=0;
  while(*p){
    uint32_t cp; int cn=sp_utf8_decode(p,&cp);
    size_t mi=fn;
    for(size_t j=0;j<fn;j++)if(fcps[j]==cp){mi=j;break;}
    int in_set=(mi<fn);
    if(negate)in_set=!in_set;
    uint32_t emit_cp;
    int translated=0;
    if(in_set){
      if(tn>0){
        emit_cp=negate?tcps[tn-1]:(mi<tn?tcps[mi]:tcps[tn-1]);
        translated=1;
      } else {
        p+=cn; continue;
      }
    } else {
      emit_cp=cp;
      translated=0;
    }
    /* Squeeze only when both the previous and current emit were
       translated, AND the emitted codepoints match. */
    if(has_last && last_was_translated && translated && last_emit==emit_cp){
      /* skip */
    } else {
      n+=sp_utf8_encode(emit_cp,buf+n);
      last_emit=emit_cp;
      has_last=1;
      last_was_translated=translated;
    }
    p+=cn;
  }
  buf[n]=0;
  char*r=sp_str_alloc(n);
  memcpy(r,buf,n+1);
  free(buf); free(fcps); free(tcps);
  return r;
}
static const char*sp_str_delete(const char*s,const char*chars){if(!s)return sp_str_empty;if(!chars)return s;int negate=0;const char*csp=chars;if(*csp=='^'&&*(csp+1)){negate=1;csp++;}size_t setn;uint32_t*set=sp_utf8_decode_charset(csp,&setn);size_t bl=strlen(s);char*r=sp_str_alloc_raw(bl+1);size_t n=0;const char*p=s;while(*p){uint32_t cp;int cn=sp_utf8_decode(p,&cp);int in_set=sp_utf8_set_has(set,setn,cp);if(negate)in_set=!in_set;if(!in_set){memcpy(r+n,p,cn);n+=cn;}p+=cn;}r[n]=0;sp_str_set_len(r,n);free(set);return r;}
/* Issue #921: shrink the heap-string header length to match the
   squeezed payload — the alloc gives bl+1 bytes, the squeezed
   write fills n<=bl, leaving the header's stored length stale.
   `bytes` / `length` consult the header (not strlen), so callers
   would see the alloc size and trailing NULs. */
static const char*sp_str_squeeze(const char*s){if(!s)return sp_str_empty;size_t bl=strlen(s);char*r=sp_str_alloc_raw(bl+1);size_t n=0;uint32_t prev=0xFFFFFFFFu;const char*p=s;while(*p){uint32_t cp;int cn=sp_utf8_decode(p,&cp);if(cp!=prev){memcpy(r+n,p,cn);n+=cn;prev=cp;}p+=cn;}r[n]=0;sp_str_set_len(r,n);return r;}
/* String#squeeze(chars) — only squeeze chars listed in the charset
   (same charset syntax as tr: a-z, ^x, etc.). Consecutive runs of
   non-listed chars pass through untouched. */
static const char*sp_str_squeeze_chars(const char*s,const char*cs){if(!s)return sp_str_empty;if(!cs||!*cs)return sp_str_squeeze(s);int negate=0;const char*csp=cs;if(*csp=='^'&&*(csp+1)){negate=1;csp++;}size_t fn;uint32_t*fcps=sp_utf8_decode_charset(csp,&fn);size_t bl=strlen(s);char*r=sp_str_alloc_raw(bl+1);size_t n=0;uint32_t prev=0xFFFFFFFFu;const char*p=s;while(*p){uint32_t cp;int cn=sp_utf8_decode(p,&cp);int in_set=0;for(size_t j=0;j<fn;j++)if(fcps[j]==cp){in_set=1;break;}if(negate)in_set=!in_set;if(!in_set){memcpy(r+n,p,cn);n+=cn;prev=0xFFFFFFFFu;}else if(cp!=prev){memcpy(r+n,p,cn);n+=cn;prev=cp;}p+=cn;}r[n]=0;sp_str_set_len(r,n);free(fcps);return r;}
/* Multi-arg delete/squeeze: delete (or squeeze runs of) characters that
   are in the INTERSECTION of all n charset args, mirroring
   sp_str_count_n. Each arg is a charset spec (^negation, a-z ranges). */
static const char*sp_str_delete_n(const char*s,const char**chars,mrb_int n){if(!s)return sp_str_empty;if(n<=0)return s;size_t*setns=(size_t*)malloc(n*sizeof(size_t));uint32_t**sets=(uint32_t**)malloc(n*sizeof(uint32_t*));int*negs=(int*)malloc(n*sizeof(int));for(mrb_int i=0;i<n;i++){if(!chars[i])sp_raise_cls("TypeError","no implicit conversion of nil into String");const char*cs=chars[i];negs[i]=0;if(*cs=='^'&&*(cs+1)){negs[i]=1;cs++;}sets[i]=sp_utf8_decode_charset(cs,&setns[i]);}size_t bl=strlen(s);char*r=sp_str_alloc_raw(bl+1);size_t m=0;const char*p=s;while(*p){uint32_t cp;int cn=sp_utf8_decode(p,&cp);int all=1;for(mrb_int i=0;i<n;i++){int in_set=sp_utf8_set_has(sets[i],setns[i],cp);if(negs[i])in_set=!in_set;if(!in_set){all=0;break;}}if(!all){memcpy(r+m,p,cn);m+=cn;}p+=cn;}r[m]=0;sp_str_set_len(r,m);for(mrb_int i=0;i<n;i++)free(sets[i]);free(sets);free(setns);free(negs);return r;}
static const char*sp_str_squeeze_n(const char*s,const char**chars,mrb_int n){if(!s)return sp_str_empty;if(n<=0)return sp_str_squeeze(s);size_t*setns=(size_t*)malloc(n*sizeof(size_t));uint32_t**sets=(uint32_t**)malloc(n*sizeof(uint32_t*));int*negs=(int*)malloc(n*sizeof(int));for(mrb_int i=0;i<n;i++){if(!chars[i])sp_raise_cls("TypeError","no implicit conversion of nil into String");const char*cs=chars[i];negs[i]=0;if(*cs=='^'&&*(cs+1)){negs[i]=1;cs++;}sets[i]=sp_utf8_decode_charset(cs,&setns[i]);}size_t bl=strlen(s);char*r=sp_str_alloc_raw(bl+1);size_t m=0;uint32_t prev=0xFFFFFFFFu;const char*p=s;while(*p){uint32_t cp;int cn=sp_utf8_decode(p,&cp);int all=1;for(mrb_int i=0;i<n;i++){int in_set=sp_utf8_set_has(sets[i],setns[i],cp);if(negs[i])in_set=!in_set;if(!in_set){all=0;break;}}if(!all){memcpy(r+m,p,cn);m+=cn;prev=0xFFFFFFFFu;}else if(cp!=prev){memcpy(r+m,p,cn);m+=cn;prev=cp;}p+=cn;}r[m]=0;sp_str_set_len(r,m);for(mrb_int i=0;i<n;i++)free(sets[i]);free(sets);free(setns);free(negs);return r;}

/* Forward decl from sp_crypto.h (libspinel_rt.a). Used by
   sp_str_crypt below to provide a deterministic crypt-like
   hash without dragging in libc crypt(3). */
const char *sp_crypto_hmac_sha256_b64url(const char *key, const char *msg);

/* String#crypt — Spinel's crypt is NOT the libc DES crypt. It
   returns `salt[0..1] || hmac_sha256(salt, password)[0..10]` as
   a 13-char string (same length as DES crypt, deterministic,
   stronger primitive). CRuby's spec says String#crypt is impl-
   defined and "should not be used for security"; this matches
   that contract while keeping outputs reproducible across
   spinel builds. Short salts get padded with '.' so the result
   still has the canonical first-2-chars-are-salt shape. */
static const char *sp_str_crypt(const char *s, const char *salt) {
  if (!salt) salt = "";
  char salt2[3];
  salt2[0] = salt[0] ? salt[0] : '.';
  salt2[1] = (salt[0] && salt[1]) ? salt[1] : '.';
  salt2[2] = 0;
  const char *digest = sp_crypto_hmac_sha256_b64url(salt2, s ? s : "");
  char *r = sp_str_alloc(13);
  r[0] = salt2[0];
  r[1] = salt2[1];
  for (int i = 0; i < 11; i++) {
    char c = digest[i];
    /* Map b64url's `-`/`_` to crypt-alphabet `.`/`/` so the
       output stays in `[./0-9A-Za-z]` like the historical
       crypt result. */
    if (c == '-') c = '.';
    else if (c == '_') c = '/';
    r[2 + i] = c;
  }
  r[13] = 0;
  sp_str_set_len(r, 13);
  return r;
}

/* String#scrub — walk the bytes; for each valid UTF-8 lead +
   continuation sequence, copy through. For invalid bytes (lone
   continuation, truncated multi-byte, overlong, etc.), emit the
   replacement string and skip one byte. NULL replacement uses
   U+FFFD (3 UTF-8 bytes: EF BF BD), matching CRuby. */
static const char *sp_str_scrub(const char *s, const char *repl) {
  if (!s) return sp_str_empty;
  static const char fffd[] = "\xEF\xBF\xBD";
  const char *r = repl ? repl : fffd;
  size_t rlen = strlen(r);
  size_t bl = sp_str_byte_len(s);
  size_t cap = bl + 64;
 /* malloc scratch (grown with realloc on invalid-byte runs); the final
    string is emitted at the exact length below. */
  char *out = (char *)malloc(cap);
  size_t olen = 0;
  size_t i = 0;
  while (i < bl) {
    unsigned char c = (unsigned char)s[i];
    int expected = sp_utf8_char_len(c);
    int valid = 1;
    if (expected == 1) {
      if (c >= 0x80) valid = 0;
    } else {
      if (i + (size_t)expected > bl) valid = 0;
      else {
        for (int k = 1; k < expected; k++) {
          if (((unsigned char)s[i + k] & 0xC0) != 0x80) { valid = 0; break; }
        }
      }
    }
    if (valid) {
      if (olen + (size_t)expected + 1 >= cap) { cap = (olen + expected) * 2 + 64; out = (char*)realloc(out, cap); }
      memcpy(out + olen, s + i, (size_t)expected);
      olen += (size_t)expected;
      i += (size_t)expected;
    } else {
      if (olen + rlen + 1 >= cap) { cap = (olen + rlen) * 2 + 64; out = (char*)realloc(out, cap); }
      memcpy(out + olen, r, rlen);
      olen += rlen;
      i += 1;
    }
  }
  char *res = sp_str_alloc(olen);
  memcpy(res, out, olen);
  free(out);
  return res;
}
static const char*sp_str_ljust(const char*s,mrb_int w){if(!s)return sp_str_empty;mrb_int cl=sp_str_length(s);if(cl>=w)return s;size_t bl=strlen(s);size_t pad=(size_t)(w-cl);char*r=sp_str_alloc_raw(bl+pad+1);memcpy(r,s,bl);memset(r+bl,' ',pad);r[bl+pad]=0;return r;}
static const char*sp_str_rjust(const char*s,mrb_int w){if(!s)return sp_str_empty;mrb_int cl=sp_str_length(s);if(cl>=w)return s;size_t bl=strlen(s);size_t pad=(size_t)(w-cl);char*r=sp_str_alloc_raw(bl+pad+1);memset(r,' ',pad);memcpy(r+pad,s,bl);r[bl+pad]=0;return r;}
static const char*sp_str_center(const char*s,mrb_int w){if(!s)return sp_str_empty;mrb_int cl=sp_str_length(s);if(cl>=w)return s;size_t bl=strlen(s);mrb_int pad=w-cl;mrb_int left=pad/2;mrb_int right=pad-left;char*r=sp_str_alloc_raw(bl+pad+1);memset(r,' ',left);memcpy(r+left,s,bl);memset(r+left+bl,' ',right);r[bl+pad]=0;return r;}
static const char*sp_str_ljust2(const char*s,mrb_int w,const char*pad){mrb_int cl=sp_str_length(s);if(cl>=w)return s;size_t bl=strlen(s);size_t pn;uint32_t*pcps=sp_utf8_decode_all(pad,&pn);if(pn==0){free(pcps);char*r=sp_str_alloc_raw(bl+1);memcpy(r,s,bl+1);return r;}mrb_int need=w-cl;size_t padb=0;for(mrb_int i=0;i<need;i++){char tmp[4];padb+=sp_utf8_encode(pcps[i%pn],tmp);}char*r=sp_str_alloc_raw(bl+padb+1);memcpy(r,s,bl);size_t n=bl;for(mrb_int i=0;i<need;i++)n+=sp_utf8_encode(pcps[i%pn],r+n);r[n]=0;free(pcps);return r;}
static const char*sp_str_rjust2(const char*s,mrb_int w,const char*pad){mrb_int cl=sp_str_length(s);if(cl>=w)return s;size_t bl=strlen(s);size_t pn;uint32_t*pcps=sp_utf8_decode_all(pad,&pn);if(pn==0){free(pcps);char*r=sp_str_alloc_raw(bl+1);memcpy(r,s,bl+1);return r;}mrb_int need=w-cl;size_t padb=0;for(mrb_int i=0;i<need;i++){char tmp[4];padb+=sp_utf8_encode(pcps[i%pn],tmp);}char*r=sp_str_alloc_raw(bl+padb+1);size_t n=0;for(mrb_int i=0;i<need;i++)n+=sp_utf8_encode(pcps[i%pn],r+n);memcpy(r+n,s,bl);r[n+bl]=0;free(pcps);return r;}
static const char*sp_str_center2(const char*s,mrb_int w,const char*pad){mrb_int cl=sp_str_length(s);if(cl>=w)return s;size_t bl=strlen(s);size_t pn;uint32_t*pcps=sp_utf8_decode_all(pad,&pn);if(pn==0){free(pcps);char*r=sp_str_alloc_raw(bl+1);memcpy(r,s,bl+1);return r;}mrb_int pd=w-cl;mrb_int left=pd/2;mrb_int right=pd-left;size_t leftb=0,rightb=0;{char tmp[4];for(mrb_int i=0;i<left;i++)leftb+=sp_utf8_encode(pcps[i%pn],tmp);for(mrb_int i=0;i<right;i++)rightb+=sp_utf8_encode(pcps[i%pn],tmp);}char*r=sp_str_alloc_raw(leftb+bl+rightb+1);size_t n=0;for(mrb_int i=0;i<left;i++)n+=sp_utf8_encode(pcps[i%pn],r+n);memcpy(r+n,s,bl);n+=bl;for(mrb_int i=0;i<right;i++)n+=sp_utf8_encode(pcps[i%pn],r+n);r[n]=0;free(pcps);return r;}
static const char*sp_str_lstrip(const char*s){if(!s)return sp_str_empty;size_t len=sp_str_byte_len(s);size_t a=0;while(a<len&&(isspace((unsigned char)s[a])||s[a]=='\0'))a++;size_t n=len-a;char*r=sp_str_alloc(n);memcpy(r,s+a,n);r[n]=0;return r;}
static const char*sp_str_rstrip(const char*s){if(!s)return sp_str_empty;size_t len=sp_str_byte_len(s);size_t b=len;while(b>0&&(isspace((unsigned char)s[b-1])||s[b-1]=='\0'))b--;char*r=sp_str_alloc(b);memcpy(r,s,b);r[b]=0;return r;}
static const char*sp_str_dup(const char*s){if(!s)return NULL;size_t l=strlen(s);char*r=sp_str_alloc_raw(l+1);memcpy(r,s,l+1);return r;}

/* String#setbyte: mutate s[i] = v in place. Spinel adopts
   `# frozen_string_literal: true` semantics globally — all
   string literals are frozen, mutation requires a heap-allocated
   buffer (e.g. via .dup or string concatenation). The runtime
   marker byte at s[-1] tells us the provenance:
     0xfe / 0xfc -> sp_str_alloc heap (writable, GC unmarked / marked)
     0xfd        -> sp_String wrapper buffer (writable)
     0xff        -> rodata literal (frozen -> FrozenError)
     other       -> FFI / unknown provenance, treated as frozen
   Returns the byte value (CRuby setbyte return). */
static inline mrb_int sp_str_setbyte(const char *s, mrb_int i, mrb_int v) {
  if (!s) {
    sp_raise_cls("FrozenError", "can't modify frozen String");
    return v;
  }
  unsigned char m = ((const unsigned char *)s)[-1];
  if (m == 0xfe || m == 0xfc || m == 0xfd) {
    ((char *)s)[i] = (char)v;
    return v;
  }
  sp_raise_cls("FrozenError", "can't modify frozen String");
  return v;
}

typedef struct{char*data;int64_t len;int64_t cap;}sp_String;
/* Per-mutable-string freeze flag rides in the GC header alongside
   `marked`. sp_String_freeze sets it; the in-place mutators below
   raise FrozenError when the bit is set. Literal `const char *`
   strings stay frozen via the 0xff marker byte (see sp_str_setbyte). */
static inline mrb_bool sp_String_is_frozen(sp_String*s){if(!s)return TRUE;sp_gc_hdr*h=(sp_gc_hdr*)((char*)s-sizeof(sp_gc_hdr));return h->frozen;}
static inline sp_String*sp_String_freeze(sp_String*s){if(s){sp_gc_hdr*h=(sp_gc_hdr*)((char*)s-sizeof(sp_gc_hdr));h->frozen=1;}return s;}
static void sp_String_fin(void*p){free(((sp_String*)p)->data-1);}
static sp_String*sp_String_new(const char*s){
  /* Copy s's payload into a raw-malloc'd buffer BEFORE sp_gc_alloc.
     If s is a heap string (sp_str_alloc) whose only liveness anchor
     is this C stack frame, sp_gc_alloc can trigger sp_gc_collect →
     sp_str_sweep, which would free s mid-call; a post-alloc strlen +
     memcpy on s would then read freed memory (heap-use-after-free
     under ASAN, non-deterministic crash on Windows MinGW where the
     allocator reuses freed regions faster). The malloc'd buffer is
     not on the GC heap and not on the string heap, so it survives
     any GC inside sp_gc_alloc. */
  int64_t len=(int64_t)strlen(s);
  int64_t cap=len*2+16;
  char*raw=(char*)malloc(cap+2);
  raw[0]=(char)0xfd;
  char*data=raw+1;
  memcpy(data,s,len+1);
  sp_String*r=(sp_String*)sp_gc_alloc(sizeof(sp_String),sp_String_fin,NULL);
  r->len=len;r->cap=cap;r->data=data;
  {sp_gc_hdr*h=(sp_gc_hdr*)((char*)r-sizeof(sp_gc_hdr));h->size+=r->cap+2;sp_gc_bytes+=r->cap+2;}
  return r;
}
/* Issue #757: realloc on growth used to overwrite s->data unconditionally,
   leaking the old buffer + null-dereferencing if realloc fails. Now we
   check the result and bail without mutating on failure. */
static inline void sp_String_append(sp_String*s,const char*t){if(!s||!t)return;if(sp_String_is_frozen(s)){sp_raise_cls("FrozenError","can't modify frozen String");return;}int64_t tl=(int64_t)strlen(t);if(s->len+tl>=s->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)s-sizeof(sp_gc_hdr));int64_t new_cap=(s->len+tl)*2+16;char*raw=(char*)realloc(s->data-1,new_cap+2);if(!raw)return;sp_gc_bytes-=s->cap+2;h->size-=s->cap+2;s->cap=new_cap;raw[0]=(char)0xfd;s->data=raw+1;h->size+=s->cap+2;sp_gc_bytes+=s->cap+2;}memcpy(s->data+s->len,t,tl+1);s->len+=tl;}
static inline void sp_String_prepend(sp_String*s,const char*t){if(!s||!t)return;if(sp_String_is_frozen(s)){sp_raise_cls("FrozenError","can't modify frozen String");return;}int64_t tl=(int64_t)strlen(t);if(s->len+tl>=s->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)s-sizeof(sp_gc_hdr));int64_t new_cap=(s->len+tl)*2+16;char*raw=(char*)realloc(s->data-1,new_cap+2);if(!raw)return;sp_gc_bytes-=s->cap+2;h->size-=s->cap+2;s->cap=new_cap;raw[0]=(char)0xfd;s->data=raw+1;h->size+=s->cap+2;sp_gc_bytes+=s->cap+2;}memmove(s->data+tl,s->data,s->len+1);memcpy(s->data,t,tl);s->len+=tl;}
/* Issue #741: String#insert(idx, str) -- insert str at idx. Negative
   idx is relative to len+1 (insert before tail). */
static inline void sp_String_insert(sp_String*s,int64_t idx,const char*t){if(!s||!t)return;if(sp_String_is_frozen(s)){sp_raise_cls("FrozenError","can't modify frozen String");return;}int64_t tl=(int64_t)strlen(t);if(tl==0)return;if(idx<0)idx+=s->len+1;if(idx<0)idx=0;if(idx>s->len)idx=s->len;if(s->len+tl>=s->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)s-sizeof(sp_gc_hdr));int64_t new_cap=(s->len+tl)*2+16;char*raw=(char*)realloc(s->data-1,new_cap+2);if(!raw)return;sp_gc_bytes-=s->cap+2;h->size-=s->cap+2;s->cap=new_cap;raw[0]=(char)0xfd;s->data=raw+1;h->size+=s->cap+2;sp_gc_bytes+=s->cap+2;}memmove(s->data+idx+tl,s->data+idx,s->len-idx+1);memcpy(s->data+idx,t,tl);s->len+=tl;}
/* Issue #740/#741 sibling: String#replace(s) -- replace entire content. */
static inline void sp_String_replace(sp_String*s,const char*t){if(!s||!t)return;if(sp_String_is_frozen(s)){sp_raise_cls("FrozenError","can't modify frozen String");return;}int64_t tl=(int64_t)strlen(t);if(tl>=s->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)s-sizeof(sp_gc_hdr));int64_t new_cap=tl*2+16;char*raw=(char*)realloc(s->data-1,new_cap+2);if(!raw)return;sp_gc_bytes-=s->cap+2;h->size-=s->cap+2;s->cap=new_cap;raw[0]=(char)0xfd;s->data=raw+1;h->size+=s->cap+2;sp_gc_bytes+=s->cap+2;}memcpy(s->data,t,tl);s->data[tl]='\0';s->len=tl;}
static inline const char*sp_String_cstr(sp_String*s){return s->data;}
static inline int64_t sp_String_length(sp_String*s){return s->len;}
static sp_String*sp_String_dup(sp_String*s){return sp_String_new(s->data);}

/* `File.open(path, mode)` without a block returns an sp_File * — a
   GC-managed wrapper around `FILE *fp`. The finalizer fclose()s any
   still-open fp so a dropped file handle doesn't leak. `path` and
   `mode` are kept live for `f.path` / `f.mode` introspection; they
   come from the caller's already-live string slot so no extra mark
   is needed. */
/* File / IO handle ops live in libspinel_rt.a (lib/sp_io.c); the sp_File
   struct and the allocation-free op prototypes come from sp_io.h. The
   string-returning readers below (gets / read / read_n / path) stay
   inline here because they allocate via the hot static sp_str_alloc,
   whose per-TU sp_str_heap can't be shared across translation units. */
#include "sp_io.h"
static inline const char *sp_File_gets(sp_File *f) {
  if (!f || !f->fp) return NULL;
  char buf[65536];
  if (!fgets(buf, (int)sizeof(buf), f->fp)) return NULL;
  size_t n = strlen(buf);
  char *r = sp_str_alloc(n);
  memcpy(r, buf, n);
  return r;
}
static inline const char *sp_File_read(sp_File *f) {
  if (!f || !f->fp) return sp_str_empty;
  long pos = ftell(f->fp);
  if (pos >= 0 && fseek(f->fp, 0, SEEK_END) == 0) {
    long end = ftell(f->fp);
    fseek(f->fp, pos, SEEK_SET);
    long n = (end > pos) ? (end - pos) : 0;
    if (n <= 0) return sp_str_empty;
    char *r = sp_str_alloc((size_t)n);
    size_t got = fread(r, 1, (size_t)n, f->fp);
    r[got] = 0;
    return r;
  }
  /* Non-seekable stream (IO.pipe end, socket): read to EOF in chunks. */
  size_t cap = 256, len = 0;
  char *buf = (char *)malloc(cap);
  if (!buf) return sp_str_empty;
  for (;;) {
    if (len + 4096 + 1 > cap) {
      cap = (len + 4096 + 1) * 2;
      char *nb = (char *)realloc(buf, cap);
      if (!nb) { free(buf); return sp_str_empty; }
      buf = nb;
    }
    size_t got = fread(buf + len, 1, 4096, f->fp);
    len += got;
    if (got < 4096) break;
  }
  char *r = sp_str_alloc(len);
  memcpy(r, buf, len);
  r[len] = 0;
  free(buf);
  return r;
}
/* IO#read(n): read up to n bytes from the current position. Returns NULL
   (nil) at EOF for a positive n, "" for n == 0, and the whole rest for a
   negative n (treated as the no-count read). A short read produces a
   string of the bytes actually read. */
static inline const char *sp_File_read_n(sp_File *f, mrb_int n) {
  if (!f || !f->fp) return NULL;
  if (n < 0) return sp_File_read(f);
  if (n == 0) return sp_str_empty;
  char *r = sp_str_alloc((size_t)n);
  size_t got = fread(r, 1, (size_t)n, f->fp);
  if (got == 0) return NULL;
  if ((mrb_int)got == n) { r[got] = 0; return r; }
  char *s = sp_str_alloc(got);
  memcpy(s, r, got);
  s[got] = 0;
  return s;
}
static inline const char *sp_File_path(sp_File *f) { return f && f->path ? f->path : sp_str_empty; }

/* Array#inspect for each typed array: `[elem1, elem2, ...]` with each
   element rendered via its own primitive inspect. Matches CRuby's
   Array#inspect output byte-for-byte. Returns a GC-managed C string. */
/* The inspect helpers NULL-guard their receiver: a NULL array can
   reach here when an unresolved call (e.g. an unsupported Array method)
   emits a 0 placeholder that flows into `.inspect`, and dereferencing
   a->len would segfault. Rendering "[]" stops the crash and degrades to
   the same shape as the empty-array case. */
static const char*sp_IntArray_inspect(sp_IntArray*a){if(!a)return "[]";sp_String*s=sp_String_new("[");for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_int_to_s(a->data[a->start+i]));}sp_String_append(s,"]");return s->data;}
static const char*sp_FloatArray_inspect(sp_FloatArray*a){if(!a)return "[]";sp_String*s=sp_String_new("[");for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_float_inspect(a->data[i]));}sp_String_append(s,"]");return s->data;}
/* Array#join for float arrays -- each element via the Ruby-faithful
   sp_float_to_s ("1.0", not "1"). Mirrors sp_IntArray_join exactly: build in a
   malloc buffer, return an sp_str_alloc'd copy. (Not sp_String#data, whose owner
   isn't GC-rooted across the return.) sp_float_to_s's result is copied
   immediately, before the next call can reuse its buffer. */
static const char*sp_FloatArray_join(sp_FloatArray*a,const char*sep){size_t sl=strlen(sep),cap=256;char*buf=(char*)malloc(cap);size_t len=0;if(a){for(mrb_int i=0;i<a->len;i++){if(i>0){if(len+sl>=cap){cap*=2;buf=(char*)realloc(buf,cap);}memcpy(buf+len,sep,sl);len+=sl;}const char*es=sp_float_to_s(a->data[i]);size_t el=strlen(es);if(len+el>=cap){while(len+el>=cap)cap*=2;buf=(char*)realloc(buf,cap);}memcpy(buf+len,es,el);len+=el;}}buf[len]=0;char*r=sp_str_alloc(len);memcpy(r,buf,len);free(buf);return r;}
static mrb_bool sp_FloatArray_eq(sp_FloatArray*a,sp_FloatArray*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++)if(a->data[i]!=b->data[i])return FALSE;return TRUE;}
static const char*sp_StrArray_inspect(sp_StrArray*a){if(!a)return "[]";sp_String*s=sp_String_new("[");for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_str_inspect(a->data[i]));}sp_String_append(s,"]");return s->data;}
static mrb_bool sp_StrArray_eq(sp_StrArray*a,sp_StrArray*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++)if(!sp_str_eq(a->data[i],b->data[i]))return FALSE;return TRUE;}
/* Symbol arrays share the IntArray representation (sp_sym = mrb_int),
   but each element is rendered as ":name" via sp_sym_to_s. */
static inline const char*sp_SymArray_inspect(sp_IntArray*a){if(!a)return "[]";sp_String*s=sp_String_new("[");for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,":");sp_String_append(s,sp_sym_to_s((sp_sym)a->data[a->start+i]));}sp_String_append(s,"]");return s->data;}
/* PtrArray elements are object pointers without a per-element class
   tag, so we render them as `#<Object>` rather than recursing. */
static const char*sp_PtrArray_inspect(sp_PtrArray*a){if(!a)return "[]";sp_String*s=sp_String_new("[");for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,"#<Object>");}sp_String_append(s,"]");return s->data;}
/* Issue #851: Hash#inspect for typed-hash variants beyond
   sym_int_hash. Renders Ruby's `{"k"=>v, ...}` (string keys),
   `{42=>"v", ...}` (int keys), or `{:k=>v, ...}` (sym keys but
   non-int value, since the bare `k: v` shorthand only applies
   when values are inspectable as one-liners — match CRuby). */
static const char*sp_StrIntHash_inspect(sp_StrIntHash*h){sp_String*s=sp_String_new("{");if(h){for(mrb_int i=0;i<h->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_str_inspect(h->order[i]));sp_String_append(s,"=>");sp_String_append(s,sp_int_to_s(sp_StrIntHash_get(h,h->order[i])));}}sp_String_append(s,"}");return s->data;}
/* Hash#to_proc lookup fn — cap is the hash, args[0] the string key. */
static mrb_int sp_StrIntHash_proc_fn(void *cap, mrb_int *args) { return sp_StrIntHash_get((sp_StrIntHash *)cap, (const char *)(uintptr_t)args[0]); }
static const char*sp_StrStrHash_inspect(sp_StrStrHash*h){sp_String*s=sp_String_new("{");if(h){for(mrb_int i=0;i<h->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_str_inspect(h->order[i]));sp_String_append(s,"=>");sp_String_append(s,sp_str_inspect(sp_StrStrHash_get(h,h->order[i])));}}sp_String_append(s,"}");return s->data;}
static const char*sp_IntStrHash_inspect(sp_IntStrHash*h){sp_String*s=sp_String_new("{");if(h){for(mrb_int i=0;i<h->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_int_to_s(h->order[i]));sp_String_append(s,"=>");sp_String_append(s,sp_str_inspect(sp_IntStrHash_get(h,h->order[i])));}}sp_String_append(s,"}");return s->data;}
static const char*sp_IntIntHash_inspect(sp_IntIntHash*h){sp_String*s=sp_String_new("{");if(h){for(mrb_int i=0;i<h->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_int_to_s(h->order[i]));sp_String_append(s,"=>");sp_String_append(s,sp_int_to_s(sp_IntIntHash_get(h,h->order[i])));}}sp_String_append(s,"}");return s->data;}
/* Nested-array inspect: when codegen knows the ptr_array's element
   type is one of the four built-in T_array shapes, recurse into the
   matching primitive inspect . */
static const char*sp_IntArrayPtrArray_inspect(sp_PtrArray*a){sp_String*s=sp_String_new("[");for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_IntArray_inspect((sp_IntArray*)a->data[i]));}sp_String_append(s,"]");return s->data;}
/* Issue #742: Array#combination(k) on int_array -- emit all
   k-element ordered combinations as a PtrArray of IntArrays. */
static void sp_int_combination_recur(sp_IntArray*src,mrb_int start,mrb_int k,sp_IntArray*acc,sp_PtrArray*out){if(k==0){sp_IntArray*cp=sp_IntArray_new();for(mrb_int i=0;i<acc->len;i++)sp_IntArray_push(cp,acc->data[acc->start+i]);sp_PtrArray_push(out,cp);return;}for(mrb_int i=start;i<=src->len-k;i++){sp_IntArray_push(acc,src->data[src->start+i]);sp_int_combination_recur(src,i+1,k-1,acc,out);acc->len--;}}
static sp_PtrArray*sp_IntArray_combination(sp_IntArray*a,mrb_int k){sp_PtrArray*out=sp_PtrArray_new();if(!a||k<0||k>a->len)return out;sp_IntArray*acc=sp_IntArray_new();sp_int_combination_recur(a,0,k,acc,out);return out;}
/* repeated_combination: like combination but an index may repeat, so the
   recursion restarts at `i` rather than `i+1`. */
static void sp_int_repeated_combination_recur(sp_IntArray*src,mrb_int start,mrb_int k,sp_IntArray*acc,sp_PtrArray*out){if(k==0){sp_IntArray*cp=sp_IntArray_new();for(mrb_int i=0;i<acc->len;i++)sp_IntArray_push(cp,acc->data[acc->start+i]);sp_PtrArray_push(out,cp);return;}for(mrb_int i=start;i<src->len;i++){sp_IntArray_push(acc,src->data[src->start+i]);sp_int_repeated_combination_recur(src,i,k-1,acc,out);acc->len--;}}
static sp_PtrArray*sp_IntArray_repeated_combination(sp_IntArray*a,mrb_int k){sp_PtrArray*out=sp_PtrArray_new();if(!a||k<0)return out;sp_IntArray*acc=sp_IntArray_new();sp_int_repeated_combination_recur(a,0,k,acc,out);return out;}

/* Cartesian product of two int arrays. Returns a PtrArray of
   2-element IntArrays. */
static sp_PtrArray *sp_IntArray_product(sp_IntArray *a, sp_IntArray *b) {
  sp_PtrArray *out = sp_PtrArray_new();
  if (!a || !b) return out;
  for (mrb_int i = 0; i < a->len; i++) {
    for (mrb_int j = 0; j < b->len; j++) {
      sp_IntArray *pair = sp_IntArray_new();
      sp_IntArray_push(pair, a->data[a->start + i]);
      sp_IntArray_push(pair, b->data[b->start + j]);
      sp_PtrArray_push(out, pair);
    }
  }
  return out;
}
/* Array#permutation(k) -- ordered k-permutations. */
static void sp_int_permutation_recur(sp_IntArray*src,mrb_int k,sp_IntArray*used,sp_IntArray*acc,sp_PtrArray*out){if(k==0){sp_IntArray*cp=sp_IntArray_new();for(mrb_int i=0;i<acc->len;i++)sp_IntArray_push(cp,acc->data[acc->start+i]);sp_PtrArray_push(out,cp);return;}for(mrb_int i=0;i<src->len;i++){if(used->data[used->start+i])continue;used->data[used->start+i]=1;sp_IntArray_push(acc,src->data[src->start+i]);sp_int_permutation_recur(src,k-1,used,acc,out);acc->len--;used->data[used->start+i]=0;}}
static sp_PtrArray*sp_IntArray_permutation(sp_IntArray*a,mrb_int k){sp_PtrArray*out=sp_PtrArray_new();if(!a||k<0||k>a->len)return out;sp_IntArray*used=sp_IntArray_new();for(mrb_int i=0;i<a->len;i++)sp_IntArray_push(used,0);sp_IntArray*acc=sp_IntArray_new();sp_int_permutation_recur(a,k,used,acc,out);return out;}
static const char*sp_FloatArrayPtrArray_inspect(sp_PtrArray*a){sp_String*s=sp_String_new("[");for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_FloatArray_inspect((sp_FloatArray*)a->data[i]));}sp_String_append(s,"]");return s->data;}
static const char*sp_StrArrayPtrArray_inspect(sp_PtrArray*a){sp_String*s=sp_String_new("[");for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_StrArray_inspect((sp_StrArray*)a->data[i]));}sp_String_append(s,"]");return s->data;}
/* sp_PolyArrayPtrArray_inspect lives below sp_PolyArray_inspect's
   forward declaration (sp_PolyArray isn't defined until ~2542). */
static const char*sp_SymArrayPtrArray_inspect(sp_PtrArray*a){sp_String*s=sp_String_new("[");for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_SymArray_inspect((sp_IntArray*)a->data[i]));}sp_String_append(s,"]");return s->data;}
/* issue #526: join for a sp_PtrArray of sp_String* (mutable_str_ptr_array).
   Sibling to sp_StrArray_join, but takes advantage of sp_String's known
   length: two-pass — sum the exact total, sp_str_alloc once, then memcpy
   each element by its s->len (preserves embedded NULs). Avoids the
   realloc-grow loop's leak-on-failure and long-separator overflow
   risks, and skips an intermediate malloc'd buffer. NULL entries
   contribute zero length. */
static const char*sp_PtrArray_str_join(sp_PtrArray*a,const char*sep){mrb_int al=a->len;if(al==0)return sp_str_empty;size_t sl=strlen(sep),total=0;for(mrb_int i=0;i<al;i++){if(i>0)total+=sl;sp_String*s=(sp_String*)a->data[i];if(s)total+=(size_t)s->len;}char*r=sp_str_alloc(total);size_t cur=0;for(mrb_int i=0;i<al;i++){if(i>0){memcpy(r+cur,sep,sl);cur+=sl;}sp_String*s=(sp_String*)a->data[i];if(s&&s->len){memcpy(r+cur,s->data,(size_t)s->len);cur+=(size_t)s->len;}}return r;}

#ifdef __FreeBSD__

#define re_exec spinel_re_exec

#define MAP_NORESERVE 0

#endif

/* Regexp engine (link with libspre.a from lib/regexp/) */
typedef struct mrb_regexp_pattern mrb_regexp_pattern;
mrb_regexp_pattern* re_compile(const char *pattern, int64_t len, uint32_t flags);
void re_free(mrb_regexp_pattern *pat);
int re_exec(const mrb_regexp_pattern *pat, const char *str, int64_t len, int64_t start, int *captures, int captures_size);

/* Regexp globals: $1-$9 captures */
static const char *sp_re_captures[10] = {0};
static int sp_re_caps[64];
/* NULL (not "") so sp_mark_string's null-guard handles the unset case
   without reaching the `s[-1]` access. The rodata `""` literal would
   trigger -Wstringop-overflow under -O3 + sp_mark_string inlining at
   the call site in sp_re_mark_globals — gcc proves the `s[-1] = 0xfc`
   write would be out-of-bounds even though the runtime guard
   `s[-1] == 0xfe` (always false for rodata) prevents it from firing. */
static const char *sp_re_last_str = NULL;

/* Symbolic back-references populated alongside the numbered captures.
   Read by codegen's BackReferenceReadNode arm:
     $&  -> sp_re_match_str (the whole matched substring)
     $`  -> sp_re_match_pre  (substring before the match)
     $'  -> sp_re_match_post (substring after the match)
   $~ falls back to $& since Spinel has no MatchData wrapper. */
static const char *sp_re_match_str = NULL;
static const char *sp_re_match_pre = NULL;
static const char *sp_re_match_post = NULL;

/* ARGV runtime: argv[i] strings are dup'd via sp_str_dup_external on
   main() entry, which allocates from the str-heap with mark byte
   0xfe. Without explicit marking they get reaped on the first
   sp_str_sweep, leaving sp_argv.data[i] as a dangling pointer that
   later `ARGV[i]` reads dereference. The exact length boundary
   triggering the segfault depends on malloc's reuse pattern (so the
   bug surfaces non-deterministically by string length), but the
   underlying issue is unconditional. */
typedef struct{const char**data;mrb_int len;}sp_Argv;
static sp_Argv sp_argv;

/* Mark active in-flight exception messages. Most raises pass string
   literals (rodata, marker byte ≠ 0xfe → no-op for sp_mark_string),
   but raises that build messages dynamically via sp_sprintf (e.g.
   sp_str_to_i_strict's `"invalid value for Integer(): \"%s\""`)
   hand a heap-allocated string to sp_raise_cls. Without marking, a
   GC cycle between the raise and the rescue handler reading the
   message would sweep the message and leave sp_exc_msg[i] dangling.
   The helper itself is defined further down (after sp_exc_msg /
   sp_exc_top are declared); only the prototype lives here. */
static void sp_mark_in_flight_exceptions(void);

/* Mark the regex globals as live during GC. Each holds a pointer to a
   string allocated via sp_str_alloc_raw on the str-heap; without this
   sp_str_sweep would reap them on the next collect, leaving dangling
   pointers in $1..$9, $&, $`, $'. sp_mark_string is null-safe and
   no-ops on non-heap strings (the empty-string default of
   sp_re_last_str), so it's safe to call unconditionally. */
/* sp_fiber_root.storage needs marking here because sp_fiber_root is
   a static (not sp_gc_alloc'd), so its normal sp_Fiber_scan never
   runs via the heap walker. Without this, top-level `Fiber[:k] = v`
   writes get prematurely collected. The forward declaration is
   needed because sp_fiber_root is defined further down in the
   Fiber runtime block. */
/* Forward declared: defined alongside the PolyArray pool below.
   Pooled sp_PolyArray*s are not user-visible roots, so they need
   explicit marking during a GC mark phase or the sweep will free
   them while pointers remain in the pool. */
static void sp_PolyArray_pool_scan_roots(void);

/* External linkage: lib/sp_gc.c's sp_gc_mark_all reaches this by name. */
static void sp_re_mark_globals(void) {
  sp_mark_string(sp_re_last_str);
  for (int i = 0; i < 10; i++) sp_mark_string(sp_re_captures[i]);
  sp_mark_string(sp_re_match_str);
  sp_mark_string(sp_re_match_pre);
  sp_mark_string(sp_re_match_post);
  for (mrb_int i = 0; i < sp_argv.len; i++) sp_mark_string(sp_argv.data[i]);
  sp_PolyArray_pool_scan_roots();
  sp_mark_in_flight_exceptions();
  sp_mark_fiber_root_storage();
}

/* Hand the collector (lib/sp_gc.c) this TU's root-marking and string-heap
   sweep. Runs before main, so the hooks are set before the first
   allocation can trigger a collection. */
__attribute__((constructor)) static void sp_gc_install_tu_hooks(void) {
  sp_gc_mark_globals_hook = sp_re_mark_globals;
  sp_gc_str_sweep_hook = sp_str_sweep;
}

/* `$+` / `$LAST_PAREN_MATCH` — contents of the highest-indexed group
   that participated in the match. Walks sp_re_captures[] from 9 down
   and returns the first non-NULL entry. NULL when no group matched
   (codegen ternary falls back to ""). Matches CRuby's behaviour:
   for /(a)(b)?/ matching "a", $+ is "a"; for /(a)(b)/ matching "ab",
   $+ is "b". */
static const char *sp_re_last_paren_match(void) {
  for (int i = 9; i >= 1; i--) {
    if (sp_re_captures[i]) return sp_re_captures[i];
  }
  return NULL;
}

static void sp_re_set_captures(const char *str, int *caps, int ncaps) {
  sp_re_last_str = str;
  for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
  for (int i = 1; i < ncaps && i < 10; i++) {
    if (caps[i*2] >= 0 && caps[i*2+1] >= 0) {
      int len = caps[i*2+1] - caps[i*2];
      char *buf = sp_str_alloc_raw(len+1);
      memcpy(buf, str+caps[i*2], len); buf[len] = 0;
      sp_re_captures[i] = buf;
    }
  }
  /* Populate the symbolic back-references from caps[0]/[1] (the whole
     match span). NULL when the match failed; the codegen ternary
     falls back to "". */
  sp_re_match_str = NULL;
  sp_re_match_pre = NULL;
  sp_re_match_post = NULL;
  if (ncaps >= 1 && caps[0] >= 0 && caps[1] >= 0) {
    int slen = (int)strlen(str);
    int mlen = caps[1] - caps[0];
    char *m = sp_str_alloc_raw(mlen + 1);
    memcpy(m, str + caps[0], mlen); m[mlen] = 0;
    sp_re_match_str = m;
    char *pre = sp_str_alloc_raw(caps[0] + 1);
    memcpy(pre, str, caps[0]); pre[caps[0]] = 0;
    sp_re_match_pre = pre;
    int post_len = slen - caps[1];
    char *post = sp_str_alloc_raw(post_len + 1);
    memcpy(post, str + caps[1], post_len); post[post_len] = 0;
    sp_re_match_post = post;
  }
}

/* `=~` returns the match position (0-indexed) or -1 on miss.
   Codegen's regex truthy check (regex_match_call_node? arm in
   compile_cond_expr) compares against -1 so match-at-position-0
   is correctly truthy. Direct value use lines up with CRuby's
   `String#=~` int semantics: "abc" =~ /b/ -> 1, not 2. */
static mrb_int sp_re_match(mrb_regexp_pattern *pat, const char *str) {
  int64_t slen = (int64_t)strlen(str);
  int ncaps = 32;
  int n = re_exec(pat, str, slen, 0, sp_re_caps, ncaps);
  if (n > 0) { sp_re_set_captures(str, sp_re_caps, n/2); return sp_re_caps[0]; }
  /* Issue #848: clear backrefs on no-match so a subsequent `$1`
     reads as nil rather than the previous match's group. */
  for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
  sp_re_last_str = NULL;
  sp_re_match_str = NULL;
  sp_re_match_pre = NULL;
  sp_re_match_post = NULL;
  return -1;
}

/* `s.rindex(regex)` — last match start, in BYTE offset (matches
   the way sp_str_rindex reports indices for plain-string search;
   codepoint translation would require a UTF-8 walk and the
   handful of call sites that consume rindex don't need it).
   Walks forward through successive matches and remembers the
   latest start. Issue #504: previously the codegen routed
   `s.rindex(/re/)` to sp_str_rindex(s, 0) and SEGV'd at
   strlen(NULL). Returns -1 on no match. */
static mrb_int sp_re_rindex(mrb_regexp_pattern *pat, const char *str) {
  int64_t slen = (int64_t)strlen(str);
  int caps[2];
  int64_t pos = 0;
  mrb_int last = -1;
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 2);
    if (n <= 0) break;
    last = caps[0];
    /* Advance past the match; for zero-width matches step by 1
       to avoid an infinite loop. */
    int64_t next = caps[1];
    if (next <= pos) next = pos + 1;
    pos = next;
  }
  return last;
}

static mrb_bool sp_re_match_p(mrb_regexp_pattern *pat, const char *str) {
  int64_t slen = (int64_t)strlen(str);
  int caps[2];
  return re_exec(pat, str, slen, 0, caps, 2) > 0;
}

/* Issue #869: Regexp#match?(str, pos) starts matching at byte
   offset `pos`. Negative pos counts from the end (CRuby compat).
   Out-of-range pos returns false. */
static mrb_bool sp_re_match_p_at(mrb_regexp_pattern *pat, const char *str, mrb_int pos) {
  int64_t slen = (int64_t)strlen(str);
  if (pos < 0) pos += slen;
  if (pos < 0 || pos > slen) return FALSE;
  int caps[2];
  return re_exec(pat, str, slen, (mrb_int)pos, caps, 2) > 0;
}
/* String#match?(/re/, pos) — pos is a codepoint index (CRuby semantics),
   unlike Regexp#match?(str, pos) which uses byte offset. Convert the
   codepoint index to a byte offset before dispatching to re_exec. */
static mrb_bool sp_str_re_match_p_at(mrb_regexp_pattern *pat, const char *str, mrb_int cpos) {
  mrb_int cl = sp_str_length(str);
  if (cpos < 0) cpos += cl;
  if (cpos < 0 || cpos > cl) return FALSE;
  size_t boff = sp_utf8_byte_offset(str, cpos);
  int64_t slen = (int64_t)strlen(str);
  int caps[2];
  return re_exec(pat, str, slen, (mrb_int)boff, caps, 2) > 0;
}

/* Issue #855: expand `\1`..`\9` / `\&` / `\0` backreferences in
   the replacement string against the current caps[] array. `\\`
   is a literal backslash. Writes to *out_io at *olen_io, growing
   *out_io / *cap_io as needed. */
static void sp_re_expand_rep(char **out_io, size_t *olen_io, size_t *cap_io,
                             const char *rep, size_t rlen,
                             const char *src, int *caps, int ncaps) {
  size_t olen = *olen_io;
  char *out = *out_io;
  size_t cap = *cap_io;
  size_t i = 0;
  while (i < rlen) {
    char c = rep[i];
    if (c == '\\' && i + 1 < rlen) {
      char d = rep[i+1];
      if ((d >= '0' && d <= '9') || d == '&') {
        int gi = (d == '&') ? 0 : (d - '0');
        if (gi*2 + 1 < ncaps && caps[gi*2] >= 0 && caps[gi*2+1] >= 0) {
          int g_len = caps[gi*2+1] - caps[gi*2];
          if (olen + g_len + 1 >= cap) { cap = (olen + g_len) * 2 + 64; out = (char*)realloc(out, cap); }
          memcpy(out+olen, src + caps[gi*2], g_len);
          olen += g_len;
        }
        i += 2;
        continue;
      } else if (d == '\\') {
        if (olen + 1 >= cap) { cap = cap * 2 + 64; out = (char*)realloc(out, cap); }
        out[olen++] = '\\';
        i += 2;
        continue;
      }
    }
    if (olen + 1 >= cap) { cap = cap * 2 + 64; out = (char*)realloc(out, cap); }
    out[olen++] = c;
    i++;
  }
  *out_io = out; *olen_io = olen; *cap_io = cap;
}

static const char *sp_re_gsub(mrb_regexp_pattern *pat, const char *str, const char *rep) {
  int64_t slen = (int64_t)strlen(str); size_t rlen = strlen(rep);
  size_t cap = slen * 2 + rlen * 4 + 64;
 /* Build into a plain malloc scratch: the buffer is grown with realloc
    here and inside sp_re_expand_rep, which is only valid on a real
    malloc base (a sp_str body pointer is offset past its header). The
    final string is allocated at the exact length below. */
  char *out = (char *)malloc(cap); size_t olen = 0;
  int64_t pos = 0; int caps[64];
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 64);
    if (n <= 0 || caps[0] < 0) break;
    size_t before = caps[0] - pos;
    if (olen+before+rlen >= cap) { cap = (olen+before+rlen)*2+64; out = (char*)realloc(out, cap); }
    memcpy(out+olen, str+pos, before); olen += before;
    sp_re_expand_rep(&out, &olen, &cap, rep, rlen, str, caps, n);
    if (caps[0] == caps[1]) {
 /* Zero-width match (/^/, /$/, /\b/, an empty pattern): Ruby inserts
    the replacement before the char at this position, keeps that char,
    and advances past it. Copy the char and step by one so the scan
    makes progress without dropping it or spinning on the same spot. */
      if (caps[1] < slen) {
        if (olen+1 >= cap) { cap = olen*2+64; out = (char*)realloc(out, cap); }
        out[olen++] = str[caps[1]];
      }
      pos = caps[1] + 1;
    } else {
      pos = caps[1];
    }
  }
 /* pos can land at slen+1 after a zero-width match at the end; guard the
    tail copy so `slen - pos` doesn't underflow size_t. */
  if (pos < slen) {
    size_t rest = slen - pos;
    if (olen+rest+1 >= cap) { cap = olen+rest+64; out = (char*)realloc(out, cap); }
    memcpy(out+olen, str+pos, rest); olen += rest;
  }
 /* Emit a string sized to exactly the bytes written (sp_str_alloc sets
    the length and null-terminates); release the scratch. */
  char *res = sp_str_alloc(olen);
  memcpy(res, out, olen);
  free(out);
  return res;
}

/* String#gsub(regex, hash) — per-match hash lookup form. CRuby's
 * semantics: each matched substring is looked up as a key in the
 * hash; the value (if present) is the replacement, otherwise the
 * matched substring is dropped (CRuby returns "", not the match).
 * Used by html_escape / json_escape idioms (gsub(/[&<>]/, ESCAPES)). */
static const char *sp_re_gsub_str_str_hash(mrb_regexp_pattern *pat, const char *str, sp_StrStrHash *h) {
  int64_t slen = (int64_t)strlen(str);
 /* malloc scratch (realloc-safe); exact-sized string emitted below. */
  size_t cap = slen * 2 + 64; char *out = (char *)malloc(cap); size_t olen = 0;
  int64_t pos = 0; int caps[64];
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 64);
    if (n <= 0 || caps[0] < 0) break;
    size_t before = caps[0] - pos;
    int mlen = caps[1] - caps[0];
    char keybuf[64];
    char *key = mlen < (int)sizeof(keybuf) ? keybuf : (char *)malloc(mlen + 1);
    memcpy(key, str + caps[0], mlen);
    key[mlen] = 0;
    const char *rep = sp_StrStrHash_has_key(h, key) ? sp_StrStrHash_get(h, key) : "";
    size_t rlen = strlen(rep);
    if (olen + before + rlen >= cap) { cap = (olen + before + rlen) * 2 + 64; out = (char *)realloc(out, cap); }
    memcpy(out + olen, str + pos, before); olen += before;
    memcpy(out + olen, rep, rlen); olen += rlen;
    if (key != keybuf) free(key);
    if (caps[0] == caps[1]) {
 /* Zero-width match: keep the source char at this position and step
    past it (see sp_re_gsub for the rationale). */
      if (caps[1] < slen) {
        if (olen + 1 >= cap) { cap = olen * 2 + 64; out = (char *)realloc(out, cap); }
        out[olen++] = str[caps[1]];
      }
      pos = caps[1] + 1;
    } else {
      pos = caps[1];
    }
  }
  if (pos < slen) {
    size_t rest = slen - pos;
    if (olen + rest >= cap) { cap = olen + rest + 1; out = (char *)realloc(out, cap); }
    memcpy(out + olen, str + pos, rest); olen += rest;
  }
  char *res = sp_str_alloc(olen);
  memcpy(res, out, olen);
  free(out);
  return res;
}

/* Issue #910: sub(regex, hash) — same lookup semantics as
   sp_re_gsub_str_str_hash but only the first match. */
static const char *sp_re_sub_str_str_hash(mrb_regexp_pattern *pat, const char *str, sp_StrStrHash *h) {
  int64_t slen = (int64_t)strlen(str);
  int caps[64];
  int n = re_exec(pat, str, slen, 0, caps, 64);
  if (n <= 0 || caps[0] < 0) return str;
  int mlen = caps[1] - caps[0];
  char keybuf[64];
  char *key = mlen < (int)sizeof(keybuf) ? keybuf : (char *)malloc(mlen + 1);
  memcpy(key, str + caps[0], mlen);
  key[mlen] = 0;
  const char *rep = (h && sp_StrStrHash_has_key(h, key)) ? sp_StrStrHash_get(h, key) : "";
  size_t rlen = strlen(rep);
  size_t rest = slen - caps[1];
  size_t total = caps[0] + rlen + rest;
  char *out = sp_str_alloc_raw(total + 1);
  memcpy(out, str, caps[0]);
  memcpy(out + caps[0], rep, rlen);
  memcpy(out + caps[0] + rlen, str + caps[1], rest);
  out[total] = 0;
  if (key != keybuf) free(key);
  return out;
}

/* Issue #910: sub(string, hash) — literal-substring pattern
   with a hash replacement. Replaces only the first match. */
static const char *sp_str_sub_str_str_hash(const char *str, const char *pat, sp_StrStrHash *h) {
  if (!str || !pat) return str;
  const char *found = strstr(str, pat);
  if (!found) return str;
  size_t before = (size_t)(found - str);
  size_t plen = strlen(pat);
  const char *rep = (h && sp_StrStrHash_has_key(h, pat)) ? sp_StrStrHash_get(h, pat) : "";
  size_t rlen = strlen(rep);
  size_t rest = strlen(str) - before - plen;
  size_t total = before + rlen + rest;
  char *out = sp_str_alloc_raw(total + 1);
  memcpy(out, str, before);
  memcpy(out + before, rep, rlen);
  memcpy(out + before + rlen, found + plen, rest);
  out[total] = 0;
  return out;
}

static const char *sp_re_sub(mrb_regexp_pattern *pat, const char *str, const char *rep) {
  int64_t slen = (int64_t)strlen(str); size_t rlen = strlen(rep);
  int caps[64];
  int n = re_exec(pat, str, slen, 0, caps, 64);
  if (n <= 0 || caps[0] < 0) return str;
  /* Issue #855: expand `\1`..`\9` / `\&` from rep against caps. */
  size_t cap = caps[0] + rlen * 4 + (slen - caps[1]) + 64;
 /* malloc scratch: sp_re_expand_rep and the tail grow it with realloc,
    which needs a real malloc base. Exact-sized string emitted below. */
  char *out = (char *)malloc(cap);
  memcpy(out, str, caps[0]);
  size_t olen = caps[0];
  sp_re_expand_rep(&out, &olen, &cap, rep, rlen, str, caps, n);
  size_t rest = slen - caps[1];
  if (olen + rest + 1 >= cap) { cap = olen + rest + 64; out = (char*)realloc(out, cap); }
  memcpy(out+olen, str+caps[1], rest); olen += rest;
  char *res = sp_str_alloc(olen);
  memcpy(res, out, olen);
  free(out);
  return res;
}

static sp_StrArray *sp_re_scan(mrb_regexp_pattern *pat, const char *str) {
  sp_StrArray *arr = sp_StrArray_new();
  int64_t slen = (int64_t)strlen(str); int64_t pos = 0; int caps[64];
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 64);
    if (n <= 0 || caps[0] < 0) break;
    int len = caps[1] - caps[0];
    char *m = sp_str_alloc_raw(len+1); memcpy(m, str+caps[0], len); m[len] = 0;
    sp_StrArray_push(arr, m);
    pos = caps[1]; if (caps[0] == caps[1]) pos++;
  }
  return arr;
}

static sp_StrArray *sp_re_split(mrb_regexp_pattern *pat, const char *str) {
  sp_StrArray *arr = sp_StrArray_new();
  int64_t slen = (int64_t)strlen(str); int64_t pos = 0; int caps[64];
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 64);
    if (n <= 0 || caps[0] < 0) {
      int len = slen - pos; char *m = sp_str_alloc_raw(len+1);
      memcpy(m, str+pos, len); m[len] = 0; sp_StrArray_push(arr, m); break;
    }
    int len = caps[0] - pos; char *m = sp_str_alloc_raw(len+1);
    memcpy(m, str+pos, len); m[len] = 0; sp_StrArray_push(arr, m);
    /* Issue #856: when the splitter regex has capture groups,
       Ruby splices each captured substring into the result
       between the surrounding segments (caps[2..n-1] hold groups
       1..(n/2-1); group 0 is the whole match). */
    for (int gi = 1; gi * 2 + 1 < n; gi++) {
      if (caps[gi*2] >= 0 && caps[gi*2+1] >= 0) {
        int glen = caps[gi*2+1] - caps[gi*2];
        char *gm = sp_str_alloc_raw(glen+1);
        memcpy(gm, str + caps[gi*2], glen); gm[glen] = 0;
        sp_StrArray_push(arr, gm);
      } else {
        sp_StrArray_push(arr, sp_str_empty);
      }
    }
    pos = caps[1]; if (caps[0] == caps[1]) pos++;
  }
  return arr;
}

/* NaN-boxed polymorphic value */
typedef uint64_t sp_RbValue;
#define SP_TAG_INT 0
#define SP_TAG_STR 1
#define SP_TAG_FLT 2
#define SP_TAG_BOOL 3
#define SP_TAG_NIL 4
#define SP_TAG_OBJ 5
#define SP_TAG_SYM 6
/* Class values boxed into a poly slot (e.g.
   as ancestors-array elements). cls_id of the sp_RbVal carries
   the boxed sp_Class's cls_id directly so unboxing is just a
   field read. */
#define SP_TAG_CLASS 7
/* Encoding values boxed into a poly slot. v.s carries the canonical
   encoding name (`"UTF-8"` in Spinel's current runtime model). */
#define SP_TAG_ENCODING 8
/* Negative cls_id values let SP_TAG_OBJ also carry built-in pointer
   types (IntArray, FloatArray, ...) — avoids minting a new SP_TAG_*
   per type. Non-negative cls_id stays an index into the user-class
   table as before. The element-type tag and the array cls_id are
   paired by `array_cls_id = -element_tag - 1`. */
#define SP_BUILTIN_ARRAY_OF(tag) (-(tag) - 1)
#define SP_BUILTIN_INT_ARRAY SP_BUILTIN_ARRAY_OF(SP_TAG_INT) /* -1 */
#define SP_BUILTIN_STR_ARRAY SP_BUILTIN_ARRAY_OF(SP_TAG_STR) /* -2 */
#define SP_BUILTIN_FLT_ARRAY SP_BUILTIN_ARRAY_OF(SP_TAG_FLT) /* -3 */
#define SP_BUILTIN_PTR_ARRAY SP_BUILTIN_ARRAY_OF(SP_TAG_OBJ) /* -6 */
#define SP_BUILTIN_SYM_ARRAY SP_BUILTIN_ARRAY_OF(SP_TAG_SYM) /* -7 */
#define SP_BUILTIN_PROC (-9) /* sp_Proc *, distinct from any tag-based id */
#define SP_BUILTIN_RANGE (-10) /* sp_Range *, heap copy of stack-typed sp_Range when crossing into poly */
#define SP_BUILTIN_TIME (-11) /* sp_Time *, heap copy of stack-typed sp_Time when crossing into poly */
#define SP_BUILTIN_POLY_ARRAY (-12) /* sp_PolyArray *, array of sp_RbVal */
/* Hash variant cls_ids — boxed into the cls_id of a poly slot so
   Hash#dig can recover the concrete hash type at runtime. */
#define SP_BUILTIN_STR_INT_HASH (-13)
#define SP_BUILTIN_STR_STR_HASH (-14)
#define SP_BUILTIN_INT_STR_HASH (-15)
#define SP_BUILTIN_SYM_INT_HASH (-16)
#define SP_BUILTIN_SYM_STR_HASH (-17)
#define SP_BUILTIN_STR_POLY_HASH (-18)
#define SP_BUILTIN_SYM_POLY_HASH (-19)
#define SP_BUILTIN_POLY_POLY_HASH (-20)
/* sp_RbVal is defined in sp_gc.h (the mark helpers dispatch on its tag). */
static sp_RbVal sp_box_int(mrb_int v) { sp_RbVal r; r.tag = SP_TAG_INT; r.cls_id = 0; r.v.i = v; return r; }
static sp_RbVal sp_box_str(const char *v) { sp_RbVal r; r.tag = SP_TAG_STR; r.cls_id = 0; r.v.s = v; return r; }
static sp_RbVal sp_box_float(mrb_float v) { sp_RbVal r; r.tag = SP_TAG_FLT; r.cls_id = 0; r.v.f = v; return r; }
static sp_RbVal sp_box_bool(mrb_bool v) { sp_RbVal r; r.tag = SP_TAG_BOOL; r.cls_id = 0; r.v.b = v; return r; }
static sp_RbVal sp_box_nil(void) { sp_RbVal r; r.tag = SP_TAG_NIL; r.cls_id = 0; r.v.i = 0; return r; }
static sp_RbVal sp_box_obj(void *p, int cls_id) { sp_RbVal r; r.tag = SP_TAG_OBJ; r.cls_id = cls_id; r.v.p = p; return r; }
static sp_RbVal sp_box_sym(sp_sym v) { sp_RbVal r; r.tag = SP_TAG_SYM; r.cls_id = 0; r.v.i = (mrb_int)v; return r; }
/* box a sp_Class into a poly slot. */
static sp_RbVal sp_box_class(sp_Class c) { sp_RbVal r; r.tag = SP_TAG_CLASS; r.cls_id = (int)c.cls_id; r.v.i = c.cls_id; return r; }
static sp_RbVal sp_box_encoding(sp_Encoding e) { sp_RbVal r; r.tag = SP_TAG_ENCODING; r.cls_id = 0; r.v.s = sp_encoding_name(e); return r; }

/* every non-value-type sp_<C> starts
   with `mrb_int cls_id`. Read it back from a void* when
   the static type at the call site has lost the cls_id (e.g.
   sp_PtrArray_get returning void*). NULL-safe via the guard
   on p; expects p to actually point at a user-class struct
   when non-NULL. */
static inline mrb_int sp_obj_cls_id_of(void *p) { return p ? *(mrb_int *)p : 0; }
static sp_RbVal sp_box_nullable_str(const char *v) { return v ? sp_box_str(v) : sp_box_nil(); }
/* String#index / #rindex return a boxed nil for not-found, boxed
   int for found. Issue #532: typed-int slot can't represent CRuby's
   nil-vs-real-index distinction in-band; widening the result type
   to sp_RbVal at the call site lets `pos.nil?` and `puts pos.inspect`
   work via the standard poly-tag dispatch. The -1 sentinel comes
   from the underlying sp_str_*_index helpers; we widen here at the
   boxing layer so existing call sites that want the raw int still
   work via `sp_str_index` directly. */
static sp_RbVal sp_str_index_poly(const char *s, const char *sub) { mrb_int n = sp_str_index(s, sub); return n < 0 ? sp_box_nil() : sp_box_int(n); }
static sp_RbVal sp_str_index_from_poly(const char *s, const char *sub, mrb_int start) { mrb_int n = sp_str_index_from(s, sub, start); return n < 0 ? sp_box_nil() : sp_box_int(n); }
static sp_RbVal sp_str_rindex_poly(const char *s, const char *sub) { mrb_int n = sp_str_rindex(s, sub); return n < 0 ? sp_box_nil() : sp_box_int(n); }
/* int? siblings of the String#index family. Same not-found
   semantics as the _poly variants, but the result is the int?
   sentinel (SP_INT_NIL) rather than a boxed sp_RbVal — keeps the
   `i = s.index(sub); return ... if i.nil?; i + 1` idiom on the
   direct integer arithmetic path. */
static mrb_int sp_str_index_opt(const char *s, const char *sub)                          { mrb_int n = sp_str_index(s, sub);              return n < 0 ? SP_INT_NIL : n; }
static mrb_int sp_str_index_from_opt(const char *s, const char *sub, mrb_int start)      { mrb_int n = sp_str_index_from(s, sub, start);  return n < 0 ? SP_INT_NIL : n; }
static mrb_int sp_str_rindex_opt(const char *s, const char *sub)                         { mrb_int n = sp_str_rindex(s, sub);             return n < 0 ? SP_INT_NIL : n; }
static sp_RbVal sp_re_rindex_poly(mrb_regexp_pattern *pat, const char *str) { mrb_int n = sp_re_rindex(pat, str); return n < 0 ? sp_box_nil() : sp_box_int(n); }
/* `s.index(regex)` -- first match start (byte offset, as sp_re_match reports;
   matches the rindex regex variant, which also reports bytes). sp_re_match
   sets $~ as a side effect, which CRuby's String#index(regex) also does.
   Boxed Integer | nil. The plain-string path would lower the regex pattern to
   0 and feed a bogus arg to sp_str_index_opt. */
static sp_RbVal sp_re_index_poly(mrb_regexp_pattern *pat, const char *str) { mrb_int n = sp_re_match(pat, str); return n < 0 ? sp_box_nil() : sp_box_int(n); }
/* CRuby-compatible =~ wrapper: SP_TAG_INT(pos) on match, SP_TAG_NIL
   on miss. Codegen routes the `=~` operator here so
   `("abc" =~ /xyz/).nil?` answers true and `puts("abc" =~ /xyz/)`
   prints an empty line, matching CRuby. The raw `sp_re_match`
   (returning -1) stays available for internal callers needing the
   sentinel form. */
static sp_RbVal sp_re_match_poly(mrb_regexp_pattern *pat, const char *str) { mrb_int n = sp_re_match(pat, str); return n < 0 ? sp_box_nil() : sp_box_int(n); }

/* Regexp.escape(s) / Regexp.quote(s) -- prefix every regex metachar
   and whitespace byte with a single backslash, returning a heap
   string that callers can feed into `Regexp.new(...)` to match the
   original bytes literally. Matches CRuby's rb_reg_quote for the
   ASCII range (the only range Spinel's regex engine indexes today,
   so multibyte passes through unchanged).

   The metachars covered: \\ . ? * + ^ $ | ( ) [ ] { } # -
   The whitespace covered: ' ' \t \n \r \f \v
   Everything else copies byte-for-byte. */
static const char *sp_re_escape(const char *src) {
  size_t i, in_len = strlen(src);
  size_t out_len = 0;
  for (i = 0; i < in_len; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c == '\\' || c == '.' || c == '?' || c == '*' || c == '+' ||
        c == '^' || c == '$' || c == '|' || c == '(' || c == ')' ||
        c == '[' || c == ']' || c == '{' || c == '}' || c == '#' ||
        c == '-' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
        c == '\f' || c == '\v') {
      out_len += 2;
    } else {
      out_len += 1;
    }
  }
  if (out_len == in_len) {
    return src;
  }
  char *buf = sp_str_alloc(out_len);
  size_t j = 0;
  for (i = 0; i < in_len; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c == '\\' || c == '.' || c == '?' || c == '*' || c == '+' ||
        c == '^' || c == '$' || c == '|' || c == '(' || c == ')' ||
        c == '[' || c == ']' || c == '{' || c == '}' || c == '#' ||
        c == '-' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
        c == '\f' || c == '\v') {
      buf[j++] = '\\';
      buf[j++] = (char)c;
    } else {
      buf[j++] = (char)c;
    }
  }
  buf[j] = 0;
  return buf;
}
static sp_RbVal sp_box_nullable_obj(void *p, int cls_id) { return p ? sp_box_obj(p, cls_id) : sp_box_nil(); }
/* Built-in pointer boxes — share SP_TAG_OBJ with a reserved negative
   cls_id so the dispatch path is uniform. */
static sp_RbVal sp_box_int_array(void *p)   { return sp_box_obj(p, SP_BUILTIN_INT_ARRAY); }
static sp_RbVal sp_box_float_array(void *p) { return sp_box_obj(p, SP_BUILTIN_FLT_ARRAY); }
static sp_RbVal sp_box_str_array(void *p)   { return sp_box_obj(p, SP_BUILTIN_STR_ARRAY); }
static sp_RbVal sp_box_sym_array(void *p)   { return sp_box_obj(p, SP_BUILTIN_SYM_ARRAY); }
static sp_RbVal sp_box_ptr_array(void *p)   { return sp_box_obj(p, SP_BUILTIN_PTR_ARRAY); }
static sp_RbVal sp_box_proc(void *p)        { return sp_box_obj(p, SP_BUILTIN_PROC); }

/* CRuby-compatible Array#index / #rindex / #find_index: returns
   sp_RbVal (nil tag for not-found, int tag with the position when
   found). spinel's raw `_index` helpers return the -1 sentinel,
   which diverges from CRuby's nil. Codegen routes
   `arr.index(x)` / `arr.find_index(x)` / `arr.rindex(x)` through
   these `_poly` wrappers so `.nil?` / `== nil` / `inspect` etc.
   on the result behave the CRuby way. Sibling to
   `sp_str_index_poly` above; same widening rationale.
   Issue raised during #585 follow-up: spinel positions itself
   as a Ruby SUBSET, so documented Ruby APIs must match CRuby
   behavior. */
static sp_RbVal sp_IntArray_index_poly(sp_IntArray *a, mrb_int v)         { mrb_int n = sp_IntArray_index(a, v);   return n < 0 ? sp_box_nil() : sp_box_int(n); }
static sp_RbVal sp_IntArray_rindex_poly(sp_IntArray *a, mrb_int v)        { mrb_int n = sp_IntArray_rindex(a, v);  return n < 0 ? sp_box_nil() : sp_box_int(n); }
static sp_RbVal sp_StrArray_index_poly(sp_StrArray *a, const char *v)     { mrb_int n = sp_StrArray_index(a, v);   return n < 0 ? sp_box_nil() : sp_box_int(n); }
static sp_RbVal sp_StrArray_rindex_poly(sp_StrArray *a, const char *v)    { mrb_int n = sp_StrArray_rindex(a, v);  return n < 0 ? sp_box_nil() : sp_box_int(n); }
/* int? siblings of the *_index_poly wrappers above. Same not-found
   semantics, but return the int? sentinel (SP_INT_NIL) instead of
   boxing into sp_RbVal. Used when the call site's static type
   tracking carries the result as int? rather than poly — eliminates
   the box/unbox round-trip for the common `i = arr.index(x);
   i.nil? ? ... : <use i as int>` idiom. */
static mrb_int sp_IntArray_index_opt(sp_IntArray *a, mrb_int v)           { mrb_int n = sp_IntArray_index(a, v);   return n < 0 ? SP_INT_NIL : n; }
static mrb_int sp_IntArray_rindex_opt(sp_IntArray *a, mrb_int v)          { mrb_int n = sp_IntArray_rindex(a, v);  return n < 0 ? SP_INT_NIL : n; }
/* Inspect / to_s for an int? value. CRuby distinguishes the two on
   nil: `nil.to_s` is "" while `nil.inspect` is "nil". For a real
   integer they agree (Integer#to_s and #inspect are both the decimal
   form). Two wrappers keep call-site emit local. */
static const char *sp_int_opt_inspect(mrb_int v) { return sp_int_is_nil(v) ? "nil" : sp_int_to_s(v); }
static const char *sp_int_opt_to_s(mrb_int v)    { return sp_int_is_nil(v) ? "" : sp_int_to_s(v); }
/* float? (nullable float) counterparts: a non-nil value formats exactly
   like a plain Float (delegates to sp_float_to_s), nil renders "nil"
   (inspect) / "" (to_s). */
static const char *sp_float_opt_inspect(mrb_float v) { return sp_float_is_nil(v) ? "nil" : sp_float_to_s(v); }
static const char *sp_float_opt_to_s(mrb_float v)    { return sp_float_is_nil(v) ? "" : sp_float_to_s(v); }
/* sp_Range is a 16-byte value type that doesn't fit in sp_RbVal's union
   (max 8 bytes). When a Range crosses into a poly slot (heterogeneous
   hash / array / param / ivar), copy it onto the GC heap and box the
   pointer via SP_BUILTIN_RANGE. The Range has no internal pointer fields
   so no scanner is needed. */
static sp_RbVal sp_box_range(sp_Range v) {
  sp_Range *p = (sp_Range *)sp_gc_alloc(sizeof(sp_Range), NULL, NULL);
  *p = v;
  return sp_box_obj(p, SP_BUILTIN_RANGE);
}
static const char *sp_Range_inspect(sp_Range *r) {
  /* "first..last" / "first...last" form. Buffer sized for two int64s
     plus the dots. */
  char *buf = sp_str_alloc_raw(48);
  snprintf(buf, 48, r->excl ? "%lld...%lld" : "%lld..%lld", (long long)r->first, (long long)r->last);
  return buf;
}
/* Same heap-box rationale as sp_Range: sp_Time is 12+ bytes (tv_sec +
   tv_nsec), wider than sp_RbVal's 8-byte union. No internal pointers
   so no scanner is needed. */
static sp_RbVal sp_box_time(sp_Time v) {
  sp_Time *p = (sp_Time *)sp_gc_alloc(sizeof(sp_Time), NULL, NULL);
  *p = v;
  return sp_box_obj(p, SP_BUILTIN_TIME);
}
static const char *sp_Time_inspect(sp_Time *t) {
  /* "YYYY-MM-DD HH:MM:SS UTC" form via gmtime. CRuby uses localtime
     with a numeric tz offset, but the spinel runtime keeps Time
     timezone-naive — UTC is the unambiguous choice that doesn't need
     the platform's tzdata. Buffer is 32 chars + a margin. */
  char *buf = sp_str_alloc_raw(40);
  time_t sec = (time_t)t->tv_sec;
  struct tm *tm_ = gmtime(&sec);
  if (tm_) {
    strftime(buf, 40, "%Y-%m-%d %H:%M:%S UTC", tm_);
  } else {
    snprintf(buf, 40, "Time(%lld)", (long long)t->tv_sec);
  }
  return buf;
}
static sp_RbVal sp_box_poly_array(void *p)  { return sp_box_obj(p, SP_BUILTIN_POLY_ARRAY); }
static const char *sp_class_to_s(sp_Class c); /* fwd decl: sp_poly_puts' SP_TAG_CLASS arm */
static inline void sp_poly_puts(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT: printf("%lld\n", (long long)v.v.i); break;
    case SP_TAG_STR: if (v.v.s) { fputs(v.v.s, stdout); if (!*v.v.s || v.v.s[strlen(v.v.s)-1] != '\n') putchar('\n'); } else putchar('\n'); break;
    case SP_TAG_FLT: { fputs(sp_float_to_s(v.v.f), stdout); putchar('\n'); break; }
    case SP_TAG_BOOL: puts(v.v.b ? "true" : "false"); break;
    case SP_TAG_NIL: putchar('\n'); break;
    case SP_TAG_SYM: { const char *_ss = sp_sym_to_s((sp_sym)v.v.i); fputs(_ss, stdout); putchar('\n'); break; }
    case SP_TAG_ENCODING: { const char *_es = v.v.s ? v.v.s : sp_str_empty; fputs(_es, stdout); putchar('\n'); break; }
    case SP_TAG_CLASS: { sp_Class _c = {v.v.i}; fputs(sp_class_to_s(_c), stdout); putchar('\n'); break; }
    case SP_TAG_OBJ: {
      /* MRI's `puts arr` iterates an Array, printing one element per
         line (using to_s on each); a non-Array OBJ falls back to
         inspect / class-name. */
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: {
          sp_IntArray *_a = (sp_IntArray *)v.v.p;
          for (mrb_int _i = 0; _i < _a->len; _i++)
            printf("%lld\n", (long long)_a->data[_a->start + _i]);
          break;
        }
        case SP_BUILTIN_FLT_ARRAY: {
          sp_FloatArray *_a = (sp_FloatArray *)v.v.p;
          for (mrb_int _i = 0; _i < _a->len; _i++) {
            fputs(sp_float_to_s(_a->data[_i]), stdout); putchar('\n');
          }
          break;
        }
        case SP_BUILTIN_STR_ARRAY: {
          sp_StrArray *_a = (sp_StrArray *)v.v.p;
          for (mrb_int _i = 0; _i < _a->len; _i++) {
            const char *_s = _a->data[_i];
            if (_s) { fputs(_s, stdout); if (!*_s || _s[strlen(_s)-1] != '\n') putchar('\n'); } else putchar('\n');
          }
          break;
        }
        case SP_BUILTIN_SYM_ARRAY: {
          sp_IntArray *_a = (sp_IntArray *)v.v.p;
          for (mrb_int _i = 0; _i < _a->len; _i++) {
            const char *_s = sp_sym_to_s((sp_sym)_a->data[_a->start + _i]);
            fputs(_s, stdout); putchar('\n');
          }
          break;
        }
        case SP_BUILTIN_RANGE: puts(sp_Range_inspect((sp_Range *)v.v.p)); break;
        case SP_BUILTIN_TIME: puts(sp_Time_inspect((sp_Time *)v.v.p)); break;
        default: printf("#<Object:0x%p>\n", v.v.p); break;
      }
      break;
    }
    default: printf("%lld\n", (long long)v.v.i); break;
  }
}
static mrb_bool sp_poly_nil_p(sp_RbVal v) { return v.tag == SP_TAG_NIL; }
static mrb_bool sp_poly_truthy(sp_RbVal v) { return !(v.tag == SP_TAG_NIL || (v.tag == SP_TAG_BOOL && !v.v.b)); }
/* forward-declare the program-emitted class
   name lookup so sp_poly_to_s's SP_TAG_CLASS arm resolves.
   The codegen emits a 1-line stub when no class const is used,
   or the real body when @needs_class_table fires. The forward
   decl always needs a definition somewhere because -Werror
   trips on "used but never defined" otherwise. */
static const char *sp_class_to_s(sp_Class c);
static inline const char *sp_poly_to_s(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT: return sp_int_to_s(v.v.i);
    case SP_TAG_STR: return v.v.s ? v.v.s : sp_str_empty;
    case SP_TAG_FLT: return sp_float_to_s(v.v.f);
    case SP_TAG_BOOL: return v.v.b ? SPL("true") : SPL("false");
    case SP_TAG_NIL: return sp_str_empty;
    case SP_TAG_SYM: return sp_sym_to_s((sp_sym)v.v.i);
    case SP_TAG_CLASS: { sp_Class c = {v.v.i}; return sp_class_to_s(c); }
    case SP_TAG_ENCODING: return v.v.s ? v.v.s : sp_str_empty;
    case SP_TAG_OBJ:
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: return sp_IntArray_inspect((sp_IntArray *)v.v.p);
        case SP_BUILTIN_FLT_ARRAY: return sp_FloatArray_inspect((sp_FloatArray *)v.v.p);
        case SP_BUILTIN_STR_ARRAY: return sp_StrArray_inspect((sp_StrArray *)v.v.p);
        case SP_BUILTIN_SYM_ARRAY: return sp_SymArray_inspect((sp_IntArray *)v.v.p);
        case SP_BUILTIN_PTR_ARRAY: return sp_PtrArray_inspect((sp_PtrArray *)v.v.p);
        case SP_BUILTIN_RANGE: return sp_Range_inspect((sp_Range *)v.v.p);
        case SP_BUILTIN_TIME: return sp_Time_inspect((sp_Time *)v.v.p);
        default: return sp_str_empty;
      }
    default: return sp_str_empty;
  }
}
static sp_RbVal sp_poly_add(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return sp_box_int(a.v.i + b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f + b.v.f); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_FLT) return sp_box_float((mrb_float)a.v.i + b.v.f); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_INT) return sp_box_float(a.v.f + (mrb_float)b.v.i); if (a.tag == SP_TAG_STR && b.tag == SP_TAG_STR) return sp_box_str(sp_str_concat(a.v.s, b.v.s)); return sp_box_int(0); }
static sp_RbVal sp_poly_sub(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return sp_box_int(a.v.i - b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f - b.v.f); return sp_box_int(0); }
static sp_RbVal sp_poly_mul(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return sp_box_int(a.v.i * b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f * b.v.f); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_FLT) return sp_box_float((mrb_float)a.v.i * b.v.f); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_INT) return sp_box_float(a.v.f * (mrb_float)b.v.i); return sp_box_int(0); }
static mrb_int sp_poly_to_i(sp_RbVal v) { if (v.tag == SP_TAG_INT || v.tag == SP_TAG_SYM) return v.v.i; if (v.tag == SP_TAG_STR) return (mrb_int)strtoll(v.v.s ? v.v.s : sp_str_empty, NULL, 10); if (v.tag == SP_TAG_FLT) return (mrb_int)v.v.f; if (v.tag == SP_TAG_BOOL) return v.v.b ? 1 : 0; return 0; }
static mrb_float sp_poly_to_f(sp_RbVal v) { if (v.tag == SP_TAG_FLT) return v.v.f; if (v.tag == SP_TAG_INT || v.tag == SP_TAG_SYM) return (mrb_float)v.v.i; if (v.tag == SP_TAG_BOOL) return v.v.b ? 1.0 : 0.0; return 0.0; }
static mrb_bool sp_poly_numeric_p(sp_RbVal v) { return v.tag == SP_TAG_INT || v.tag == SP_TAG_FLT; }
static mrb_bool sp_poly_eq(sp_RbVal a, sp_RbVal b) { if (sp_poly_numeric_p(a) && sp_poly_numeric_p(b)) return sp_poly_to_f(a) == sp_poly_to_f(b); if (a.tag != b.tag) return FALSE; switch (a.tag) { case SP_TAG_INT: return a.v.i == b.v.i; case SP_TAG_STR: return (a.v.s == NULL || b.v.s == NULL) ? (a.v.s == b.v.s) : (strcmp(a.v.s, b.v.s) == 0); case SP_TAG_FLT: return a.v.f == b.v.f; case SP_TAG_BOOL: return a.v.b == b.v.b; case SP_TAG_NIL: return TRUE; case SP_TAG_SYM: return a.v.i == b.v.i; case SP_TAG_ENCODING: return (a.v.s == NULL || b.v.s == NULL) ? (a.v.s == b.v.s) : (strcmp(a.v.s, b.v.s) == 0); case SP_TAG_OBJ: return a.cls_id == b.cls_id && a.v.p == b.v.p; case SP_TAG_CLASS: return a.v.i == b.v.i; default: return FALSE; } }
static mrb_int sp_poly_cmp(sp_RbVal a, sp_RbVal b, mrb_bool *comparable) { if (sp_poly_numeric_p(a) && sp_poly_numeric_p(b)) { mrb_float af = sp_poly_to_f(a), bf = sp_poly_to_f(b); *comparable = TRUE; return (af > bf) - (af < bf); } if (a.tag == SP_TAG_STR && b.tag == SP_TAG_STR) { if (a.v.s == NULL || b.v.s == NULL) { *comparable = (a.v.s == b.v.s); return 0; } *comparable = TRUE; return strcmp(a.v.s, b.v.s); } if (a.tag == SP_TAG_SYM && b.tag == SP_TAG_SYM) { *comparable = TRUE; return (a.v.i > b.v.i) - (a.v.i < b.v.i); } *comparable = FALSE; return 0; }
static mrb_bool sp_poly_lt(sp_RbVal a, sp_RbVal b) { mrb_bool comparable; mrb_int cmp = sp_poly_cmp(a, b, &comparable); return comparable ? (cmp < 0) : FALSE; }
static mrb_bool sp_poly_le(sp_RbVal a, sp_RbVal b) { mrb_bool comparable; mrb_int cmp = sp_poly_cmp(a, b, &comparable); return comparable ? (cmp <= 0) : FALSE; }
static mrb_bool sp_poly_gt(sp_RbVal a, sp_RbVal b) { mrb_bool comparable; mrb_int cmp = sp_poly_cmp(a, b, &comparable); return comparable ? (cmp > 0) : FALSE; }
static mrb_bool sp_poly_ge(sp_RbVal a, sp_RbVal b) { mrb_bool comparable; mrb_int cmp = sp_poly_cmp(a, b, &comparable); return comparable ? (cmp >= 0) : FALSE; }
static sp_RbVal sp_poly_div(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(sp_poly_to_f(a) / sp_poly_to_f(b)); return sp_box_int(sp_idiv(sp_poly_to_i(a), sp_poly_to_i(b))); }
static sp_RbVal sp_poly_mod(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(fmod(sp_poly_to_f(a), sp_poly_to_f(b))); return sp_box_int(sp_imod(sp_poly_to_i(a), sp_poly_to_i(b))); }
/* Integer #** : Spinel has no Rational, so a negative integer exponent --
   which CRuby evaluates to a Rational like (1/2) -- raises RangeError rather
   than silently truncating toward 0. Float ** negative stays a float
   (CRuby-compatible: 2.0 ** -1 == 0.5). See docs/INCOMPATIBILITIES.md. */
static mrb_int sp_int_pow(mrb_int base, mrb_int exp) __attribute__((unused));
static mrb_int sp_int_pow(mrb_int base, mrb_int exp) { if (exp < 0) sp_raise_cls("RangeError", "negative exponent"); return (mrb_int)pow((double)base, (double)exp); }
static sp_RbVal sp_poly_pow(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT && b.v.i < 0) sp_raise_cls("RangeError", "negative exponent"); double r = pow((double)sp_poly_to_f(a), (double)sp_poly_to_f(b)); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT && b.v.i >= 0) return sp_box_int((mrb_int)r); return sp_box_float((mrb_float)r); }
/* sp_poly_shl is defined after sp_PolyArray_push (below) so the
   push-dispatch path can call it directly. The Integer-bit-shift
   semantics fall through when the recv isn't an array. */
static sp_RbVal sp_poly_shl(sp_RbVal a, sp_RbVal b);
static sp_RbVal sp_poly_shr(sp_RbVal a, sp_RbVal b) { return sp_box_int(sp_poly_to_i(a) >> sp_poly_to_i(b)); }
static sp_RbVal sp_poly_band(sp_RbVal a, sp_RbVal b) { return sp_box_int(sp_poly_to_i(a) & sp_poly_to_i(b)); }
static sp_RbVal sp_poly_bor(sp_RbVal a, sp_RbVal b) { return sp_box_int(sp_poly_to_i(a) | sp_poly_to_i(b)); }
static sp_RbVal sp_poly_bxor(sp_RbVal a, sp_RbVal b) { return sp_box_int(sp_poly_to_i(a) ^ sp_poly_to_i(b)); }
static sp_RbVal sp_poly_neg(sp_RbVal a) { if (a.tag == SP_TAG_FLT) return sp_box_float(-a.v.f); return sp_box_int(-sp_poly_to_i(a)); }

/* PolyArray: array of sp_RbVal */
typedef struct { sp_RbVal *data; mrb_int len; mrb_int cap; mrb_int frozen; } sp_PolyArray;
/* sp_mark_rbval: inline helper in sp_gc.h. */
/* Definition of the root-entry marker forward-declared near
   sp_gc_mark_all: a low-bit-tagged slot is an sp_RbVal* root. */
/* sp_gc_mark_root_entry is an inline helper in sp_gc.h. */
static void sp_PolyArray_scan(void *p) { sp_PolyArray *a = (sp_PolyArray *)p; for (mrb_int i = 0; i < a->len; i++) sp_mark_rbval(a->data[i]); }
static void sp_PolyArray_fin(void *p) { sp_PolyArray *a = (sp_PolyArray *)p; sp_gc_hdr *h = (sp_gc_hdr *)((char *)a - sizeof(sp_gc_hdr)); sp_gc_bytes -= sizeof(sp_RbVal) * a->cap; h->size -= sizeof(sp_RbVal) * a->cap; free(a->data); }
static sp_PolyArray *sp_PolyArray_new(void) { sp_PolyArray *a = (sp_PolyArray *)sp_gc_alloc(sizeof(sp_PolyArray), sp_PolyArray_fin, sp_PolyArray_scan); a->cap = 16; a->data = (sp_RbVal *)malloc(sizeof(sp_RbVal) * a->cap); if (!a->data) sp_oom_die(); a->len = 0; { sp_gc_hdr *h = (sp_gc_hdr *)((char *)a - sizeof(sp_gc_hdr)); h->size += sizeof(sp_RbVal) * a->cap; sp_gc_bytes += sizeof(sp_RbVal) * a->cap; } return a; }

/* PolyArray free-list pool (manual recycle, GC-independent).

   sp_PolyArray_pool_acquire() pops a pre-cleared PolyArray off a
   process-global LIFO; if the pool is empty it falls back to
   sp_PolyArray_new(). sp_PolyArray_pool_release(a) clears `a` and
   pushes it back (capped by SP_POLY_ARRAY_POOL_CAP, default 4096).

   Intended use: short-lived arrays whose lifetime is bounded by the
   enclosing call frame. The canonical case is tree-walking
   interpreters that evaluate per-call argument lists (e.g.
   `evaluate_args(node)` returning a fresh PolyArray that the called
   method consumes via a `bind_params` step and never retains). In
   that pattern the pool eliminates one ~448 B alloc (16 sp_RbVal
   cap + sp_gc_hdr + sp_PolyArray header) per call site, which under
   SPINEL_NO_GC=1 is otherwise an unrecoverable leak driving
   memory-pressure kills on long-running interpreters.

   The release helper does NOT consult sp_gc_bytes; the array stays
   tracked in the GC heap the same as any other PolyArray. The pool
   just keeps the (header, data buffer) pair in a per-process LIFO
   so the next acquire skips malloc.

   Safety: the caller must guarantee that no live reference to `a`
   exists past the release point. Releasing an array that is still
   reachable from a live root will result in use-after-free the
   next time acquire hands it out. */
#ifndef SP_POLY_ARRAY_POOL_CAP
#define SP_POLY_ARRAY_POOL_CAP 4096
#endif
static sp_PolyArray *sp_PolyArray_pool_buf[SP_POLY_ARRAY_POOL_CAP];
static int sp_PolyArray_pool_count = 0;
/* GC root-scan callback for the pool. Called from
   sp_re_mark_globals (forward-declared above) so pooled arrays are
   kept alive across a GC cycle. Without this, sp_gc_collect would
   sweep pooled-but-not-otherwise-reachable arrays and the pool
   would dispense dangling pointers. */
static void sp_PolyArray_pool_scan_roots(void) {
  for (int i = 0; i < sp_PolyArray_pool_count; i++) {
    sp_gc_mark(sp_PolyArray_pool_buf[i]);
  }
}
static inline sp_PolyArray *sp_PolyArray_pool_acquire(void) {
  if (sp_PolyArray_pool_count > 0) {
    sp_PolyArray *a = sp_PolyArray_pool_buf[--sp_PolyArray_pool_count];
    a->len = 0;
    /* Clear frozen so reuse doesn't surface stale FrozenError on
       Array#push (post-#760 the flag persists on the struct). */
    a->frozen = 0;
    return a;
  }
  return sp_PolyArray_new();
}
static inline void sp_PolyArray_pool_release(sp_PolyArray *a) {
  if (a == NULL) return;
  if (sp_PolyArray_pool_count >= SP_POLY_ARRAY_POOL_CAP) return;
  a->len = 0;
  a->frozen = 0;
  sp_PolyArray_pool_buf[sp_PolyArray_pool_count++] = a;
}
static void sp_PolyArray_push(sp_PolyArray *a, sp_RbVal v) { if (!a) return; if (a->frozen) { sp_raise_frozen_array(); return; } if (a->len >= a->cap) { sp_gc_hdr *h = (sp_gc_hdr *)((char *)a - sizeof(sp_gc_hdr)); sp_gc_bytes -= sizeof(sp_RbVal) * a->cap; h->size -= sizeof(sp_RbVal) * a->cap; a->cap = a->cap * 2 + 1; void *nd = realloc(a->data, sizeof(sp_RbVal) * a->cap); if (!nd) sp_oom_die(); a->data = (sp_RbVal *)nd; h->size += sizeof(sp_RbVal) * a->cap; sp_gc_bytes += sizeof(sp_RbVal) * a->cap; } a->data[a->len++] = v; }
static sp_PolyArray *sp_re_scan_poly(mrb_regexp_pattern *pat, const char *str) {
  sp_PolyArray *arr = sp_PolyArray_new();
  SP_GC_ROOT(arr);
  int64_t slen = (int64_t)strlen(str);
  int64_t pos = 0;
  int ncaps = 64;
  int caps[64];
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, ncaps);
    if (n <= 0 || caps[0] < 0) break;
    int pairs = (n > ncaps ? ncaps : n) / 2;
    if (pairs <= 1) {
      int len = caps[1] - caps[0];
      char *m = sp_str_alloc_raw(len + 1);
      memcpy(m, str + caps[0], len);
      m[len] = 0;
      sp_PolyArray_push(arr, sp_box_str(m));
    } else {
      sp_PolyArray *row = sp_PolyArray_new();
      for (int gi = 1; gi < pairs; gi++) {
        if (caps[gi * 2] >= 0 && caps[gi * 2 + 1] >= 0) {
          int glen = caps[gi * 2 + 1] - caps[gi * 2];
          char *gm = sp_str_alloc_raw(glen + 1);
          memcpy(gm, str + caps[gi * 2], glen);
          gm[glen] = 0;
          sp_PolyArray_push(row, sp_box_str(gm));
        } else {
          sp_PolyArray_push(row, sp_box_nil());
        }
      }
      sp_PolyArray_push(arr, sp_box_poly_array(row));
    }
    pos = caps[1];
    if (caps[0] == caps[1]) pos++;
  }
  return arr;
}
static sp_PolyArray *sp_re_match_data(mrb_regexp_pattern *pat, const char *str) {
  int64_t slen = (int64_t)strlen(str);
  int ncaps = 64;
  int n = re_exec(pat, str, slen, 0, sp_re_caps, ncaps);
  if (n <= 0 || sp_re_caps[0] < 0) {
    for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
    sp_re_last_str = NULL;
    sp_re_match_str = NULL;
    sp_re_match_pre = NULL;
    sp_re_match_post = NULL;
    return NULL;
  }
  int pairs = (n > ncaps ? ncaps : n) / 2;
  sp_re_set_captures(str, sp_re_caps, pairs);
  sp_PolyArray *arr = sp_PolyArray_new();
  for (int i = 0; i < pairs; i++) {
    int start = sp_re_caps[i * 2];
    int end = sp_re_caps[i * 2 + 1];
    if (start >= 0 && end >= start) {
      int len = end - start;
      char *buf = sp_str_alloc_raw(len + 1);
      memcpy(buf, str + start, len);
      buf[len] = 0;
      sp_PolyArray_push(arr, sp_box_str(buf));
    } else {
      sp_PolyArray_push(arr, sp_box_nil());
    }
  }
  return arr;
}
/* MatchData — holds the source string and the per-group byte offsets
   the engine produced. `[]`/captures extract substrings on demand;
   offset/begin/end report CHARACTER offsets (CRuby semantics), so
   byte offsets are converted via sp_str_count_chars. Group i occupies
   caps[2i] (begin) and caps[2i+1] (end); -1 marks a non-participating
   group. Issue #974. */
typedef struct { const char *source; int caps[64]; int ncap; } sp_MatchData;
static void sp_MatchData_scan(void *p) { sp_MatchData *m = (sp_MatchData *)p; if (m->source) sp_mark_string(m->source); }
static sp_MatchData *sp_re_matchdata(mrb_regexp_pattern *pat, const char *str) {
  int64_t slen = (int64_t)strlen(str);
  int caps[64];
  int n = re_exec(pat, str, slen, 0, caps, 64);
  if (n <= 0 || caps[0] < 0) {
    for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
    sp_re_last_str = NULL; sp_re_match_str = NULL;
    sp_re_match_pre = NULL; sp_re_match_post = NULL;
    return NULL;
  }
  int pairs = (n > 64 ? 64 : n) / 2;
  sp_re_set_captures(str, caps, pairs);
  sp_MatchData *m = (sp_MatchData *)sp_gc_alloc(sizeof(sp_MatchData), NULL, sp_MatchData_scan);
  m->source = str;
  m->ncap = pairs;
  for (int i = 0; i < pairs * 2; i++) m->caps[i] = caps[i];
  return m;
}
/* String#match(/re/, pos) — pos is a codepoint index (CRuby semantics). */
static sp_MatchData *sp_re_matchdata_at(mrb_regexp_pattern *pat, const char *str, mrb_int cpos) {
  mrb_int cl = sp_str_length(str);
  if (cpos < 0) cpos += cl;
  if (cpos < 0 || cpos > cl) return NULL;
  size_t boff = sp_utf8_byte_offset(str, cpos);
  int64_t slen = (int64_t)strlen(str);
  int caps[64];
  int n = re_exec(pat, str, slen, (mrb_int)boff, caps, 64);
  if (n <= 0 || caps[0] < 0) {
    for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
    sp_re_last_str = NULL; sp_re_match_str = NULL;
    sp_re_match_pre = NULL; sp_re_match_post = NULL;
    return NULL;
  }
  int pairs = (n > 64 ? 64 : n) / 2;
  sp_re_set_captures(str, caps, pairs);
  sp_MatchData *m = (sp_MatchData *)sp_gc_alloc(sizeof(sp_MatchData), NULL, sp_MatchData_scan);
  m->source = str;
  m->ncap = pairs;
  for (int i = 0; i < pairs * 2; i++) m->caps[i] = caps[i];
  return m;
}
/* group i substring, or NULL for a non-participating / out-of-range group */
static const char *sp_MatchData_aref(sp_MatchData *m, mrb_int i) {
  if (!m || i < 0 || i >= m->ncap) return NULL;
  int s = m->caps[i * 2], e = m->caps[i * 2 + 1];
  if (s < 0 || e < s) return NULL;
  int len = e - s;
  char *b = sp_str_alloc((size_t)len);
  memcpy(b, m->source + s, len);
  b[len] = 0;
  sp_str_set_len(b, (size_t)len);
  return b;
}
static mrb_int sp_MatchData_length(sp_MatchData *m) { return m ? m->ncap : 0; }
/* char offset of a byte position within source */
static mrb_int sp_md_char_off(sp_MatchData *m, int byteoff) {
  if (byteoff < 0) return SP_INT_NIL;
  return sp_str_count_chars(m->source, (size_t)byteoff);
}
static mrb_int sp_MatchData_begin(sp_MatchData *m, mrb_int i) {
  if (!m || i < 0 || i >= m->ncap) return SP_INT_NIL;
  return sp_md_char_off(m, m->caps[i * 2]);
}
static mrb_int sp_MatchData_end(sp_MatchData *m, mrb_int i) {
  if (!m || i < 0 || i >= m->ncap) return SP_INT_NIL;
  return sp_md_char_off(m, m->caps[i * 2 + 1]);
}
static sp_IntArray *sp_MatchData_offset(sp_MatchData *m, mrb_int i) {
  sp_IntArray *a = sp_IntArray_new();
  if (!m || i < 0 || i >= m->ncap) { sp_IntArray_push(a, SP_INT_NIL); sp_IntArray_push(a, SP_INT_NIL); return a; }
  sp_IntArray_push(a, sp_md_char_off(m, m->caps[i * 2]));
  sp_IntArray_push(a, sp_md_char_off(m, m->caps[i * 2 + 1]));
  return a;
}
/* whole-match string (group 0) — also MatchData#to_s */
static const char *sp_MatchData_to_s(sp_MatchData *m) { const char *r = sp_MatchData_aref(m, 0); return r ? r : sp_str_empty; }
/* captures: groups 1..n-1 as a poly array (nil for non-participating) */
static sp_PolyArray *sp_MatchData_captures(sp_MatchData *m) {
  sp_PolyArray *r = sp_PolyArray_new();
  if (!m) return r;
  SP_GC_ROOT(r);
  for (mrb_int i = 1; i < m->ncap; i++) {
    const char *g = sp_MatchData_aref(m, i);
    sp_PolyArray_push(r, g ? sp_box_str(g) : sp_box_nil());
  }
  return r;
}
/* to_a: group 0 + captures */
static sp_PolyArray *sp_MatchData_to_a(sp_MatchData *m) {
  sp_PolyArray *r = sp_PolyArray_new();
  if (!m) return r;
  SP_GC_ROOT(r);
  for (mrb_int i = 0; i < m->ncap; i++) {
    const char *g = sp_MatchData_aref(m, i);
    sp_PolyArray_push(r, g ? sp_box_str(g) : sp_box_nil());
  }
  return r;
}
static const char *sp_MatchData_pre_match(sp_MatchData *m) {
  if (!m) return sp_str_empty;
  int e = m->caps[0];
  if (e <= 0) return sp_str_empty;
  char *b = sp_str_alloc((size_t)e);
  memcpy(b, m->source, e); b[e] = 0; sp_str_set_len(b, (size_t)e);
  return b;
}
static const char *sp_MatchData_post_match(sp_MatchData *m) {
  if (!m) return sp_str_empty;
  int s = m->caps[1];
  size_t sl = strlen(m->source);
  if (s < 0 || (size_t)s >= sl) return sp_str_empty;
  size_t len = sl - (size_t)s;
  char *b = sp_str_alloc(len);
  memcpy(b, m->source + s, len); b[len] = 0; sp_str_set_len(b, len);
  return b;
}

static sp_RbVal sp_poly_shl(sp_RbVal a, sp_RbVal b) {
  /* Dispatch by recv cls_id: an IntArray / PolyArray / etc. boxed
     into a poly slot still wants Array#<< (push), not Integer#<<
     (bit-shift). Falls through to bit-shift only when the recv is
     a non-array. Returns the recv (matching `<<`s chainability). */
  if (a.tag == SP_TAG_OBJ) {
    if (a.cls_id == SP_BUILTIN_INT_ARRAY) {
      sp_IntArray_push((sp_IntArray *)a.v.p, b.tag == SP_TAG_INT ? b.v.i : sp_poly_to_i(b));
      return a;
    }
    if (a.cls_id == SP_BUILTIN_POLY_ARRAY) {
      sp_PolyArray_push((sp_PolyArray *)a.v.p, b);
      return a;
    }
    if (a.cls_id == SP_BUILTIN_PTR_ARRAY) {
      sp_PtrArray_push((sp_PtrArray *)a.v.p, b.v.p);
      return a;
    }
    if (a.cls_id == SP_BUILTIN_FLT_ARRAY) {
      sp_FloatArray_push((sp_FloatArray *)a.v.p, b.tag == SP_TAG_FLT ? b.v.f : (mrb_float)sp_poly_to_i(b));
      return a;
    }
    if (a.cls_id == SP_BUILTIN_STR_ARRAY) {
      sp_StrArray_push((sp_StrArray *)a.v.p, b.tag == SP_TAG_STR ? (const char *)b.v.p : sp_str_empty);
      return a;
    }
  }
  return sp_box_int(sp_poly_to_i(a) << sp_poly_to_i(b));
}
static mrb_int sp_PolyArray_length(sp_PolyArray *a) { if (!a) return 0; return a->len; }
static sp_RbVal sp_PolyArray_get(sp_PolyArray *a, mrb_int i) { if (!a) return sp_box_nil(); if (i < 0) i += a->len; if (i < 0 || i >= a->len) return sp_box_nil(); return a->data[i]; }
/* Issues #770, #789: NULL + bounds guard. Out-of-range set no-ops. */
static void sp_PolyArray_set(sp_PolyArray *a, mrb_int i, sp_RbVal v) { if (!a) return; if (a->frozen) { sp_raise_frozen_array(); return; } mrb_int orig=i; if (i < 0) i += a->len; if (i < 0) sp_raise_cls("IndexError", sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)-a->len)); if (i >= a->len) return; a->data[i] = v; }
static sp_PolyArray *sp_PolyArray_slice(sp_PolyArray *a, mrb_int start, mrb_int len) { if (start < 0) start += a->len; if (start < 0) start = 0; sp_PolyArray *b = sp_PolyArray_new(); if (start >= a->len || len <= 0) return b; if (start + len > a->len) len = a->len - start; for (mrb_int i = 0; i < len; i++) sp_PolyArray_push(b, a->data[start + i]); return b; }
static sp_PolyArray *sp_PolyArray_slice_bang(sp_PolyArray *a, mrb_int from, mrb_int n) {
  if (!a) return sp_PolyArray_new();
  if (a->frozen) { sp_raise_frozen_array(); return sp_PolyArray_new(); }
  if (from < 0) from += a->len;
  if (from < 0) from = 0;
  if (from > a->len) from = a->len;
  if (n < 0) n = 0;
  if (from + n > a->len) n = a->len - from;
  sp_PolyArray *r = sp_PolyArray_new();
  for (mrb_int i = 0; i < n; i++) sp_PolyArray_push(r, a->data[from + i]);
  for (mrb_int i = from; i + n < a->len; i++) a->data[i] = a->data[i + n];
  a->len -= n;
  return r;
}
static sp_PolyArray *sp_PolyArray_dup(sp_PolyArray *a) { sp_PolyArray *b = sp_PolyArray_new(); for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_push(b, a->data[i]); return b; }
/* Array#compact for poly_array: keep elements whose tag is not SP_TAG_NIL. */
static sp_PolyArray *sp_PolyArray_compact(sp_PolyArray *a) { sp_PolyArray *b = sp_PolyArray_new(); if (!a) return b; for (mrb_int i = 0; i < a->len; i++) { if (a->data[i].tag != SP_TAG_NIL) sp_PolyArray_push(b, a->data[i]); } return b; }
/* Issue #738: Hash#to_a as poly_array of [key, value] poly_array pairs. */
static sp_PolyArray*sp_StrIntHash_to_a(sp_StrIntHash*h){sp_PolyArray*r=sp_PolyArray_new();if(!h)return r;for(mrb_int i=0;i<h->len;i++){sp_PolyArray*p=sp_PolyArray_new();sp_PolyArray_push(p,sp_box_str(h->order[i]));sp_PolyArray_push(p,sp_box_int(sp_StrIntHash_get(h,h->order[i])));sp_PolyArray_push(r,sp_box_poly_array(p));}return r;}
static sp_PolyArray*sp_StrStrHash_to_a(sp_StrStrHash*h){sp_PolyArray*r=sp_PolyArray_new();if(!h)return r;for(mrb_int i=0;i<h->len;i++){sp_PolyArray*p=sp_PolyArray_new();sp_PolyArray_push(p,sp_box_str(h->order[i]));sp_PolyArray_push(p,sp_box_str(sp_StrStrHash_get(h,h->order[i])));sp_PolyArray_push(r,sp_box_poly_array(p));}return r;}
static sp_PolyArray*sp_IntStrHash_to_a(sp_IntStrHash*h){sp_PolyArray*r=sp_PolyArray_new();if(!h)return r;for(mrb_int i=0;i<h->len;i++){sp_PolyArray*p=sp_PolyArray_new();sp_PolyArray_push(p,sp_box_int(h->order[i]));sp_PolyArray_push(p,sp_box_str(sp_IntStrHash_get(h,h->order[i])));sp_PolyArray_push(r,sp_box_poly_array(p));}return r;}
/* Array#flatten -- walk into nested array values recursively. Each
   array-tagged element (IntArray / StrArray / SymArray / FloatArray /
   PolyArray) is expanded inline; scalars are appended as-is. Issue
   #739. */
static void sp_PolyArray_flatten_into(sp_PolyArray *dst, sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ) { sp_PolyArray_push(dst, v); return; }
  if (v.cls_id == SP_BUILTIN_INT_ARRAY) { sp_IntArray *ia = (sp_IntArray *)v.v.p; for (mrb_int i = 0; i < ia->len; i++) sp_PolyArray_push(dst, sp_box_int(ia->data[ia->start + i])); return; }
  if (v.cls_id == SP_BUILTIN_STR_ARRAY) { sp_StrArray *sa = (sp_StrArray *)v.v.p; for (mrb_int i = 0; i < sa->len; i++) sp_PolyArray_push(dst, sp_box_str(sa->data[i])); return; }
  if (v.cls_id == SP_BUILTIN_POLY_ARRAY) { sp_PolyArray *pa = (sp_PolyArray *)v.v.p; for (mrb_int i = 0; i < pa->len; i++) sp_PolyArray_flatten_into(dst, pa->data[i]); return; }
  /* Other array variants fall through as opaque elements; rare for
     deep-flatten use cases. */
  sp_PolyArray_push(dst, v);
}
static sp_PolyArray *sp_PolyArray_flatten(sp_PolyArray *a) { sp_PolyArray *b = sp_PolyArray_new(); if (!a) return b; for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_flatten_into(b, a->data[i]); return b; }

/* Box-into-poly converters used by the printf-with-array codegen
   (`"%fmt" % typed_array`). The format helper expects sp_RbVal
   slots so it can dispatch per-element. */
static sp_PolyArray *sp_IntArray_to_poly(sp_IntArray *a) {
  sp_PolyArray *r = sp_PolyArray_new();
  if (!a) return r;
  for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_push(r, sp_box_int(a->data[a->start + i]));
  return r;
}
static sp_PolyArray *sp_StrArray_to_poly_fmt(sp_StrArray *a) {
  sp_PolyArray *r = sp_PolyArray_new();
  if (!a) return r;
  for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_push(r, sp_box_str(a->data[i]));
  return r;
}

/* String#% with a poly_array argument. Walks the format and for
   each spec ("%s", "%d", "%f", "%x", "%o", etc.) pulls the next
   array element. Width / flag chars between `%` and the conv
   letter (`-+0 #`, digits, `.`) are copied verbatim so printf
   does the substitution work. */
static const char *sp_str_format_polyarr(const char *fmt, sp_PolyArray *a) {
  size_t cap = strlen(fmt) + 64;
  char *buf = (char *)malloc(cap);
  if (!buf) { perror("malloc"); exit(1); }
  size_t out = 0; mrb_int idx = 0; const char *p = fmt;
  while (*p) {
    if (*p != '%') {
      if (out + 1 >= cap) { cap = cap * 2; buf = (char *)realloc(buf, cap); }
      buf[out++] = *p++; continue;
    }
    if (p[1] == '%') {
      if (out + 1 >= cap) { cap = cap * 2; buf = (char *)realloc(buf, cap); }
      buf[out++] = '%'; p += 2; continue;
    }
    char spec[64]; size_t sl = 0; spec[sl++] = *p++;
    while (*p && sl < sizeof(spec) - 4) {
      char c = *p;
      if (c == '-' || c == '+' || c == ' ' || c == '#' || c == '0' || c == '.' || (c >= '0' && c <= '9')) { spec[sl++] = c; p++; }
      else break;
    }
    if (!*p) break;
    char conv = *p++; spec[sl++] = conv;
    char fmt_use[80];
    if (conv == 'd' || conv == 'i' || conv == 'x' || conv == 'X' || conv == 'o') {
      memcpy(fmt_use, spec, sl - 1);
      fmt_use[sl - 1] = 'l'; fmt_use[sl] = 'l'; fmt_use[sl + 1] = conv; fmt_use[sl + 2] = 0;
    } else {
      memcpy(fmt_use, spec, sl); fmt_use[sl] = 0;
    }
    char tmp[256]; int wn = 0;
    sp_RbVal v = (idx < a->len) ? a->data[idx] : sp_box_nil();
    idx++;
    if (conv == 'd' || conv == 'i' || conv == 'x' || conv == 'X' || conv == 'o') {
      long long lv = 0;
      if (v.tag == SP_TAG_INT) lv = (long long)v.v.i;
      else if (v.tag == SP_TAG_FLT) lv = (long long)v.v.f;
      else if (v.tag == SP_TAG_STR && v.v.s) lv = strtoll(v.v.s, NULL, 10);
      wn = snprintf(tmp, sizeof(tmp), fmt_use, lv);
    } else if (conv == 'f' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G') {
      double dv = 0;
      if (v.tag == SP_TAG_FLT) dv = v.v.f;
      else if (v.tag == SP_TAG_INT) dv = (double)v.v.i;
      wn = snprintf(tmp, sizeof(tmp), fmt_use, dv);
    } else if (conv == 's') {
      const char *sv = "";
      char num_buf[32];
      if (v.tag == SP_TAG_STR) sv = v.v.s ? v.v.s : "";
      else if (v.tag == SP_TAG_INT) { snprintf(num_buf, sizeof(num_buf), "%lld", (long long)v.v.i); sv = num_buf; }
      else if (v.tag == SP_TAG_FLT) { snprintf(num_buf, sizeof(num_buf), "%g", v.v.f); sv = num_buf; }
      wn = snprintf(tmp, sizeof(tmp), fmt_use, sv);
    } else if (conv == 'c') {
      int cv = 0;
      if (v.tag == SP_TAG_INT) cv = (int)v.v.i;
      else if (v.tag == SP_TAG_STR && v.v.s && v.v.s[0]) cv = (unsigned char)v.v.s[0];
      wn = snprintf(tmp, sizeof(tmp), fmt_use, cv);
    } else {
      memcpy(tmp, spec, sl); tmp[sl] = 0; wn = (int)sl; idx--;
    }
    if (wn < 0) continue;
    if (out + (size_t)wn + 1 >= cap) { cap = (out + wn) * 2 + 64; buf = (char *)realloc(buf, cap); }
    memcpy(buf + out, tmp, wn); out += wn;
  }
  buf[out] = 0;
  char *r = sp_str_alloc(out); memcpy(r, buf, out); free(buf); return r;
}

/* Array#assoc — return the first sub-array (poly_array element)
   whose first element equals `key`. Returns NULL when no match
   so the caller's `.inspect` round-trips to "nil". */
static sp_PolyArray *sp_PolyArray_assoc(sp_PolyArray *a, sp_RbVal key) {
  if (!a) return NULL;
  for (mrb_int i = 0; i < a->len; i++) {
    sp_RbVal el = a->data[i];
    if (el.tag != SP_TAG_OBJ) continue;
    if (el.cls_id != SP_BUILTIN_POLY_ARRAY) continue;
    sp_PolyArray *sub = (sp_PolyArray *)el.v.p;
    if (sub && sub->len >= 1 && sp_poly_eq(sub->data[0], key)) return sub;
  }
  return NULL;
}

/* Array#rassoc — same as assoc but matches against the second
   element of each sub-array. */
static sp_PolyArray *sp_PolyArray_rassoc(sp_PolyArray *a, sp_RbVal val) {
  if (!a) return NULL;
  for (mrb_int i = 0; i < a->len; i++) {
    sp_RbVal el = a->data[i];
    if (el.tag != SP_TAG_OBJ) continue;
    if (el.cls_id != SP_BUILTIN_POLY_ARRAY) continue;
    sp_PolyArray *sub = (sp_PolyArray *)el.v.p;
    if (sub && sub->len >= 2 && sp_poly_eq(sub->data[1], val)) return sub;
  }
  return NULL;
}
/* Depth-bounded variant. depth==0 returns a shallow copy; each
   recursive step decrements depth, and a negative depth means
   "unlimited" (same as flatten without arg). Used by
   `Array#flatten(n)`. */
static void sp_PolyArray_flatten_into_n(sp_PolyArray *dst, sp_RbVal v, mrb_int depth) {
  if (depth == 0 || v.tag != SP_TAG_OBJ) { sp_PolyArray_push(dst, v); return; }
  if (v.cls_id == SP_BUILTIN_INT_ARRAY) { sp_IntArray *ia = (sp_IntArray *)v.v.p; for (mrb_int i = 0; i < ia->len; i++) sp_PolyArray_push(dst, sp_box_int(ia->data[ia->start + i])); return; }
  if (v.cls_id == SP_BUILTIN_STR_ARRAY) { sp_StrArray *sa = (sp_StrArray *)v.v.p; for (mrb_int i = 0; i < sa->len; i++) sp_PolyArray_push(dst, sp_box_str(sa->data[i])); return; }
  if (v.cls_id == SP_BUILTIN_POLY_ARRAY) { sp_PolyArray *pa = (sp_PolyArray *)v.v.p; for (mrb_int i = 0; i < pa->len; i++) sp_PolyArray_flatten_into_n(dst, pa->data[i], depth - 1); return; }
  sp_PolyArray_push(dst, v);
}
static sp_PolyArray *sp_PolyArray_flatten_n(sp_PolyArray *a, mrb_int depth) {
  sp_PolyArray *b = sp_PolyArray_new();
  if (!a) return b;
  if (depth < 0) depth = INT64_MAX;
  for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_flatten_into_n(b, a->data[i], depth);
  return b;
}
/* Sum the integer-tagged elements of a poly_array. Used by
   `Array#sum` on a poly_array whose runtime tags are uniform int
   (e.g. the result of `arr.map { _1[:int_key] }`). Non-int tags
   contribute zero. */
static mrb_int sp_PolyArray_sum_int(sp_PolyArray *a) { if (!a) return 0; mrb_int s = 0; for (mrb_int i = 0; i < a->len; i++) { if (a->data[i].tag == SP_TAG_INT) s += a->data[i].v.i; } return s; }
static void sp_PolyArray_shuffle_bang(sp_PolyArray *a) { if (!a || a->frozen) { if (a && a->frozen) sp_raise_frozen_array(); return; } for (mrb_int i = a->len - 1; i > 0; i--) { mrb_int j = (mrb_int)(rand() % (i + 1)); sp_RbVal t = a->data[i]; a->data[i] = a->data[j]; a->data[j] = t; } }
static void sp_PolyArray_rotate_bang(sp_PolyArray*a,mrb_int n){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}if(a->len<=0)return;n=((n%a->len)+a->len)%a->len;if(n==0)return;sp_RbVal*d=a->data;mrb_int lo=0,hi=n-1;while(lo<hi){sp_RbVal t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=n;hi=a->len-1;while(lo<hi){sp_RbVal t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=0;hi=a->len-1;while(lo<hi){sp_RbVal t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}}
static sp_PolyArray *sp_PolyArray_shuffle(sp_PolyArray *a) { sp_PolyArray *b = sp_PolyArray_dup(a); sp_PolyArray_shuffle_bang(b); return b; }
static sp_RbVal sp_PolyArray_sample(sp_PolyArray *a) { if (a->len <= 0) return sp_box_nil(); return a->data[(mrb_int)(rand()%a->len)]; }

/* Forward decl: sp_poly_inspect dispatches into sp_PolyArray_inspect
   for nested poly arrays (under promote, an `each_cons` chain's outer
   accumulator boxes each inner poly_array element), but the
   sp_PolyArray_inspect body lives a few lines below. */
static const char *sp_PolyArray_inspect(sp_PolyArray *a);
static const char*sp_PolyArrayPtrArray_inspect(sp_PtrArray*a){sp_String*s=sp_String_new("[");for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_PolyArray_inspect((sp_PolyArray*)a->data[i]));}sp_String_append(s,"]");return s->data;}

/* Object#inspect for a tagged sp_RbVal. Dispatches on the runtime tag;
   each branch reuses the matching primitive inspect helper. Falls back
   to "#<Object>" for SP_TAG_OBJ because the runtime has no class-name
   table yet (follow-up PR). Returns a GC-managed C string. */
static inline const char *sp_poly_inspect(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT:  return sp_int_to_s(v.v.i);
    case SP_TAG_STR:  return sp_str_inspect(v.v.s);
    case SP_TAG_FLT:  return sp_float_to_s(v.v.f);
    case SP_TAG_BOOL: return v.v.b ? SPL("true") : SPL("false");
    case SP_TAG_NIL:  return SPL("nil");
    case SP_TAG_SYM:  return sp_str_concat(SPL(":"), sp_sym_to_s((sp_sym)v.v.i));
    case SP_TAG_ENCODING: return sp_sprintf("#<Encoding:%s>", v.v.s ? v.v.s : "");
    case SP_TAG_OBJ:
 /* Built-in container / value-type tags get their typed inspect
    helper. Matches the dispatch shape in sp_poly_to_s above and the
    `puts` poly arm earlier in this file; without it, a Range / Time
    / typed Array stored as an sp_RbVal value (e.g. a sym_poly_hash
    that mixes `200..299` and `404`) reported "#<Object>" from
    `.inspect`, which was both wrong for CRuby parity and useless
    for debugging. */
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: return sp_IntArray_inspect((sp_IntArray *)v.v.p);
        case SP_BUILTIN_FLT_ARRAY: return sp_FloatArray_inspect((sp_FloatArray *)v.v.p);
        case SP_BUILTIN_STR_ARRAY: return sp_StrArray_inspect((sp_StrArray *)v.v.p);
        case SP_BUILTIN_SYM_ARRAY: return sp_SymArray_inspect((sp_IntArray *)v.v.p);
        case SP_BUILTIN_PTR_ARRAY: return sp_PtrArray_inspect((sp_PtrArray *)v.v.p);
        case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_inspect((sp_PolyArray *)v.v.p);
        case SP_BUILTIN_RANGE:     return sp_Range_inspect((sp_Range *)v.v.p);
        case SP_BUILTIN_TIME:      return sp_Time_inspect((sp_Time *)v.v.p);
        default:                   return SPL("#<Object>");
      }
    default:          return sp_str_empty;
  }
}
/* Array#inspect for heterogeneous poly arrays. Each element dispatches
   through sp_poly_inspect, so a mixed `[1, "x", :y]` renders
   `[1, "x", :y]` byte-for-byte identical to CRuby. NULL renders
   "nil" so callers that store a nil-returning slot (assoc/rassoc
   miss, etc.) round-trip cleanly through `.inspect`. */
static const char *sp_PolyArray_inspect(sp_PolyArray *a) {
  if (!a) { char *r = sp_str_alloc(3); r[0] = 'n'; r[1] = 'i'; r[2] = 'l'; r[3] = 0; sp_str_set_len(r, 3); return r; }
  sp_String *s = sp_String_new("[");
  for (mrb_int i = 0; i < a->len; i++) {
    if (i > 0) sp_String_append(s, ", ");
    sp_String_append(s, sp_poly_inspect(a->data[i]));
  }
  sp_String_append(s, "]");
  return s->data;
}
/* Array#join for a mixed-element (poly) array: to_s each element via
   sp_poly_to_s and concatenate with sep. Mirrors sp_StrArray_join for
   the boxed-element case. */
static const char *sp_PolyArray_join(sp_PolyArray *a, const char *sep) {
  if (!a) return sp_str_empty;
  sp_String *s = sp_String_new("");
  for (mrb_int i = 0; i < a->len; i++) {
    if (i > 0 && sep) sp_String_append(s, sep);
    sp_String_append(s, sp_poly_to_s(a->data[i]));
  }
  return s->data;
}
static mrb_bool sp_PolyArray_eq(sp_PolyArray *a, sp_PolyArray *b) {
  if (!a || !b) return a == b;
  if (a->len != b->len) return FALSE;
  for (mrb_int i = 0; i < a->len; i++) {
    if (!sp_poly_eq(a->data[i], b->data[i])) return FALSE;
  }
  return TRUE;
}
static mrb_bool sp_PolyArray_include(sp_PolyArray *a, sp_RbVal v) {
  if (!a) return FALSE;
  for (mrb_int i = 0; i < a->len; i++) {
    if (sp_poly_eq(a->data[i], v)) return TRUE;
  }
  return FALSE;
}

/* Mark the embedded GC reference inside an sp_RbVal (string or obj).
   Used as the scan hook for containers that store polymorphic values. */
/* sp_mark_rbval is an inline helper in sp_gc.h. */

/* StrPolyHash: string keys, sp_RbVal values — for hashes with mixed value types. */
/* `dproc` holds a Hash.new{} default block, lowered to a dedicated C
   fn `sp_RbVal (*)(sp_StrPolyHash *self, const char *key)` with typed
   params (codegen emits it). Called by _get on a miss. Issue #912. */
typedef struct sp_StrPolyHash sp_StrPolyHash;
typedef sp_RbVal (*sp_strpoly_dproc_t)(sp_StrPolyHash *, const char *);
struct sp_StrPolyHash{const char**keys;sp_RbVal*vals;const char**order;mrb_int len;mrb_int cap;mrb_int mask;sp_RbVal default_v;sp_strpoly_dproc_t dproc;};
static void sp_StrPolyHash_fin(void*p){sp_StrPolyHash*h=(sp_StrPolyHash*)p;free(h->keys);free(h->vals);free(h->order);}
static void sp_StrPolyHash_scan(void*p){sp_StrPolyHash*h=(sp_StrPolyHash*)p;for(mrb_int i=0;i<h->cap;i++){if(h->keys[i]){sp_mark_string(h->keys[i]);sp_mark_rbval(h->vals[i]);}}sp_mark_rbval(h->default_v);}
static sp_StrPolyHash*sp_StrPolyHash_new(void){sp_StrPolyHash*h=(sp_StrPolyHash*)sp_gc_alloc(sizeof(sp_StrPolyHash),sp_StrPolyHash_fin,sp_StrPolyHash_scan);h->cap=16;h->mask=15;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(const char**)malloc(sizeof(const char*)*h->cap);h->len=0;h->default_v=sp_box_nil();return h;}
static sp_StrPolyHash*sp_StrPolyHash_new_with_default(sp_RbVal d){sp_StrPolyHash*h=sp_StrPolyHash_new();h->default_v=d;return h;}
static sp_StrPolyHash*sp_StrPolyHash_new_dproc(sp_strpoly_dproc_t fn){sp_StrPolyHash*h=sp_StrPolyHash_new();h->dproc=fn;return h;}
static void sp_StrPolyHash_grow(sp_StrPolyHash*h){mrb_int oc=h->cap;const char**ok=h->keys;sp_RbVal*ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(const char**)realloc(h->order,sizeof(const char*)*h->cap);h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]){mrb_int idx=(mrb_int)(sp_str_hash(ok[i])&h->mask);while(h->keys[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}
static sp_RbVal sp_StrPolyHash_get(sp_StrPolyHash*h,const char*k){if(!h)return sp_box_nil();mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return h->vals[idx];idx=(idx+1)&h->mask;}if(h->dproc)return h->dproc(h,k);return h->default_v;}
static void sp_StrPolyHash_set(sp_StrPolyHash*h,const char*k,sp_RbVal v){if(h->len*2>=h->cap)sp_StrPolyHash_grow(h);mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}
static mrb_bool sp_StrPolyHash_has_key(sp_StrPolyHash*h,const char*k){if(!h)return FALSE;mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_int sp_StrPolyHash_length(sp_StrPolyHash*h){return h->len;}
static sp_StrArray*sp_StrPolyHash_keys(sp_StrPolyHash*h){sp_StrArray*a=sp_StrArray_new();if(!h)return a;for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,h->order[i]);return a;}
static sp_PolyArray*sp_StrPolyHash_values(sp_StrPolyHash*h){sp_PolyArray*a=sp_PolyArray_new();for(mrb_int i=0;i<h->len;i++)sp_PolyArray_push(a,sp_StrPolyHash_get(h,h->order[i]));return a;}
/* Hash#merge for str_poly_hash. Same shape as the
   StrIntHash / SymPolyHash siblings -- copy recv's entries into a
   fresh hash, then overlay other's. */
static sp_StrPolyHash*sp_StrPolyHash_merge(sp_StrPolyHash*a,sp_StrPolyHash*b){sp_StrPolyHash*r=sp_StrPolyHash_new();r->default_v=a->default_v;for(mrb_int i=0;i<a->len;i++)sp_StrPolyHash_set(r,a->order[i],sp_StrPolyHash_get(a,a->order[i]));for(mrb_int i=0;i<b->len;i++)sp_StrPolyHash_set(r,b->order[i],sp_StrPolyHash_get(b,b->order[i]));return r;}
static sp_StrPolyHash*sp_StrPolyHash_dup(sp_StrPolyHash*h){sp_StrPolyHash*r=sp_StrPolyHash_new();r->default_v=h->default_v;for(mrb_int i=0;i<h->len;i++)sp_StrPolyHash_set(r,h->order[i],sp_StrPolyHash_get(h,h->order[i]));return r;}
static mrb_bool sp_StrPolyHash_eq(sp_StrPolyHash*a,sp_StrPolyHash*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++){const char*k=a->order[i];if(!sp_StrPolyHash_has_key(b,k))return FALSE;if(!sp_poly_eq(sp_StrPolyHash_get(a,k),sp_StrPolyHash_get(b,k)))return FALSE;}return TRUE;}
/* Issue #851: inspect for str_poly_hash. */
static const char*sp_StrPolyHash_inspect(sp_StrPolyHash*h){sp_String*s=sp_String_new("{");if(h){for(mrb_int i=0;i<h->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_str_inspect(h->order[i]));sp_String_append(s,"=>");sp_String_append(s,sp_poly_inspect(sp_StrPolyHash_get(h,h->order[i])));}}sp_String_append(s,"}");return s->data;}
/* Convert a narrower StrStrHash to a StrPolyHash. Needed when the
   analyzer widens an LV slot to sp_StrPolyHash* (e.g. later poly-value
   writes) but the initial RHS is a sibling narrower hash variant —
   raw pointer assignment would mix incompatible struct layouts
   (vals[] of const char** vs sp_RbVal*). See issue #614. */
static sp_StrPolyHash*sp_StrPolyHash_from_str_str_hash(sp_StrStrHash*h){sp_StrPolyHash*r=sp_StrPolyHash_new();if(!h)return r;if(h->default_v)r->default_v=sp_box_str(h->default_v);for(mrb_int i=0;i<h->len;i++){const char*k=h->order[i];sp_StrPolyHash_set(r,k,sp_box_str(sp_StrStrHash_get(h,k)));}return r;}
static sp_StrPolyHash*sp_StrPolyHash_from_str_int_hash(sp_StrIntHash*h){sp_StrPolyHash*r=sp_StrPolyHash_new();if(!h)return r;r->default_v=sp_box_int(h->default_v);for(mrb_int i=0;i<h->len;i++){const char*k=h->order[i];sp_StrPolyHash_set(r,k,sp_box_int(sp_StrIntHash_get(h,k)));}return r;}

/* SymPolyHash: symbol keys, sp_RbVal values — same shape as SymStrHash but with poly values. */
/* Named struct so lib/sp_fiber.c can forward-declare it for sp_Fiber's
   `storage` member without pulling in the full poly-hash machinery. */
typedef struct sp_SymPolyHash{sp_sym*keys;sp_RbVal*vals;sp_sym*order;mrb_int len;mrb_int cap;mrb_int mask;sp_RbVal default_v;}sp_SymPolyHash;
static void sp_SymPolyHash_fin(void*p){sp_SymPolyHash*h=(sp_SymPolyHash*)p;free(h->keys);free(h->vals);free(h->order);}
static void sp_SymPolyHash_scan(void*p){sp_SymPolyHash*h=(sp_SymPolyHash*)p;for(mrb_int i=0;i<h->cap;i++){if(h->keys[i]>=0)sp_mark_rbval(h->vals[i]);}sp_mark_rbval(h->default_v);}
static sp_SymPolyHash*sp_SymPolyHash_new(void){sp_SymPolyHash*h=(sp_SymPolyHash*)sp_gc_alloc(sizeof(sp_SymPolyHash),sp_SymPolyHash_fin,sp_SymPolyHash_scan);h->cap=16;h->mask=15;h->keys=(sp_sym*)malloc(sizeof(sp_sym)*h->cap);for(mrb_int i=0;i<h->cap;i++)h->keys[i]=-1;h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(sp_sym*)malloc(sizeof(sp_sym)*h->cap);h->len=0;h->default_v=sp_box_nil();return h;}
static sp_SymPolyHash*sp_SymPolyHash_new_with_default(sp_RbVal d){sp_SymPolyHash*h=sp_SymPolyHash_new();h->default_v=d;return h;}
static void sp_SymPolyHash_grow(sp_SymPolyHash*h){mrb_int oc=h->cap;sp_sym*ok=h->keys;sp_RbVal*ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(sp_sym*)malloc(sizeof(sp_sym)*h->cap);for(mrb_int i=0;i<h->cap;i++)h->keys[i]=-1;h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(sp_sym*)realloc(h->order,sizeof(sp_sym)*h->cap);h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]>=0){mrb_int idx=(mrb_int)(((mrb_int)ok[i])&h->mask);while(h->keys[idx]>=0)idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}
static sp_RbVal sp_SymPolyHash_get(sp_SymPolyHash*h,sp_sym k){if(!h)return sp_box_nil();mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k)return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
static void sp_SymPolyHash_set(sp_SymPolyHash*h,sp_sym k,sp_RbVal v){if(h->len*2>=h->cap)sp_SymPolyHash_grow(h);mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}
static mrb_bool sp_SymPolyHash_has_key(sp_SymPolyHash*h,sp_sym k){mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_int sp_SymPolyHash_length(sp_SymPolyHash*h){return h->len;}
static sp_IntArray*sp_SymPolyHash_keys(sp_SymPolyHash*h){sp_IntArray*a=sp_IntArray_new();for(mrb_int i=0;i<h->len;i++)sp_IntArray_push(a,(mrb_int)h->order[i]);return a;}
static sp_PolyArray*sp_SymPolyHash_values(sp_SymPolyHash*h){sp_PolyArray*a=sp_PolyArray_new();for(mrb_int i=0;i<h->len;i++)sp_PolyArray_push(a,sp_SymPolyHash_get(h,h->order[i]));return a;}
static sp_SymPolyHash*sp_SymPolyHash_merge(sp_SymPolyHash*a,sp_SymPolyHash*b){sp_SymPolyHash*r=sp_SymPolyHash_new();r->default_v=a->default_v;for(mrb_int i=0;i<a->len;i++)sp_SymPolyHash_set(r,a->order[i],sp_SymPolyHash_get(a,a->order[i]));for(mrb_int i=0;i<b->len;i++)sp_SymPolyHash_set(r,b->order[i],sp_SymPolyHash_get(b,b->order[i]));return r;}
/* Hash#delete for sym_poly_hash. Removes key and re-tombstones the
   slot, shifting probe-chain successors backward and dropping the
   key from the insertion-order list. Issue #510. */
static void sp_SymPolyHash_delete(sp_SymPolyHash*h,sp_sym k){mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k){h->keys[idx]=-1;h->vals[idx]=sp_box_nil();h->len--;mrb_int j=(idx+1)&h->mask;while(h->keys[j]>=0){mrb_int nj=(mrb_int)(((mrb_int)h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->keys[j]=-1;h->vals[j]=sp_box_nil();idx=j;}j=(j+1)&h->mask;}{mrb_int oi=0;while(oi<=h->len){if(h->order[oi]==k){while(oi<h->len){h->order[oi]=h->order[oi+1];oi++;}break;}oi++;}}return;}idx=(idx+1)&h->mask;}}
static sp_SymPolyHash*sp_SymPolyHash_dup(sp_SymPolyHash*h){sp_SymPolyHash*r=sp_SymPolyHash_new();r->default_v=h->default_v;for(mrb_int i=0;i<h->len;i++)sp_SymPolyHash_set(r,h->order[i],sp_SymPolyHash_get(h,h->order[i]));return r;}
static mrb_bool sp_SymPolyHash_eq(sp_SymPolyHash*a,sp_SymPolyHash*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++){sp_sym k=a->order[i];if(!sp_SymPolyHash_has_key(b,k))return FALSE;if(!sp_poly_eq(sp_SymPolyHash_get(a,k),sp_SymPolyHash_get(b,k)))return FALSE;}return TRUE;}
/* Hash#inspect for sym_poly_hash. CRuby 4.0 renders symbol keys
   in shorthand: `{a: 1, b: "x"}` rather than `{:a=>1, :b=>"x"}`. */
static const char*sp_SymPolyHash_inspect(sp_SymPolyHash*h){if(!h){char*r=sp_str_alloc_raw(3);r[0]='{';r[1]='}';r[2]=0;sp_str_set_len(r,2);return r;}sp_String*s=sp_String_new("{");for(mrb_int i=0;i<h->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_sym_to_s(h->order[i]));sp_String_append(s,": ");sp_String_append(s,sp_poly_inspect(sp_SymPolyHash_get(h,h->order[i])));}sp_String_append(s,"}");return s->data;}

/* PolyPolyHash: heterogeneous keys + values (both sp_RbVal). For
   primitives the hash/eql is tag-based (value equality); for OBJ tag
   the default is pointer identity. Codegen patches sp_obj_hash_hook /
   sp_obj_eql_hook with class-aware overrides (e.g. Method#eql? compares
   bound receiver + fn_ptr) when the program needs them — null hooks
   leave the runtime at identity, which is the right default for typed
   pointers like IntArray. */
typedef mrb_int  (*sp_obj_hash_fn)(int cls_id, void *p);
typedef mrb_bool (*sp_obj_eql_fn)(int cls_id, void *a, void *b);
static sp_obj_hash_fn sp_obj_hash_hook = NULL;
static sp_obj_eql_fn  sp_obj_eql_hook  = NULL;
static mrb_int sp_rbval_hash_key(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT: case SP_TAG_BOOL: case SP_TAG_NIL: case SP_TAG_SYM:
      return (mrb_int)v.v.i;
    case SP_TAG_STR:
      return v.v.s ? (mrb_int)sp_str_hash(v.v.s) : 0;
    case SP_TAG_ENCODING:
      return v.v.s ? (mrb_int)sp_str_hash(v.v.s) : 0;
    case SP_TAG_FLT: { uint64_t b; memcpy(&b, &v.v.f, sizeof(b)); return (mrb_int)b; }
    case SP_TAG_OBJ:
      if (sp_obj_hash_hook) return sp_obj_hash_hook(v.cls_id, v.v.p);
      return (mrb_int)((uintptr_t)v.v.p);
  }
  return 0;
}
static mrb_bool sp_rbval_eql_key(sp_RbVal a, sp_RbVal b) {
  if (a.tag != b.tag) return FALSE;
  switch (a.tag) {
    case SP_TAG_INT: case SP_TAG_BOOL: case SP_TAG_NIL: case SP_TAG_SYM:
      return a.v.i == b.v.i;
    case SP_TAG_STR:
      if (a.v.s == b.v.s) return TRUE;
      if (!a.v.s || !b.v.s) return FALSE;
      return strcmp(a.v.s, b.v.s) == 0;
    case SP_TAG_ENCODING:
      if (a.v.s == b.v.s) return TRUE;
      if (!a.v.s || !b.v.s) return FALSE;
      return strcmp(a.v.s, b.v.s) == 0;
    case SP_TAG_FLT:
      return a.v.f == b.v.f;
    case SP_TAG_OBJ:
      if (a.cls_id != b.cls_id) return FALSE;
      if (a.v.p == b.v.p) return TRUE;
      if (sp_obj_eql_hook) return sp_obj_eql_hook(a.cls_id, a.v.p, b.v.p);
      return FALSE;
  }
  return FALSE;
}
typedef struct{sp_RbVal*keys;sp_RbVal*vals;mrb_int*order;mrb_bool*occ;mrb_int len;mrb_int cap;mrb_int mask;}sp_PolyPolyHash;
static void sp_PolyPolyHash_fin(void*p){sp_PolyPolyHash*h=(sp_PolyPolyHash*)p;free(h->keys);free(h->vals);free(h->order);free(h->occ);}
static void sp_PolyPolyHash_scan(void*p){sp_PolyPolyHash*h=(sp_PolyPolyHash*)p;for(mrb_int i=0;i<h->cap;i++){if(h->occ[i]){sp_mark_rbval(h->keys[i]);sp_mark_rbval(h->vals[i]);}}}
static sp_PolyPolyHash*sp_PolyPolyHash_new(void){sp_PolyPolyHash*h=(sp_PolyPolyHash*)sp_gc_alloc(sizeof(sp_PolyPolyHash),sp_PolyPolyHash_fin,sp_PolyPolyHash_scan);h->cap=16;h->mask=15;h->keys=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(mrb_int*)malloc(sizeof(mrb_int)*h->cap);h->occ=(mrb_bool*)calloc(h->cap,sizeof(mrb_bool));h->len=0;return h;}
static void sp_PolyPolyHash_grow(sp_PolyPolyHash*h){mrb_int oc=h->cap;sp_RbVal*ok=h->keys;sp_RbVal*ov=h->vals;mrb_bool*oo=h->occ;mrb_int*oord=h->order;mrb_int olen=h->len;h->cap*=2;h->mask=h->cap-1;h->keys=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(mrb_int*)malloc(sizeof(mrb_int)*h->cap);h->occ=(mrb_bool*)calloc(h->cap,sizeof(mrb_bool));for(mrb_int i=0;i<olen;i++){mrb_int oi=oord[i];sp_RbVal k=ok[oi];mrb_int idx=(mrb_int)(sp_rbval_hash_key(k)&h->mask);while(h->occ[idx])idx=(idx+1)&h->mask;h->keys[idx]=k;h->vals[idx]=ov[oi];h->occ[idx]=TRUE;h->order[i]=idx;}free(ok);free(ov);free(oo);free(oord);}
static sp_RbVal sp_PolyPolyHash_get(sp_PolyPolyHash*h,sp_RbVal k){if(!h)return sp_box_nil();mrb_int idx=(mrb_int)(sp_rbval_hash_key(k)&h->mask);while(h->occ[idx]){if(sp_rbval_eql_key(h->keys[idx],k))return h->vals[idx];idx=(idx+1)&h->mask;}return sp_box_nil();}
static void sp_PolyPolyHash_set(sp_PolyPolyHash*h,sp_RbVal k,sp_RbVal v){if(h->len*2>=h->cap)sp_PolyPolyHash_grow(h);mrb_int idx=(mrb_int)(sp_rbval_hash_key(k)&h->mask);while(h->occ[idx]){if(sp_rbval_eql_key(h->keys[idx],k)){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->occ[idx]=TRUE;h->order[h->len]=idx;h->len++;}
static mrb_bool sp_PolyPolyHash_has_key(sp_PolyPolyHash*h,sp_RbVal k){mrb_int idx=(mrb_int)(sp_rbval_hash_key(k)&h->mask);while(h->occ[idx]){if(sp_rbval_eql_key(h->keys[idx],k))return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_int sp_PolyPolyHash_length(sp_PolyPolyHash*h){return h->len;}
static sp_PolyArray*sp_PolyPolyHash_keys(sp_PolyPolyHash*h){sp_PolyArray*a=sp_PolyArray_new();for(mrb_int i=0;i<h->len;i++)sp_PolyArray_push(a,h->keys[h->order[i]]);return a;}
static sp_PolyArray*sp_PolyPolyHash_values(sp_PolyPolyHash*h){sp_PolyArray*a=sp_PolyArray_new();for(mrb_int i=0;i<h->len;i++)sp_PolyArray_push(a,h->vals[h->order[i]]);return a;}
static sp_PolyPolyHash*sp_PolyPolyHash_dup(sp_PolyPolyHash*h){sp_PolyPolyHash*r=sp_PolyPolyHash_new();for(mrb_int i=0;i<h->len;i++)sp_PolyPolyHash_set(r,h->keys[h->order[i]],h->vals[h->order[i]]);return r;}
/* Issue #738: poly_poly_hash inspect using sp_poly_inspect on each
   k,v. Output mirrors Ruby's `{k=>v, ...}` for non-symbol keys and
   `{k: v, ...}` shorthand for symbol keys. */
static const char *sp_poly_inspect(sp_RbVal v);
static const char*sp_PolyPolyHash_inspect(sp_PolyPolyHash*h){sp_String*s=sp_String_new("{");if(!h){sp_String_append(s,"}");return s->data;}for(mrb_int i=0;i<h->len;i++){if(i>0)sp_String_append(s,", ");sp_RbVal k=h->keys[h->order[i]];if(k.tag==SP_TAG_SYM){sp_String_append(s,sp_sym_to_s((sp_sym)k.v.i));sp_String_append(s,": ");}else{sp_String_append(s,sp_poly_inspect(k));sp_String_append(s,"=>");}sp_String_append(s,sp_poly_inspect(h->vals[h->order[i]]));}sp_String_append(s,"}");return s->data;}
/* Issue #738: Hash#invert -- swap keys and values. Returns a
   poly_poly_hash so any (key, value) pair shape is uniformly
   representable. str_str_hash_invert lives above (line ~1132)
   and stays as a same-type round-trip. */
static sp_PolyPolyHash*sp_StrIntHash_invert_poly(sp_StrIntHash*h){sp_PolyPolyHash*r=sp_PolyPolyHash_new();if(!h)return r;for(mrb_int i=0;i<h->len;i++)sp_PolyPolyHash_set(r,sp_box_int(sp_StrIntHash_get(h,h->order[i])),sp_box_str(h->order[i]));return r;}
static sp_PolyPolyHash*sp_IntStrHash_invert(sp_IntStrHash*h){sp_PolyPolyHash*r=sp_PolyPolyHash_new();if(!h)return r;for(mrb_int i=0;i<h->len;i++)sp_PolyPolyHash_set(r,sp_box_str(sp_IntStrHash_get(h,h->order[i])),sp_box_int(h->order[i]));return r;}
static mrb_bool sp_PolyPolyHash_eq(sp_PolyPolyHash*a,sp_PolyPolyHash*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++){sp_RbVal k=a->keys[a->order[i]];if(!sp_PolyPolyHash_has_key(b,k))return FALSE;if(!sp_poly_eq(sp_PolyPolyHash_get(a,k),sp_PolyPolyHash_get(b,k)))return FALSE;}return TRUE;}

/* JSON.generate support. sp_json_str quotes + escapes a string per
   JSON rules (control chars -> \uNNNN, plus the \" \\ \n \t \r \b \f
   shorthands; UTF-8 bytes >= 0x80 pass through unescaped, matching
   CRuby's default). sp_json_val recursively serializes any boxed
   value -- scalars, arrays, and the typed hash variants -- in CRuby's
   compact form (no spaces, `:` separator). Object keys are coerced to
   JSON strings (symbol -> name, integer -> decimal). Raw container
   element / value types are boxed and routed back through sp_json_val
   so nesting works. The sp_String accumulator is GC-rooted so a
   collection triggered by a nested allocation can't free it. */
static const char *sp_json_str(const char *s) __attribute__((unused));
static const char *sp_json_str(const char *s) {
  sp_String *o = sp_String_new("\"");
  SP_GC_ROOT(o);
  if (s) { const char *p = s; while (*p) { unsigned char c = (unsigned char)*p;
    if (c == '"') sp_String_append(o, "\\\"");
    else if (c == '\\') sp_String_append(o, "\\\\");
    else if (c == '\n') sp_String_append(o, "\\n");
    else if (c == '\t') sp_String_append(o, "\\t");
    else if (c == '\r') sp_String_append(o, "\\r");
    else if (c == '\b') sp_String_append(o, "\\b");
    else if (c == '\f') sp_String_append(o, "\\f");
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", (unsigned)c); sp_String_append(o, b); }
    else { char ch[2]; ch[0] = (char)c; ch[1] = 0; sp_String_append(o, ch); }
    p++; } }
  sp_String_append(o, "\"");
  return o->data;
}
static const char *sp_json_val(sp_RbVal v) __attribute__((unused));
static const char *sp_json_key(sp_RbVal k) {
  if (k.tag == SP_TAG_STR) return sp_json_str(k.v.s);
  if (k.tag == SP_TAG_SYM) return sp_json_str(sp_sym_to_s((sp_sym)k.v.i));
  if (k.tag == SP_TAG_INT) return sp_json_str(sp_int_to_s(k.v.i));
  if (k.tag == SP_TAG_BOOL) return sp_json_str(k.v.b ? "true" : "false");
  if (k.tag == SP_TAG_NIL) return sp_json_str("");
  return sp_json_str("");
}
static const char *sp_json_val(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT:  return sp_int_to_s(v.v.i);
    case SP_TAG_FLT:  return sp_float_to_s(v.v.f);
    case SP_TAG_BOOL: return v.v.b ? SPL("true") : SPL("false");
    case SP_TAG_NIL:  return SPL("null");
    case SP_TAG_STR:  return sp_json_str(v.v.s);
    case SP_TAG_SYM:  return sp_json_str(sp_sym_to_s((sp_sym)v.v.i));
    case SP_TAG_OBJ: {
      sp_String *o;
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: { sp_IntArray *a=(sp_IntArray*)v.v.p; o=sp_String_new("["); SP_GC_ROOT(o); if(a){for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(o,",");sp_String_append(o,sp_int_to_s(a->data[a->start+i]));}} sp_String_append(o,"]"); return o->data; }
        case SP_BUILTIN_FLT_ARRAY: { sp_FloatArray *a=(sp_FloatArray*)v.v.p; o=sp_String_new("["); SP_GC_ROOT(o); if(a){for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(o,",");sp_String_append(o,sp_float_to_s(a->data[i]));}} sp_String_append(o,"]"); return o->data; }
        case SP_BUILTIN_STR_ARRAY: { sp_StrArray *a=(sp_StrArray*)v.v.p; o=sp_String_new("["); SP_GC_ROOT(o); if(a){for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(o,",");sp_String_append(o,sp_json_str(a->data[i]));}} sp_String_append(o,"]"); return o->data; }
        case SP_BUILTIN_SYM_ARRAY: { sp_IntArray *a=(sp_IntArray*)v.v.p; o=sp_String_new("["); SP_GC_ROOT(o); if(a){for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(o,",");sp_String_append(o,sp_json_str(sp_sym_to_s((sp_sym)a->data[a->start+i])));}} sp_String_append(o,"]"); return o->data; }
        case SP_BUILTIN_POLY_ARRAY: { sp_PolyArray *a=(sp_PolyArray*)v.v.p; o=sp_String_new("["); SP_GC_ROOT(o); if(a){for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(o,",");sp_String_append(o,sp_json_val(a->data[i]));}} sp_String_append(o,"]"); return o->data; }
        case SP_BUILTIN_STR_INT_HASH: { sp_StrIntHash *h=(sp_StrIntHash*)v.v.p; o=sp_String_new("{"); SP_GC_ROOT(o); if(h){for(mrb_int i=0;i<h->len;i++){if(i>0)sp_String_append(o,",");sp_String_append(o,sp_json_str(h->order[i]));sp_String_append(o,":");sp_String_append(o,sp_int_to_s(sp_StrIntHash_get(h,h->order[i])));}} sp_String_append(o,"}"); return o->data; }
        case SP_BUILTIN_STR_STR_HASH: { sp_StrStrHash *h=(sp_StrStrHash*)v.v.p; o=sp_String_new("{"); SP_GC_ROOT(o); if(h){for(mrb_int i=0;i<h->len;i++){if(i>0)sp_String_append(o,",");sp_String_append(o,sp_json_str(h->order[i]));sp_String_append(o,":");sp_String_append(o,sp_json_str(sp_StrStrHash_get(h,h->order[i])));}} sp_String_append(o,"}"); return o->data; }
        case SP_BUILTIN_INT_STR_HASH: { sp_IntStrHash *h=(sp_IntStrHash*)v.v.p; o=sp_String_new("{"); SP_GC_ROOT(o); if(h){for(mrb_int i=0;i<h->len;i++){if(i>0)sp_String_append(o,",");sp_String_append(o,sp_json_str(sp_int_to_s(h->order[i])));sp_String_append(o,":");sp_String_append(o,sp_json_str(sp_IntStrHash_get(h,h->order[i])));}} sp_String_append(o,"}"); return o->data; }
        /* SYM_INT_HASH / SYM_STR_HASH structs are codegen-emitted (not
           in this static header), so their serializers are emitted by
           codegen as sp_SymIntHash_json / sp_SymStrHash_json and called
           directly from the JSON dispatch. A nested sym-int/str hash
           reaching here as a boxed poly value falls through to null. */
        case SP_BUILTIN_STR_POLY_HASH: { sp_StrPolyHash *h=(sp_StrPolyHash*)v.v.p; o=sp_String_new("{"); SP_GC_ROOT(o); if(h){for(mrb_int i=0;i<h->len;i++){if(i>0)sp_String_append(o,",");sp_String_append(o,sp_json_str(h->order[i]));sp_String_append(o,":");sp_String_append(o,sp_json_val(sp_StrPolyHash_get(h,h->order[i])));}} sp_String_append(o,"}"); return o->data; }
        case SP_BUILTIN_SYM_POLY_HASH: { sp_SymPolyHash *h=(sp_SymPolyHash*)v.v.p; o=sp_String_new("{"); SP_GC_ROOT(o); if(h){for(mrb_int i=0;i<h->len;i++){if(i>0)sp_String_append(o,",");sp_String_append(o,sp_json_str(sp_sym_to_s((sp_sym)h->order[i])));sp_String_append(o,":");sp_String_append(o,sp_json_val(sp_SymPolyHash_get(h,h->order[i])));}} sp_String_append(o,"}"); return o->data; }
        case SP_BUILTIN_POLY_POLY_HASH: { sp_PolyPolyHash *h=(sp_PolyPolyHash*)v.v.p; o=sp_String_new("{"); SP_GC_ROOT(o); if(h){for(mrb_int i=0;i<h->len;i++){if(i>0)sp_String_append(o,",");sp_String_append(o,sp_json_key(h->keys[h->order[i]]));sp_String_append(o,":");sp_String_append(o,sp_json_val(h->vals[h->order[i]]));}} sp_String_append(o,"}"); return o->data; }
        default: return SPL("null");
      }
    }
    default: return SPL("null");
  }
}

#include <setjmp.h>
#define SP_EXC_STACK_MAX 64
static jmp_buf sp_exc_stack[SP_EXC_STACK_MAX];
static const char *sp_exc_msg[SP_EXC_STACK_MAX];
static volatile int sp_exc_top = 0;
static const char *sp_exc_cls[SP_EXC_STACK_MAX];
static volatile const char *sp_last_exc_cls = sp_str_empty;
/* ---- Native backtrace formatting (spinel --debug) ---------------------- */
/* True for sp_<name> symbols that are runtime helpers, not user Ruby methods.
   A denylist of the lowercase runtime prefixes; user methods are sp_<rubyname>
   (top-level) or sp_<Class>_<method>, which don't match. Heuristic — a leaf
   runtime frame may occasionally slip through; refine with an emitted
   user-method allowlist later. */
#if SP_BT_AVAILABLE
static int sp_bt_is_runtime(const char *n) {
  static const char *pfx[] = {
    "int_", "str_", "float_", "sym_", "gc_", "bigint", "sprintf", "raise",
    "exc_", "range", "utf8", "oom", "bt_", "backtrace", "caller", "StrArray",
    "IntArray", "FloatArray", "PtrArray", "PolyArray", "Str", "Int", "Float",
    "Hash", "Range", "Complex", "Rational", "Sym", "alloc", "free", "to_s",
    "dup", "new", "pack", "unpack", "regex", "re_",
    /* arithmetic/runtime helpers that can raise and sit between the raise
       and the user frame (ZeroDivisionError via sp_idiv/sp_imod, etc.) */
    "idiv", "imod", "gcd", "fdiv", "ipow", "iclamp", "div_", "mod_", 0
  };
  for (int i = 0; pfx[i]; i++) {
    size_t l = strlen(pfx[i]);
    if (strncmp(n, pfx[i], l) == 0) return 1;
  }
  return 0;
}

/* Extract the symbol token from a backtrace_symbols line. Two formats:
     - glibc/Linux: "<module>(<symbol>+0x<off>) [0x<addr>]". The symbol is
       empty for unresolved frames (static or stripped fns) -> skip.
     - macOS:       "<idx> <image> <addr> <symbol> + <off>".
   Returns NULL if it isn't a keepable user frame. Detect Linux by the '('
   that delimits the symbol (the macOS format has none). */
static const char *sp_bt_symbol(const char *line) {
  char sym[256];
  const char *lp = strchr(line, '(');
  if (lp) {                                       /* glibc/Linux paren form */
    const char *p = lp + 1;
    const char *end = p;
    while (*end && *end != '+' && *end != ')') end++;
    size_t len = (size_t)(end - p);
    if (len == 0 || len > 250) return 0;          /* unresolved (static/stripped) */
    memcpy(sym, p, len); sym[len] = 0;
  } else {                                        /* macOS: "<idx> <image> <addr> <symbol> + <off>" */
    /* The symbol is the token just before the " + <off>" delimiter. Parse
       backward from the last " + " rather than forward from "0x" — an image
       path containing "0x" (e.g. /path/0x_proj/bin) would otherwise misparse. */
    const char *plus = 0, *q = line;
    while ((q = strstr(q, " + ")) != 0) { plus = q; q += 3; }
    if (!plus) return 0;
    const char *end = plus;
    const char *p = end;
    while (p > line && p[-1] != ' ') p--;          /* back up to the symbol's start */
    size_t len = (size_t)(end - p);
    if (len == 0 || len > 250) return 0;
    memcpy(sym, p, len); sym[len] = 0;
  }
  if (strcmp(sym, "main") == 0) return strdup("<main>");
  if (strncmp(sym, "sp_", 3) != 0) return 0;     /* skip non-Spinel frames */
  const char *name = sym + 3;
  if (sp_bt_is_runtime(name)) return 0;
  /* De-mangle sp_<Class>_<method> back to Ruby. A Spinel symbol is a path of
     CamelCase class segments (each from a `::`, joined by `_`) followed by the
     method; the method is the first segment that starts lowercase. A literal
     `cls` segment marks a singleton method:
       sp_Helper_cls_boom          -> Helper.boom
       sp_Tep_Url_parse_query      -> Tep::Url#parse_query
       sp_Tep_AuthOAuth2_cls_find  -> Tep::AuthOAuth2.find
       sp_toplevel                 -> toplevel   (top-level method, no class)
     (Method names stay sanitized — e.g. enabled? is enabled_p; reversing that
     needs the emitted name table, a separate refinement.) */
  const char *mstart = 0;   /* first lowercase-starting segment = the method */
  for (const char *p = name; *p; p++) {
    int seg_start = (p == name) || (p[-1] == '_');
    if (seg_start && *p >= 'a' && *p <= 'z') { mstart = p; break; }
  }
  if (!mstart) return strdup(name);            /* all-uppercase: leave as-is */
  if (mstart == name) {                        /* no class path: top-level */
    if (strncmp(name, "cls_", 4) == 0) return strdup(name + 4);  /* top-level singleton */
    return strdup(name);
  }
  char out[256]; size_t o = 0;
  const char *meth; char sep;
  if (strncmp(mstart, "cls_", 4) == 0) { meth = mstart + 4; sep = '.'; }  /* singleton */
  else                                 { meth = mstart;     sep = '#'; }  /* instance */
  for (const char *p = name; p < mstart - 1 && o + 2 < sizeof(out); p++) {
    if (*p == '_') { out[o++] = ':'; out[o++] = ':'; }   /* class-path `_` was a `::` */
    else out[o++] = *p;
  }
  if (o + 1 < sizeof(out)) out[o++] = sep;
  size_t ml = strlen(meth);
  if (o + ml < sizeof(out)) { memcpy(out + o, meth, ml); o += ml; }
  out[o] = 0;
  return strdup(out);
}

static sp_StrArray *sp_bt_format(void **buf, int n) {
  sp_StrArray *a = sp_StrArray_new();
  if (!sp_bt_enabled || n <= 0) return a;
  char **syms = backtrace_symbols(buf, n);
  if (!syms) return a;
  const char *src = (sp_bt_srcfile && sp_bt_srcfile[0]) ? sp_bt_srcfile : "(spinel)";
  for (int i = 0; i < n; i++) {
    char *name = (char *)sp_bt_symbol(syms[i]);  /* always strdup'd; free after use */
    if (!name) continue;
    sp_StrArray_push(a, sp_sprintf("%s:in `%s'", src, name));
    free(name);
  }
  free(syms);
  return a;
}
#endif

/* Backtrace captured at the most recent raise (for Exception#backtrace). */
static sp_StrArray *sp_backtrace_captured(void) {
#if SP_BT_AVAILABLE
  return sp_bt_format(sp_bt_buf, sp_bt_n);
#else
  return sp_StrArray_new();
#endif
}

/* The current stack, captured now (for Kernel#caller). */
static sp_StrArray *sp_caller_now(void) {
#if SP_BT_AVAILABLE
  if (!sp_bt_enabled) return sp_StrArray_new();
  void *buf[256];
  int n = backtrace(buf, 256);
  return sp_bt_format(buf, n);
#else
  return sp_StrArray_new();
#endif
}

SP_NORETURN SP_COLD void sp_raise_cls(const char *cls, const char *msg) {
#if SP_BT_AVAILABLE
  if (sp_bt_enabled) sp_bt_n = backtrace(sp_bt_buf, 256);
#endif
  if (sp_exc_top > 0) { sp_exc_msg[sp_exc_top-1] = msg; sp_exc_cls[sp_exc_top-1] = cls; sp_last_exc_cls = cls; longjmp(sp_exc_stack[sp_exc_top-1], 1); } fprintf(stderr, "unhandled exception: %s\n", msg); exit(1); }
static void sp_raise(const char *msg) { sp_raise_cls("RuntimeError", msg); }

/* Issue #781: bridge between the regex compile-error path (which lives
   in the .a library and can't see the user program's static-inline
   sp_raise_cls) and the user's Ruby-level exception machinery. The
   library calls sp_re_set_error_handler at startup -- codegen emits
   the install call after the exception infrastructure is set up. */
static void sp_re_default_error_handler(const char *msg) {
  sp_raise_cls("RegexpError", msg);
}
/* Issue #846: during sp_re_init (before main()'s setjmp scope is
   active), a bad literal `Regexp.new("[invalid")` pattern would
   route through sp_raise_cls -> "unhandled exception" + exit
   because sp_exc_top is 0. Install a startup handler that longjmps
   back to sp_re_init's wrapping setjmp; the codegen-emitted loop
   then stashes the error per slot for a deferred raise from the
   first use site (where the user's begin/rescue is active). The
   re_compile contract requires the error callback to not return
   (otherwise the library's fall-through fprintf+exit fires) so
   we longjmp out. */
static const char *sp_re_startup_err = NULL;
static jmp_buf sp_re_startup_jmp;
static void sp_re_startup_error_handler(const char *msg) {
  if (msg) {
    size_t n = strlen(msg);
    char *buf = sp_str_alloc_raw(n + 1);
    memcpy(buf, msg, n);
    buf[n] = 0;
    sp_re_startup_err = buf;
  }
  longjmp(sp_re_startup_jmp, 1);
}
extern void sp_re_set_error_handler(void (*fn)(const char *msg));
static void sp_mark_in_flight_exceptions(void) { for (int i = 0; i < sp_exc_top; i++) sp_mark_string(sp_exc_msg[i]); }

/* sp_Exception: first-class exception object. cls_name is a pointer
   to the per-class const string literal emitted by codegen
   (sp_class_names[] entry; not GC-managed). msg is GC-managed
   (sp_str_alloc'd). */
typedef struct sp_Exception_s {
  const char *cls_name;
  const char *msg;
} sp_Exception;
static void sp_exc_gc_scan(void *p) {
  sp_Exception *e = (sp_Exception *)p;
  if (e->msg) sp_mark_string(e->msg);
  /* cls_name points into the .rodata sp_class_names[] table -- not
     a GC-managed string, no mark needed. */
}
static sp_Exception *sp_exc_new(const char *cls_name, const char *msg) {
  sp_Exception *e = (sp_Exception *)sp_gc_alloc(sizeof(sp_Exception), NULL, sp_exc_gc_scan);
  e->cls_name = cls_name ? cls_name : "RuntimeError";
  e->msg = msg ? msg : sp_str_empty;
  return e;
}
/* Accept `volatile` pointers: LV slots holding sp_Exception * are
   declared volatile when they live across setjmp, so callers may
   pass volatile-qualified pointers in. The pointee itself isn't
   volatile (cls_name/msg are stable post-construction), so we
   strip volatile internally for one access. */
static const char *sp_exc_class_name(volatile sp_Exception *ve) {
  sp_Exception *e = (sp_Exception *)ve;
  return e ? e->cls_name : "RuntimeError";
}
static const char *sp_exc_message(volatile sp_Exception *ve) {
  sp_Exception *e = (sp_Exception *)ve;
  return e ? e->msg : sp_str_empty;
}
static void sp_raise_exc(volatile sp_Exception *ve) {
  sp_Exception *e = (sp_Exception *)ve;
  if (!e) sp_raise("(nil exception)");
  sp_raise_cls(e->cls_name, e->msg);
}

/* Cross-TU bridge for sp_bigint.c (compiled as a separate translation
   unit; can't see static helpers in this header). Defined non-static
   so sp_bigint.c's mrb_raise macro can dispatch into spinel's
   longjmp-based rescue net rather than fprintf+exit. */
void sp_bigint_raise_zerodiv(const char *msg) { sp_raise_cls("ZeroDivisionError", msg); }
static mrb_bool sp_exc_is_a(const char *cls, const char *target) { return strcmp(cls, target) == 0; }

#define SP_CATCH_STACK_MAX 64
static jmp_buf sp_catch_stack[SP_CATCH_STACK_MAX];
static const char *sp_catch_tag[SP_CATCH_STACK_MAX];
static mrb_int sp_catch_val[SP_CATCH_STACK_MAX];
static volatile int sp_catch_top = 0;
static void sp_throw(const char *tag, mrb_int val) { int i = sp_catch_top - 1; while (i >= 0) { if (strcmp(sp_catch_tag[i], tag) == 0) { sp_catch_val[i] = val; sp_catch_top = i + 1; longjmp(sp_catch_stack[i], 1); } i--; } fprintf(stderr, "uncaught throw: %s\n", tag); exit(1); }

/* Kernel#sleep with sub-second precision. Argument is seconds as a
   double so `sleep(0.5)` actually waits 500ms; the legacy `sleep((unsigned)0.5)`
   cast truncated to 0 and returned immediately. POSIX uses
   nanosleep(); Windows uses Sleep() (milliseconds). Negative or NaN
   inputs no-op. */
static void sp_sleep(mrb_float s) {
  if (!(s > 0.0)) return;
#ifdef _WIN32
  DWORD ms = (DWORD)(s * 1000.0);
  if (ms == 0) ms = 1;
  Sleep(ms);
#else
  struct timespec req;
  req.tv_sec = (time_t)s;
  req.tv_nsec = (long)((s - (double)req.tv_sec) * 1e9);
  if (req.tv_nsec < 0) req.tv_nsec = 0;
  if (req.tv_nsec >= 1000000000L) req.tv_nsec = 999999999L;
  while (nanosleep(&req, &req) == -1 && errno == EINTR) {}
#endif
}

#ifdef _WIN32
static DWORD sp_file_attributes(const char *path) {
  if (!path) return INVALID_FILE_ATTRIBUTES;
  int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  if (len <= 0) return INVALID_FILE_ATTRIBUTES;
  wchar_t *wpath = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)len);
  if (!wpath) return INVALID_FILE_ATTRIBUTES;
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, len) <= 0) {
    free(wpath);
    return INVALID_FILE_ATTRIBUTES;
  }
  DWORD attrs = GetFileAttributesW(wpath);
  free(wpath);
  return attrs;
}

static mrb_bool sp_file_directory(const char *path) {
  DWORD attrs = sp_file_attributes(path);
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static mrb_bool sp_file_file(const char *path) {
  DWORD attrs = sp_file_attributes(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}
#else
static mrb_bool sp_file_directory(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static mrb_bool sp_file_file(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}
#endif

/* Text mode ("r") matches CRuby's File.read: on Windows, CRLF is
   normalized to LF on read, which cancels out fopen("w")'s
   LF→CRLF on write. Without this, content from File.read passed to
   puts goes through stdout's text-mode translation a second time
   and `\r\n` becomes `\r\r\n`. fread's actual byte count drives
   null-termination because text mode shrinks the byte count below
   ftell's raw-file size. */
static const char *sp_file_read(const char *path) {
  if (sp_file_directory(path)) {
    sp_raise_cls("Errno::EISDIR", sp_sprintf("Is a directory @ io_fread - %s", path));
  }
  FILE *f = fopen(path, "r");
  if (!f) return &("\xff" "")[1];
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = sp_str_alloc(sz);
  size_t n = 0;
  if (sz > 0) {
    n = fread(buf, 1, sz, f);
  }
  buf[n] = 0;
  fclose(f);
  return buf;
}

static void sp_file_write(const char *path, const char *data) {
  if (sp_file_directory(path)) {
    sp_raise_cls("Errno::EISDIR", sp_sprintf("Is a directory @ rb_sysopen - %s", path));
  }
  FILE *f = fopen(path, "wb");
  if (f) {
    fwrite(data, 1, sp_str_byte_len(data), f);
    fclose(f);
  }
}
static mrb_bool sp_file_exist(const char *path) { FILE *f = fopen(path, "r"); if (f) { fclose(f); return TRUE; } return FALSE; }
static void sp_file_delete(const char *path) { remove(path); }
static sp_Time sp_file_mtime(const char *path) {
  if (!path) {
    sp_raise_cls("TypeError", "no implicit conversion of nil into String");
    return (sp_Time){0, 0, 0};
  }
#if defined(_WIN32)
  int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  if (len > 0) {
    wchar_t *wpath = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)len);
    if (wpath) {
      if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, len) > 0) {
        WIN32_FILE_ATTRIBUTE_DATA attr_data;
        if (GetFileAttributesExW(wpath, GetFileExInfoStandard, &attr_data)) {
          free(wpath);
          ULARGE_INTEGER ull;
          ull.LowPart = attr_data.ftLastWriteTime.dwLowDateTime;
          ull.HighPart = attr_data.ftLastWriteTime.dwHighDateTime;
          int64_t total_ns = (ull.QuadPart - 116444736000000000ULL) * 100;
          return (sp_Time){total_ns / 1000000000LL, (int32_t)(total_ns % 1000000000LL), 0};
        }
      }
      free(wpath);
    }
  }
  sp_raise_cls("Errno::ENOENT", sp_sprintf("No such file or directory @ File.mtime - %s", path));
  return (sp_Time){0, 0, 0};
#else
  struct stat st;
  if (stat(path, &st) == -1) {
    sp_raise_cls(errno == ENOENT ? "Errno::ENOENT" : "RuntimeError", sp_sprintf("%s @ File.mtime - %s", strerror(errno), path));
    return (sp_Time){0, 0, 0};
  }
#if defined(__APPLE__)
  return (sp_Time){(int64_t)st.st_mtimespec.tv_sec, (int32_t)st.st_mtimespec.tv_nsec, 0};
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
  return (sp_Time){(int64_t)st.st_mtimespec.tv_sec, (int32_t)st.st_mtimespec.tv_nsec, 0};
#else
  /* Linux / others with st_mtim */
  return (sp_Time){(int64_t)st.st_mtim.tv_sec, (int32_t)st.st_mtim.tv_nsec, 0};
#endif
#endif
}
static const char *sp_backtick(const char *cmd) { FILE *p = popen(cmd, "r"); if (!p) return sp_str_empty; char *buf = sp_str_alloc_raw(4096); size_t n = fread(buf, 1, 4095, p); buf[n] = 0; pclose(p); return buf; }
static const char *sp_file_basename(const char *path) {
  const char *s = strrchr(path, '/');
  const char *base = s ? s + 1 : path;
  /* sp_gc_mark looks at byte[-1] to distinguish heap strings (`\xfe`)
     from literals (`\xff`). A `s+1` mid-path pointer has whatever the
     '/' was before it — and for an arbitrary string that's not a tag,
     so the GC tries to dereference it as a heap header and segfaults.
     Return a fresh sp_str_alloc'd copy so the prefix marker is right. */
  size_t n = strlen(base);
  char *buf = sp_str_alloc(n);
  memcpy(buf, base, n + 1);
  return buf;
}
/* Issue #892: File.dirname / File.extname / Dir.pwd. */
static const char *sp_file_dirname(const char *path) {
  const char *s = strrchr(path, '/');
  if (!s) { char *r = sp_str_alloc(1); r[0] = '.'; r[1] = 0; return r; }
  if (s == path) { char *r = sp_str_alloc(1); r[0] = '/'; r[1] = 0; return r; }
  size_t n = (size_t)(s - path);
  char *buf = sp_str_alloc(n);
  memcpy(buf, path, n); buf[n] = 0;
  return buf;
}
static const char *sp_file_extname(const char *path) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  const char *dot = strrchr(base, '.');
  /* CRuby: leading-dot files (".bashrc") return "". Trailing-dot
     paths ("foo.") keep the dot since Ruby 2.7. */
  if (!dot || dot == base) return sp_str_empty;
  size_t n = strlen(dot);
  char *buf = sp_str_alloc(n);
  memcpy(buf, dot, n + 1);
  return buf;
}
static const char *sp_dir_pwd(void) {
  char tmp[4096];
  if (!getcwd(tmp, sizeof(tmp))) { return sp_str_empty; }
  size_t n = strlen(tmp);
  char *buf = sp_str_alloc(n);
  memcpy(buf, tmp, n + 1);
  return buf;
}
/* Dir singleton methods. mkdir/rmdir/chdir use the platform call (the
   Windows _-prefixed variants take a single path argument); each returns
   0 on success, matching CRuby's `Dir.mkdir` etc. */
static mrb_int sp_dir_mkdir(const char *path) {
#ifdef _WIN32
  return (mrb_int)_mkdir(path);
#else
  return (mrb_int)mkdir(path, 0777);
#endif
}
static mrb_int sp_dir_rmdir(const char *path) {
#ifdef _WIN32
  return (mrb_int)_rmdir(path);
#else
  return (mrb_int)rmdir(path);
#endif
}
static mrb_int sp_dir_chdir(const char *path) {
#ifdef _WIN32
  return (mrb_int)_chdir(path);
#else
  return (mrb_int)chdir(path);
#endif
}
static const char *sp_dir_home(void) {
  const char *h = getenv("HOME");
#ifdef _WIN32
  if (!h || !*h) h = getenv("USERPROFILE");
#endif
  if (!h) return sp_str_empty;
  return sp_str_dup_external(h);
}
/* Wildcard match for a single path component: `*` (any run, no `/`),
   `?` (one char). Recursive over `*`; adequate for the common
   single-directory glob patterns. */
static int sp_fnmatch1(const char *pat, const char *str) {
  while (*pat) {
    if (*pat == '*') {
      pat++;
      if (!*pat) return 1;
      while (*str) { if (sp_fnmatch1(pat, str)) return 1; str++; }
      return sp_fnmatch1(pat, str);
    } else if (*pat == '?') {
      if (!*str) return 0;
      pat++; str++;
    } else {
      if (*pat != *str) return 0;
      pat++; str++;
    }
  }
  return *str == 0;
}
/* Dir.glob(pattern): list directory entries matching the last component
   of `pattern` (an optional leading `dir/` selects the directory). Hidden
   entries match only when the pattern itself begins with `.`. Results are
   sorted, matching Ruby 3.0+ default glob ordering. */
static sp_StrArray *sp_dir_glob(const char *pattern) {
  sp_StrArray *a = sp_StrArray_new();
  if (!pattern) return a;
  const char *slash = strrchr(pattern, '/');
  char dirbuf[1024];
  const char *dirpath;
  const char *base_pat;
  if (slash) {
    size_t dl = (size_t)(slash - pattern);
    if (dl >= sizeof(dirbuf)) return a;
    memcpy(dirbuf, pattern, dl);
    dirbuf[dl] = 0;
    dirpath = (dl == 0) ? "/" : dirbuf;
    base_pat = slash + 1;
  } else {
    dirpath = ".";
    base_pat = pattern;
  }
  DIR *d = opendir(dirpath);
  if (!d) return a;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    const char *name = e->d_name;
    if (name[0] == '.' && base_pat[0] != '.') continue;
    if (sp_fnmatch1(base_pat, name)) {
      char full[2048];
      if (slash) snprintf(full, sizeof(full), "%s/%s", dirbuf, name);
      else snprintf(full, sizeof(full), "%s", name);
      char *copy = sp_str_alloc(strlen(full));
      strcpy(copy, full);
      sp_StrArray_push(a, copy);
    }
  }
  closedir(d);
  sp_StrArray_sort_bang(a);
  return a;
}

/* File.expand_path(path[, base]) -- CRuby-compatible pure-string
   expansion (does NOT require the path to exist). A leading `~` / `~/`
   becomes $HOME; a relative path is resolved against `base` (itself
   expanded; NULL means the current working directory); and `.` / `..` /
   duplicate-slash segments are collapsed. `~user` is unsupported and is
   left as-is. */
static const char *sp_file_expand_path(const char *path, const char *base) {
  char raw[8192];
  char cwd[4096];
  const char *home = getenv("HOME");
  if (!home) home = "";
  if (!path) path = "";

  /* The `%.4000s` precision caps each component so the compiler can
     prove the combined output (<= 8001) fits in the 8192 buffer -- this
     silences -Wformat-truncation (an error under the test harness's
     -Werror). 4000 chars/component is well past PATH_MAX, so real paths
     never truncate. */
  if (path[0] == '~' && (path[1] == '\0' || path[1] == '/')) {
    snprintf(raw, sizeof(raw), "%.4000s%.4000s", home, path + 1);
  } else if (path[0] == '/') {
    snprintf(raw, sizeof(raw), "%.4000s", path);
  } else {
    char basebuf[8192];
    const char *b;
    if (base && base[0]) {
      if (base[0] == '~' && (base[1] == '\0' || base[1] == '/')) {
        snprintf(basebuf, sizeof(basebuf), "%.4000s%.4000s", home, base + 1);
        b = basebuf;
      } else if (base[0] == '/') {
        b = base;
      } else {
        if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
        snprintf(basebuf, sizeof(basebuf), "%.4000s/%.4000s", cwd, base);
        b = basebuf;
      }
    } else {
      if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
      b = cwd;
    }
    snprintf(raw, sizeof(raw), "%.4000s/%.4000s", b, path);
  }

  /* Normalize: walk segments, collapsing `.`/`..`/`//`. seg_start[k]
     records the output length to roll back to when a `..` pops the
     k-th kept segment. */
  size_t rawlen = strlen(raw);
  char *out = sp_str_alloc(rawlen + 1);
  size_t seg_start[1024];
  int nseg = 0;
  size_t olen = 0;
  out[olen++] = '/';
  const char *p = raw;
  while (*p) {
    if (*p == '/') { p++; continue; }
    const char *q = p;
    while (*q && *q != '/') q++;
    size_t slen = (size_t)(q - p);
    if (slen == 1 && p[0] == '.') {
      /* current dir -- skip */
    } else if (slen == 2 && p[0] == '.' && p[1] == '.') {
      if (nseg > 0) { nseg--; olen = seg_start[nseg]; }
    } else {
      size_t mark = olen;
      if (olen > 1) out[olen++] = '/';
      memcpy(out + olen, p, slen);
      olen += slen;
      if (nseg < 1024) seg_start[nseg++] = mark;
    }
    p = q;
  }
  out[olen] = 0;
  return out;
}

/* Read a file's bytes into a fresh IntArray. Distinct from
   `sp_str_bytes(sp_file_read(path))` because plain sp_str_bytes uses
   null-termination and stops at the first 0x00 byte — wrong for
   binary data (e.g. .nes ROM files). */
static sp_IntArray *sp_file_binread_bytes(const char *path) {
  if (sp_file_directory(path)) {
    sp_raise_cls("Errno::EISDIR", sp_sprintf("Is a directory @ io_fread - %s", path));
  }
  FILE *f = fopen(path, "rb");
  sp_IntArray *a = sp_IntArray_new();
  if (!f) return a;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *buf = (unsigned char *)malloc(sz > 0 ? (size_t)sz : 1);
  if (buf && sz > 0) {
    /* Use fread's actual byte count, not the raw file size — a
       partial read otherwise pushes uninitialized memory. */
    size_t r = fread(buf, 1, (size_t)sz, f);
    for (size_t i = 0; i < r; i++) sp_IntArray_push(a, (mrb_int)buf[i]);
  }
  free(buf);
  fclose(f);
  return a;
}

/* `arr.slice!(from, n)` — returns a fresh array of `n` elements
   starting at `from` and removes them from `a`. IntArray uses its
   `start` field for an O(1) head peel (from == 0); the others
   shift the tail down to fill the hole. */
static sp_IntArray *sp_IntArray_slice_bang(sp_IntArray *a, mrb_int from, mrb_int n) {
  if (!a) return sp_IntArray_new();
  if (a->frozen) { sp_raise_frozen_array(); return sp_IntArray_new(); }
  if (from < 0) from += a->len;
  if (from < 0) from = 0;
  if (from > a->len) from = a->len;
  if (n < 0) n = 0;
  if (from + n > a->len) n = a->len - from;
  sp_IntArray *r = sp_IntArray_new();
  for (mrb_int i = 0; i < n; i++) sp_IntArray_push(r, a->data[a->start + from + i]);
  if (from == 0) {
    a->start += n;
    a->len -= n;
  } else {
    for (mrb_int i = from; i + n < a->len; i++) a->data[a->start + i] = a->data[a->start + i + n];
    a->len -= n;
  }
  return r;
}
static sp_FloatArray *sp_FloatArray_slice_bang(sp_FloatArray *a, mrb_int from, mrb_int n) {
  if (!a) return sp_FloatArray_new();
  if (a->frozen) { sp_raise_frozen_array(); return sp_FloatArray_new(); }
  if (from < 0) from += a->len;
  if (from < 0) from = 0;
  if (from > a->len) from = a->len;
  if (n < 0) n = 0;
  if (from + n > a->len) n = a->len - from;
  sp_FloatArray *r = sp_FloatArray_new();
  for (mrb_int i = 0; i < n; i++) sp_FloatArray_push(r, a->data[from + i]);
  for (mrb_int i = from; i + n < a->len; i++) a->data[i] = a->data[i + n];
  a->len -= n;
  return r;
}
static sp_StrArray *sp_StrArray_slice_bang(sp_StrArray *a, mrb_int from, mrb_int n) {
  if (!a) return sp_StrArray_new();
  if (a->frozen) { sp_raise_frozen_array(); return sp_StrArray_new(); }
  if (from < 0) from += a->len;
  if (from < 0) from = 0;
  if (from > a->len) from = a->len;
  if (n < 0) n = 0;
  if (from + n > a->len) n = a->len - from;
  sp_StrArray *r = sp_StrArray_new();
  for (mrb_int i = 0; i < n; i++) sp_StrArray_push(r, a->data[from + i]);
  for (mrb_int i = from; i + n < a->len; i++) a->data[i] = a->data[i + n];
  a->len -= n;
  return r;
}
/* at_exit hooks: a static LIFO of registered procs. Initialized
   zero-len in BSS; main()'s tail walks it in reverse-registration
   order before returning. */
#define SP_AT_EXIT_MAX 256
struct sp_Proc;
static struct sp_Proc *sp_at_exit_hooks[SP_AT_EXIT_MAX];
static mrb_int sp_at_exit_count = 0;
static sp_PtrArray *sp_PtrArray_slice_bang(sp_PtrArray *a, mrb_int from, mrb_int n) {
  if (!a) return sp_PtrArray_new_scan(NULL);
  if (a->frozen) { sp_raise_frozen_array(); return sp_PtrArray_new_scan(a->scan_elem); }
  if (from < 0) from += a->len;
  if (from < 0) from = 0;
  if (from > a->len) from = a->len;
  if (n < 0) n = 0;
  if (from + n > a->len) n = a->len - from;
  sp_PtrArray *r = sp_PtrArray_new_scan(a->scan_elem);
  for (mrb_int i = 0; i < n; i++) sp_PtrArray_push(r, a->data[from + i]);
  for (mrb_int i = from; i + n < a->len; i++) a->data[i] = a->data[i + n];
  a->len -= n;
  return r;
}

typedef struct sp_Proc { void *fn; void *cap; void (*cap_scan)(void *); mrb_int arity; mrb_bool lambda_p; mrb_int param_count; const sp_sym *param_kinds; const sp_sym *param_names; } sp_Proc;
static void sp_Proc_scan(void *p) { sp_Proc *pr = (sp_Proc *)p; if (pr->cap && pr->cap_scan) pr->cap_scan(pr->cap); }
static sp_Proc *sp_proc_new_meta(void *fn, void *cap, void (*cap_scan)(void *), mrb_int arity, mrb_bool lambda_p, mrb_int param_count, const sp_sym *param_kinds, const sp_sym *param_names) { sp_Proc *p = (sp_Proc *)sp_gc_alloc(sizeof(sp_Proc), NULL, sp_Proc_scan); p->fn = fn; p->cap = cap; p->cap_scan = cap_scan; p->arity = arity; p->lambda_p = lambda_p; p->param_count = param_count; p->param_kinds = param_kinds; p->param_names = param_names; return p; }
static sp_Proc *sp_proc_new(void *fn, void *cap, void (*cap_scan)(void *)) { return sp_proc_new_meta(fn, cap, cap_scan, 0, FALSE, 0, NULL, NULL); }
static mrb_int sp_proc_arity(sp_Proc *p) { return p ? p->arity : 0; }
static mrb_bool sp_proc_lambda_p(sp_Proc *p) { return p ? p->lambda_p : FALSE; }
static mrb_int sp_proc_call(sp_Proc *p, mrb_int *args) { if (!p || !p->fn) return 0; if (!args) { mrb_int noargs[16] = {0}; return ((mrb_int (*)(void *, mrb_int *))p->fn)(p->cap, noargs); } return ((mrb_int (*)(void *, mrb_int *))p->fn)(p->cap, args); }
static sp_PolyArray *sp_proc_parameters(sp_Proc *p) { sp_PolyArray *r = sp_PolyArray_new(); if (!p || p->param_count <= 0 || !p->param_kinds) return r; SP_GC_ROOT(r); for (mrb_int i = 0; i < p->param_count; i++) { sp_PolyArray *pair = sp_PolyArray_new(); sp_PolyArray_push(pair, sp_box_sym(p->param_kinds[i])); if (p->param_names && p->param_names[i] >= 0) sp_PolyArray_push(pair, sp_box_sym(p->param_names[i])); sp_PolyArray_push(r, sp_box_poly_array(pair)); } return r; }

/* Proc#<< / Proc#>> composition. The composed proc captures the two
   operands and, on call, threads its single argument through inner
   then outer: `(f << g).call(x)` == f(g(x)). For `>>` the codegen
   swaps the operands so `(f >> g).call(x)` == g(f(x)). */
typedef struct { sp_Proc *outer; sp_Proc *inner; } sp_ProcCompose;
static void sp_proc_compose_scan(void *p) { sp_ProcCompose *c = (sp_ProcCompose *)p; if (c->outer) sp_gc_mark(c->outer); if (c->inner) sp_gc_mark(c->inner); }
static mrb_int sp_proc_compose_fn(void *cap, mrb_int *args) {
  sp_ProcCompose *c = (sp_ProcCompose *)cap;
  mrb_int inner_args[16] = {0};
  if (args) inner_args[0] = args[0];
  mrb_int mid = sp_proc_call(c->inner, inner_args);
  mrb_int outer_args[16] = {0};
  outer_args[0] = mid;
  return sp_proc_call(c->outer, outer_args);
}
static sp_Proc *sp_proc_compose(sp_Proc *outer, sp_Proc *inner) {
  sp_ProcCompose *c = (sp_ProcCompose *)sp_gc_alloc(sizeof(sp_ProcCompose), NULL, sp_proc_compose_scan);
  c->outer = outer;
  c->inner = inner;
  return sp_proc_new_meta((void *)sp_proc_compose_fn, c, sp_proc_compose_scan, 1, TRUE, 1, NULL, NULL);
}
/* Hash#to_proc cap-scan: the proc's `cap` field IS the source hash
   (a single GC pointer), so marking it keeps the hash alive for the
   proc's lifetime. The per-variant lookup fn is emitted by codegen
   alongside the hash type it closes over. */
static void sp_hashproc_cap_scan(void *p) { sp_gc_mark(p); }

/* Random — per-instance PRNG. CRuby uses MT19937; spinel uses a
   portable xorshift64 (rand_r is POSIX-only, absent on MinGW), so
   the *sequence* differs from MRI but each Random object keeps its
   own reproducible stream from its seed. Issue #898. */
typedef struct { uint64_t state; } sp_Random;
static uint64_t sp_random_next(sp_Random *r) {
  uint64_t x = r->state ? r->state : 0x9E3779B97F4A7C15ULL;
  x ^= x << 13; x ^= x >> 7; x ^= x << 17;
  r->state = x;
  return x;
}
static sp_Random *sp_Random_new(mrb_int seed) {
  sp_Random *r = (sp_Random *)sp_gc_alloc(sizeof(sp_Random), NULL, NULL);
  r->state = (uint64_t)seed ^ 0x9E3779B97F4A7C15ULL;
  return r;
}
static mrb_int sp_Random_rand_int(sp_Random *r, mrb_int n) {
  if (!r || n <= 0) return 0;
  return (mrb_int)(sp_random_next(r) % (uint64_t)n);
}
static mrb_float sp_Random_rand_float(sp_Random *r) {
  if (!r) return 0.0;
  return (mrb_float)(sp_random_next(r) >> 11) / (mrb_float)(1ULL << 53);
}
/* Class-method forms (`Random.rand` / `Random.bytes`) share one
   lazily-seeded default instance, mirroring CRuby's Random::DEFAULT. */
static sp_Random sp_random_default = { 0 };
static sp_Random *sp_random_default_get(void) {
  if (sp_random_default.state == 0) sp_random_default.state = (uint64_t)time(NULL) ^ 0x9E3779B97F4A7C15ULL;
  return &sp_random_default;
}
/* Random#bytes(n) — n random bytes as a String. Uses sp_str_set_len
   so embedded NULs are preserved and #length reports n. */
static const char *sp_Random_bytes(sp_Random *r, mrb_int n) {
  if (n < 0) n = 0;
  char *b = sp_str_alloc((size_t)n);
  for (mrb_int i = 0; i < n; i++) b[i] = (char)(sp_random_next(r) & 0xff);
  b[n] = 0;
  sp_str_set_len(b, (size_t)n);
  return b;
}

/* ---- StringIO runtime ---- */
typedef struct { char *buf; int64_t len; int64_t cap; int64_t pos; int64_t lineno; int closed; } sp_StringIO;
static void sio_grow(sp_StringIO *sio, int64_t need) { int64_t req = sio->pos + need; if (req <= sio->cap) return; int64_t nc = sio->cap ? sio->cap : 64; while (nc < req) nc *= 2; sio->buf = (char *)realloc(sio->buf, nc + 1); sio->cap = nc; }
static int64_t sio_write(sp_StringIO *sio, const char *d, int64_t dl) { sio_grow(sio, dl); if (sio->pos > sio->len) memset(sio->buf + sio->len, 0, sio->pos - sio->len); memcpy(sio->buf + sio->pos, d, dl); sio->pos += dl; if (sio->pos > sio->len) sio->len = sio->pos; sio->buf[sio->len] = '\0'; return dl; }
static sp_StringIO *sp_StringIO_new(void) { sp_StringIO *s = (sp_StringIO *)calloc(1, sizeof(sp_StringIO)); s->buf = (char *)calloc(1, 64); s->cap = 63; return s; }
static sp_StringIO *sp_StringIO_new_s(const char *init) { if (!init) sp_raise_cls("TypeError", "no implicit conversion of nil into String"); sp_StringIO *s = (sp_StringIO *)calloc(1, sizeof(sp_StringIO)); int64_t l = (int64_t)strlen(init); int64_t c = l < 63 ? 63 : l; s->buf = (char*)malloc(c+1); memcpy(s->buf, init, l); s->buf[l]='\0'; s->len = l; s->cap = c; return s; }
/* StringIO.new(str, mode): the mode's first char selects the initial
   content/position. "w"/"w+" truncate to empty; "a"/"a+" keep the
   content and seek to the end (subsequent writes append); "r"/"r+" and
   anything else keep the content at position 0. Read-only enforcement
   ("r" rejecting writes) is not modelled. */
static sp_StringIO *sp_StringIO_new_sm(const char *init, const char *mode) {
  if (!init) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  if (!mode || !mode[0]) return sp_StringIO_new_s(init);
  char m0 = mode[0];
  if (m0 == 'w') return sp_StringIO_new();
  sp_StringIO *s = sp_StringIO_new_s(init);
  if (m0 == 'a') s->pos = s->len;
  return s;
}
static const char *sp_StringIO_string(sp_StringIO *s) { return s->buf ? s->buf : sp_str_empty; }
static int64_t sp_StringIO_pos(sp_StringIO *s) { return s->pos; }
static int64_t sp_StringIO_size(sp_StringIO *s) { return s->len; }
static int64_t sp_StringIO_write(sp_StringIO *s, const char *str) { return sio_write(s, str, (int64_t)strlen(str)); }
static int64_t sp_StringIO_puts(sp_StringIO *s, const char *str) { int64_t l = (int64_t)strlen(str); sio_write(s, str, l); if (l == 0 || str[l-1] != '\n') sio_write(s, "\n", 1); return 0; }
static int64_t sp_StringIO_puts_empty(sp_StringIO *s) { sio_write(s, "\n", 1); return 0; }
static int64_t sp_StringIO_print(sp_StringIO *s, const char *str) { return sio_write(s, str, (int64_t)strlen(str)); }
static int64_t sp_StringIO_putc(sp_StringIO *s, int64_t ch) { char c = (char)(ch & 0xFF); sio_write(s, &c, 1); return ch; }
static const char *sp_StringIO_read(sp_StringIO *s) { if (s->pos >= s->len) return sp_str_empty; size_t rem = s->len - s->pos; char *r = sp_str_alloc(rem); memcpy(r, s->buf + s->pos, rem); r[rem] = 0; s->pos = s->len; return r; }
static const char *sp_StringIO_read_n(sp_StringIO *s, int64_t n) { if (s->pos >= s->len) return sp_str_empty; int64_t rem = s->len - s->pos; if (n > rem) n = rem; char *r = sp_str_alloc_raw(n+1); memcpy(r, s->buf + s->pos, n); r[n] = '\0'; s->pos += n; return r; }
static const char *sp_StringIO_gets(sp_StringIO *s) { if (s->pos >= s->len) return NULL; const char *st = s->buf + s->pos; const char *nl = memchr(st, '\n', s->len - s->pos); int64_t ll = nl ? (nl - st) + 1 : s->len - s->pos; char *r = sp_str_alloc_raw(ll+1); memcpy(r, st, ll); r[ll] = '\0'; s->pos += ll; s->lineno++; return r; }
static const char *sp_StringIO_getc(sp_StringIO *s) { if (s->pos >= s->len) return NULL; char *gc = sp_str_alloc_raw(2); gc[0] = s->buf[s->pos++]; gc[1] = '\0'; return gc; }
static int64_t sp_StringIO_getbyte(sp_StringIO *s) { if (s->pos >= s->len) return -1; return (int64_t)(unsigned char)s->buf[s->pos++]; }
static int64_t sp_StringIO_rewind(sp_StringIO *s) { s->pos = 0; s->lineno = 0; return 0; }
static int64_t sp_StringIO_seek(sp_StringIO *s, int64_t off) { if (off < 0) off = 0; s->pos = off; return 0; }
static int64_t sp_StringIO_tell(sp_StringIO *s) { return s->pos; }
static mrb_bool sp_StringIO_eof_p(sp_StringIO *s) { return s->pos >= s->len; }
static int64_t sp_StringIO_truncate(sp_StringIO *s, int64_t l) { if (l < 0) l = 0; if (l < s->len) { s->len = l; s->buf[l] = '\0'; } return 0; }
static int64_t sp_StringIO_close(sp_StringIO *s) { s->closed = 1; return 0; }
static mrb_bool sp_StringIO_closed_p(sp_StringIO *s) { return s->closed; }
static sp_StringIO *sp_StringIO_flush(sp_StringIO *s) { return s; }
static mrb_bool sp_StringIO_sync(sp_StringIO *s) { (void)s; return 1; }
static mrb_bool sp_StringIO_isatty(sp_StringIO *s) { (void)s; return 0; }

/* ---- Lambda/closure runtime (sp_Val) ---- */
typedef struct sp_Val sp_Val;
typedef sp_Val *(*sp_fn_t)(sp_Val *self, sp_Val *arg);
struct sp_Val { enum { SP_PROC2, SP_INT2, SP_BOOL2, SP_NIL2 } tag; union { struct { sp_fn_t fn; int ncaptures; } proc; mrb_int ival; mrb_bool bval; } u; sp_Val *captures[]; };
#ifdef _WIN32
#define SP_ARENA_SIZE ((size_t)256ULL * 1024 * 1024)
#else
#define SP_ARENA_SIZE ((size_t)16ULL * 1024 * 1024 * 1024)
#endif
static char *sp_arena = NULL; static size_t sp_arena_pos = 0;
static void *sp_lam_alloc(size_t sz) { sz = (sz + 7) & ~(size_t)7; if (!sp_arena) { sp_arena = (char *)mmap(NULL, SP_ARENA_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0); if (sp_arena == MAP_FAILED) { perror("mmap"); exit(1); } sp_arena_pos = 0; } if (sp_arena_pos + sz > SP_ARENA_SIZE) { fprintf(stderr, "arena exhausted\n"); exit(1); } void *p = sp_arena + sp_arena_pos; sp_arena_pos += sz; return p; }
static sp_Val *sp_lam_proc(sp_fn_t fn, int ncap) { sp_Val *v = (sp_Val *)sp_lam_alloc(sizeof(sp_Val) + sizeof(sp_Val *) * ncap); v->tag = SP_PROC2; v->u.proc.fn = fn; v->u.proc.ncaptures = ncap; return v; }
static sp_Val *sp_lam_int(mrb_int n) { sp_Val *v = (sp_Val *)sp_lam_alloc(sizeof(sp_Val)); v->tag = SP_INT2; v->u.ival = n; return v; }
static sp_Val *sp_lam_bool(mrb_bool b) { sp_Val *v = (sp_Val *)sp_lam_alloc(sizeof(sp_Val)); v->tag = SP_BOOL2; v->u.bval = b; return v; }
static sp_Val sp_lam_nil_val = { .tag = SP_NIL2 };
static sp_Val *sp_lam_call(sp_Val *f, sp_Val *arg) { return f->u.proc.fn(f, arg); }
/* Multi-arg lambda dispatch . The fn pointer is stored
   typed as `sp_fn_t` (1-arg) so re-cast at the call site to the
   right arity. The generated lambda body is declared with the
   matching arity so the actual ABI matches. */
typedef sp_Val *(*sp_fn2_t)(sp_Val *self, sp_Val *a, sp_Val *b);
typedef sp_Val *(*sp_fn3_t)(sp_Val *self, sp_Val *a, sp_Val *b, sp_Val *c);
typedef sp_Val *(*sp_fn4_t)(sp_Val *self, sp_Val *a, sp_Val *b, sp_Val *c, sp_Val *d);
static sp_Val *sp_lam_call2(sp_Val *f, sp_Val *a, sp_Val *b) { return ((sp_fn2_t)(uintptr_t)f->u.proc.fn)(f, a, b); }
static sp_Val *sp_lam_call3(sp_Val *f, sp_Val *a, sp_Val *b, sp_Val *c) { return ((sp_fn3_t)(uintptr_t)f->u.proc.fn)(f, a, b, c); }
static sp_Val *sp_lam_call4(sp_Val *f, sp_Val *a, sp_Val *b, sp_Val *c, sp_Val *d) { return ((sp_fn4_t)(uintptr_t)f->u.proc.fn)(f, a, b, c, d); }
/* lambda#<< / #>> composition over the sp_Val * representation.
   captures[0] = outer, captures[1] = inner; `(f << g).(x)` == f(g(x)).
   The codegen swaps operands for `>>`. */
static sp_Val *sp_lam_compose_fn(sp_Val *self, sp_Val *arg) { return sp_lam_call(self->captures[0], sp_lam_call(self->captures[1], arg)); }
static sp_Val *sp_lam_compose(sp_Val *outer, sp_Val *inner) { sp_Val *v = sp_lam_proc(sp_lam_compose_fn, 2); v->captures[0] = outer; v->captures[1] = inner; return v; }
/* Proc#curry over the sp_Val * representation. The curried value is a
   proc whose captures are [target, arity, arg0, arg1, ...]; applying
   one more arg either returns a fresh curried proc (still short of
   arity) or invokes the target with the full argument list. Each `[]`
   application supplies a single argument. */
static sp_Val *sp_lam_curry_fn(sp_Val *self, sp_Val *arg) {
  sp_Val *target = self->captures[0];
  mrb_int arity = self->captures[1]->u.ival;
  int have = self->u.proc.ncaptures - 2;
  if (have + 1 < arity) {
    sp_Val *v = sp_lam_proc(sp_lam_curry_fn, self->u.proc.ncaptures + 1);
    v->captures[0] = target;
    v->captures[1] = self->captures[1];
    for (int i = 0; i < have; i++) v->captures[2 + i] = self->captures[2 + i];
    v->captures[2 + have] = arg;
    return v;
  }
  if (arity == 1) return sp_lam_call(target, arg);
  if (arity == 2) return sp_lam_call2(target, self->captures[2], arg);
  if (arity == 3) return sp_lam_call3(target, self->captures[2], self->captures[3], arg);
  if (arity == 4) return sp_lam_call4(target, self->captures[2], self->captures[3], self->captures[4], arg);
  return &sp_lam_nil_val;
}
static sp_Val *sp_lam_curry(sp_Val *f, mrb_int arity) { sp_Val *v = sp_lam_proc(sp_lam_curry_fn, 2); v->captures[0] = f; v->captures[1] = sp_lam_int(arity); return v; }
static mrb_int sp_lam_to_int(sp_Val *v) { return v->u.ival; }


/* Bigint (linked from sp_bigint.o) */
typedef struct sp_Bigint sp_Bigint;
sp_Bigint *sp_bigint_new_int(int64_t v);
sp_Bigint *sp_bigint_new_str(const char *s, int base);
sp_Bigint *sp_bigint_add(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_sub(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mul(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_div(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mod(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_pow(sp_Bigint *base, int64_t exp);
int sp_bigint_cmp(sp_Bigint *a, sp_Bigint *b);
int64_t sp_bigint_to_int(sp_Bigint *b);
const char *sp_bigint_to_s(sp_Bigint *b);
void sp_bigint_free(sp_Bigint *b);

/* Bitwise ops on bigint operands. Used by --int-overflow=promote
   mode where all int slots widen to sp_Bigint *. Implemented via
   int64 round-trip: bigint values produced by promotion are
   almost always derived from small ints (counters, masks, bit-
   width-sized values <= 64 bits), so the int64 path preserves
   the full Ruby-side semantics for any value that fits. Values
   exceeding int64 lose precision through these helpers -- those
   are extremely rare in practice (Ruby bitops on integers >
   2^63), and proper mpz_and / mpz_or / mpz_xor support can be
   added later in lib/sp_bigint.c if a real workload needs it. */
sp_Bigint *sp_bigint_and(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_or(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_xor(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_shl(sp_Bigint *a, int64_t n);
sp_Bigint *sp_bigint_shr(sp_Bigint *a, int64_t n);
sp_Bigint *sp_bigint_not(sp_Bigint *a);


/* Bigint (linked from libspinel_rt.a) */
typedef struct sp_Bigint sp_Bigint;
sp_Bigint *sp_bigint_new_int(int64_t v);
sp_Bigint *sp_bigint_new_str(const char *s, int base);
sp_Bigint *sp_bigint_add(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_sub(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mul(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_div(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mod(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_pow(sp_Bigint *base, int64_t exp);
int sp_bigint_cmp(sp_Bigint *a, sp_Bigint *b);
int64_t sp_bigint_to_int(sp_Bigint *b);
const char *sp_bigint_to_s(sp_Bigint *b);
void sp_bigint_free(sp_Bigint *b);
sp_Bigint *sp_bigint_and(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_or(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_xor(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_shl(sp_Bigint *a, int64_t n);
sp_Bigint *sp_bigint_shr(sp_Bigint *a, int64_t n);
sp_Bigint *sp_bigint_not(sp_Bigint *a);

/* ---- Pack / Unpack (linked from sp_pack.o) ----
   Implementation lives in libspinel_rt.a; the entry points
   below call into the static GC helpers in this header via the
   sp_ext_* shims defined further down. */
const char *sp_IntArray_pack(sp_IntArray *arr, const char *fmt);
const char *sp_PolyArray_pack(sp_PolyArray *arr, const char *fmt);
sp_PolyArray *sp_str_unpack(const char *str, const char *fmt);

/* ---- StringScanner (linked from sp_strscan.o) ----
   GC-allocated; the source / matched fields are marked by
   sp_StringScanner_scan_gc through the shim layer. */
typedef struct sp_StringScanner sp_StringScanner;
sp_StringScanner *sp_StringScanner_new(const char *str);
const char *sp_StringScanner_scan(sp_StringScanner *sc, mrb_regexp_pattern *pat);
const char *sp_StringScanner_check(sp_StringScanner *sc, mrb_regexp_pattern *pat);
const char *sp_StringScanner_scan_until(sp_StringScanner *sc, mrb_regexp_pattern *pat);
const char *sp_StringScanner_aref(sp_StringScanner *sc, mrb_int n);
const char *sp_StringScanner_matched(sp_StringScanner *sc);
mrb_bool    sp_StringScanner_matched_p(sp_StringScanner *sc);
mrb_int     sp_StringScanner_pos(sp_StringScanner *sc);
mrb_int     sp_StringScanner_pos_set(sp_StringScanner *sc, mrb_int p);
mrb_bool    sp_StringScanner_eos_p(sp_StringScanner *sc);
const char *sp_StringScanner_getch(sp_StringScanner *sc);
const char *sp_StringScanner_peek(sp_StringScanner *sc, mrb_int n);
sp_StringScanner *sp_StringScanner_unscan(sp_StringScanner *sc);
const char *sp_StringScanner_rest(sp_StringScanner *sc);
mrb_int     sp_StringScanner_rest_size(sp_StringScanner *sc);
mrb_bool    sp_StringScanner_rest_p(sp_StringScanner *sc);
sp_StringScanner *sp_StringScanner_terminate(sp_StringScanner *sc);
const char *sp_StringScanner_string(sp_StringScanner *sc);
const char *sp_StringScanner_pre_match(sp_StringScanner *sc);
const char *sp_StringScanner_post_match(sp_StringScanner *sc);
sp_StringScanner *sp_StringScanner_reset(sp_StringScanner *sc);

/* External-TU shim wrappers. The runtime's GC-allocating helpers
   are `static inline`; separately-compiled .c files in
   libspinel_rt.a can't reach the per-TU state. Define plain
   external-linkage wrappers here so the main binary's single TU
   owns the GC state and lib code calls in via stable symbols.
   sp_runtime.h is included exactly once per binary (in the
   generated main file), so there's no multi-definition risk. */
sp_PolyArray *sp_ext_poly_array_new(void) { return sp_PolyArray_new(); }
void sp_ext_poly_array_push_int(sp_PolyArray *a, int64_t v) { sp_PolyArray_push(a, sp_box_int((mrb_int)v)); }
void sp_ext_poly_array_push_str(sp_PolyArray *a, const char *s) { sp_PolyArray_push(a, sp_box_str(s)); }
char *sp_ext_str_alloc(size_t n) { return sp_str_alloc(n); }
void sp_ext_str_set_len(char *s, size_t n) { sp_str_set_len(s, n); }
const char *sp_ext_str_empty(void) { return sp_str_empty; }
size_t sp_ext_str_byte_len(const char *s) { return sp_str_byte_len(s); }
void *sp_ext_gc_alloc(size_t sz, void (*fin)(void *), void (*scan)(void *)) { return sp_gc_alloc(sz, fin, scan); }
void sp_ext_mark_string(const char *s) { sp_mark_string(s); }

#ifdef __APPLE__
#pragma clang diagnostic pop
#endif

#endif /* SP_RUNTIME_H */
