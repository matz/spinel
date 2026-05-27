/* Spinel Runtime Library */
#ifndef SP_RUNTIME_H
#define SP_RUNTIME_H

#ifdef __APPLE__
/* getcontext/makecontext/swapcontext are gated behind _XOPEN_SOURCE on
   Darwin and marked deprecated since 10.6. _DARWIN_C_SOURCE re-enables
   Darwin extensions (MAP_ANON, etc.) that _XOPEN_SOURCE alone would hide.
   Suppress the deprecation warning so -Werror builds pass — we knowingly
   use these APIs because Spinel's Fiber implementation depends on them. */
#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

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
#ifdef _WIN32
#include <windows.h>
#include <process.h>
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

typedef int64_t mrb_int;
typedef double mrb_float;
typedef bool mrb_bool;

/* Sentinel value reserved by the int? (scalar-nullable int) type. An
   int? slot is bit-compatible with mrb_int; SP_INT_NIL marks the
   "nil" inhabitant. The chosen pattern is INT64_MIN, which Ruby's
   Integer would auto-promote to Bignum (#597 limitation), so the
   reservation lines up with spinel's existing fast-path-only spec.
   `sp_int_is_nil(v)` is the canonical predicate; treat any int? value
   produced by runtime helpers as opaque outside this macro. */
#define SP_INT_NIL ((mrb_int)INT64_MIN)
#define sp_int_is_nil(v) ((v) == SP_INT_NIL)
/* sp_sym is defined per-program in emit_sym_runtime, but poly helpers
   below need to reference it by forward declaration. */
typedef mrb_int sp_sym;
static const char *sp_sym_to_s(sp_sym id);
#ifndef TRUE
#define TRUE true
#endif
#ifndef FALSE
#define FALSE false
#endif

/* sp_raise_cls forward decl — defined later in this header (line ~1017).
   Used by the integer-division helpers below to match CRuby semantics:
   `a / 0`, `a % 0`, `a.divmod(0)`, `a.ceildiv(0)`, and `a.pow(e, 0)` all
   raise ZeroDivisionError instead of triggering C undefined behaviour
   (SIGFPE on x86) or silently returning 0. */
static void sp_raise_cls(const char *cls, const char *msg);

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
/* Portable fallback. mrb_int is int64_t (see typedef above), so the
   add / sub fallbacks compute in mrb_uint (= uint64_t) -- unsigned
   integer overflow is well-defined wrap-around in C -- and detect
   signed overflow via the sign-bit XOR trick. Mul checks bounds
   before multiplying because the 128-bit intermediate isn't
   portable. */
#define SP_INT_OVERFLOW_MASK ((mrb_uint)1 << 63)
static inline mrb_bool sp_int_add_overflow_p(mrb_int a, mrb_int b, mrb_int *r) {
  mrb_uint x = (mrb_uint)a, y = (mrb_uint)b, z = x + y;
  *r = (mrb_int)z;
  return !!(((x ^ z) & (y ^ z)) & SP_INT_OVERFLOW_MASK);
}
static inline mrb_bool sp_int_sub_overflow_p(mrb_int a, mrb_int b, mrb_int *r) {
  mrb_uint x = (mrb_uint)a, y = (mrb_uint)b, z = x - y;
  *r = (mrb_int)z;
  return !!(((x ^ z) & (~y ^ z)) & SP_INT_OVERFLOW_MASK);
}
static inline mrb_bool sp_int_mul_overflow_p(mrb_int a, mrb_int b, mrb_int *r) {
  if (a > 0 && b > 0 && a > MRB_INT_MAX / b) { *r = a * b; return TRUE; }
  if (a < 0 && b > 0 && a < MRB_INT_MIN / b) { *r = a * b; return TRUE; }
  if (a > 0 && b < 0 && b < MRB_INT_MIN / a) { *r = a * b; return TRUE; }
  if (a < 0 && b < 0 && (a <= MRB_INT_MIN || b <= MRB_INT_MIN || -a > MRB_INT_MAX / -b)) {
    *r = a * b; return TRUE;
  }
  *r = a * b;
  return FALSE;
}
#undef SP_INT_OVERFLOW_MASK
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

static mrb_int sp_gcd(mrb_int a,mrb_int b){if(a<0)a=-a;if(b<0)b=-b;while(b){mrb_int t=b;b=a%b;a=t;}return a;}
static mrb_int sp_lcm(mrb_int a,mrb_int b){if(a==0||b==0)return 0;mrb_int g=sp_gcd(a,b);if(a<0)a=-a;if(b<0)b=-b;return (a/g)*b;}
static mrb_int sp_powmod(mrb_int base,mrb_int exp,mrb_int mod){if(mod==0)sp_raise_cls("ZeroDivisionError","divided by 0");mrb_int r=1;mrb_int m=mod<0?-mod:mod;if(m==1){r=0;}else{base=base%m;if(base<0)base+=m;while(exp>0){if(exp%2==1)r=r*base%m;exp=exp/2;base=base*base%m;}}if(mod<0&&r>0)r-=m;return r;}
static mrb_int sp_ceildiv(mrb_int a,mrb_int b){if(b==0)sp_raise_cls("ZeroDivisionError","divided by 0");if(b==-1)return -a;mrb_int q=a/b;if(a%b!=0&&((a^b)>=0))q++;return q;}
static mrb_int sp_int_clamp(mrb_int v,mrb_int lo,mrb_int hi){return v<lo?lo:v>hi?hi:v;}
/* Integer square root via Newton's method — exact for the full mrb_int
   range (no double-precision rounding loss for n > 2^53). CRuby raises
   on negative input; we mirror Spinel's other arithmetic helpers and
   return 0 to avoid an exception path. */
static mrb_int sp_int_sqrt(mrb_int n){if(n<0)return 0;if(n<2)return n;mrb_int x=n,y=(x+1)/2;while(y<x){x=y;y=(x+n/x)/2;}return x;}
static inline char *sp_str_alloc_raw(size_t total_with_null);  /* fwd decl */
static const char*sp_int_chr(mrb_int n){char*s=sp_str_alloc_raw(2);s[0]=(char)n;s[1]=0;return s;}

/* Forward decls so sp_str_to_i_strict can use them. */
static void sp_raise_cls(const char *cls, const char *msg);
static const char *sp_sprintf(const char *fmt, ...);

/* CRuby's `String#to_i` accepts a leading sign, then digits with
   `_` between consecutive digits, and stops at the first non-digit
   (returning what it has so far rather than raising). `"1_2_3asdf"`
   -> 123. spinel previously emitted `(mrb_int)atoll(s)` which stops
   at the first `_`, returning 1 instead. Issue #619. */
static mrb_int sp_str_to_i_cruby(const char *s) {
  if (!s) return 0;
  const char *p = s;
  while (isspace((unsigned char)*p)) p++;
  int neg = 0;
  if (*p == '+') p++;
  else if (*p == '-') { neg = 1; p++; }
  mrb_int v = 0;
  int any = 0;
  while (*p) {
    if (*p >= '0' && *p <= '9') {
      /* Signed-overflow on `v * 10 + digit` is undefined behavior;
         detect via __builtin_*_overflow. CRuby promotes to Bignum
         on overflow but spinel's int model is int64-only — raise
         RangeError instead of silently saturating, so a user-side
         `rescue` can react. */
      mrb_int t;
      if (__builtin_mul_overflow(v, 10, &t) ||
          __builtin_add_overflow(t, (mrb_int)(*p - '0'), &v)) {
        sp_raise_cls("RangeError", sp_sprintf("integer overflow parsing \"%s\"", s));
      }
      any = 1;
      p++;
    } else if (*p == '_' && any && p[1] >= '0' && p[1] <= '9') {
      p++;
    } else {
      break;
    }
  }
  if (!any) return 0;
  return neg ? -v : v;
}

/* `String#to_i(base)` with a non-decimal base. Accepts bases 2..36
   like MRI; `_` is allowed between digits the same way as base 10.
   Stops at the first invalid digit and returns what's parsed so
   far. Issue #883. */
static mrb_int sp_str_to_i_base(const char *s, mrb_int base) {
  if (!s) return 0;
  /* base 0 = auto-detect from prefix (0x → 16, 0b → 2, 0/0o → 8,
     otherwise 10). Per CRuby, only base 0 enables prefix-based
     dispatch — explicit bases just *accept* the matching prefix. */
  if (base != 0 && (base < 2 || base > 36)) base = 10;
  const char *p = s;
  while (isspace((unsigned char)*p)) p++;
  int neg = 0;
  if (*p == '+') p++;
  else if (*p == '-') { neg = 1; p++; }
  if (base == 0) {
    if (*p == '0') {
      if (p[1] == 'x' || p[1] == 'X') { base = 16; p += 2; }
      else if (p[1] == 'b' || p[1] == 'B') { base = 2; p += 2; }
      else if (p[1] == 'o' || p[1] == 'O') { base = 8; p += 2; }
      else if (p[1] == 'd' || p[1] == 'D') { base = 10; p += 2; }
      else if (p[1] >= '0' && p[1] <= '7') { base = 8; p++; }
      else { base = 10; }
    } else {
      base = 10;
    }
  } else if (*p == '0' && p[1] != 0) {
    /* Explicit base accepts the matching prefix. */
    if ((base == 16) && (p[1] == 'x' || p[1] == 'X')) p += 2;
    else if ((base == 2) && (p[1] == 'b' || p[1] == 'B')) p += 2;
    else if ((base == 8) && (p[1] == 'o' || p[1] == 'O')) p += 2;
  }
  mrb_int v = 0;
  int any = 0;
  while (*p) {
    int d = -1;
    if (*p >= '0' && *p <= '9') d = *p - '0';
    else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
    else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
    if (d < 0 || d >= (int)base) {
      if (*p == '_' && any) {
        /* Lookahead: only consume `_` between digits. */
        int n = -1;
        char c = p[1];
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'z') n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') n = c - 'A' + 10;
        if (n >= 0 && n < (int)base) { p++; continue; }
      }
      break;
    }
    mrb_int t;
    if (__builtin_mul_overflow(v, base, &t) ||
        __builtin_add_overflow(t, (mrb_int)d, &v)) {
      sp_raise_cls("RangeError", sp_sprintf("integer overflow parsing \"%s\"", s));
    }
    any = 1;
    p++;
  }
  if (!any) return 0;
  return neg ? -v : v;
}

/* CRuby's `Integer(s)` raises ArgumentError for unparseable input
   (empty string, leading/trailing junk, all-whitespace). The bare
   `(mrb_int)strtoll(s, NULL, 10)` spinel previously emitted silently
   returned 0 instead, which made `Integer(s) rescue 0` always take
   the main branch. This helper matches CRuby semantics: skips
   leading/trailing whitespace, requires at least one valid digit,
   rejects trailing junk. Accepts an optional leading `+` / `-`. */
static mrb_int sp_str_to_i_strict(const char *s) {
  if (!s) sp_raise_cls("ArgumentError", "invalid value for Integer(): nil");
  const char *p = s;
  while (isspace((unsigned char)*p)) p++;
  if (*p == '\0') sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  char *endptr;
  errno = 0;
  long long v = strtoll(p, &endptr, 10);
  if (endptr == p) sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  /* strtoll signals overflow via errno=ERANGE and clamps to
     LLONG_MAX/MIN — raise rather than silently saturate so the
     caller can distinguish "fits in int64" from "too big". */
  if (errno == ERANGE) sp_raise_cls("RangeError", sp_sprintf("integer overflow parsing \"%s\"", s));
  while (isspace((unsigned char)*endptr)) endptr++;
  if (*endptr != '\0') sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  return (mrb_int)v;
}

/* `Integer(s, base)` with explicit base. Bases 2..36, MRI-compatible
   prefix recognition (0x / 0b / 0o when the base matches). Raises
   ArgumentError on invalid input or unsupported base. Issue #887. */
static mrb_int sp_str_to_i_strict_base(const char *s, mrb_int base) {
  if (!s) sp_raise_cls("ArgumentError", "invalid value for Integer(): nil");
  if (base < 2 || base > 36) sp_raise_cls("ArgumentError", sp_sprintf("invalid radix %lld", (long long)base));
  const char *p = s;
  while (isspace((unsigned char)*p)) p++;
  int neg = 0;
  if (*p == '+') p++;
  else if (*p == '-') { neg = 1; p++; }
  if (*p == '0' && p[1] != 0) {
    if ((base == 16) && (p[1] == 'x' || p[1] == 'X')) p += 2;
    else if ((base == 2) && (p[1] == 'b' || p[1] == 'B')) p += 2;
    else if ((base == 8) && (p[1] == 'o' || p[1] == 'O')) p += 2;
  }
  if (*p == '\0') sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  mrb_int v = 0;
  int any = 0;
  while (*p) {
    int d = -1;
    if (*p >= '0' && *p <= '9') d = *p - '0';
    else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
    else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
    if (d < 0 || d >= (int)base) {
      if (*p == '_' && any) {
        int n = -1;
        char c = p[1];
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'z') n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') n = c - 'A' + 10;
        if (n >= 0 && n < (int)base) { p++; continue; }
      }
      break;
    }
    {
      mrb_int t;
      if (__builtin_mul_overflow(v, base, &t) ||
          __builtin_add_overflow(t, (mrb_int)d, &v)) {
        sp_raise_cls("RangeError", sp_sprintf("integer overflow parsing \"%s\"", s));
      }
    }
    any = 1;
    p++;
  }
  if (!any) sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  while (isspace((unsigned char)*p)) p++;
  if (*p != '\0') sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  return neg ? -v : v;
}

/* Kernel#Float() raises ArgumentError on unparseable input. strtod
   on its own would silently return 0.0 for "abc" or empty input;
   match MRI semantics by validating at-least-one-digit + no-trailing-
   junk. Whitespace flanking is fine. Issue #888. */
static mrb_float sp_str_to_f_strict(const char *s) {
  if (!s) sp_raise_cls("ArgumentError", "invalid value for Float(): nil");
  const char *p = s;
  while (isspace((unsigned char)*p)) p++;
  if (*p == '\0') sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Float(): \"%s\"", s));
  char *endptr;
  double v = strtod(p, &endptr);
  if (endptr == p) sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Float(): \"%s\"", s));
  while (isspace((unsigned char)*endptr)) endptr++;
  if (*endptr != '\0') sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Float(): \"%s\"", s));
  return (mrb_float)v;
}
typedef struct{mrb_int first;mrb_int last;}sp_Range;
static sp_Range sp_range_new(mrb_int f,mrb_int l){sp_Range r;r.first=f;r.last=l;return r;}
/* Inclusive-form `Range#include?`/`#cover?` on the boxed (SP_TAG_OBJ
   cls_id SP_BUILTIN_RANGE) Range value. The direct sp_Range typed
   path inlines this same check via compile_range_method_expr;
   poly-recv dispatch needs the wrapper so the cls_id arm in
   emit_poly_builtin_dispatch can land on a single C expression. The
   sp_Range struct doesn't track exclude_end, so behaviour matches
   the inclusive form -- consistent with the direct-typed emit. */
static mrb_bool sp_range_include(sp_Range *r, mrb_int x){return r->first<=x && x<=r->last;}

/* ---- Class object ----
   Value-type Class reference: a single class id that indexes into
   the per-program sp_class_names[] table emitted by codegen. Lets
   `c = Foo` produce a runtime value (`(sp_Class){<id>}`) instead of
   a bare C identifier, and `c.to_s` lower to a names-table lookup.
   Other Class methods (`.name`, `.inspect`, `.==`, `.!=`,
   `.superclass`, `.ancestors`, dynamic `is_a?(c)` against a
   variable, etc.) are not yet supported. */
typedef struct{mrb_int cls_id;}sp_Class;

/* ---- Complex runtime ---- */
/* Value-type Cartesian Complex: 16 bytes, passed by value. Used by
   optcarrot's nestopia palette generator; the palette is precomputed
   in the default code path so this is exercised only with
   `--nestopia-palette`. */
typedef struct{mrb_float re;mrb_float im;}sp_Complex;
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
typedef struct{mrb_int num;mrb_int den;}sp_Rational;
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
/* sp_Time keeps Time.now / Time.at as value-typed structs. d78149b's
   sub-second precision is preserved by inlining tv_sec + tv_nsec/1e9
   at every Time#to_f / Time#- call site (see spinel_codegen.rb).
   `is_utc` distinguishes UTC-coerced times (set by
   `Time#utc`) from local-zone times — a presentation-only flag,
   the underlying epoch is the same instant either way. iso8601 /
   strftime check the flag at format time. Every `(sp_Time){...}`
   site initializes all three fields explicitly (is_utc=0 for the
   local-zone constructors); C99 would zero-init a missing field,
   but spelling it out keeps intent visible and -Wmissing-field-
   initializers clean. */
typedef struct { int64_t tv_sec; int32_t tv_nsec; int8_t is_utc; } sp_Time;
static inline sp_Time sp_time_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (sp_Time){ ts.tv_sec, (int32_t)ts.tv_nsec, 0 };
}
static inline sp_Time sp_time_at_int(mrb_int sec) {
  return (sp_Time){ (int64_t)sec, 0, 0 };
}
/* POSIX convention: keep tv_nsec in [0, 1e9). For negative epoch with a
   non-integer fractional part, decrement tv_sec and roll the fraction
   into the positive nsec range — so Time.at(-0.5).to_i returns -1, not 0. */
static inline sp_Time sp_time_at_float(mrb_float epoch) {
  int64_t sec = (int64_t)epoch;
  mrb_float frac = epoch - (mrb_float)sec;
  if (frac < 0.0) {
    sec -= 1;
    frac += 1.0;
  }
  return (sp_Time){ sec, (int32_t)(frac * 1e9), 0 };
}


/* `recycle`: optional sweep hook. If non-NULL, sp_gc_collect calls
   recycle(h) on the unmarked object instead of finalize+free. The
   hook is responsible for deciding whether to keep the storage
   (pool push) or free it. Used by class-instance free-list pools. */
typedef struct sp_gc_hdr { struct sp_gc_hdr *next; void (*finalize)(void *); void (*scan)(void *); size_t size; unsigned marked : 1; void (*recycle)(struct sp_gc_hdr *); } sp_gc_hdr;
static sp_gc_hdr *sp_gc_heap = NULL; static size_t sp_gc_bytes = 0; static size_t sp_gc_threshold = 256*1024;

/* ---- String GC ---- */
typedef struct sp_str_hdr { struct sp_str_hdr *next; size_t size; size_t len; } sp_str_hdr;
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

static void sp_oom_die(void);

/* ---- Encoding runtime ----
   Spinel currently assumes UTF-8 source and string data. Keep Encoding
   as a tiny value type so `__ENCODING__` and `String#encoding` can
   answer Ruby's Encoding-shaped protocol without carrying full
   transcoding state. */
typedef struct{const char *name;}sp_Encoding;
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
static mrb_int sp_str_ord(const char*s){if(!*s)return 0;uint32_t cp;sp_utf8_decode(s,&cp);return(mrb_int)cp;}
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

static inline void sp_mark_string(const char *s) {
  if (!s) return;
  if ((unsigned char)s[-1] == 0xfe) {
    ((char *)s)[-1] = (char)0xfc;
  }
}

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

/* Time#strftime — format the time per `fmt` using libc
   strftime. The is_utc flag selects gmtime vs localtime so a
   `Time.now.utc.strftime("%H")` runs against the UTC broken-down
   value. Returns a freshly-allocated string (sp_str_dup_external'd
   into the str heap so GC tracks it). The 256-byte buffer is enough
   for every format CRuby builds in (the longest is %c which produces
   ~25 bytes); custom formats that exceed it get truncated. */
static const char *sp_time_strftime(sp_Time t, const char *fmt) {
  char buf[256];
  time_t s = (time_t)t.tv_sec;
  struct tm *lt = t.is_utc ? gmtime(&s) : localtime(&s);
  if (lt == NULL) return SPL("");
  size_t n = strftime(buf, sizeof(buf), fmt, lt);
  if (n == 0) return SPL("");
  buf[n] = 0;
  return sp_str_dup_external(buf);
}

/* Time#iso8601 — RFC 3339 style. Format the date+time
   prefix with strftime, then compute the UTC offset manually via
   `mktime(gmtime(s)) - s` rather than relying on strftime's %z,
   because MSVCRT's %z emits the timezone *name* rather than ±HHMM,
   so a Windows-MinGW build of the same code produces 30+ char
   strings like "...Coordinated Universal Time" instead of CRuby's
   "...+09:00". Computing the offset from the gmtime/mktime
   difference gives the same answer on every libc we target.
   Sub-second precision is intentionally omitted: CRuby's iso8601
   also drops it unless the caller passes a precision arg, which
   we don't support here.
   when is_utc is set, format against gmtime and emit the
   trailing "Z" suffix CRuby uses for UTC iso8601. */
static const char *sp_time_iso8601(sp_Time t) {
  char buf[64];
  time_t s = (time_t)t.tv_sec;
  if (t.is_utc) {
    struct tm *gt = gmtime(&s);
    if (gt == NULL) return SPL("");
    size_t n = strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", gt);
    if (n == 0) return SPL("");
    buf[n] = 0;
    return sp_str_dup_external(buf);
  }
  struct tm *lt = localtime(&s);
  if (lt == NULL) return SPL("");
  size_t n = strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", lt);
  if (n == 0 || n + 6 >= sizeof(buf)) return SPL("");
  /* Compute UTC offset portably: re-interpret gmtime's broken-down
     value as if it were local-time via mktime, then the difference
     from the original epoch is the offset in seconds. mktime mutates
     tm_isdst, so use a fresh copy. */
  struct tm gm = *gmtime(&s);
  gm.tm_isdst = -1;
  time_t gm_as_if_local = mktime(&gm);
  long offset_sec = (long)(s - gm_as_if_local);
  char sign = offset_sec >= 0 ? '+' : '-';
  long abs_off = offset_sec < 0 ? -offset_sec : offset_sec;
  int oh = (int)(abs_off / 3600);
  int om = (int)((abs_off / 60) % 60);
  buf[n++] = sign;
  buf[n++] = (char)('0' + (oh / 10));
  buf[n++] = (char)('0' + (oh % 10));
  buf[n++] = ':';
  buf[n++] = (char)('0' + (om / 10));
  buf[n++] = (char)('0' + (om % 10));
  buf[n] = 0;
  return sp_str_dup_external(buf);
}

/* Time#utc — same instant, presentation flag flipped to
   UTC. iso8601 / strftime check is_utc at format time. CRuby's Time
   internally tracks a similar "gmt" flag; the value-typed sp_Time
   stores it inline. */
static inline sp_Time sp_time_utc(sp_Time t) {
  t.is_utc = 1;
  return t;
}

/* ---- Time broken-down accessors / local Time.new / scalar inspect ----
   sp_time_vtm is the single zone resolver: is_utc selects gmtime vs
   localtime, and for local times the UTC offset is computed via
   mktime(gmtime(s))-s (same portable technique as sp_time_iso8601's
   #414 follow-up; avoids relying on strftime %z which MSVCRT renders
   as a name). The sp_Time struct is unchanged. */
static void sp_time_vtm(sp_Time t, struct tm *bd, int32_t *off, char *zbuf) {
  time_t s = (time_t)t.tv_sec;
  if (t.is_utc) {
    struct tm *g = gmtime(&s);
    if (g) { *bd = *g; } else { memset(bd, 0, sizeof(*bd)); }
    if (off) *off = 0;
    if (zbuf) { zbuf[0]='U'; zbuf[1]='T'; zbuf[2]='C'; zbuf[3]=0; }
  } else {
    struct tm *l = localtime(&s);
    if (l) { *bd = *l; } else { memset(bd, 0, sizeof(*bd)); }
    if (off) {
      struct tm gm = *gmtime(&s);
      gm.tm_isdst = -1;
      *off = (int32_t)(s - (time_t)mktime(&gm));
    }
    if (zbuf) {
      if (strftime(zbuf, 8, "%Z", bd) == 0) zbuf[0] = 0;
    }
  }
}

/* Time.new(y[,mo[,d[,h[,mi[,s]]]]]) — local construction. mktime
   interprets the broken-down value in the host local zone and resolves
   DST itself (tm_isdst=-1), matching CRuby's local Time.new. timegm /
   days-from-civil are intentionally not used. is_utc=0, struct
   unchanged. The fixed-offset 7-arg form is a separate Issue. */
static sp_Time sp_time_new(mrb_int y, mrb_int mo, mrb_int d,
                           mrb_int h, mrb_int mi, mrb_int s) {
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  tm.tm_year = (int)y - 1900;
  tm.tm_mon  = (int)mo - 1;
  tm.tm_mday = (int)d;
  tm.tm_hour = (int)h;
  tm.tm_min  = (int)mi;
  tm.tm_sec  = (int)s;
  tm.tm_isdst = -1;
  time_t e = mktime(&tm);
  return (sp_Time){ (int64_t)e, 0, 0 };
}

static mrb_int sp_time_year(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (mrb_int)(b.tm_year+1900);}
static mrb_int sp_time_mon(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (mrb_int)(b.tm_mon+1);}
static mrb_int sp_time_mday(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (mrb_int)b.tm_mday;}
static mrb_int sp_time_hour(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (mrb_int)b.tm_hour;}
static mrb_int sp_time_min(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (mrb_int)b.tm_min;}
static mrb_int sp_time_sec(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (mrb_int)b.tm_sec;}
static mrb_int sp_time_wday(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (mrb_int)b.tm_wday;}
static mrb_int sp_time_yday(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (mrb_int)(b.tm_yday+1);}
static mrb_int sp_time_isdst(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (mrb_int)(b.tm_isdst>0?1:0);}
/* Time + Numeric / Time - Numeric. secs may be fractional (CRuby's
   Numeric includes Float), so split into whole seconds plus a
   sub-second carry and keep tv_nsec normalized to [0,1e9). is_utc is
   inherited from the receiver. */
static sp_Time sp_time_add(sp_Time t, mrb_float secs) {
  int64_t whole = (int64_t)secs;
  mrb_float frac = secs - (mrb_float)whole;
  int64_t ns = (int64_t)t.tv_nsec + (int64_t)(frac * 1e9);
  int64_t carry = ns / 1000000000;
  ns -= carry * 1000000000;
  if (ns < 0) { ns += 1000000000; carry -= 1; }
  return (sp_Time){ t.tv_sec + whole + carry, (int32_t)ns, t.is_utc };
}
static mrb_int sp_time_utc_offset(sp_Time t){int32_t o;struct tm b;sp_time_vtm(t,&b,&o,NULL);return (mrb_int)o;}
static const char *sp_time_zone(sp_Time t){char z[8];struct tm b;sp_time_vtm(t,&b,NULL,z);return sp_str_dup_external(z);}

/* Scalar Time inspect. CRuby form: local "YYYY-MM-DD HH:MM:SS +0900",
   UTC "YYYY-MM-DD HH:MM:SS UTC". The poly-box path keeps its own
   sp_Time_inspect; this value-taking variant is for the scalar
   p/puts/to_s codegen path that previously fell into the integer
   printf cast and failed C compilation. */
static const char *sp_time_inspect_v(sp_Time t) {
  char buf[40];
  struct tm b;
  int32_t off;
  sp_time_vtm(t, &b, &off, NULL);
  size_t n = strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &b);
  if (n == 0) {
    snprintf(buf, sizeof(buf), "Time(%lld)", (long long)t.tv_sec);
    return sp_str_dup_external(buf);
  }
  if (t.is_utc) {
    buf[n++]=' '; buf[n++]='U'; buf[n++]='T'; buf[n++]='C'; buf[n]=0;
  } else {
    char sign = off >= 0 ? '+' : '-';
    long a = off < 0 ? -(long)off : (long)off;
    int oh = (int)(a / 3600);
    int om = (int)((a / 60) % 60);
    buf[n++]=' '; buf[n++]=sign;
    buf[n++]=(char)('0'+oh/10); buf[n++]=(char)('0'+oh%10);
    buf[n++]=(char)('0'+om/10); buf[n++]=(char)('0'+om%10);
    buf[n]=0;
  }
  return sp_str_dup_external(buf);
}

#define SP_GC_STACK_MAX 65536
static void **sp_gc_roots[SP_GC_STACK_MAX]; static int sp_gc_nroots = 0;
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
#define SP_GC_RESTORE() sp_gc_nroots = _gc_saved
#define SP_GC_MARK_STACK_MAX (1024*64)
static void**sp_gc_mark_stack=NULL;static int sp_gc_mark_top=0;
/* Tag bytes on the byte preceding `obj`:
 *   0xfe : heap-allocated string (sp_str_alloc), unmarked -> bump to 0xfc.
 *   0xfc : heap string already marked this cycle -> skip.
 *   0xff : frozen literal in rodata (sp_str_empty, "\xff"+literal) -> skip.
 *   0xfd : data buffer owned by an sp_String wrapper. The wrapper
 *          itself carries an sp_gc_hdr and is reached as a separate
 *          root; the bare data pointer is just a borrow. Skip here.
 *          Issue #583: without this arm, a `return lv__buf->data;`
 *          from an ERB-style render whose result later lands in a
 *          SP_GC_ROOT slot would fall through to the sp_gc_hdr cast
 *          path on the next collect cycle, read garbage as h->scan,
 *          and crash with SIGBUS / SIGSEGV.
 *   else : a real GC-allocated object; cast back to sp_gc_hdr and
 *          recurse through its scan function. */
static void sp_gc_mark(void*obj){if(!obj)return;unsigned char pm=((unsigned char*)obj)[-1];if(pm==0xfe){((char*)obj)[-1]=(char)0xfc;return;}if(pm==0xfc||pm==0xff||pm==0xfd)return;sp_gc_hdr*h=(sp_gc_hdr*)((char*)obj-sizeof(sp_gc_hdr));if(h->marked)return;h->marked=1;if(h->scan){if(sp_gc_mark_stack&&sp_gc_mark_top<SP_GC_MARK_STACK_MAX){sp_gc_mark_stack[sp_gc_mark_top++]=obj;}else{h->scan(obj);}}}
/* Forward decl: defined alongside the regex globals it marks. */
static void sp_re_mark_globals(void);
/* Hook installed by the Fiber section below to mark every
   suspended fiber's saved-root region during a GC pass. While
   no fibers exist (the common case for non-fiber programs) the
   pointer stays NULL and sp_gc_mark_all skips the walk.
   Issue #636. */
static void (*sp_gc_mark_suspended_fibers_hook)(void) = NULL;
static void sp_gc_mark_all(void){if(!sp_gc_mark_stack)sp_gc_mark_stack=(void**)malloc(sizeof(void*)*SP_GC_MARK_STACK_MAX);sp_gc_mark_top=0;for(int i=0;i<sp_gc_nroots;i++){void*obj=*sp_gc_roots[i];if(obj)sp_gc_mark(obj);}if(sp_gc_mark_suspended_fibers_hook)sp_gc_mark_suspended_fibers_hook();sp_re_mark_globals();while(sp_gc_mark_top>0){void*obj=sp_gc_mark_stack[--sp_gc_mark_top];sp_gc_hdr*h=(sp_gc_hdr*)((char*)obj-sizeof(sp_gc_hdr));if(h->scan)h->scan(obj);}}
static void sp_gc_cleanup(int*p){sp_gc_nroots=*p;}
#define SP_GC_NBUCKETS 32
static sp_gc_hdr*sp_gc_buckets[SP_GC_NBUCKETS];
static inline int sp_gc_bucket(size_t sz){int b=(int)(sz/16);return b<SP_GC_NBUCKETS?b:SP_GC_NBUCKETS-1;}
static int sp_gc_cycle=0;
static sp_gc_hdr*sp_gc_old_heap=NULL;static size_t sp_gc_old_bytes=0;
#define SP_GC_FULL_INTERVAL 8
static void sp_gc_collect(void){int full=(sp_gc_cycle%SP_GC_FULL_INTERVAL==0);sp_gc_cycle++;sp_gc_hdr*hh=sp_gc_old_heap;while(hh){hh->marked=0;hh=hh->next;}sp_gc_mark_all();if(full){sp_gc_hdr**pp=&sp_gc_old_heap;sp_gc_old_bytes=0;while(*pp){sp_gc_hdr*h=*pp;if(!h->marked){*pp=h->next;if(h->recycle){h->recycle(h);}else{if(h->finalize)h->finalize((char*)h+sizeof(sp_gc_hdr));free(h);}}else{h->marked=1;sp_gc_old_bytes+=h->size;pp=&h->next;}}}else{hh=sp_gc_old_heap;while(hh){hh->marked=1;hh=hh->next;}}sp_gc_hdr**pp=&sp_gc_heap;sp_gc_bytes=sp_gc_old_bytes;while(*pp){sp_gc_hdr*h=*pp;if(!h->marked){*pp=h->next;if(h->recycle){h->recycle(h);}else{if(h->finalize)h->finalize((char*)h+sizeof(sp_gc_hdr));free(h);}}else{h->marked=1;*pp=h->next;h->next=sp_gc_old_heap;sp_gc_old_heap=h;sp_gc_old_bytes+=h->size;sp_gc_bytes+=h->size;}}sp_str_sweep();if(full)malloc_trim(0);}
static size_t sp_gc_threshold_init=256*1024;
/* Issue #755: bail out cleanly on OOM. Hot-path allocators check the
   returned pointer and route through this helper rather than
   returning NULL into a caller that would deref it next. The exit
   code is 1, the message format matches the existing
   "unhandled exception" style so post-mortem scripts treat it the
   same way. */
static void sp_oom_die(void){fputs("unhandled exception: out of memory\n",stderr);exit(1);}
void*sp_gc_alloc(size_t sz,void(*fin)(void*),void(*scn)(void*)){if(sp_gc_bytes>sp_gc_threshold){size_t before=sp_gc_bytes;sp_gc_collect();size_t freed=before-sp_gc_bytes;if(freed<before/4){sp_gc_threshold=before*2;}else if(sp_gc_bytes>0){sp_gc_threshold=sp_gc_bytes*4;if(sp_gc_threshold<sp_gc_threshold_init)sp_gc_threshold=sp_gc_threshold_init;}else{sp_gc_threshold=sp_gc_threshold_init;}}size_t need=sizeof(sp_gc_hdr)+sz;sp_gc_hdr*h=(sp_gc_hdr*)calloc(1,need);if(!h)sp_oom_die();h->finalize=fin;h->scan=scn;h->size=need;h->marked=0;h->next=sp_gc_heap;sp_gc_heap=h;sp_gc_bytes+=need;return(char*)h+sizeof(sp_gc_hdr);}
void*sp_gc_alloc_nogc(size_t sz,void(*fin)(void*),void(*scn)(void*)){size_t need=sizeof(sp_gc_hdr)+sz;sp_gc_hdr*h=(sp_gc_hdr*)calloc(1,need);if(!h)sp_oom_die();h->finalize=fin;h->scan=scn;h->size=need;h->marked=0;h->next=sp_gc_heap;sp_gc_heap=h;sp_gc_bytes+=need;return(char*)h+sizeof(sp_gc_hdr);}
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

typedef struct{mrb_int*data;mrb_int start;mrb_int len;mrb_int cap;}sp_IntArray;
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
static sp_IntArray*sp_IntArray_slice_range(sp_IntArray*a,mrb_int start,mrb_int end_,mrb_int excl){if(end_<0)end_+=a->len;mrb_int n=end_-start+(excl?0:1);if(n<0)n=0;return sp_IntArray_slice(a,start,n);}
static void sp_IntArray_replace(sp_IntArray*dst,sp_IntArray*src){dst->len=0;dst->start=0;if(src->len>dst->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)dst-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*dst->cap;h->size-=sizeof(mrb_int)*dst->cap;void*nd=realloc(dst->data,sizeof(mrb_int)*src->len);if(!nd){perror("realloc");exit(1);}dst->data=(mrb_int*)nd;dst->cap=src->len;h->size+=sizeof(mrb_int)*dst->cap;sp_gc_bytes+=sizeof(mrb_int)*dst->cap;}memcpy(dst->data,src->data+src->start,sizeof(mrb_int)*src->len);dst->len=src->len;}
static void __attribute__((noinline)) sp_IntArray_push_grow(sp_IntArray*a){if(a->start>0){memmove(a->data,a->data+a->start,sizeof(mrb_int)*a->len);a->start=0;if(a->len<a->cap)return;}{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*a->cap;h->size-=sizeof(mrb_int)*a->cap;a->cap=a->cap*2+1;void*nd=realloc(a->data,sizeof(mrb_int)*a->cap);if(!nd)sp_oom_die();a->data=(mrb_int*)nd;h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}}
static inline void sp_IntArray_push(sp_IntArray*a,mrb_int v){if(a->start+a->len>=a->cap)sp_IntArray_push_grow(a);a->data[a->start+a->len]=v;a->len++;}
/* Issue #826: guard empty arrays. CRuby returns nil; spinel's int slot
   collapses nil to 0. Without the guard, `--a->len` wraps to -1 and
   reads past the buffer start. */
/* Issue #832: empty pop/shift return SP_INT_NIL (nullable int sentinel)
   to match MRI's nil semantics; callers must treat as int? */
static inline mrb_int sp_IntArray_pop(sp_IntArray*a){if(!a||a->len<=0)return SP_INT_NIL;return a->data[a->start+--a->len];}
static inline mrb_int sp_IntArray_shift(sp_IntArray*a){if(!a||a->len<=0)return SP_INT_NIL;mrb_int v=a->data[a->start];a->start++;a->len--;return v;}
static inline mrb_int sp_IntArray_length(sp_IntArray*a){return a->len;}
static inline mrb_bool sp_IntArray_empty(sp_IntArray*a){return a->len==0;}
static inline mrb_int sp_IntArray_get(sp_IntArray*a,mrb_int i){if(i<0)i+=a->len;return a->data[a->start+i];}
/* Issue #769: a very-negative i (e.g. `a[-999] = 42` on a 3-elt
   array) leaves i negative after the `i += a->len` adjustment.
   CRuby raises IndexError; spinel no-ops as the safest fallback
   (raising from a typed-array set would need setjmp plumbing
   throughout the call chain). */
static void sp_IntArray_set_slow(sp_IntArray*a,mrb_int i,mrb_int v){if(i<0)return;while(a->start+i>=a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*a->cap;h->size-=sizeof(mrb_int)*a->cap;a->cap=a->cap*2+1;a->data=(mrb_int*)realloc(a->data,sizeof(mrb_int)*a->cap);h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}while(i>=a->len){a->data[a->start+a->len]=0;a->len++;}a->data[a->start+i]=v;}
/* Issue #839: an extreme negative index (still negative after
   `i += len`) raises IndexError per MRI. */
static inline void sp_IntArray_set(sp_IntArray*a,mrb_int i,mrb_int v){if(!a)return;mrb_int orig=i;if(i<0)i+=a->len;if(i<0)sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)-a->len));if(i<a->len){a->data[a->start+i]=v;return;}sp_IntArray_set_slow(a,i,v);}
static void sp_IntArray_reverse_bang(sp_IntArray*a){for(mrb_int i=0,j=a->len-1;i<j;i++,j--){mrb_int t=a->data[a->start+i];a->data[a->start+i]=a->data[a->start+j];a->data[a->start+j]=t;}}
static void sp_IntArray_rotate_bang(sp_IntArray*a,mrb_int n){if(a->len<=0)return;n=((n%a->len)+a->len)%a->len;if(n==0)return;mrb_int*tmp=(mrb_int*)malloc(sizeof(mrb_int)*a->len);for(mrb_int i=0;i<a->len;i++)tmp[i]=a->data[a->start+(i+n)%a->len];for(mrb_int i=0;i<a->len;i++)a->data[a->start+i]=tmp[i];free(tmp);}
static int _sp_int_cmp(const void*a,const void*b){mrb_int va=*(const mrb_int*)a,vb=*(const mrb_int*)b;return(va>vb)-(va<vb);}
static sp_IntArray*sp_IntArray_sort(sp_IntArray*a){sp_IntArray*b=sp_IntArray_dup(a);qsort(b->data+b->start,b->len,sizeof(mrb_int),_sp_int_cmp);return b;}
static void sp_IntArray_sort_bang(sp_IntArray*a){qsort(a->data+a->start,a->len,sizeof(mrb_int),_sp_int_cmp);}
static void sp_IntArray_shuffle_bang(sp_IntArray*a){for(mrb_int i=a->len-1;i>0;i--){mrb_int j=(mrb_int)(rand()%(i+1));mrb_int t=a->data[a->start+i];a->data[a->start+i]=a->data[a->start+j];a->data[a->start+j]=t;}}
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
static mrb_int sp_IntArray_delete_at(sp_IntArray*a,mrb_int i){if(i<0)i+=a->len;if(i<0||i>=a->len)return 0;mrb_int v=a->data[a->start+i];for(mrb_int j=i;j<a->len-1;j++)a->data[a->start+j]=a->data[a->start+j+1];a->len--;return v;}
static mrb_int sp_IntArray_delete(sp_IntArray*a,mrb_int v){mrb_int w=0;for(mrb_int i=0;i<a->len;i++){if(a->data[a->start+i]!=v){a->data[a->start+w]=a->data[a->start+i];w++;}}mrb_int d=a->len-w;a->len=w;return d>0?v:0;}
/* Issue #788: clamp i so a very-negative index doesn't underflow past
   a->start and write into the array's GC header. */
static void sp_IntArray_insert(sp_IntArray*a,mrb_int i,mrb_int v){if(!a)return;if(i<0)i+=a->len+1;if(i<0)i=0;if(i>a->len)i=a->len;sp_IntArray_push(a,0);for(mrb_int j=a->len-1;j>i;j--)a->data[a->start+j]=a->data[a->start+j-1];a->data[a->start+i]=v;}
static sp_IntArray*sp_int_digits(mrb_int n,mrb_int base){sp_IntArray*a=sp_IntArray_new();if(base<2)base=10;if(n==0){sp_IntArray_push(a,0);return a;}if(n<0)n=-n;while(n>0){sp_IntArray_push(a,n%base);n/=base;}return a;}
static sp_IntArray*sp_IntArray_uniq(sp_IntArray*a){sp_IntArray*b=sp_IntArray_new();for(mrb_int i=0;i<a->len;i++){int found=0;for(mrb_int j=0;j<b->len;j++){if(b->data[b->start+j]==a->data[a->start+i]){found=1;break;}}if(!found)sp_IntArray_push(b,a->data[a->start+i]);}return b;}
static sp_IntArray*sp_IntArray_intersect(sp_IntArray*a,sp_IntArray*b){sp_IntArray*r=sp_IntArray_new();for(mrb_int i=0;i<a->len;i++){mrb_int v=a->data[a->start+i];if(sp_IntArray_include(b,v)&&!sp_IntArray_include(r,v))sp_IntArray_push(r,v);}return r;}
static sp_IntArray*sp_IntArray_union(sp_IntArray*a,sp_IntArray*b){sp_IntArray*r=sp_IntArray_new();for(mrb_int i=0;i<a->len;i++){mrb_int v=a->data[a->start+i];if(!sp_IntArray_include(r,v))sp_IntArray_push(r,v);}if(b){for(mrb_int i=0;i<b->len;i++){mrb_int v=b->data[b->start+i];if(!sp_IntArray_include(r,v))sp_IntArray_push(r,v);}}return r;}
/* Array#- / Array#difference: keep every LHS element NOT in RHS,
   preserving the LHS's duplicates. CRuby's Array#- is not a set
   subtraction — `[1,1,2,3] - [3]` is `[1,1,2]`, not `[1,2]`. */
static sp_IntArray*sp_IntArray_difference(sp_IntArray*a,sp_IntArray*b){sp_IntArray*r=sp_IntArray_new();for(mrb_int i=0;i<a->len;i++){mrb_int v=a->data[a->start+i];if(!sp_IntArray_include(b,v))sp_IntArray_push(r,v);}return r;}
static void sp_IntArray_unshift(sp_IntArray*a,mrb_int v){if(a->start>0){a->start--;a->data[a->start]=v;a->len++;}else{mrb_int e=a->len+1;if(e>a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*a->cap;h->size-=sizeof(mrb_int)*a->cap;a->cap=a->cap*2+1;a->data=(mrb_int*)realloc(a->data,sizeof(mrb_int)*a->cap);h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}memmove(a->data+1,a->data,sizeof(mrb_int)*a->len);a->data[0]=v;a->len++;}}
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

typedef struct{mrb_float*data;mrb_int len;mrb_int cap;}sp_FloatArray;
static void sp_FloatArray_fin(void*p){sp_FloatArray*a=(sp_FloatArray*)p;sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_float)*a->cap;h->size-=sizeof(mrb_float)*a->cap;free(a->data);}
static sp_FloatArray*sp_FloatArray_new(void){sp_FloatArray*a=(sp_FloatArray*)sp_gc_alloc(sizeof(sp_FloatArray),sp_FloatArray_fin,NULL);a->cap=16;a->data=(mrb_float*)malloc(sizeof(mrb_float)*a->cap);if(!a->data)sp_oom_die();a->len=0;{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));h->size+=sizeof(mrb_float)*a->cap;sp_gc_bytes+=sizeof(mrb_float)*a->cap;}return a;}
static inline void sp_FloatArray_push(sp_FloatArray*a,mrb_float v){if(a->len>=a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_float)*a->cap;h->size-=sizeof(mrb_float)*a->cap;a->cap=a->cap*2+1;a->data=(mrb_float*)realloc(a->data,sizeof(mrb_float)*a->cap);h->size+=sizeof(mrb_float)*a->cap;sp_gc_bytes+=sizeof(mrb_float)*a->cap;}a->data[a->len++]=v;}
static mrb_float sp_FloatArray_min(sp_FloatArray*a){if(a->len==0)return 0;mrb_float m=a->data[0];for(mrb_int i=1;i<a->len;i++)if(a->data[i]<m)m=a->data[i];return m;}
static mrb_float sp_FloatArray_max(sp_FloatArray*a){if(a->len==0)return 0;mrb_float m=a->data[0];for(mrb_int i=1;i<a->len;i++)if(a->data[i]>m)m=a->data[i];return m;}
static mrb_float sp_FloatArray_sum(sp_FloatArray*a,mrb_float init){mrb_float s=init;for(mrb_int i=0;i<a->len;i++)s+=a->data[i];return s;}
static void sp_FloatArray_replace(sp_FloatArray*dst,sp_FloatArray*src){dst->len=0;if(src->len>dst->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)dst-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_float)*dst->cap;h->size-=sizeof(mrb_float)*dst->cap;void*nd=realloc(dst->data,sizeof(mrb_float)*src->len);if(!nd){perror("realloc");exit(1);}dst->data=(mrb_float*)nd;dst->cap=src->len;h->size+=sizeof(mrb_float)*dst->cap;sp_gc_bytes+=sizeof(mrb_float)*dst->cap;}memcpy(dst->data,src->data,sizeof(mrb_float)*src->len);dst->len=src->len;}
static inline mrb_float sp_FloatArray_pop(sp_FloatArray*a){if(!a||a->len<=0)return 0.0;return a->data[--a->len];}
static inline mrb_float sp_FloatArray_shift(sp_FloatArray*a){if(a->len==0)return 0.0;mrb_float v=a->data[0];for(mrb_int i=0;i+1<a->len;i++)a->data[i]=a->data[i+1];a->len--;return v;}
static inline mrb_int sp_FloatArray_length(sp_FloatArray*a){return a->len;}
static inline mrb_bool sp_FloatArray_empty(sp_FloatArray*a){return a->len==0;}
static inline mrb_float sp_FloatArray_get(sp_FloatArray*a,mrb_int i){if(i<0)i+=a->len;return a->data[i];}
/* a[start, len] / a[start..end] for FloatArray. Same negative-start
 * and length-clamping semantics as sp_IntArray_slice. */
static sp_FloatArray*sp_FloatArray_slice(sp_FloatArray*a,mrb_int start,mrb_int len){if(start<0)start+=a->len;if(start<0)start=0;sp_FloatArray*b=sp_FloatArray_new();if(start>=a->len||len<=0)return b;if(start+len>a->len)len=a->len-start;if(len>b->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)b-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_float)*b->cap;h->size-=sizeof(mrb_float)*b->cap;b->cap=len;b->data=(mrb_float*)realloc(b->data,sizeof(mrb_float)*b->cap);h->size+=sizeof(mrb_float)*b->cap;sp_gc_bytes+=sizeof(mrb_float)*b->cap;}memcpy(b->data,a->data+start,sizeof(mrb_float)*len);b->len=len;return b;}
/* See sp_IntArray_slice_range -- same shape, issue #496. */
static sp_FloatArray*sp_FloatArray_slice_range(sp_FloatArray*a,mrb_int start,mrb_int end_,mrb_int excl){if(end_<0)end_+=a->len;mrb_int n=end_-start+(excl?0:1);if(n<0)n=0;return sp_FloatArray_slice(a,start,n);}
/* Issue #769: no-op for negative index after adjustment. */
static inline void sp_FloatArray_set(sp_FloatArray*a,mrb_int i,mrb_float v){if(!a)return;mrb_int orig=i;if(i<0)i+=a->len;if(i<0)sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)-a->len));while(i>=a->cap){a->cap=a->cap*2+1;a->data=(mrb_float*)realloc(a->data,sizeof(mrb_float)*a->cap);}while(i>=a->len){a->data[a->len]=0.0;a->len++;}a->data[i]=v;}
static void sp_FloatArray_reverse_bang(sp_FloatArray*a){for(mrb_int i=0,j=a->len-1;i<j;i++,j--){mrb_float t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
static void sp_FloatArray_rotate_bang(sp_FloatArray*a,mrb_int n){if(a->len<=0)return;n=((n%a->len)+a->len)%a->len;if(n==0)return;mrb_float*tmp=(mrb_float*)malloc(sizeof(mrb_float)*a->len);for(mrb_int i=0;i<a->len;i++)tmp[i]=a->data[(i+n)%a->len];for(mrb_int i=0;i<a->len;i++)a->data[i]=tmp[i];free(tmp);}
static int _sp_float_cmp(const void*a,const void*b){mrb_float va=*(const mrb_float*)a,vb=*(const mrb_float*)b;return(va>vb)-(va<vb);}
static void sp_FloatArray_sort_bang(sp_FloatArray*a){qsort(a->data,a->len,sizeof(mrb_float),_sp_float_cmp);}
static void sp_FloatArray_shuffle_bang(sp_FloatArray*a){for(mrb_int i=a->len-1;i>0;i--){mrb_int j=(mrb_int)(rand()%(i+1));mrb_float t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
static sp_FloatArray*sp_FloatArray_shuffle(sp_FloatArray*a){sp_FloatArray*r=sp_FloatArray_new();sp_FloatArray_replace(r,a);sp_FloatArray_shuffle_bang(r);return r;}
static mrb_float sp_FloatArray_sample(sp_FloatArray*a){if(a->len<=0)return 0.0;return a->data[(mrb_int)(rand()%a->len)];}
/* IEEE 754 == on mrb_float: NaN never matches; +0.0 == -0.0 (diverges from Float#eql?). */
static mrb_bool sp_FloatArray_include(sp_FloatArray*a,mrb_float v){for(mrb_int i=0;i<a->len;i++)if(a->data[i]==v)return TRUE;return FALSE;}
static sp_FloatArray*sp_FloatArray_intersect(sp_FloatArray*a,sp_FloatArray*b){sp_FloatArray*r=sp_FloatArray_new();for(mrb_int i=0;i<a->len;i++){mrb_float v=a->data[i];if(sp_FloatArray_include(b,v)&&!sp_FloatArray_include(r,v))sp_FloatArray_push(r,v);}return r;}
static sp_FloatArray*sp_FloatArray_union(sp_FloatArray*a,sp_FloatArray*b){sp_FloatArray*r=sp_FloatArray_new();for(mrb_int i=0;i<a->len;i++){mrb_float v=a->data[i];if(!sp_FloatArray_include(r,v))sp_FloatArray_push(r,v);}if(b){for(mrb_int i=0;i<b->len;i++){mrb_float v=b->data[i];if(!sp_FloatArray_include(r,v))sp_FloatArray_push(r,v);}}return r;}
static sp_FloatArray*sp_FloatArray_difference(sp_FloatArray*a,sp_FloatArray*b){sp_FloatArray*r=sp_FloatArray_new();for(mrb_int i=0;i<a->len;i++){mrb_float v=a->data[i];if(!sp_FloatArray_include(b,v))sp_FloatArray_push(r,v);}return r;}

/* ---- PtrArray: array of void* pointers ---- */
typedef struct{void**data;mrb_int len;mrb_int cap;void(*scan_elem)(void*);}sp_PtrArray;
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
static inline void sp_PtrArray_push(sp_PtrArray*a,void*v){if(!a)return;if(a->len>=a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(void*)*a->cap;h->size-=sizeof(void*)*a->cap;a->cap=a->cap*2+1;void*nd=realloc(a->data,sizeof(void*)*a->cap);if(!nd)sp_oom_die();a->data=(void**)nd;h->size+=sizeof(void*)*a->cap;sp_gc_bytes+=sizeof(void*)*a->cap;}a->data[a->len++]=v;}
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
static void sp_PtrArray_rotate_bang(sp_PtrArray*a,mrb_int n){if(a->len<=0)return;n=((n%a->len)+a->len)%a->len;if(n==0)return;void**tmp=(void**)malloc(sizeof(void*)*a->len);for(mrb_int i=0;i<a->len;i++)tmp[i]=a->data[(i+n)%a->len];for(mrb_int i=0;i<a->len;i++)a->data[i]=tmp[i];free(tmp);}
static sp_PtrArray*sp_PtrArray_dup(sp_PtrArray*a){sp_PtrArray*b=sp_PtrArray_new_scan(a->scan_elem);for(mrb_int i=0;i<a->len;i++)sp_PtrArray_push(b,a->data[i]);return b;}
static sp_PtrArray*sp_PtrArray_slice(sp_PtrArray*a,mrb_int start,mrb_int len){if(start<0)start+=a->len;if(start<0)start=0;sp_PtrArray*b=sp_PtrArray_new_scan(a->scan_elem);if(start>=a->len||len<=0)return b;if(start+len>a->len)len=a->len-start;for(mrb_int i=0;i<len;i++)sp_PtrArray_push(b,a->data[start+i]);return b;}
static void sp_PtrArray_shuffle_bang(sp_PtrArray*a){for(mrb_int i=a->len-1;i>0;i--){mrb_int j=(mrb_int)(rand()%(i+1));void*t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
static sp_PtrArray*sp_PtrArray_shuffle(sp_PtrArray*a){sp_PtrArray*b=sp_PtrArray_dup(a);sp_PtrArray_shuffle_bang(b);return b;}
static void *sp_PtrArray_sample(sp_PtrArray*a){if(a->len<=0)return NULL;return a->data[(mrb_int)(rand()%a->len)];}

/* Small-array optimization: keep the first SP_STRARR_INLINE elements
 * inside the struct so empty/short StrArrays skip the data malloc.
 * Common idiom "".split(",") and most class-metadata lists stay small.
 * data == inline_data is the discriminator for "still on inline storage". */
#define SP_STRARR_INLINE 4
typedef struct{const char**data;mrb_int len;mrb_int cap;const char*inline_data[SP_STRARR_INLINE];}sp_StrArray;
static void sp_StrArray_fin(void*p){sp_StrArray*a=(sp_StrArray*)p;if(a->data!=a->inline_data){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(const char*)*a->cap;h->size-=sizeof(const char*)*a->cap;free(a->data);}}
static void sp_StrArray_scan(void*p){sp_StrArray*a=(sp_StrArray*)p;for(mrb_int i=0;i<a->len;i++)sp_mark_string(a->data[i]);}
static sp_StrArray*sp_StrArray_new(void){sp_StrArray*a=(sp_StrArray*)sp_gc_alloc(sizeof(sp_StrArray),sp_StrArray_fin,sp_StrArray_scan);a->cap=SP_STRARR_INLINE;a->data=a->inline_data;a->len=0;return a;}
static inline void sp_StrArray_push(sp_StrArray*a,const char*v){if(a->len>=a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));mrb_int nc=a->cap*2+1;if(a->data==a->inline_data){const char**nd=(const char**)malloc(sizeof(const char*)*nc);if(!nd)sp_oom_die();memcpy(nd,a->data,sizeof(const char*)*a->len);a->data=nd;}else{sp_gc_bytes-=sizeof(const char*)*a->cap;h->size-=sizeof(const char*)*a->cap;void*nd=realloc(a->data,sizeof(const char*)*nc);if(!nd)sp_oom_die();a->data=(const char**)nd;}a->cap=nc;h->size+=sizeof(const char*)*a->cap;sp_gc_bytes+=sizeof(const char*)*a->cap;}a->data[a->len++]=v;}
static void sp_StrArray_replace(sp_StrArray*dst,sp_StrArray*src){dst->len=0;if(src->len>dst->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)dst-sizeof(sp_gc_hdr));void*nd;if(dst->data==dst->inline_data){nd=malloc(sizeof(const char*)*src->len);if(!nd){perror("malloc");exit(1);}}else{sp_gc_bytes-=sizeof(const char*)*dst->cap;h->size-=sizeof(const char*)*dst->cap;nd=realloc(dst->data,sizeof(const char*)*src->len);if(!nd){perror("realloc");exit(1);}}dst->data=(const char**)nd;dst->cap=src->len;h->size+=sizeof(const char*)*dst->cap;sp_gc_bytes+=sizeof(const char*)*dst->cap;}memcpy(dst->data,src->data,sizeof(const char*)*src->len);dst->len=src->len;}
static const char*sp_StrArray_pop(sp_StrArray*a){if(!a||a->len<=0)return NULL;return a->data[--a->len];}
static inline mrb_int sp_StrArray_length(sp_StrArray*a){return a->len;}
static inline mrb_bool sp_StrArray_empty(sp_StrArray*a){return a->len==0;}
static inline const char*sp_StrArray_get(sp_StrArray*a,mrb_int i){if(i<0)i+=a->len;return a->data[i];}
/* a[start, len] / a[start..end] for StrArray. Same negative-start and
 * length-clamping semantics as sp_IntArray_slice. Out-of-bounds start
 * returns an empty StrArray (we don't have a nullable form). */
static sp_StrArray*sp_StrArray_slice(sp_StrArray*a,mrb_int start,mrb_int len){if(start<0)start+=a->len;if(start<0)start=0;sp_StrArray*b=sp_StrArray_new();if(start>=a->len||len<=0)return b;if(start+len>a->len)len=a->len-start;for(mrb_int i=0;i<len;i++)sp_StrArray_push(b,a->data[start+i]);return b;}
/* See sp_IntArray_slice_range -- same shape, issue #496. */
static sp_StrArray*sp_StrArray_slice_range(sp_StrArray*a,mrb_int start,mrb_int end_,mrb_int excl){if(end_<0)end_+=a->len;mrb_int n=end_-start+(excl?0:1);if(n<0)n=0;return sp_StrArray_slice(a,start,n);}
static inline void sp_StrArray_set(sp_StrArray*a,mrb_int i,const char*v){if(!a)return;mrb_int orig=i;if(i<0)i+=a->len;if(i<0)sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)-a->len));while(i>=a->len)sp_StrArray_push(a,sp_str_empty);a->data[i]=v;}
static void sp_StrArray_reverse_bang(sp_StrArray*a){for(mrb_int i=0,j=a->len-1;i<j;i++,j--){const char*t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
static void sp_StrArray_rotate_bang(sp_StrArray*a,mrb_int n){if(a->len<=0)return;n=((n%a->len)+a->len)%a->len;if(n==0)return;const char**tmp=(const char**)malloc(sizeof(const char*)*a->len);for(mrb_int i=0;i<a->len;i++)tmp[i]=a->data[(i+n)%a->len];for(mrb_int i=0;i<a->len;i++)a->data[i]=tmp[i];free(tmp);}
static int _sp_str_cmp(const void*a,const void*b){return strcmp(*(const char*const*)a,*(const char*const*)b);}
static void sp_StrArray_sort_bang(sp_StrArray*a){qsort(a->data,a->len,sizeof(const char*),_sp_str_cmp);}
static const char*sp_StrArray_join(sp_StrArray*a,const char*sep){size_t sl=strlen(sep),cap=256;char*buf=(char*)malloc(cap);size_t len=0;for(mrb_int i=0;i<a->len;i++){if(i>0){if(len+sl>=cap){cap*=2;buf=(char*)realloc(buf,cap);}memcpy(buf+len,sep,sl);len+=sl;}size_t el=strlen(a->data[i]);if(len+el>=cap){cap=(len+el)*2+1;buf=(char*)realloc(buf,cap);}memcpy(buf+len,a->data[i],el);len+=el;}buf[len]=0;char*r=sp_str_alloc(len);memcpy(r,buf,len);free(buf);return r;}
static mrb_bool sp_StrArray_include(sp_StrArray*a,const char*v){for(mrb_int i=0;i<a->len;i++)if(strcmp(a->data[i],v)==0)return TRUE;return FALSE;}
static sp_StrArray*sp_StrArray_intersect(sp_StrArray*a,sp_StrArray*b){sp_StrArray*r=sp_StrArray_new();for(mrb_int i=0;i<a->len;i++){const char*v=a->data[i];if(sp_StrArray_include(b,v)&&!sp_StrArray_include(r,v))sp_StrArray_push(r,v);}return r;}
static sp_StrArray*sp_StrArray_union(sp_StrArray*a,sp_StrArray*b){sp_StrArray*r=sp_StrArray_new();for(mrb_int i=0;i<a->len;i++){const char*v=a->data[i];if(!sp_StrArray_include(r,v))sp_StrArray_push(r,v);}if(b){for(mrb_int i=0;i<b->len;i++){const char*v=b->data[i];if(!sp_StrArray_include(r,v))sp_StrArray_push(r,v);}}return r;}
static sp_StrArray*sp_StrArray_difference(sp_StrArray*a,sp_StrArray*b){sp_StrArray*r=sp_StrArray_new();for(mrb_int i=0;i<a->len;i++){const char*v=a->data[i];if(!sp_StrArray_include(b,v))sp_StrArray_push(r,v);}return r;}
static mrb_int sp_StrArray_index(sp_StrArray*a,const char*v){for(mrb_int i=0;i<a->len;i++)if(strcmp(a->data[i],v)==0)return i;return -1;}
static mrb_int sp_StrArray_rindex(sp_StrArray*a,const char*v){for(mrb_int i=a->len-1;i>=0;i--)if(strcmp(a->data[i],v)==0)return i;return -1;}
static sp_StrArray*sp_StrArray_compact(sp_StrArray*a){sp_StrArray*r=sp_StrArray_new();for(mrb_int i=0;i<a->len;i++)if(a->data[i]!=NULL)sp_StrArray_push(r,a->data[i]);return r;}
static const char*sp_StrArray_delete_at(sp_StrArray*a,mrb_int i){if(i<0)i+=a->len;if(i<0||i>=a->len)return NULL;const char*v=a->data[i];for(mrb_int j=i;j<a->len-1;j++)a->data[j]=a->data[j+1];a->len--;return v;}
static const char*sp_StrArray_delete(sp_StrArray*a,const char*v){mrb_int w=0;const char*found=NULL;for(mrb_int i=0;i<a->len;i++){if(strcmp(a->data[i],v)!=0){a->data[w]=a->data[i];w++;}else{found=a->data[i];}}a->len=w;return found;}
static void sp_StrArray_insert(sp_StrArray*a,mrb_int i,const char*v){if(i<0)i+=a->len+1;sp_StrArray_push(a,sp_str_empty);for(mrb_int j=a->len-1;j>i;j--)a->data[j]=a->data[j-1];a->data[i]=v;}
static void sp_StrArray_shuffle_bang(sp_StrArray*a){for(mrb_int i=a->len-1;i>0;i--){mrb_int j=(mrb_int)(rand()%(i+1));const char*t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
static sp_StrArray*sp_StrArray_shuffle(sp_StrArray*a){sp_StrArray*r=sp_StrArray_new();sp_StrArray_replace(r,a);sp_StrArray_shuffle_bang(r);return r;}
static const char *sp_StrArray_sample(sp_StrArray*a){if(a->len<=0)return sp_str_empty;return a->data[(mrb_int)(rand()%a->len)];}

static inline uint64_t sp_str_hash(const char*s){uint64_t h=14695981039346656037ULL;while(*s){h^=(unsigned char)*s++;h*=1099511628211ULL;}return h;}
typedef struct{const char**keys;mrb_int*vals;const char**order;mrb_int len;mrb_int cap;mrb_int mask;mrb_int default_v;}sp_StrIntHash;
static void sp_StrIntHash_fin(void*p){sp_StrIntHash*h=(sp_StrIntHash*)p;free(h->keys);free(h->vals);free(h->order);}
static void sp_StrIntHash_scan(void*p){sp_StrIntHash*h=(sp_StrIntHash*)p;for(mrb_int i=0;i<h->cap;i++){if(h->keys[i])sp_mark_string(h->keys[i]);}}
static sp_StrIntHash*sp_StrIntHash_new(void){sp_StrIntHash*h=(sp_StrIntHash*)sp_gc_alloc(sizeof(sp_StrIntHash),sp_StrIntHash_fin,sp_StrIntHash_scan);h->cap=16;h->mask=15;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(const char**)malloc(sizeof(const char*)*h->cap);h->len=0;h->default_v=0;return h;}
static sp_StrIntHash*sp_StrIntHash_new_with_default(mrb_int d){sp_StrIntHash*h=sp_StrIntHash_new();h->default_v=d;return h;}
static void sp_StrIntHash_grow(sp_StrIntHash*h){mrb_int oc=h->cap;const char**ok=h->keys;mrb_int*ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(const char**)realloc(h->order,sizeof(const char*)*h->cap);mrb_int ol=h->len;h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]){mrb_int idx=(mrb_int)(sp_str_hash(ok[i])&h->mask);while(h->keys[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}
static mrb_int sp_StrIntHash_get(sp_StrIntHash*h,const char*k){if(!h)return 0;mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
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

typedef struct{const char**keys;const char**vals;const char**order;mrb_int len;mrb_int cap;mrb_int mask;const char*default_v;}sp_StrStrHash;
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

typedef struct{mrb_int*keys;const char**vals;mrb_int*order;mrb_bool*used;mrb_int len;mrb_int cap;mrb_int mask;const char*default_v;}sp_IntStrHash;
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
typedef struct{mrb_int*keys;mrb_int*vals;mrb_int*order;mrb_bool*used;mrb_int len;mrb_int cap;mrb_int mask;mrb_int default_v;}sp_IntIntHash;
static void sp_IntIntHash_fin(void*p){sp_IntIntHash*h=(sp_IntIntHash*)p;free(h->keys);free(h->vals);free(h->order);free(h->used);}
static sp_IntIntHash*sp_IntIntHash_new(void){sp_IntIntHash*h=(sp_IntIntHash*)sp_gc_alloc(sizeof(sp_IntIntHash),sp_IntIntHash_fin,NULL);h->cap=16;h->mask=15;h->keys=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(mrb_int*)malloc(sizeof(mrb_int)*h->cap);h->used=(mrb_bool*)calloc(h->cap,sizeof(mrb_bool));h->len=0;h->default_v=0;return h;}
static sp_IntIntHash*sp_IntIntHash_new_with_default(mrb_int d){sp_IntIntHash*h=sp_IntIntHash_new();h->default_v=d;return h;}
static void sp_IntIntHash_grow(sp_IntIntHash*h){mrb_int oc=h->cap,ol=h->len;mrb_int*ok=h->keys;mrb_int*ov=h->vals;mrb_bool*ou=h->used;mrb_int*oo=h->order;h->cap*=2;h->mask=h->cap-1;h->keys=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(mrb_int*)malloc(sizeof(mrb_int)*h->cap);h->used=(mrb_bool*)calloc(h->cap,sizeof(mrb_bool));h->len=ol;for(mrb_int i=0;i<oc;i++){if(!ou[i])continue;mrb_int k=ok[i];mrb_int v=ov[i];mrb_int di=_sp_istr_idx(h->mask,k);while(h->used[di])di=(di+1)&h->mask;h->used[di]=TRUE;h->keys[di]=k;h->vals[di]=v;}for(mrb_int i=0;i<ol;i++)h->order[i]=oo[i];free(ok);free(ov);free(ou);free(oo);}
static void sp_IntIntHash_set(sp_IntIntHash*h,mrb_int k,mrb_int v){if(h->len*2>=h->cap)sp_IntIntHash_grow(h);mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->used[idx]=TRUE;h->keys[idx]=k;h->vals[idx]=v;h->order[h->len++]=k;}
static mrb_int sp_IntIntHash_get(sp_IntIntHash*h,mrb_int k){if(!h)return 0;mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
static mrb_bool sp_IntIntHash_has_key(sp_IntIntHash*h,mrb_int k){mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_int sp_IntIntHash_length(sp_IntIntHash*h){return h?h->len:0;}
static sp_IntIntHash*sp_IntIntHash_dup(sp_IntIntHash*h){sp_IntIntHash*r=sp_IntIntHash_new();r->default_v=h->default_v;for(mrb_int i=0;i<h->len;i++)sp_IntIntHash_set(r,h->order[i],sp_IntIntHash_get(h,h->order[i]));return r;}
/* Array#tally on int_array. CRuby returns an Integer-keyed Hash
   mapping each distinct element to its occurrence count. */
static sp_IntIntHash*sp_IntArray_tally_int(sp_IntArray*a){sp_IntIntHash*h=sp_IntIntHash_new();if(!a)return h;for(mrb_int i=0;i<a->len;i++){mrb_int k=a->data[a->start+i];sp_IntIntHash_set(h,k,sp_IntIntHash_get(h,k)+1);}return h;}

/* Reuse an existing StrArray for split, avoiding GC alloc.
   Clears a->len and refills.  Substring strings are still malloc'd. */
static void sp_str_split_into(sp_StrArray*a,const char*s,const char*sep){
  a->len=0;if(*s==0)return;size_t sl=strlen(sep);
  if(sl==0){const char*p=s;while(*p){int cn=sp_utf8_advance(p);char*c=sp_str_alloc_raw(cn+1);memcpy(c,p,cn);c[cn]=0;sp_StrArray_push(a,c);p+=cn;}return;}
  const char*p=s;while(1){const char*f=strstr(p,sep);if(!f){char*r=sp_str_alloc_raw(strlen(p)+1);strcpy(r,p);sp_StrArray_push(a,r);break;}
  size_t n=f-p;char*r=sp_str_alloc_raw(n+1);memcpy(r,p,n);r[n]=0;sp_StrArray_push(a,r);p=f+sl;}}
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
static const char*sp_str_strip(const char*s){if(!s)return sp_str_empty;while(*s&&isspace((unsigned char)*s))s++;size_t l=strlen(s);while(l>0&&isspace((unsigned char)s[l-1]))l--;char*r=sp_str_alloc_raw(l+1);memcpy(r,s,l);r[l]=0;return r;}
static const char*sp_str_chomp(const char*s){if(!s)return sp_str_empty;size_t l=strlen(s);while(l>0&&(s[l-1]=='\n'||s[l-1]=='\r'))l--;char*r=sp_str_alloc_raw(l+1);memcpy(r,s,l);r[l]=0;return r;}

/* Issue #881: `"hello!".chomp("!")` strips the explicit separator.
   Empty sep strips any trailing newlines (CRuby paragraph mode).
   NULL sep is caller's responsibility (codegen routes nil to a
   no-op before calling). */
static const char *sp_str_chomp_sep(const char *s, const char *sep) {
  if (!s) return sp_str_empty;
  size_t l = strlen(s);
  if (!sep || !*sep) {
    /* Empty sep = strip all trailing \n / \r\n / \r repeatedly. */
    while (l > 0 && (s[l-1] == '\n' || s[l-1] == '\r')) l--;
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
static mrb_bool sp_str_include(const char*s,const char*sub){if(!s||!sub)return FALSE;return strstr(s,sub)!=NULL;}
static mrb_bool sp_str_start_with(const char*s,const char*p){if(!s||!p)return FALSE;return strncmp(s,p,strlen(p))==0;}
static mrb_bool sp_str_end_with(const char*s,const char*suf){if(!s||!suf)return FALSE;size_t ls=strlen(s),lsuf=strlen(suf);if(lsuf>ls)return FALSE;return strcmp(s+ls-lsuf,suf)==0;}
static sp_StrArray*sp_str_split(const char*s,const char*sep){sp_StrArray*a=sp_StrArray_new();if(*s==0)return a;size_t sl=strlen(sep);if(sl==0){const char*p=s;while(*p){int cn=sp_utf8_advance(p);char*c=sp_str_alloc_raw(cn+1);memcpy(c,p,cn);c[cn]=0;sp_StrArray_push(a,c);p+=cn;}return a;}const char*p=s;while(1){const char*f=strstr(p,sep);if(!f){char*r=sp_str_alloc_raw(strlen(p)+1);strcpy(r,p);sp_StrArray_push(a,r);break;}size_t n=f-p;char*r=sp_str_alloc_raw(n+1);memcpy(r,p,n);r[n]=0;sp_StrArray_push(a,r);p=f+sl;}return a;}
/* `s.split(sep, n)` with explicit limit. Positive n caps the result
   at n elements: the last element holds the unsplit remainder.
   n <= 0 (CRuby's "no limit" or "keep trailing empties") falls back
   to the plain split. Empty separator works the same as the no-limit
   path -- splits into Unicode characters; the limit caps the array.
   Issue #619 puzzle 2. */
static sp_StrArray*sp_str_split_limit(const char*s,const char*sep,mrb_int n){if(n<=0)return sp_str_split(s,sep);sp_StrArray*a=sp_StrArray_new();if(*s==0)return a;size_t sl=strlen(sep);if(sl==0){const char*p=s;mrb_int k=0;while(*p&&k<n-1){int cn=sp_utf8_advance(p);char*c=sp_str_alloc_raw(cn+1);memcpy(c,p,cn);c[cn]=0;sp_StrArray_push(a,c);p+=cn;k++;}if(*p){char*r=sp_str_alloc_raw(strlen(p)+1);strcpy(r,p);sp_StrArray_push(a,r);}return a;}const char*p=s;mrb_int k=0;while(k<n-1){const char*f=strstr(p,sep);if(!f)break;size_t m=f-p;char*r=sp_str_alloc_raw(m+1);memcpy(r,p,m);r[m]=0;sp_StrArray_push(a,r);p=f+sl;k++;}char*r=sp_str_alloc_raw(strlen(p)+1);strcpy(r,p);sp_StrArray_push(a,r);return a;}
/* `s.split` / `s.split(nil)` -- whitespace mode: split on runs of
   ASCII whitespace, skip leading whitespace. Issue #507: the no-arg
   form previously emitted `sp_str_split(s, 0)` and segfaulted at
   strlen(NULL). */
static sp_StrArray*sp_str_split_ws(const char*s){sp_StrArray*a=sp_StrArray_new();const char*p=s;while(*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p=='\f'||*p=='\v')p++;while(*p){const char*start=p;while(*p&&!(*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p=='\f'||*p=='\v'))p++;size_t n=p-start;char*r=sp_str_alloc_raw(n+1);memcpy(r,start,n);r[n]=0;sp_StrArray_push(a,r);while(*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p=='\f'||*p=='\v')p++;}return a;}
/* String#lines: split on \n but PRESERVE the trailing newline on each
   line (CRuby semantics). The last line keeps its terminator if present;
   if absent, it just stops there. Empty string returns an empty array.
   `end` is computed once at entry so a string with no newlines avoids
   a redundant strlen call on the trailing piece. */
static sp_StrArray*sp_str_lines(const char*s){sp_StrArray*a=sp_StrArray_new();if(*s==0)return a;const char*end=s+strlen(s);const char*p=s;while(p<end){const char*nl=strchr(p,'\n');size_t n=nl?(size_t)(nl-p+1):(size_t)(end-p);char*r=sp_str_alloc_raw(n+1);memcpy(r,p,n);r[n]=0;sp_StrArray_push(a,r);if(!nl)break;p=nl+1;}return a;}
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
static mrb_int sp_str_rindex(const char*s,const char*sub){if(!sub)return -1;size_t sl=strlen(sub);if(sl==0)return sp_str_length(s);const char*last=NULL;const char*p=s;while((p=strstr(p,sub))){last=p;p++;}if(!last)return -1;mrb_int n=0;const char*q=s;while(q<last){q+=sp_utf8_advance(q);n++;}return n;}
static char sp_char_cache[256][3];
static int sp_char_cache_init = 0;
/* start/len are codepoint indices/counts. */
static const char*sp_str_sub_range(const char*s,mrb_int start,mrb_int len){mrb_int cl=sp_str_length(s);if(start<0)start+=cl;if(start<0)start=0;if(start>=cl||len<=0){return &("\xff" "")[1];}if(start+len>cl)len=cl-start;size_t boff=sp_utf8_byte_offset(s,start);size_t blen_total=sp_str_byte_len(s);size_t bp=boff;mrb_int rem=len;while(rem>0&&bp<blen_total){bp+=sp_utf8_advance(s+bp);rem--;}if(bp>blen_total)bp=blen_total;size_t bend=bp;size_t blen=bend-boff;if(len==1&&blen==1){unsigned char c=(unsigned char)s[boff];if(!sp_char_cache_init){for(int i=0;i<256;i++){sp_char_cache[i][0]=(char)0xff;sp_char_cache[i][1]=(char)i;sp_char_cache[i][2]=0;}sp_char_cache_init=1;}return &sp_char_cache[c][1];}char*r=sp_str_alloc_raw(blen+1);memcpy(r,s+boff,blen);r[blen]=0;return r;}
/* Single-character form of `s[i]`. Returns NULL on out-of-bounds to
   match CRuby's `"hello"[20] -> nil`. The two-arg `s[i, len]` /
   range forms keep returning "" on OOB via sp_str_sub_range; only
   the bare single-int index aliases here. Issue #619 puzzle 3. */
static const char*sp_str_char_at_or_nil(const char*s,mrb_int i){mrb_int cl=sp_str_length(s);if(i<0)i+=cl;if(i<0||i>=cl)return NULL;return sp_str_sub_range(s,i,1);}
/* Char-indexed variant; the second arg used to be a hoisted byte length, now a
   hoisted codepoint count.  We don't need it for correctness, but keeping the
   ABI lets callers pass it without a wrapper. */
static const char*sp_str_sub_range_len(const char*s,mrb_int cl,mrb_int start,mrb_int len){if(start<0)start+=cl;if(start<0)start=0;if(start>=cl||len<=0){return &("\xff" "")[1];}if(start+len>cl)len=cl-start;size_t boff=sp_utf8_byte_offset(s,start);size_t blen_total=sp_str_byte_len(s);size_t bp=boff;mrb_int rem=len;while(rem>0&&bp<blen_total){bp+=sp_utf8_advance(s+bp);rem--;}if(bp>blen_total)bp=blen_total;size_t bend=bp;size_t blen=bend-boff;if(len==1&&blen==1){unsigned char c=(unsigned char)s[boff];if(!sp_char_cache_init){for(int i=0;i<256;i++){sp_char_cache[i][0]=(char)0xff;sp_char_cache[i][1]=(char)i;sp_char_cache[i][2]=0;}sp_char_cache_init=1;}return &sp_char_cache[c][1];}char*r=sp_str_alloc_raw(blen+1);memcpy(r,s+boff,blen);r[blen]=0;return r;}
/* String s[start..end] / s[start...end] with possibly negative
   endpoints. Mirrors sp_IntArray_slice_range; issue #496. */
static const char*sp_str_sub_range_r(const char*s,mrb_int start,mrb_int end_,mrb_int excl){mrb_int cl=sp_str_length(s);if(end_<0)end_+=cl;mrb_int n=end_-start+(excl?0:1);if(n<0)n=0;return sp_str_sub_range_len(s,cl,start,n);}
static const char*sp_str_sub_range_len_r(const char*s,mrb_int cl,mrb_int start,mrb_int end_,mrb_int excl){if(end_<0)end_+=cl;mrb_int n=end_-start+(excl?0:1);if(n<0)n=0;return sp_str_sub_range_len(s,cl,start,n);}
static const char*sp_sprintf(const char*fmt,...){char _sp_tmp[4096];va_list ap;va_start(ap,fmt);int _sp_n=vsnprintf(_sp_tmp,sizeof(_sp_tmp),fmt,ap);va_end(ap);if(_sp_n<0)_sp_n=0;if(_sp_n>=(int)sizeof(_sp_tmp))_sp_n=(int)sizeof(_sp_tmp)-1;char*b=sp_str_alloc(_sp_n);memcpy(b,_sp_tmp,_sp_n);return b;}
/* Use a temp pointer for realloc so the original buffer is not leaked
   on allocation failure. Match the perror+exit pattern used elsewhere
   (see sp_IntArray_replace) instead of returning a partial result. */
static const char*sp_str_format_strarr(const char*fmt,sp_StrArray*a){size_t cap=strlen(fmt)+64;char*buf=(char*)malloc(cap);if(!buf){perror("malloc");exit(1);}size_t out=0;mrb_int idx=0;const char*p=fmt;while(*p){if(*p=='%'){if(p[1]=='s'){const char*s=(idx<a->len)?a->data[idx]:"";size_t sl=strlen(s);if(out+sl>=cap){size_t nc=(out+sl)*2+1;char*nb=(char*)realloc(buf,nc);if(!nb){free(buf);perror("realloc");exit(1);}buf=nb;cap=nc;}memcpy(buf+out,s,sl);out+=sl;idx++;p+=2;}else if(p[1]=='%'){if(out+1>=cap){size_t nc=cap*2;char*nb=(char*)realloc(buf,nc);if(!nb){free(buf);perror("realloc");exit(1);}buf=nb;cap=nc;}buf[out++]='%';p+=2;}else{if(out+1>=cap){size_t nc=cap*2;char*nb=(char*)realloc(buf,nc);if(!nb){free(buf);perror("realloc");exit(1);}buf=nb;cap=nc;}buf[out++]=*p++;}}else{if(out+1>=cap){size_t nc=cap*2;char*nb=(char*)realloc(buf,nc);if(!nb){free(buf);perror("realloc");exit(1);}buf=nb;cap=nc;}buf[out++]=*p++;}}buf[out]=0;char*r=sp_str_alloc(out);memcpy(r,buf,out);free(buf);return r;}

static const char*sp_str_reverse(const char*s){if(!s)return sp_str_empty;size_t bl=strlen(s);char*r=sp_str_alloc_raw(bl+1);size_t end=bl;const char*p=s;while(*p){int cn=sp_utf8_advance(p);end-=cn;memcpy(r+end,p,cn);p+=cn;}r[bl]=0;return r;}
static const char*sp_str_sub(const char*s,const char*pat,const char*rep){if(!s)return sp_str_empty;if(!pat||!rep)return s;const char*f=strstr(s,pat);if(!f)return s;size_t pl=strlen(pat),rl=strlen(rep),sl=strlen(s);char*r=sp_str_alloc_raw(sl-pl+rl+1);size_t n=f-s;memcpy(r,s,n);memcpy(r+n,rep,rl);memcpy(r+n+rl,f+pl,sl-n-pl+1);return r;}
static const char*sp_str_capitalize(const char*s){if(!s)return sp_str_empty;size_t l=strlen(s);char*r=sp_str_alloc_raw(l+1);for(size_t i=0;i<=l;i++)r[i]=tolower((unsigned char)s[i]);if(l>0)r[0]=toupper((unsigned char)r[0]);return r;}
static mrb_int sp_str_count(const char*s,const char*chars){if(!chars)return 0;size_t setn;uint32_t*set=sp_utf8_decode_charset(chars,&setn);mrb_int c=0;const char*p=s;while(*p){uint32_t cp;p+=sp_utf8_decode(p,&cp);if(sp_utf8_set_has(set,setn,cp))c++;}free(set);return c;}
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
static const char*sp_str_tr(const char*s,const char*from,const char*to){if(!s)return sp_str_empty;if(!from||!to)return s;size_t fn,tn;uint32_t*fcps=sp_utf8_decode_charset(from,&fn);uint32_t*tcps=sp_utf8_decode_charset(to,&tn);size_t bl=strlen(s);size_t cap=bl*4+1;char*buf=(char*)malloc(cap);size_t n=0;const char*p=s;while(*p){uint32_t cp;int cn=sp_utf8_decode(p,&cp);size_t mi=fn;for(size_t j=0;j<fn;j++)if(fcps[j]==cp){mi=j;break;}if(mi<fn&&tn>0){uint32_t rep=mi<tn?tcps[mi]:tcps[tn-1];n+=sp_utf8_encode(rep,buf+n);}else{memcpy(buf+n,p,cn);n+=cn;}p+=cn;}buf[n]=0;char*r=sp_str_alloc(n);memcpy(r,buf,n+1);free(buf);free(fcps);free(tcps);return r;}
/* Issue #902: String#tr_s -- translate AND squeeze consecutive
   identical results into one. Walks codepoint-by-codepoint and
   collapses adjacent duplicates only among the translated bytes
   (untranslated runs keep their original characters). */
static const char*sp_str_tr_s(const char*s,const char*from,const char*to){
  if(!s)return sp_str_empty;
  if(!from||!to)return s;
  size_t fn,tn;
  uint32_t*fcps=sp_utf8_decode_charset(from,&fn);
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
    uint32_t emit_cp;
    int translated=0;
    if(mi<fn&&tn>0){
      emit_cp=mi<tn?tcps[mi]:tcps[tn-1];
      translated=1;
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
static const char*sp_str_delete(const char*s,const char*chars){if(!s)return sp_str_empty;if(!chars)return s;size_t setn;uint32_t*set=sp_utf8_decode_charset(chars,&setn);size_t bl=strlen(s);char*r=sp_str_alloc_raw(bl+1);size_t n=0;const char*p=s;while(*p){uint32_t cp;int cn=sp_utf8_decode(p,&cp);if(!sp_utf8_set_has(set,setn,cp)){memcpy(r+n,p,cn);n+=cn;}p+=cn;}r[n]=0;sp_str_set_len(r,n);free(set);return r;}
/* Issue #921: shrink the heap-string header length to match the
   squeezed payload — the alloc gives bl+1 bytes, the squeezed
   write fills n<=bl, leaving the header's stored length stale.
   `bytes` / `length` consult the header (not strlen), so callers
   would see the alloc size and trailing NULs. */
static const char*sp_str_squeeze(const char*s){if(!s)return sp_str_empty;size_t bl=strlen(s);char*r=sp_str_alloc_raw(bl+1);size_t n=0;uint32_t prev=0xFFFFFFFFu;const char*p=s;while(*p){uint32_t cp;int cn=sp_utf8_decode(p,&cp);if(cp!=prev){memcpy(r+n,p,cn);n+=cn;prev=cp;}p+=cn;}r[n]=0;sp_str_set_len(r,n);return r;}

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
  char *out = sp_str_alloc_raw(cap);
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
  out[olen] = 0;
  sp_str_set_len(out, olen);
  return out;
}
static const char*sp_str_ljust(const char*s,mrb_int w){if(!s)return sp_str_empty;mrb_int cl=sp_str_length(s);if(cl>=w)return s;size_t bl=strlen(s);size_t pad=(size_t)(w-cl);char*r=sp_str_alloc_raw(bl+pad+1);memcpy(r,s,bl);memset(r+bl,' ',pad);r[bl+pad]=0;return r;}
static const char*sp_str_rjust(const char*s,mrb_int w){if(!s)return sp_str_empty;mrb_int cl=sp_str_length(s);if(cl>=w)return s;size_t bl=strlen(s);size_t pad=(size_t)(w-cl);char*r=sp_str_alloc_raw(bl+pad+1);memset(r,' ',pad);memcpy(r+pad,s,bl);r[bl+pad]=0;return r;}
static const char*sp_str_center(const char*s,mrb_int w){if(!s)return sp_str_empty;mrb_int cl=sp_str_length(s);if(cl>=w)return s;size_t bl=strlen(s);mrb_int pad=w-cl;mrb_int left=pad/2;mrb_int right=pad-left;char*r=sp_str_alloc_raw(bl+pad+1);memset(r,' ',left);memcpy(r+left,s,bl);memset(r+left+bl,' ',right);r[bl+pad]=0;return r;}
static const char*sp_str_ljust2(const char*s,mrb_int w,const char*pad){mrb_int cl=sp_str_length(s);if(cl>=w)return s;size_t bl=strlen(s);size_t pn;uint32_t*pcps=sp_utf8_decode_all(pad,&pn);if(pn==0){free(pcps);char*r=sp_str_alloc_raw(bl+1);memcpy(r,s,bl+1);return r;}mrb_int need=w-cl;size_t padb=0;for(mrb_int i=0;i<need;i++){char tmp[4];padb+=sp_utf8_encode(pcps[i%pn],tmp);}char*r=sp_str_alloc_raw(bl+padb+1);memcpy(r,s,bl);size_t n=bl;for(mrb_int i=0;i<need;i++)n+=sp_utf8_encode(pcps[i%pn],r+n);r[n]=0;free(pcps);return r;}
static const char*sp_str_rjust2(const char*s,mrb_int w,const char*pad){mrb_int cl=sp_str_length(s);if(cl>=w)return s;size_t bl=strlen(s);size_t pn;uint32_t*pcps=sp_utf8_decode_all(pad,&pn);if(pn==0){free(pcps);char*r=sp_str_alloc_raw(bl+1);memcpy(r,s,bl+1);return r;}mrb_int need=w-cl;size_t padb=0;for(mrb_int i=0;i<need;i++){char tmp[4];padb+=sp_utf8_encode(pcps[i%pn],tmp);}char*r=sp_str_alloc_raw(bl+padb+1);size_t n=0;for(mrb_int i=0;i<need;i++)n+=sp_utf8_encode(pcps[i%pn],r+n);memcpy(r+n,s,bl);r[n+bl]=0;free(pcps);return r;}
static const char*sp_str_center2(const char*s,mrb_int w,const char*pad){mrb_int cl=sp_str_length(s);if(cl>=w)return s;size_t bl=strlen(s);size_t pn;uint32_t*pcps=sp_utf8_decode_all(pad,&pn);if(pn==0){free(pcps);char*r=sp_str_alloc_raw(bl+1);memcpy(r,s,bl+1);return r;}mrb_int pd=w-cl;mrb_int left=pd/2;mrb_int right=pd-left;size_t leftb=0,rightb=0;{char tmp[4];for(mrb_int i=0;i<left;i++)leftb+=sp_utf8_encode(pcps[i%pn],tmp);for(mrb_int i=0;i<right;i++)rightb+=sp_utf8_encode(pcps[i%pn],tmp);}char*r=sp_str_alloc_raw(leftb+bl+rightb+1);size_t n=0;for(mrb_int i=0;i<left;i++)n+=sp_utf8_encode(pcps[i%pn],r+n);memcpy(r+n,s,bl);n+=bl;for(mrb_int i=0;i<right;i++)n+=sp_utf8_encode(pcps[i%pn],r+n);r[n]=0;free(pcps);return r;}
static const char*sp_str_lstrip(const char*s){if(!s)return sp_str_empty;while(*s&&isspace((unsigned char)*s))s++;char*r=sp_str_alloc_raw(strlen(s)+1);strcpy(r,s);return r;}
static const char*sp_str_rstrip(const char*s){if(!s)return sp_str_empty;size_t l=strlen(s);while(l>0&&isspace((unsigned char)s[l-1]))l--;char*r=sp_str_alloc_raw(l+1);memcpy(r,s,l);r[l]=0;return r;}
static const char*sp_str_dup(const char*s){char*r=sp_str_alloc_raw(strlen(s)+1);strcpy(r,s);return r;}

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
static inline void sp_String_append(sp_String*s,const char*t){if(!s||!t)return;int64_t tl=(int64_t)strlen(t);if(s->len+tl>=s->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)s-sizeof(sp_gc_hdr));int64_t new_cap=(s->len+tl)*2+16;char*raw=(char*)realloc(s->data-1,new_cap+2);if(!raw)return;sp_gc_bytes-=s->cap+2;h->size-=s->cap+2;s->cap=new_cap;raw[0]=(char)0xfd;s->data=raw+1;h->size+=s->cap+2;sp_gc_bytes+=s->cap+2;}memcpy(s->data+s->len,t,tl+1);s->len+=tl;}
static inline void sp_String_prepend(sp_String*s,const char*t){if(!s||!t)return;int64_t tl=(int64_t)strlen(t);if(s->len+tl>=s->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)s-sizeof(sp_gc_hdr));int64_t new_cap=(s->len+tl)*2+16;char*raw=(char*)realloc(s->data-1,new_cap+2);if(!raw)return;sp_gc_bytes-=s->cap+2;h->size-=s->cap+2;s->cap=new_cap;raw[0]=(char)0xfd;s->data=raw+1;h->size+=s->cap+2;sp_gc_bytes+=s->cap+2;}memmove(s->data+tl,s->data,s->len+1);memcpy(s->data,t,tl);s->len+=tl;}
/* Issue #741: String#insert(idx, str) -- insert str at idx. Negative
   idx is relative to len+1 (insert before tail). */
static inline void sp_String_insert(sp_String*s,int64_t idx,const char*t){if(!s||!t)return;int64_t tl=(int64_t)strlen(t);if(tl==0)return;if(idx<0)idx+=s->len+1;if(idx<0)idx=0;if(idx>s->len)idx=s->len;if(s->len+tl>=s->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)s-sizeof(sp_gc_hdr));int64_t new_cap=(s->len+tl)*2+16;char*raw=(char*)realloc(s->data-1,new_cap+2);if(!raw)return;sp_gc_bytes-=s->cap+2;h->size-=s->cap+2;s->cap=new_cap;raw[0]=(char)0xfd;s->data=raw+1;h->size+=s->cap+2;sp_gc_bytes+=s->cap+2;}memmove(s->data+idx+tl,s->data+idx,s->len-idx+1);memcpy(s->data+idx,t,tl);s->len+=tl;}
/* Issue #740/#741 sibling: String#replace(s) -- replace entire content. */
static inline void sp_String_replace(sp_String*s,const char*t){if(!s||!t)return;int64_t tl=(int64_t)strlen(t);if(tl>=s->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)s-sizeof(sp_gc_hdr));int64_t new_cap=tl*2+16;char*raw=(char*)realloc(s->data-1,new_cap+2);if(!raw)return;sp_gc_bytes-=s->cap+2;h->size-=s->cap+2;s->cap=new_cap;raw[0]=(char)0xfd;s->data=raw+1;h->size+=s->cap+2;sp_gc_bytes+=s->cap+2;}memcpy(s->data,t,tl);s->data[tl]='\0';s->len=tl;}
static inline const char*sp_String_cstr(sp_String*s){return s->data;}
static inline int64_t sp_String_length(sp_String*s){return s->len;}
static sp_String*sp_String_dup(sp_String*s){return sp_String_new(s->data);}
/* Array#inspect for each typed array: `[elem1, elem2, ...]` with each
   element rendered via its own primitive inspect. Matches CRuby's
   Array#inspect output byte-for-byte. Returns a GC-managed C string. */
static const char*sp_IntArray_inspect(sp_IntArray*a){sp_String*s=sp_String_new("[");for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_int_to_s(a->data[a->start+i]));}sp_String_append(s,"]");return s->data;}
static const char*sp_FloatArray_inspect(sp_FloatArray*a){sp_String*s=sp_String_new("[");for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_float_inspect(a->data[i]));}sp_String_append(s,"]");return s->data;}
static mrb_bool sp_FloatArray_eq(sp_FloatArray*a,sp_FloatArray*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++)if(a->data[i]!=b->data[i])return FALSE;return TRUE;}
static const char*sp_StrArray_inspect(sp_StrArray*a){sp_String*s=sp_String_new("[");for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_str_inspect(a->data[i]));}sp_String_append(s,"]");return s->data;}
static mrb_bool sp_StrArray_eq(sp_StrArray*a,sp_StrArray*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++)if(!sp_str_eq(a->data[i],b->data[i]))return FALSE;return TRUE;}
/* Symbol arrays share the IntArray representation (sp_sym = mrb_int),
   but each element is rendered as ":name" via sp_sym_to_s. */
static inline const char*sp_SymArray_inspect(sp_IntArray*a){sp_String*s=sp_String_new("[");for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,":");sp_String_append(s,sp_sym_to_s((sp_sym)a->data[a->start+i]));}sp_String_append(s,"]");return s->data;}
/* PtrArray elements are object pointers without a per-element class
   tag, so we render them as `#<Object>` rather than recursing. */
static const char*sp_PtrArray_inspect(sp_PtrArray*a){sp_String*s=sp_String_new("[");for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,"#<Object>");}sp_String_append(s,"]");return s->data;}
/* Issue #851: Hash#inspect for typed-hash variants beyond
   sym_int_hash. Renders Ruby's `{"k"=>v, ...}` (string keys),
   `{42=>"v", ...}` (int keys), or `{:k=>v, ...}` (sym keys but
   non-int value, since the bare `k: v` shorthand only applies
   when values are inspectable as one-liners — match CRuby). */
static const char*sp_StrIntHash_inspect(sp_StrIntHash*h){sp_String*s=sp_String_new("{");if(h){for(mrb_int i=0;i<h->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_str_inspect(h->order[i]));sp_String_append(s,"=>");sp_String_append(s,sp_int_to_s(sp_StrIntHash_get(h,h->order[i])));}}sp_String_append(s,"}");return s->data;}
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
static void sp_mark_fiber_root_storage(void);
static void sp_re_mark_globals(void) {
  sp_mark_string(sp_re_last_str);
  for (int i = 0; i < 10; i++) sp_mark_string(sp_re_captures[i]);
  sp_mark_string(sp_re_match_str);
  sp_mark_string(sp_re_match_pre);
  sp_mark_string(sp_re_match_post);
  for (mrb_int i = 0; i < sp_argv.len; i++) sp_mark_string(sp_argv.data[i]);
  sp_mark_in_flight_exceptions();
  sp_mark_fiber_root_storage();
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
  char *out = sp_str_alloc_raw(cap); size_t olen = 0;
  int64_t pos = 0; int caps[64];
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 64);
    if (n <= 0 || caps[0] < 0) break;
    size_t before = caps[0] - pos;
    if (olen+before+rlen >= cap) { cap = (olen+before+rlen)*2+64; out = (char*)realloc(out, cap); }
    memcpy(out+olen, str+pos, before); olen += before;
    sp_re_expand_rep(&out, &olen, &cap, rep, rlen, str, caps, n);
    pos = caps[1]; if (caps[0] == caps[1]) pos++;
  }
  size_t rest = slen - pos;
  if (olen+rest+1 >= cap) { cap = olen+rest+64; out = (char*)realloc(out, cap); }
  memcpy(out+olen, str+pos, rest); olen += rest;
  out[olen] = 0; return out;
}

/* String#gsub(regex, hash) — per-match hash lookup form. CRuby's
 * semantics: each matched substring is looked up as a key in the
 * hash; the value (if present) is the replacement, otherwise the
 * matched substring is dropped (CRuby returns "", not the match).
 * Used by html_escape / json_escape idioms (gsub(/[&<>]/, ESCAPES)). */
static const char *sp_re_gsub_str_str_hash(mrb_regexp_pattern *pat, const char *str, sp_StrStrHash *h) {
  int64_t slen = (int64_t)strlen(str);
  size_t cap = slen * 2 + 64; char *out = sp_str_alloc_raw(cap); size_t olen = 0;
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
    pos = caps[1]; if (caps[0] == caps[1]) pos++;
  }
  size_t rest = slen - pos;
  if (olen + rest >= cap) { cap = olen + rest + 1; out = (char *)realloc(out, cap); }
  memcpy(out + olen, str + pos, rest); olen += rest;
  out[olen] = 0; return out;
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
  char *out = sp_str_alloc_raw(cap);
  memcpy(out, str, caps[0]);
  size_t olen = caps[0];
  sp_re_expand_rep(&out, &olen, &cap, rep, rlen, str, caps, n);
  size_t rest = slen - caps[1];
  if (olen + rest + 1 >= cap) { cap = olen + rest + 64; out = (char*)realloc(out, cap); }
  memcpy(out+olen, str+caps[1], rest);
  out[olen+rest] = 0;
  return out;
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
typedef struct { int tag; int cls_id; union { mrb_int i; const char *s; mrb_float f; mrb_bool b; void *p; } v; } sp_RbVal;
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
  /* "first..last" form. Buffer sized for two int64s plus the dots. */
  char *buf = sp_str_alloc_raw(48);
  snprintf(buf, 48, "%lld..%lld", (long long)r->first, (long long)r->last);
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
static inline void sp_poly_puts(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT: printf("%lld\n", (long long)v.v.i); break;
    case SP_TAG_STR: if (v.v.s) { fputs(v.v.s, stdout); if (!*v.v.s || v.v.s[strlen(v.v.s)-1] != '\n') putchar('\n'); } else putchar('\n'); break;
    case SP_TAG_FLT: { fputs(sp_float_to_s(v.v.f), stdout); putchar('\n'); break; }
    case SP_TAG_BOOL: puts(v.v.b ? "true" : "false"); break;
    case SP_TAG_NIL: putchar('\n'); break;
    case SP_TAG_SYM: { const char *_ss = sp_sym_to_s((sp_sym)v.v.i); fputs(_ss, stdout); putchar('\n'); break; }
    case SP_TAG_ENCODING: { const char *_es = v.v.s ? v.v.s : sp_str_empty; fputs(_es, stdout); putchar('\n'); break; }
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
static mrb_bool sp_poly_eq(sp_RbVal a, sp_RbVal b) { if (sp_poly_numeric_p(a) && sp_poly_numeric_p(b)) return sp_poly_to_f(a) == sp_poly_to_f(b); if (a.tag != b.tag) return FALSE; switch (a.tag) { case SP_TAG_INT: return a.v.i == b.v.i; case SP_TAG_STR: return (a.v.s == NULL || b.v.s == NULL) ? (a.v.s == b.v.s) : (strcmp(a.v.s, b.v.s) == 0); case SP_TAG_FLT: return a.v.f == b.v.f; case SP_TAG_BOOL: return a.v.b == b.v.b; case SP_TAG_NIL: return TRUE; case SP_TAG_SYM: return a.v.i == b.v.i; case SP_TAG_ENCODING: return (a.v.s == NULL || b.v.s == NULL) ? (a.v.s == b.v.s) : (strcmp(a.v.s, b.v.s) == 0); case SP_TAG_OBJ: return a.cls_id == b.cls_id && a.v.p == b.v.p; default: return FALSE; } }
static mrb_int sp_poly_cmp(sp_RbVal a, sp_RbVal b, mrb_bool *comparable) { if (sp_poly_numeric_p(a) && sp_poly_numeric_p(b)) { mrb_float af = sp_poly_to_f(a), bf = sp_poly_to_f(b); *comparable = TRUE; return (af > bf) - (af < bf); } if (a.tag == SP_TAG_STR && b.tag == SP_TAG_STR) { if (a.v.s == NULL || b.v.s == NULL) { *comparable = (a.v.s == b.v.s); return 0; } *comparable = TRUE; return strcmp(a.v.s, b.v.s); } if (a.tag == SP_TAG_SYM && b.tag == SP_TAG_SYM) { *comparable = TRUE; return (a.v.i > b.v.i) - (a.v.i < b.v.i); } *comparable = FALSE; return 0; }
static mrb_bool sp_poly_lt(sp_RbVal a, sp_RbVal b) { mrb_bool comparable; mrb_int cmp = sp_poly_cmp(a, b, &comparable); return comparable ? (cmp < 0) : FALSE; }
static mrb_bool sp_poly_le(sp_RbVal a, sp_RbVal b) { mrb_bool comparable; mrb_int cmp = sp_poly_cmp(a, b, &comparable); return comparable ? (cmp <= 0) : FALSE; }
static mrb_bool sp_poly_gt(sp_RbVal a, sp_RbVal b) { mrb_bool comparable; mrb_int cmp = sp_poly_cmp(a, b, &comparable); return comparable ? (cmp > 0) : FALSE; }
static mrb_bool sp_poly_ge(sp_RbVal a, sp_RbVal b) { mrb_bool comparable; mrb_int cmp = sp_poly_cmp(a, b, &comparable); return comparable ? (cmp >= 0) : FALSE; }
static sp_RbVal sp_poly_div(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(sp_poly_to_f(a) / sp_poly_to_f(b)); return sp_box_int(sp_idiv(sp_poly_to_i(a), sp_poly_to_i(b))); }
static sp_RbVal sp_poly_mod(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(fmod(sp_poly_to_f(a), sp_poly_to_f(b))); return sp_box_int(sp_imod(sp_poly_to_i(a), sp_poly_to_i(b))); }
static sp_RbVal sp_poly_pow(sp_RbVal a, sp_RbVal b) { double r = pow((double)sp_poly_to_f(a), (double)sp_poly_to_f(b)); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT && b.v.i >= 0) return sp_box_int((mrb_int)r); return sp_box_float((mrb_float)r); }
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
typedef struct { sp_RbVal *data; mrb_int len; mrb_int cap; } sp_PolyArray;
static inline void sp_mark_rbval(sp_RbVal v);
static void sp_PolyArray_scan(void *p) { sp_PolyArray *a = (sp_PolyArray *)p; for (mrb_int i = 0; i < a->len; i++) sp_mark_rbval(a->data[i]); }
static void sp_PolyArray_fin(void *p) { sp_PolyArray *a = (sp_PolyArray *)p; sp_gc_hdr *h = (sp_gc_hdr *)((char *)a - sizeof(sp_gc_hdr)); sp_gc_bytes -= sizeof(sp_RbVal) * a->cap; h->size -= sizeof(sp_RbVal) * a->cap; free(a->data); }
static sp_PolyArray *sp_PolyArray_new(void) { sp_PolyArray *a = (sp_PolyArray *)sp_gc_alloc(sizeof(sp_PolyArray), sp_PolyArray_fin, sp_PolyArray_scan); a->cap = 16; a->data = (sp_RbVal *)malloc(sizeof(sp_RbVal) * a->cap); if (!a->data) sp_oom_die(); a->len = 0; { sp_gc_hdr *h = (sp_gc_hdr *)((char *)a - sizeof(sp_gc_hdr)); h->size += sizeof(sp_RbVal) * a->cap; sp_gc_bytes += sizeof(sp_RbVal) * a->cap; } return a; }
static void sp_PolyArray_push(sp_PolyArray *a, sp_RbVal v) { if (!a) return; if (a->len >= a->cap) { sp_gc_hdr *h = (sp_gc_hdr *)((char *)a - sizeof(sp_gc_hdr)); sp_gc_bytes -= sizeof(sp_RbVal) * a->cap; h->size -= sizeof(sp_RbVal) * a->cap; a->cap = a->cap * 2 + 1; void *nd = realloc(a->data, sizeof(sp_RbVal) * a->cap); if (!nd) sp_oom_die(); a->data = (sp_RbVal *)nd; h->size += sizeof(sp_RbVal) * a->cap; sp_gc_bytes += sizeof(sp_RbVal) * a->cap; } a->data[a->len++] = v; }
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
static sp_RbVal sp_PolyArray_get(sp_PolyArray *a, mrb_int i) { if (!a) return sp_box_nil(); if (i < 0) i += a->len; return a->data[i]; }
/* Issues #770, #789: NULL + bounds guard. Out-of-range set no-ops. */
static void sp_PolyArray_set(sp_PolyArray *a, mrb_int i, sp_RbVal v) { if (!a) return; mrb_int orig=i; if (i < 0) i += a->len; if (i < 0) sp_raise_cls("IndexError", sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)-a->len)); if (i >= a->len) return; a->data[i] = v; }
static sp_PolyArray *sp_PolyArray_slice(sp_PolyArray *a, mrb_int start, mrb_int len) { if (start < 0) start += a->len; if (start < 0) start = 0; sp_PolyArray *b = sp_PolyArray_new(); if (start >= a->len || len <= 0) return b; if (start + len > a->len) len = a->len - start; for (mrb_int i = 0; i < len; i++) sp_PolyArray_push(b, a->data[start + i]); return b; }
static sp_PolyArray *sp_PolyArray_slice_bang(sp_PolyArray *a, mrb_int from, mrb_int n) {
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
static void sp_PolyArray_shuffle_bang(sp_PolyArray *a) { for (mrb_int i = a->len - 1; i > 0; i--) { mrb_int j = (mrb_int)(rand() % (i + 1)); sp_RbVal t = a->data[i]; a->data[i] = a->data[j]; a->data[j] = t; } }
static void sp_PolyArray_rotate_bang(sp_PolyArray*a,mrb_int n){if(a->len<=0)return;n=((n%a->len)+a->len)%a->len;if(n==0)return;sp_RbVal*tmp=(sp_RbVal*)malloc(sizeof(sp_RbVal)*a->len);for(mrb_int i=0;i<a->len;i++)tmp[i]=a->data[(i+n)%a->len];for(mrb_int i=0;i<a->len;i++)a->data[i]=tmp[i];free(tmp);}
static sp_PolyArray *sp_PolyArray_shuffle(sp_PolyArray *a) { sp_PolyArray *b = sp_PolyArray_dup(a); sp_PolyArray_shuffle_bang(b); return b; }
static sp_RbVal sp_PolyArray_sample(sp_PolyArray *a) { if (a->len <= 0) return sp_box_nil(); return a->data[(mrb_int)(rand()%a->len)]; }

/* Forward decl: sp_poly_inspect dispatches into sp_PolyArray_inspect
   for nested poly arrays (under promote, an `each_cons` chain's outer
   accumulator boxes each inner poly_array element), but the
   sp_PolyArray_inspect body lives a few lines below. */
static const char *sp_PolyArray_inspect(sp_PolyArray *a);

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
static inline void sp_mark_rbval(sp_RbVal v) {
  if (v.tag == SP_TAG_STR) sp_mark_string(v.v.s);
  else if (v.tag == SP_TAG_OBJ) sp_gc_mark(v.v.p);
}

/* StrPolyHash: string keys, sp_RbVal values — for hashes with mixed value types. */
typedef struct{const char**keys;sp_RbVal*vals;const char**order;mrb_int len;mrb_int cap;mrb_int mask;sp_RbVal default_v;}sp_StrPolyHash;
static void sp_StrPolyHash_fin(void*p){sp_StrPolyHash*h=(sp_StrPolyHash*)p;free(h->keys);free(h->vals);free(h->order);}
static void sp_StrPolyHash_scan(void*p){sp_StrPolyHash*h=(sp_StrPolyHash*)p;for(mrb_int i=0;i<h->cap;i++){if(h->keys[i]){sp_mark_string(h->keys[i]);sp_mark_rbval(h->vals[i]);}}sp_mark_rbval(h->default_v);}
static sp_StrPolyHash*sp_StrPolyHash_new(void){sp_StrPolyHash*h=(sp_StrPolyHash*)sp_gc_alloc(sizeof(sp_StrPolyHash),sp_StrPolyHash_fin,sp_StrPolyHash_scan);h->cap=16;h->mask=15;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(const char**)malloc(sizeof(const char*)*h->cap);h->len=0;h->default_v=sp_box_nil();return h;}
static sp_StrPolyHash*sp_StrPolyHash_new_with_default(sp_RbVal d){sp_StrPolyHash*h=sp_StrPolyHash_new();h->default_v=d;return h;}
static void sp_StrPolyHash_grow(sp_StrPolyHash*h){mrb_int oc=h->cap;const char**ok=h->keys;sp_RbVal*ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(const char**)realloc(h->order,sizeof(const char*)*h->cap);h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]){mrb_int idx=(mrb_int)(sp_str_hash(ok[i])&h->mask);while(h->keys[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}
static sp_RbVal sp_StrPolyHash_get(sp_StrPolyHash*h,const char*k){if(!h)return sp_box_nil();mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
static void sp_StrPolyHash_set(sp_StrPolyHash*h,const char*k,sp_RbVal v){if(h->len*2>=h->cap)sp_StrPolyHash_grow(h);mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}
static mrb_bool sp_StrPolyHash_has_key(sp_StrPolyHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
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
typedef struct{sp_sym*keys;sp_RbVal*vals;sp_sym*order;mrb_int len;mrb_int cap;mrb_int mask;sp_RbVal default_v;}sp_SymPolyHash;
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
/* Hash#inspect for sym_poly_hash. CRuby 3.4+ renders symbol keys
   in shorthand: `{b: 2, c: 3}` rather than `{:b=>2, :c=>3}`. We
   match the older `=>` form to keep round-tripping simple (callers
   that compare against `eval(str)` survive either way). */
static const char*sp_SymPolyHash_inspect(sp_SymPolyHash*h){if(!h){char*r=sp_str_alloc_raw(3);r[0]='{';r[1]='}';r[2]=0;sp_str_set_len(r,2);return r;}sp_String*s=sp_String_new("{");for(mrb_int i=0;i<h->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,":");sp_String_append(s,sp_sym_to_s(h->order[i]));sp_String_append(s,"=>");sp_String_append(s,sp_poly_inspect(sp_SymPolyHash_get(h,h->order[i])));}sp_String_append(s,"}");return s->data;}

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

#include <setjmp.h>
#define SP_EXC_STACK_MAX 64
static jmp_buf sp_exc_stack[SP_EXC_STACK_MAX];
static const char *sp_exc_msg[SP_EXC_STACK_MAX];
static volatile int sp_exc_top = 0;
static const char *sp_exc_cls[SP_EXC_STACK_MAX];
static volatile const char *sp_last_exc_cls = sp_str_empty;
static void sp_raise_cls(const char *cls, const char *msg) { if (sp_exc_top > 0) { sp_exc_msg[sp_exc_top-1] = msg; sp_exc_cls[sp_exc_top-1] = cls; sp_last_exc_cls = cls; longjmp(sp_exc_stack[sp_exc_top-1], 1); } fprintf(stderr, "unhandled exception: %s\n", msg); exit(1); }
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
static sp_PtrArray *sp_PtrArray_slice_bang(sp_PtrArray *a, mrb_int from, mrb_int n) {
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
static mrb_int sp_proc_call(sp_Proc *p, mrb_int *args) { return (p && p->fn) ? ((mrb_int (*)(void *, mrb_int *))p->fn)(p->cap, args) : 0; }
static sp_PolyArray *sp_proc_parameters(sp_Proc *p) { sp_PolyArray *r = sp_PolyArray_new(); if (!p || p->param_count <= 0 || !p->param_kinds) return r; SP_GC_ROOT(r); for (mrb_int i = 0; i < p->param_count; i++) { sp_PolyArray *pair = sp_PolyArray_new(); sp_PolyArray_push(pair, sp_box_sym(p->param_kinds[i])); if (p->param_names && p->param_names[i] >= 0) sp_PolyArray_push(pair, sp_box_sym(p->param_names[i])); sp_PolyArray_push(r, sp_box_poly_array(pair)); } return r; }

/* ---- StringIO runtime ---- */
typedef struct { char *buf; int64_t len; int64_t cap; int64_t pos; int64_t lineno; int closed; } sp_StringIO;
static void sio_grow(sp_StringIO *sio, int64_t need) { int64_t req = sio->pos + need; if (req <= sio->cap) return; int64_t nc = sio->cap ? sio->cap : 64; while (nc < req) nc *= 2; sio->buf = (char *)realloc(sio->buf, nc + 1); sio->cap = nc; }
static int64_t sio_write(sp_StringIO *sio, const char *d, int64_t dl) { sio_grow(sio, dl); if (sio->pos > sio->len) memset(sio->buf + sio->len, 0, sio->pos - sio->len); memcpy(sio->buf + sio->pos, d, dl); sio->pos += dl; if (sio->pos > sio->len) sio->len = sio->pos; sio->buf[sio->len] = '\0'; return dl; }
static sp_StringIO *sp_StringIO_new(void) { sp_StringIO *s = (sp_StringIO *)calloc(1, sizeof(sp_StringIO)); s->buf = (char *)calloc(1, 64); s->cap = 63; return s; }
static sp_StringIO *sp_StringIO_new_s(const char *init) { sp_StringIO *s = (sp_StringIO *)calloc(1, sizeof(sp_StringIO)); int64_t l = (int64_t)strlen(init); int64_t c = l < 63 ? 63 : l; s->buf = (char*)malloc(c+1); memcpy(s->buf, init, l); s->buf[l]='\0'; s->len = l; s->cap = c; return s; }
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
static mrb_int sp_lam_to_int(sp_Val *v) { return v->u.ival; }

/* ---- Fiber runtime (ucontext) ---- */
#ifdef _WIN32
#define SP_FIBER_STACK_SIZE (64*1024)
typedef struct sp_Fiber{LPVOID ctx;LPVOID caller_ctx;char*stack;int state;int transferred;sp_RbVal yielded_value;sp_RbVal resumed_value;void(*body)(struct sp_Fiber*);void*user_data;int saved_exc_top;int saved_catch_top;struct sp_Fiber*caller;sp_SymPolyHash*storage;void***saved_roots;int saved_nroots;int saved_roots_cap;struct sp_Fiber*fiber_next;struct sp_Fiber*fiber_prev;}sp_Fiber;
static sp_Fiber sp_fiber_root;
static sp_Fiber*sp_fiber_current=&sp_fiber_root;
static sp_Fiber*sp_fiber_list_head=NULL;
/* Per-fiber root save/restore (issue #636). On fiber switch, the
   outgoing fiber's active root window gets memcpy'd into its
   saved_roots buffer and the incoming fiber's saved_roots get
   memcpy'd back into the global sp_gc_roots[] array. The buffer
   grows on demand. Without this, two fibers running in
   non-LIFO order would clobber each other's slots in the shared
   sp_gc_roots[]. */
/* Issue #761: capture realloc into a temp and only commit on success
   so failure doesn't leak the old buffer. */
static void sp_fiber_save_roots(sp_Fiber*f){if(f->saved_roots_cap<sp_gc_nroots){int nc=sp_gc_nroots>64?sp_gc_nroots*2:64;void***nx=(void***)realloc(f->saved_roots,sizeof(void**)*nc);if(!nx)return;f->saved_roots=nx;f->saved_roots_cap=nc;}if(sp_gc_nroots>0)memcpy(f->saved_roots,sp_gc_roots,sizeof(void**)*sp_gc_nroots);f->saved_nroots=sp_gc_nroots;}
static void sp_fiber_restore_roots(sp_Fiber*f){if(f->saved_nroots>0)memcpy(sp_gc_roots,f->saved_roots,sizeof(void**)*f->saved_nroots);sp_gc_nroots=f->saved_nroots;}
static void sp_fiber_list_add(sp_Fiber*f){f->fiber_prev=NULL;f->fiber_next=sp_fiber_list_head;if(sp_fiber_list_head)sp_fiber_list_head->fiber_prev=f;sp_fiber_list_head=f;}
static void sp_fiber_list_remove(sp_Fiber*f){if(f->fiber_prev)f->fiber_prev->fiber_next=f->fiber_next;else if(sp_fiber_list_head==f)sp_fiber_list_head=f->fiber_next;if(f->fiber_next)f->fiber_next->fiber_prev=f->fiber_prev;f->fiber_prev=NULL;f->fiber_next=NULL;}
/* Mark roots saved by every suspended fiber. Includes sp_fiber_root
   (the main-thread synthetic fiber): when a Fiber.new'd fiber is
   running and triggers a GC pass, main's roots are sitting in
   sp_fiber_root.saved_roots (the resume side stashed them there)
   but sp_fiber_root is NOT in sp_fiber_list_head, so walking the
   list alone would miss them and free main's live locals. */
static void sp_mark_fiber_roots(sp_Fiber*f){if(f==sp_fiber_current)return;int i;for(i=0;i<f->saved_nroots;i++){void*obj=*f->saved_roots[i];if(obj)sp_gc_mark(obj);}}
static void sp_mark_suspended_fibers(void){sp_mark_fiber_roots(&sp_fiber_root);sp_Fiber*f=sp_fiber_list_head;while(f){sp_mark_fiber_roots(f);f=f->fiber_next;}}
static void sp_fiber_install_gc_hook(void){if(!sp_gc_mark_suspended_fibers_hook)sp_gc_mark_suspended_fibers_hook=sp_mark_suspended_fibers;}
static void sp_Fiber_fin(void*p){sp_Fiber*f=(sp_Fiber*)p;if(f->ctx)DeleteFiber(f->ctx);if(f->saved_roots)free(f->saved_roots);sp_fiber_list_remove(f);}
static void sp_Fiber_scan(void*p){sp_Fiber*f=(sp_Fiber*)p;if(f->user_data)sp_gc_mark(f->user_data);if(f->storage)sp_gc_mark(f->storage);}
static sp_Fiber*sp_Fiber_new(void(*body)(sp_Fiber*)){sp_Fiber*f=(sp_Fiber*)sp_gc_alloc(sizeof(sp_Fiber),sp_Fiber_fin,sp_Fiber_scan);f->ctx=NULL;f->caller_ctx=NULL;f->stack=NULL;f->state=0;f->transferred=0;f->body=body;f->yielded_value=sp_box_nil();f->resumed_value=sp_box_nil();f->user_data=NULL;f->saved_exc_top=0;f->saved_catch_top=0;f->caller=NULL;f->storage=NULL;f->saved_roots=NULL;f->saved_nroots=0;f->saved_roots_cap=0;f->fiber_next=NULL;f->fiber_prev=NULL;sp_fiber_list_add(f);sp_fiber_install_gc_hook();if(sp_fiber_current&&sp_fiber_current->storage)f->storage=sp_SymPolyHash_dup(sp_fiber_current->storage);return f;}
static sp_RbVal sp_Fiber_storage_get(sp_Fiber*f,sp_sym k){if(!f->storage)return sp_box_nil();return sp_SymPolyHash_get(f->storage,k);}
static void sp_Fiber_storage_set(sp_Fiber*f,sp_sym k,sp_RbVal v){if(!f->storage)f->storage=sp_SymPolyHash_new();sp_SymPolyHash_set(f->storage,k,v);}
static VOID CALLBACK sp_fiber_trampoline(LPVOID param){sp_Fiber*f=(sp_Fiber*)param;f->body(f);f->state=3;if(f->transferred){sp_fiber_current=&sp_fiber_root;SwitchToFiber(sp_fiber_root.ctx);}else{SwitchToFiber(f->caller->ctx);}}
static void sp_fiber_ensure_root(void){if(!sp_fiber_root.ctx){sp_fiber_root.ctx=ConvertThreadToFiber(NULL);if(!sp_fiber_root.ctx)sp_fiber_root.ctx=GetCurrentFiber();}}
static sp_RbVal sp_Fiber_resume(sp_Fiber*f,sp_RbVal val){if(f->state==3){sp_raise_cls("FiberError","attempt to resume a terminated fiber");}sp_fiber_ensure_root();f->resumed_value=val;sp_Fiber*prev=sp_fiber_current;f->caller=prev;sp_fiber_save_roots(prev);sp_fiber_restore_roots(f);sp_fiber_current=f;if(f->state==0){f->state=1;f->ctx=CreateFiber(SP_FIBER_STACK_SIZE,sp_fiber_trampoline,f);if(!f->ctx)sp_raise("failed to create fiber");}else{f->state=1;}SwitchToFiber(f->ctx);sp_fiber_save_roots(f);sp_fiber_restore_roots(prev);sp_fiber_current=prev;return f->yielded_value;}
static sp_RbVal sp_Fiber_yield(sp_RbVal val){sp_Fiber*f=sp_fiber_current;f->yielded_value=val;f->state=2;SwitchToFiber(f->caller->ctx);return f->resumed_value;}
static mrb_bool sp_Fiber_alive(sp_Fiber*f){return f->state!=3;}
static sp_RbVal sp_Fiber_transfer(sp_Fiber*f,sp_RbVal val){sp_fiber_ensure_root();f->resumed_value=val;sp_Fiber*prev=sp_fiber_current;sp_fiber_save_roots(prev);sp_fiber_restore_roots(f);sp_fiber_current=f;if(f->state==0){f->state=1;f->transferred=1;f->caller=prev;f->ctx=CreateFiber(SP_FIBER_STACK_SIZE,sp_fiber_trampoline,f);if(!f->ctx)sp_raise("failed to create fiber");}else{f->state=1;}SwitchToFiber(f->ctx);sp_fiber_save_roots(f);sp_fiber_restore_roots(prev);sp_fiber_current=prev;return prev->resumed_value;}
#else
#include <ucontext.h>
#include <sys/mman.h>
#define SP_FIBER_STACK_SIZE (64*1024)
typedef struct sp_Fiber{ucontext_t ctx;ucontext_t caller_ctx;char*stack;int state;int transferred;sp_RbVal yielded_value;sp_RbVal resumed_value;void(*body)(struct sp_Fiber*);void*user_data;int saved_exc_top;int saved_catch_top;sp_SymPolyHash*storage;void***saved_roots;int saved_nroots;int saved_roots_cap;struct sp_Fiber*fiber_next;struct sp_Fiber*fiber_prev;}sp_Fiber;
static sp_Fiber sp_fiber_root;
static sp_Fiber*sp_fiber_current=&sp_fiber_root;
static sp_Fiber*sp_fiber_list_head=NULL;
/* Per-fiber root save/restore — see Windows variant above for
   rationale (issue #636). */
/* Issue #761: capture realloc into a temp and only commit on success
   so failure doesn't leak the old buffer. */
static void sp_fiber_save_roots(sp_Fiber*f){if(f->saved_roots_cap<sp_gc_nroots){int nc=sp_gc_nroots>64?sp_gc_nroots*2:64;void***nx=(void***)realloc(f->saved_roots,sizeof(void**)*nc);if(!nx)return;f->saved_roots=nx;f->saved_roots_cap=nc;}if(sp_gc_nroots>0)memcpy(f->saved_roots,sp_gc_roots,sizeof(void**)*sp_gc_nroots);f->saved_nroots=sp_gc_nroots;}
static void sp_fiber_restore_roots(sp_Fiber*f){if(f->saved_nroots>0)memcpy(sp_gc_roots,f->saved_roots,sizeof(void**)*f->saved_nroots);sp_gc_nroots=f->saved_nroots;}
static void sp_fiber_list_add(sp_Fiber*f){f->fiber_prev=NULL;f->fiber_next=sp_fiber_list_head;if(sp_fiber_list_head)sp_fiber_list_head->fiber_prev=f;sp_fiber_list_head=f;}
static void sp_fiber_list_remove(sp_Fiber*f){if(f->fiber_prev)f->fiber_prev->fiber_next=f->fiber_next;else if(sp_fiber_list_head==f)sp_fiber_list_head=f->fiber_next;if(f->fiber_next)f->fiber_next->fiber_prev=f->fiber_prev;f->fiber_prev=NULL;f->fiber_next=NULL;}
/* Mark roots saved by every suspended fiber. Includes sp_fiber_root
   (the main-thread synthetic fiber): when a Fiber.new'd fiber is
   running and triggers a GC pass, main's roots are sitting in
   sp_fiber_root.saved_roots (the resume side stashed them there)
   but sp_fiber_root is NOT in sp_fiber_list_head, so walking the
   list alone would miss them and free main's live locals. */
static void sp_mark_fiber_roots(sp_Fiber*f){if(f==sp_fiber_current)return;int i;for(i=0;i<f->saved_nroots;i++){void*obj=*f->saved_roots[i];if(obj)sp_gc_mark(obj);}}
static void sp_mark_suspended_fibers(void){sp_mark_fiber_roots(&sp_fiber_root);sp_Fiber*f=sp_fiber_list_head;while(f){sp_mark_fiber_roots(f);f=f->fiber_next;}}
static void sp_fiber_install_gc_hook(void){if(!sp_gc_mark_suspended_fibers_hook)sp_gc_mark_suspended_fibers_hook=sp_mark_suspended_fibers;}
static void sp_Fiber_fin(void*p){sp_Fiber*f=(sp_Fiber*)p;if(f->stack)munmap(f->stack,SP_FIBER_STACK_SIZE);if(f->saved_roots)free(f->saved_roots);sp_fiber_list_remove(f);}
static void sp_Fiber_scan(void*p){sp_Fiber*f=(sp_Fiber*)p;if(f->user_data)sp_gc_mark(f->user_data);if(f->storage)sp_gc_mark(f->storage);}
/* Issue #790: mmap can fail (no swap space, vmem exhaustion). The
   previous form wrote MAP_FAILED into f->stack and crashed on the
   first stack access. Check + raise so the caller sees a Ruby
   exception instead of an opaque SEGV. */
static sp_Fiber*sp_Fiber_new(void(*body)(sp_Fiber*)){sp_Fiber*f=(sp_Fiber*)sp_gc_alloc(sizeof(sp_Fiber),sp_Fiber_fin,sp_Fiber_scan);f->stack=(char*)mmap(NULL,SP_FIBER_STACK_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(f->stack==MAP_FAILED){f->stack=NULL;sp_raise_cls("FiberError","failed to allocate fiber stack");}f->state=0;f->transferred=0;f->body=body;f->yielded_value=sp_box_nil();f->resumed_value=sp_box_nil();f->user_data=NULL;f->saved_exc_top=0;f->saved_catch_top=0;f->storage=NULL;f->saved_roots=NULL;f->saved_nroots=0;f->saved_roots_cap=0;f->fiber_next=NULL;f->fiber_prev=NULL;sp_fiber_list_add(f);sp_fiber_install_gc_hook();if(sp_fiber_current&&sp_fiber_current->storage)f->storage=sp_SymPolyHash_dup(sp_fiber_current->storage);return f;}
static sp_RbVal sp_Fiber_storage_get(sp_Fiber*f,sp_sym k){if(!f->storage)return sp_box_nil();return sp_SymPolyHash_get(f->storage,k);}
static void sp_Fiber_storage_set(sp_Fiber*f,sp_sym k,sp_RbVal v){if(!f->storage)f->storage=sp_SymPolyHash_new();sp_SymPolyHash_set(f->storage,k,v);}
static void sp_fiber_trampoline(void){sp_Fiber*f=sp_fiber_current;f->body(f);f->state=3;if(f->transferred){sp_fiber_current=&sp_fiber_root;setcontext(&sp_fiber_root.ctx);}else{swapcontext(&f->ctx,&f->caller_ctx);}}
static sp_RbVal sp_Fiber_resume(sp_Fiber*f,sp_RbVal val){if(f->state==3){sp_raise_cls("FiberError","attempt to resume a terminated fiber");}f->resumed_value=val;sp_Fiber*prev=sp_fiber_current;sp_fiber_save_roots(prev);sp_fiber_restore_roots(f);sp_fiber_current=f;if(f->state==0){f->state=1;getcontext(&f->ctx);f->ctx.uc_stack.ss_sp=f->stack;f->ctx.uc_stack.ss_size=SP_FIBER_STACK_SIZE;f->ctx.uc_link=&f->caller_ctx;makecontext(&f->ctx,(void(*)(void))sp_fiber_trampoline,0);swapcontext(&f->caller_ctx,&f->ctx);}else{f->state=1;swapcontext(&f->caller_ctx,&f->ctx);}sp_fiber_save_roots(f);sp_fiber_restore_roots(prev);sp_fiber_current=prev;return f->yielded_value;}
static sp_RbVal sp_Fiber_yield(sp_RbVal val){sp_Fiber*f=sp_fiber_current;f->yielded_value=val;f->state=2;swapcontext(&f->ctx,&f->caller_ctx);return f->resumed_value;}
static mrb_bool sp_Fiber_alive(sp_Fiber*f){return f->state!=3;}
static sp_RbVal sp_Fiber_transfer(sp_Fiber*f,sp_RbVal val){f->resumed_value=val;sp_Fiber*prev=sp_fiber_current;sp_fiber_save_roots(prev);sp_fiber_restore_roots(f);sp_fiber_current=f;if(f->state==0){f->state=1;f->transferred=1;getcontext(&f->ctx);f->ctx.uc_stack.ss_sp=f->stack;f->ctx.uc_stack.ss_size=SP_FIBER_STACK_SIZE;f->ctx.uc_link=&prev->ctx;makecontext(&f->ctx,(void(*)(void))sp_fiber_trampoline,0);swapcontext(&prev->ctx,&f->ctx);}else{f->state=1;swapcontext(&prev->ctx,&f->ctx);}sp_fiber_save_roots(f);sp_fiber_restore_roots(prev);sp_fiber_current=prev;return prev->resumed_value;}
#endif
static void sp_mark_fiber_root_storage(void){if(sp_fiber_root.storage)sp_gc_mark(sp_fiber_root.storage);}

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


/* System/backtick support */
static int sp_last_status = 0;
#ifdef _WIN32
static char *sp_win_quote_arg(const char *arg) {
  const char *p = arg;
  size_t len = 2;
  mrb_bool quote = (*p == '\0') ? TRUE : FALSE;
  while (*p) {
    if (isspace((unsigned char)*p) || *p == '"') quote = TRUE;
    if (*p == '"') {
      len += 2;
    }
    else {
      len += 1;
    }
    p++;
  }
  if (!quote) {
    char *copy = (char *)malloc(len - 1);
    if (copy) memcpy(copy, arg, len - 1);
    return copy;
  }

  p = arg;
  size_t bs = 0;
  len = 3;
  while (*p) {
    if (*p == '\\') {
      bs++;
    }
    else if (*p == '"') {
      len += bs * 2 + 2;
      bs = 0;
    }
    else {
      len += bs + 1;
      bs = 0;
    }
    p++;
  }
  len += bs * 2;

  char *out = (char *)malloc(len);
  if (!out) return NULL;
  char *q = out;
  *q++ = '"';
  p = arg;
  bs = 0;
  while (*p) {
    if (*p == '\\') {
      bs++;
    }
    else if (*p == '"') {
      while (bs > 0) {
        *q++ = '\\';
        *q++ = '\\';
        bs--;
      }
      *q++ = '\\';
      *q++ = '"';
      bs = 0;
    }
    else {
      while (bs > 0) {
        *q++ = '\\';
        bs--;
      }
      *q++ = *p;
      bs = 0;
    }
    p++;
  }
  while (bs > 0) {
    *q++ = '\\';
    *q++ = '\\';
    bs--;
  }
  *q++ = '"';
  *q = '\0';
  return out;
}
#endif

static mrb_bool sp_system_args(int argc, const char *const *argv) {
  if (argc <= 0 || argv == NULL || argv[0] == NULL) {
    sp_last_status = -1;
    return FALSE;
  }
  fflush(NULL);
#ifdef _WIN32
  char **quoted_argv = (char **)malloc(sizeof(char *) * (size_t)(argc + 1));
  if (!quoted_argv) {
    sp_last_status = -1;
    return FALSE;
  }
  int i = 0;
  while (i < argc) {
    if (argv[i] == NULL) {
      while (i-- > 0) free(quoted_argv[i]);
      free(quoted_argv);
      sp_last_status = -1;
      return FALSE;
    }
    quoted_argv[i] = sp_win_quote_arg(argv[i]);
    if (!quoted_argv[i]) {
      while (i-- > 0) free(quoted_argv[i]);
      free(quoted_argv);
      sp_last_status = -1;
      return FALSE;
    }
    i++;
  }
  quoted_argv[argc] = NULL;

  intptr_t rc = _spawnvp(_P_WAIT, argv[0], (const char *const *)quoted_argv);
  for (i = 0; i < argc; i++) free(quoted_argv[i]);
  free(quoted_argv);
  if (rc < 0) {
    sp_last_status = -1;
    return FALSE;
  }
  sp_last_status = (int)rc << 8;
  return rc == 0 ? TRUE : FALSE;
#else
  pid_t pid = fork();
  if (pid < 0) {
    sp_last_status = -1;
    return FALSE;
  }
  if (pid == 0) {
    execvp(argv[0], (char * const *)argv);
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) continue;
    sp_last_status = -1;
    return FALSE;
  }
  sp_last_status = status;
  return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? TRUE : FALSE;
#endif
}

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
