/* sp_core.c -- runtime helpers split out of spinel_rt.h into
 * libspinel_rt.a. See sp_core.h for the rationale. */
#include "sp_core.h"
#include <float.h>
#include <limits.h>
#include <string.h>
#include "sp_alloc.h"   /* sp_str_byte_len: embedded-NUL detection in Integer()/Float() */
#include "sp_dtoa.h"    /* sp_read_float: locale-independent String#to_f / Float() */
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <locale.h>
/* macOS keeps the *_l / locale_t surface in <xlocale.h>; glibc exposes it from
   <locale.h> under POSIX.1-2008. Guarded so a platform with neither is not a
   build error either. (#3370) */
#if defined(__has_include)
#  if __has_include(<xlocale.h>)
#    include <xlocale.h>
#  endif
#endif

/* Must match sp_types.h: sp_int is pointer-width (int64 on 64-bit,
   int32 on 32-bit) so this TU's helper ABI agrees with the generated TU. */
typedef intptr_t sp_int;
typedef double  sp_float;
/* 10^p fits sp_int only up to this exponent (p>= it collapses to 0):
   10^19 > INT64_MAX, 10^10 > INT32_MAX. */
#if INTPTR_MAX == INT32_MAX
#define SP_INT_POW10_LIMIT 10
#else
#define SP_INT_POW10_LIMIT 19
#endif

/* Defined in the generated translation unit (spinel_rt.h); referenced
   here and resolved at link time. */
SP_NORETURN void sp_raise_cls(const char *cls, const char *msg);
const char *sp_sprintf(const char *fmt, ...);

/* CRuby's `String#to_i` accepts a leading sign, then digits with
   `_` between consecutive digits, and stops at the first non-digit
   (returning what it has so far rather than raising). `"1_2_3asdf"`
   -> 123. spinel previously emitted `(sp_int)atoll(s)` which stops
   at the first `_`, returning 1 instead. Issue #619. */
sp_int sp_str_to_i_cruby(const char *s) {SP_GC_ROOT_STR(s);
  if (!s) return 0;
  const char *p = s;
  while (isspace((unsigned char)*p)) p++;
  int neg = 0;
  if (*p == '+') p++;
  else if (*p == '-') { neg = 1; p++; }
  sp_int v = 0;
  int any = 0;
  while (*p) {
    if (*p >= '0' && *p <= '9') {
      /* Accumulate the signed value: the magnitude of INTPTR_MIN does not
         fit sp_int, even though the negative value does. Signed-overflow
         on `v * 10 + digit` is undefined behavior;
         detect via sp_ckd_*_iptr (sp_compat.h). CRuby promotes to Bignum
         on overflow but spinel's int model is int64-only -- raise
         RangeError instead of silently saturating, so a user-side
         `rescue` can react. */
      sp_int t, digit = (sp_int)(*p - '0');
      if (neg) digit = -digit;
      if (sp_ckd_mul_iptr(v, 10, &t) ||
          sp_ckd_add_iptr(t, digit, &v)) {
        sp_raise_cls("RangeError", sp_sprintf("integer overflow parsing \"%s\"", s));
      }
      any = 1;
      p++;
    }
else if (*p == '_' && any && p[1] >= '0' && p[1] <= '9') {
      p++;
    }
else {
      break;
    }
  }
  if (!any) return 0;
  return v;
}

/* `String#to_f`: parse a leading float, tolerating `_` between digits as a
   separator (CRuby's `"1_000.5".to_f == 1000.5`), returning 0.0 when no
   number leads. Underscores are stripped into a small scratch buffer before
   strtod so the C parser (which stops at `_`) sees clean digits. */
double sp_str_to_f_cruby(const char *s) {
  if (!s) return 0.0;
  const char *p = s;
  while (isspace((unsigned char)*p)) p++;
  /* copy the leading numeric run, dropping `_` that sit between two digits */
  char buf[512];
  size_t n = 0;
  if (*p == '+' || *p == '-') { if (n < sizeof buf - 1) buf[n++] = *p; p++; }
  for (; *p && n < sizeof buf - 1; p++) {
    if ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
        *p == '+' || *p == '-') {
      buf[n++] = *p;
    }
    else if (*p == '_' && n > 0 &&
               ((buf[n-1] >= '0' && buf[n-1] <= '9')) &&
               (p[1] >= '0' && p[1] <= '9')) {
      /* separator between digits: skip it */
    }
    else {
      break;
    }
  }
  buf[n] = 0;
  double d = 0.0;
  sp_read_float(buf, NULL, &d);   /* locale-independent parse */
  return d;
}

/* Digit value of `c` in bases up to 36, or -1. */
static int sp_digit36(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return -1;
}

/* Skips leading whitespace and sign, resolves base 0 from the prefix
   (0x -> 16, 0b -> 2, 0/0o -> 8, 0d -> 10, otherwise 10), and skips a
   prefix matching the base. Per CRuby, only base 0 enables prefix-based
   dispatch -- explicit bases just *accept* the matching prefix. A leading
   0 followed by anything but a prefix letter is octal whatever comes next,
   so "08" stops at the 8 (Integer() refuses it) rather than reading 8. */
static const char *sp_int_head(const char *p, sp_int *base, int *neg) {
  while (isspace((unsigned char)*p)) p++;
  *neg = 0;
  if (*p == '+') p++;
  else if (*p == '-') { *neg = 1; p++; }
  if (*base == 0) {
    *base = 10;
    if (*p == '0') {
      char n = p[1];
      if (n == 'x' || n == 'X') *base = 16;
      else if (n == 'b' || n == 'B') *base = 2;
      else if (n == 'o' || n == 'O') *base = 8;
      else if (n != 0 && n != 'd' && n != 'D') *base = 8;
    }
  }
  if (*p == '0' && p[1] != 0) {
    char n = p[1];
    if ((*base == 16) && (n == 'x' || n == 'X')) p += 2;
    else if ((*base == 2) && (n == 'b' || n == 'B')) p += 2;
    else if ((*base == 8) && (n == 'o' || n == 'O')) p += 2;
    /* base 10 accepts the explicit decimal prefix too (#3719) */
    else if ((*base == 10) && (n == 'd' || n == 'D')) p += 2;
  }
  return p;
}

/* Accumulates digits of `base` into *v, consuming `_` only between
   digits; *any is set once a digit is read. Returns the stop position,
   or NULL on sp_int overflow. A negative number (neg) accumulates its
   signed value, as sp_str_to_i_cruby does: -2**63 fits sp_int where its
   magnitude does not. */
static const char *sp_int_scan(const char *p, sp_int base, int neg, sp_int *v, int *any) {
  *v = 0;
  *any = 0;
  for (;; p++) {
    int d = sp_digit36((unsigned char)*p);
    if (d < 0 || d >= (int)base) {
      if (*p == '_' && *any) {
        int n = sp_digit36((unsigned char)p[1]);
        if (n >= 0 && n < (int)base) continue;
      }
      return p;
    }
    sp_int t;
    if (sp_ckd_mul_iptr(*v, base, &t) ||
        sp_ckd_add_iptr(t, neg ? -(sp_int)d : (sp_int)d, v)) return NULL;
    *any = 1;
  }
}

/* The promote-mode parsers (sp_str_to_i_promote, spinel_rt.h) read the
   same text as the sp_int ones below, and take a Bignum where the sp_int
   scan overflows. Whether it does, for `s` in `base` (0 resolves from the
   prefix): */
int sp_str_int_overflows(const char *s, intptr_t base) {
  if (!s) return 0;
  int neg, any;
  sp_int v, b = base;
  const char *p = sp_int_head(s, &b, &neg);
  if (b < 2 || b > 36) return 0;
  return sp_int_scan(p, b, neg, &v, &any) == NULL;
}

/* ...and the digits as the Bignum parser wants them: a '-' for a negative
   value, then the digits without their `_` separators. *base comes back
   resolved, *rest just past the last digit. A malloc'd string. */
char *sp_int_digits_dup(const char *s, intptr_t *base, const char **rest) {
  int neg, any = 0;
  sp_int b = *base;
  const char *p = sp_int_head(s, &b, &neg);
  size_t k = 0;
  char *out = (char *)malloc(strlen(p) + 2);
  if (!out) return NULL;
  if (neg) out[k++] = '-';
  for (;; p++) {
    int d = sp_digit36((unsigned char)*p);
    if (d < 0 || d >= (int)b) {
      if (*p == '_' && any) {
        int n = sp_digit36((unsigned char)p[1]);
        if (n >= 0 && n < (int)b) continue;
      }
      break;
    }
    out[k++] = *p;
    any = 1;
  }
  out[k] = 0;
  *base = b;
  if (rest) *rest = p;
  return out;
}

/* `String#to_i(base)` with a non-decimal base. Accepts bases 2..36
   like MRI; `_` is allowed between digits the same way as base 10.
   Stops at the first invalid digit and returns what's parsed so
   far. Base 0 auto-detects from the prefix. Issue #883. */
sp_int sp_str_to_i_base(const char *s, sp_int base) {SP_GC_ROOT_STR(s);
  if (!s) return 0;
  /* CRuby rejects a radix outside 2..36 rather than falling back to 10 */
  if (base != 0 && (base < 2 || base > 36))
    sp_raise_cls("ArgumentError", sp_sprintf("invalid radix %lld", (long long)base));
  int neg, any;
  sp_int v;
  const char *p = sp_int_head(s, &base, &neg);
  if (!sp_int_scan(p, base, neg, &v, &any))
    sp_raise_cls("RangeError", sp_sprintf("integer overflow parsing \"%s\"", s));
  if (!any) return 0;
  return v;
}

/* CRuby's `Integer(s)` raises ArgumentError for unparseable input
   (empty string, leading/trailing junk, all-whitespace). The bare
   `(sp_int)strtoll(s, NULL, 10)` spinel previously emitted silently
   returned 0 instead, which made `Integer(s) rescue 0` always take
   the main branch. This helper matches CRuby semantics: skips
   leading/trailing whitespace, requires at least one valid digit,
   rejects trailing junk. Accepts an optional leading `+` / `-`. */
sp_int sp_str_to_i_strict(const char *s) {SP_GC_ROOT_STR(s);
  if (!s) sp_raise_cls("ArgumentError", "invalid value for Integer(): nil");
  /* an embedded NUL makes the Ruby string longer than its C prefix: CRuby
     rejects it, a C-string scan would silently parse the prefix. */
  if (strlen(s) != sp_str_byte_len(s))
    sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  /* Delegate to the base-aware parser with auto-detection: unlike a bare
     strtoll it handles digit-separating underscores ("1_000") and CRuby's
     prefix bases ("0x1A", "0b101", and leading-0 octal "077" -> 63). */
  return sp_str_to_i_strict_base(s, 0);
}

/* `Integer(s, base)` with explicit base. Bases 2..36, MRI-compatible
   prefix recognition (0x / 0b / 0o when the base matches). Raises
   ArgumentError on invalid input or unsupported base. Issue #887. */
/* The shared body. `lenient` is Kernel#Integer's `exception: false`: every
   rejection answers nil instead of raising (#3718); `*none` says so. */
static sp_int sp_str_to_i_base_impl(const char *s, sp_int base, int lenient, int *none) {SP_GC_ROOT_STR(s);
#define SP_INT_REJECT(cls, msg) do { if (lenient) { *none = 1; return 0; } sp_raise_cls(cls, msg); } while (0)
  if (!s) SP_INT_REJECT("ArgumentError", "invalid value for Integer(): nil");
  /* an embedded NUL makes the Ruby string longer than its C prefix: CRuby
     rejects it, a C-string scan would silently parse the prefix. */
  if (strlen(s) != sp_str_byte_len(s))
    SP_INT_REJECT("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  int neg, any;
  sp_int v;
  const char *p = sp_int_head(s, &base, &neg);
  if (base < 2 || base > 36) SP_INT_REJECT("ArgumentError", sp_sprintf("invalid radix %lld", (long long)base));
  if (*p == '\0') SP_INT_REJECT("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  p = sp_int_scan(p, base, neg, &v, &any);
  if (!p) SP_INT_REJECT("RangeError", sp_sprintf("integer overflow parsing \"%s\"", s));
  if (!any) SP_INT_REJECT("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  while (isspace((unsigned char)*p)) p++;
  if (*p != '\0') SP_INT_REJECT("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  return v;
#undef SP_INT_REJECT
}
sp_int sp_str_to_i_strict_base(const char *s, sp_int base) {
  int none = 0;
  return sp_str_to_i_base_impl(s, base, 0, &none);
}
/* Kernel#Integer(s[, base], exception: false) */
sp_oint sp_str_to_i_lenient_base(const char *s, sp_int base) {
  int none = 0;
  sp_int n = sp_str_to_i_base_impl(s, base, 1, &none);
  return none ? sp_oint_nil() : sp_oint_of(n);
}

/* The decimal text Float() accepts once its underscores are stripped: an
   optional sign, digits, an optional '.' that may have no digit after it
   ("5.", "1.e5"; at least one digit in all), and an optional exponent that
   has a digit of its own. Nothing may follow, so a second point is refused. */
static int sp_float_text_shape_ok(const char *p) {
  int nd = 0;
  if (*p == '+' || *p == '-') p++;
  while (isdigit((unsigned char)*p)) { p++; nd++; }
  if (*p == '.') { p++; while (isdigit((unsigned char)*p)) { p++; nd++; } }
  if (!nd) return 0;
  if (*p == 'e' || *p == 'E') {
    p++;
    if (*p == '+' || *p == '-') p++;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;
  }
  return *p == '\0';
}

/* Kernel#Float() raises ArgumentError on unparseable input. strtod
   on its own would silently return 0.0 for "abc" or empty input;
   match MRI semantics by validating at-least-one-digit + no-trailing-
   junk. Whitespace flanking is fine. Issue #888. */
static sp_float sp_str_to_f_impl(const char *s, int lenient, int *none) {SP_GC_ROOT_STR(s);
  if (!s) { if (lenient) { *none = 1; return 0.0; } sp_raise_cls("ArgumentError", "invalid value for Float(): nil"); }
  /* embedded NUL: the Ruby string extends past its C prefix -- reject rather
     than silently parsing the prefix ("1\\0" is not a float in CRuby). */
  size_t blen = sp_str_byte_len(s);
  if (strlen(s) != blen) goto bad0;
  {
    /* Clean into a buffer, enforcing CRuby's shape rules that strtod is looser
       about: an '_' only BETWEEN two digits of the active base (stripped);
       a hex literal is integral (hex digits only after 0x); at least one real
       digit must appear (rejects "inf"/"nan", which strtod would parse). A
       decimal literal's own shape is checked below once it is copied. */
    size_t n = strlen(s);
    char sbuf[256];
    char *buf = n < sizeof sbuf ? sbuf : (char *)malloc(n + 1);
    if (!buf) { perror("malloc"); exit(1); }
    size_t o = 0;
    int hex = 0, sawdigit = 0, sawp = 0, sawdot = 0;
    const char *q = s;
    while (isspace((unsigned char)*q)) q++;
    const char *start = q;
    if (*q == '+' || *q == '-') buf[o++] = *q++;
    if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) { hex = 1; buf[o++] = *q++; buf[o++] = *q++; }
    for (; *q && !isspace((unsigned char)*q); q++) {
      char ch = *q;
      if (ch == '_') {
        int pd = q > start && (hex ? isxdigit((unsigned char)q[-1]) : isdigit((unsigned char)q[-1]));
        int nd = hex ? isxdigit((unsigned char)q[1]) : isdigit((unsigned char)q[1]);
        if (!(pd && nd)) goto bad;
        continue;                         /* a valid digit separator: strip */
      }
      if (hex) {
        /* hex FLOATS are valid Float() input: 0x1p4 / 0x1.8p-1 (hex digits,
           an optional single point before the mandatory p-exponent, then
           decimal exponent digits with an optional sign) */
        /* sawp/sawdot flags, NOT strchr(buf, ...): buf is not yet
           NUL-terminated inside this loop, so strchr read past the written
           prefix into uninitialized stack (a stray 'p' there failed valid
           inputs like "0xa" depending on the caller's stack residue) */
        if (ch == 'p' || ch == 'P') {
          if (!sawdigit || sawp) goto bad;
          sawp = 1;
          buf[o++] = ch;
          if (q[1] == '+' || q[1] == '-') { buf[o++] = q[1]; q++; }
          if (!isdigit((unsigned char)q[1])) goto bad;
          continue;
        }
        if (sawp) {
          if (!isdigit((unsigned char)ch)) goto bad;
          buf[o++] = ch;
          continue;
        }
        if (ch == '.') {
          if (sawdot || !isxdigit((unsigned char)q[1])) goto bad;
          sawdot = 1;
          buf[o++] = ch;
          continue;
        }
        if (!isxdigit((unsigned char)ch)) goto bad;
        sawdigit = 1;
      }
      else {
        if (isdigit((unsigned char)ch)) sawdigit = 1;
      }
      buf[o++] = ch;
    }
    while (isspace((unsigned char)*q)) q++;
    if (*q || !sawdigit) goto bad;        /* junk after spaces / no digits */
    buf[o] = '\0';
    /* sp_read_float below stops at the first character it cannot use and
       succeeds, so text after a valid prefix ("1.0foo", "1e", "0b10") has to
       be refused here: digits, an optional fraction, then an optional exponent
       with at least one digit of its own. A hex literal was validated above. */
    if (!hex && !sp_float_text_shape_ok(buf)) goto bad;
    {
      char *endptr;
      double v = 0.0;
      /* A hex literal (0x...) is validated integral above and has no decimal
         point, so strtod (which parses hex floats) is safe and locale-neutral;
         a decimal literal uses the locale-independent sp_read_float. The shape
         was already validated, so a short read is malformed input. */
      if (hex) {
        v = strtod(buf, &endptr);
        if (endptr == buf || *endptr != '\0') goto bad;
      }
      else {
        /* The buffer is already a complete, validated float literal (a digit
           appeared, no trailing junk), so sp_read_float's success is enough;
           its endp is left at the start for all-zero input, so don't gate on
           it consuming the buffer. */
        if (!sp_read_float(buf, &endptr, &v)) goto bad;
      }
      if (buf != sbuf) free(buf);
      return (sp_float)v;
    }
  bad:
    if (buf != sbuf) free(buf);
  }
bad0:
  /* Kernel#Float(s, exception: false) answers nil for everything this rejects */
  if (lenient) { *none = 1; return 0.0; }
  sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Float(): \"%s\"", s));
  return 0.0;  /* unreachable */
}
sp_float sp_str_to_f_strict(const char *s)  { int none = 0; return sp_str_to_f_impl(s, 0, &none); }
sp_ofloat sp_str_to_f_lenient(const char *s) { int none = 0; sp_float f = sp_str_to_f_impl(s, 1, &none); return none ? sp_ofloat_nil() : sp_ofloat_of(f); }

/* Kernel#sprintf's float directives (%f/%e/%g/%a with width/flags) are emitted
   by faithfully delegating to libc snprintf, which is locale-sensitive for the
   decimal point. Run that one call under a pinned "C" locale so the output
   always uses '.', matching Ruby, regardless of the process locale. The number
   primitives (Float#to_s / to_f) are locale-free via fp_uscale; this is only
   for the printf-compatible field/flag machinery libc handles best. */
int sp_snprintf_c_float(char *buf, size_t size, const char *fmt, double v) {
#if defined(_WIN32)
  /* the UCRT's %a keeps every trailing zero of the mantissa ("0x1.0000000000000p+0");
     MinGW-w64's own C99 printf trims as glibc does */
  if (strpbrk(fmt, "aA")) return __mingw_snprintf(buf, size, fmt, v);
#endif
  static locale_t sp_c_loc = (locale_t)0;
  if (!sp_c_loc) sp_c_loc = newlocale(LC_ALL_MASK, "C", (locale_t)0);
  if (sp_c_loc) {
    locale_t old = uselocale(sp_c_loc);
    int n = snprintf(buf, size, fmt, v);
    uselocale(old);
    return n;
  }
  return snprintf(buf, size, fmt, v);
}

/* Ruby's float conversions go through its BSD__dtoa (missing/dtoa.c), whose
   fast path generates the digits in double arithmetic and, unlike David
   Gay's original, settles a remainder within its error bound of one half as
   a tie, to even: `format("%.2f", 2.675)` answers 2.68 where C's printf
   answers 2.67 (2.675 is stored as 2.67499999999999982), and
   `format("%.2f", 0.69 / 6)` (0.11499999999999999112) answers 0.12 (#7270).
   The fast path is run here as dtoa runs it (mode 3 for %f with the
   precision as decimals, mode 2 for %e/%g as significant digits), and the
   digits it settles on are rebuilt into the double libc then lays out. Where
   dtoa leaves the fast path for its exact arithmetic (more than 14 digits, a
   cut before the first digit, a denormal), the exact binary value is
   rounded, which is what libc does. *keep_zeros is set when dtoa answers its
   digits untrimmed: a tie settled to even whose exact value lies above the
   half keeps its trailing zeros, which %g then prints (`format("%.5g",
   7.91905000000000001)` is 7.9190). */
static double sp_float_round_ruby(double v, int mode, int ndigits, int *keep_zeros) {
  static const double tens[] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14,
    1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
  static const double bigtens[] = { 1e16, 1e32, 1e64, 1e128, 1e256 };
  if (!isfinite(v) || v == 0.0) return v;
  union { double f; uint64_t u; } x, d2, e;
  double d = v < 0 ? -v : v;
  x.f = d;
  int bexp = (int)((x.u >> 52) & 0x7ff);
  if (bexp == 0) return v;
  /* k: dtoa's estimate of floor(log10(d)), exact when it can check it */
  d2.u = (x.u & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
  int i = bexp - 1023;
  double dsl = (d2.f - 1.5) * 0.289529654602168 + 0.1760912590558 + i * 0.301029995663981;
  int k = (int)dsl;
  if (dsl < 0. && dsl != k) k--;
  int k_check = 1;
  if (k >= 0 && k <= 22) {
    if (d < tens[k]) k--;
    k_check = 0;
  }
  int ilim, ilim1;
  if (mode == 2) {
    if (ndigits <= 0) ndigits = 1;
    ilim = ilim1 = ndigits;
  }
  else {
    ilim = ndigits + k + 1;
    ilim1 = ilim - 1;
  }
  if (ilim <= 0 || ilim > 14) return v;
  int ieps = 2;
  if (k > 0) {
    double ds = tens[k & 0xf];
    int j = k >> 4;
    if (j & 0x10) { j &= 0xf; d /= bigtens[4]; ieps++; }
    for (int n = 0; j; j >>= 1, n++)
      if (j & 1) { ieps++; ds *= bigtens[n]; }
    d /= ds;
  }
  else if (k < 0) {
    int j1 = -k;
    d *= tens[j1 & 0xf];
    for (int j = j1 >> 4, n = 0; j; j >>= 1, n++)
      if (j & 1) { ieps++; d *= bigtens[n]; }
  }
  if (k_check && d < 1.) {
    if (ilim1 <= 0) return v;
    ilim = ilim1; k--; d *= 10.; ieps++;
  }
  e.f = ieps * d + 7.;
  e.u -= (uint64_t)52 << 52;
  double eps = e.f * tens[ilim - 1];
  char s[24];
  int n = 0;
  for (int t = 1;; t++, d *= 10.) {
    long L = (long)d;
    if (!(d -= L)) ilim = t;
    s[n++] = (char)('0' + L);
    if (t == ilim) {
      if (d > 0.5 + eps) goto bump_up;
      if (d < 0.5 - eps) break;
      /* within the error bound of a half: a tie, to even */
      if ((s[n - 1] - '0') & 1) goto bump_up;
      /* dtoa redoes these digits exactly and keeps them whatever the exact
         remainder; above an exact half it does not trim their zeros */
      /* an integer below 1e15 is redone by dtoa's small-integer loop, which
         stops at the last kept digit without trimming */
      if (keep_zeros && x.f == floor(x.f) && k <= 14) *keep_zeros = 1;
      else if (keep_zeros && n < 20) {
        char ex[900];
        snprintf(ex, sizeof ex, "%.800e", v < 0 ? -v : v);
        int dpos = n;               /* digit after the kept ones, in ex's mantissa */
        const char *q = ex; int seen = 0;
        while (*q && *q != 'e') {
          if (*q >= '0' && *q <= '9') {
            if (seen == dpos) break;
            seen++;
          }
          q++;
        }
        if (*q >= '0' && *q <= '9') {
          int above = *q > '5';
          if (*q == '5') for (const char *t = q + 1; *t && *t != 'e'; t++) if (*t != '0') { above = 1; break; }
          *keep_zeros = above;
        }
      }
      break;
    }
  }
  goto rebuild;
bump_up:
  while (n > 0 && s[n - 1] == '9') n--;
  if (n == 0) { s[n++] = '1'; k++; }
  else s[n - 1]++;
rebuild:
  s[n] = 0;
  char buf[64];
  snprintf(buf, sizeof buf, "0.%se%d", s, k + 1);
  char *end = NULL;
  double r = 0;
  if (!sp_read_float(buf, &end, &r) || !isfinite(r)) return v;
  return v < 0 ? -r : r;
}
/* sprintf's float directives: the C-locale delegation above, with Ruby's
   rounding. The conversion decides how many significant digits survive --
   %f counts them from the decimal point, %e from the first digit, %g is a
   significant-digit count of its own. A `*` precision takes an argument this
   call does not receive, so such a format is left to libc. */
int sp_snprintf_ruby_float(char *buf, size_t size, const char *fmt, double v) {
  size_t n = fmt ? strlen(fmt) : 0;
  char conv = n ? fmt[n - 1] : 0;
  if ((conv == 'f' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G') &&
      !strchr(fmt, '*') && isfinite(v) && v != 0.0) {
    const char *dot = strchr(fmt, '.');
    int prec = 6;   /* C's default for all five */
    if (dot) {
      prec = 0;
      for (const char *q = dot + 1; *q >= '0' && *q <= '9'; q++) prec = prec * 10 + (*q - '0');
    }
    int keep_zeros = 0;
    if (conv == 'f') v = sp_float_round_ruby(v, 3, prec, NULL);
    else if (conv == 'e' || conv == 'E') v = sp_float_round_ruby(v, 2, prec + 1, NULL);
    else {
      v = sp_float_round_ruby(v, 2, prec, &keep_zeros);
      /* %g with dtoa's digits untrimmed: the alternate form keeps the zeros;
         a point it adds with no digit after it is not Ruby's */
      if (keep_zeros && !strchr(fmt, '#') && n + 2 < 48) {
        /* the flags and the width are laid out here, around the digits */
        const char *f = fmt + 1;
        int minus = 0, zero = 0, plus = 0, space = 0, width = 0;
        for (;; f++) {
          if (*f == '-') minus = 1;
          else if (*f == '0') zero = 1;
          else if (*f == '+') plus = 1;
          else if (*f == ' ') space = 1;
          else break;
        }
        while (*f >= '0' && *f <= '9' && width < 10000) width = width * 10 + (*f++ - '0');
        if (width > 1000) return sp_snprintf_c_float(buf, size, fmt, v);
        char af[64], body[512];
        snprintf(af, sizeof af, "%%%s%s#%s", plus ? "+" : "", space ? " " : "", f);
        int w = sp_snprintf_c_float(body, sizeof body, af, v);
        if (w < 0 || (size_t)w >= sizeof body) return sp_snprintf_c_float(buf, size, fmt, v);
        char *pt = strchr(body, '.');
        if (pt && (pt[1] < '0' || pt[1] > '9')) { memmove(pt, pt + 1, strlen(pt + 1) + 1); w--; }
        int pad = width > w ? width - w : 0;
        char out[1100]; int o = 0;
        const char *b0 = body;
        if (!minus && zero && pad) {
          if (*b0 == '-' || *b0 == '+' || *b0 == ' ') out[o++] = *b0++;
          while (pad-- > 0) out[o++] = '0';
        }
        else if (!minus) while (pad-- > 0) out[o++] = ' ';
        while (*b0) out[o++] = *b0++;
        if (minus) while (pad-- > 0) out[o++] = ' ';
        out[o] = 0;
        if (size) { size_t c = (size_t)o < size - 1 ? (size_t)o : size - 1; memcpy(buf, out, c); buf[c] = 0; }
        return o;
      }
    }
  }
  return sp_snprintf_c_float(buf, size, fmt, v);
}

/* Cold integer-math and String#oct helpers, moved out of spinel_rt.h
 * so they're compiled once into libspinel_rt.a rather than re-parsed
 * in every generated translation unit. Leaf functions: arithmetic +
 * libc + sp_raise_cls only. */
sp_int sp_gcd(sp_int a,sp_int b){if(a<0)a=-a;if(b<0)b=-b;while(b){sp_int t=b;b=a%b;a=t;}return a;}
sp_int sp_lcm(sp_int a,sp_int b){if(a==0||b==0)return 0;sp_int g=sp_gcd(a,b);if(a<0)a=-a;if(b<0)b=-b;return (a/g)*b;}
sp_int sp_powmod(sp_int base,sp_int exp,sp_int mod){if(exp<0)sp_raise_cls("RangeError","Integer#pow() 1st argument cannot be negative when 2nd argument specified");if(mod==0)sp_raise_cls("ZeroDivisionError","divided by 0");sp_int r=1;sp_int m=mod<0?-mod:mod;if(m==1){r=0;}
else{base=base%m;if(base<0)base+=m;while(exp>0){if(exp%2==1)r=r*base%m;exp=exp/2;base=base*base%m;}}if(mod<0&&r>0)r-=m;return r;}
sp_int sp_ceildiv(sp_int a,sp_int b){if(b==0)sp_raise_cls("ZeroDivisionError","divided by 0");if(b==-1)return -a;sp_int q=a/b;if(a%b!=0&&((a^b)>=0))q++;return q;}
sp_int sp_int_clamp(sp_int v,sp_int lo,sp_int hi){return v<lo?lo:v>hi?hi:v;}
sp_float sp_float_clamp(sp_float v,sp_float lo,sp_float hi){return v<lo?lo:v>hi?hi:v;}
/* Integer square root via Newton's method -- exact for the full
   sp_int range. CRuby raises Math::DomainError on negative input
   (flattened runtime name "Math::DomainError"). The seed is n/2, not
   (n+1)/2: at n == INTPTR_MAX the latter overflows (signed UB), and
   n/2 is a valid Newton seed for all n >= 2. */
sp_int sp_int_sqrt(sp_int n){if(n<0)sp_raise_cls("Math::DomainError","Numerical argument is out of domain - \"isqrt\"");if(n<2)return n;sp_int x=n,y=n/2;while(y<x){x=y;y=(x+n/x)/2;}return x;}
/* Integer#round/ceil/floor/truncate at 10^(-ndigits). Pure integer
   arithmetic (no double precision loss above 2^53). 10^p fits sp_int
   only for p<=18; p>=19 collapses to 0. Round-up multiply is overflow-
   guarded and falls back to the truncated value. */
sp_int sp_ipow10(sp_int p){sp_int f=1;sp_int i=0;while(i<p){f*=10;i++;}return f;}
void sp_int_round_check_ndigits(sp_int nd) {
  if (nd > INT_MAX || nd < INT_MIN)
    sp_raise_cls("RangeError", sp_sprintf("integer %lld too %s to convert to 'int'",
                 (long long)nd, nd < 0 ? "small" : "big"));
}
sp_int sp_int_round(sp_int v,sp_int nd){sp_int_round_check_ndigits(nd);if(nd>=0)return v;sp_int p=-nd;if(p>=SP_INT_POW10_LIMIT)return 0;sp_int f=sp_ipow10(p);sp_int q=v/f,r=v%f,half=f/2;if(v>=0){if(r>=half&&q<INTPTR_MAX/f)return(q+1)*f;return q*f;}if(-r>=half&&q>INTPTR_MIN/f)return(q-1)*f;return q*f;}
sp_int sp_int_ceil(sp_int v,sp_int nd){if(nd>=0)return v;sp_int p=-nd;if(p>=SP_INT_POW10_LIMIT)return 0;sp_int f=sp_ipow10(p);sp_int q=v/f,r=v%f;if(r!=0&&v>0&&q<INTPTR_MAX/f)return(q+1)*f;return q*f;}
sp_int sp_int_floor(sp_int v,sp_int nd){if(nd>=0)return v;sp_int p=-nd;if(p>=SP_INT_POW10_LIMIT)return 0;sp_int f=sp_ipow10(p);sp_int q=v/f,r=v%f;if(r!=0&&v<0&&q>INTPTR_MIN/f)return(q-1)*f;return q*f;}
sp_int sp_int_truncate(sp_int v,sp_int nd){if(nd>=0)return v;sp_int p=-nd;if(p>=SP_INT_POW10_LIMIT)return 0;sp_int f=sp_ipow10(p);return(v/f)*f;}
/* String#oct: prefix auto-detection (0x=hex, 0b=bin, 0o/0=oct, else
   base-8). Matches CRuby. */
/* String#oct: lenient Integer(str, 8)-style parse. Skips leading whitespace,
   accepts an optional sign, an optional base prefix (0x/0b/0o/0d, or a leading
   0 = octal), and single `_` separators between digits. Stops at the first
   character not valid in the selected base (a leading/doubled underscore ends
   parsing too); an unparseable string is 0. A value past sp_int raises
   RangeError, like the sibling Integer() parsers. */
sp_int sp_str_oct(const char*s){SP_GC_ROOT_STR(s);
  if(!s)return 0;
  const char*p=s;
  while(isspace((unsigned char)*p))p++;
  int sign=1;
  if(*p=='+')p++;
  else if(*p=='-'){sign=-1;p++;}
  int base=8;
  if(p[0]=='0'){
    char c=p[1];
    if(c=='x'||c=='X'){base=16;p+=2;}
    else if(c=='b'||c=='B'){base=2;p+=2;}
    else if(c=='o'||c=='O'){base=8;p+=2;}
    else if(c=='d'||c=='D'){base=10;p+=2;}
    /* a bare leading 0 is octal; keep p on the 0 (a valid octal digit) */
  }
  sp_int val=0; int any=0, prev_us=0;
  for(;;){
    char c=*p; int d;
    if(c>='0'&&c<='9')d=c-'0';
    else if(c>='a'&&c<='z')d=c-'a'+10;
    else if(c>='A'&&c<='Z')d=c-'A'+10;
    else if(c=='_'){ if(!any||prev_us)break; prev_us=1; p++; continue; }
    else break;
    if(d>=base)break;
    sp_int t;
    if(sp_ckd_mul_iptr(val,(sp_int)base,&t)||
       sp_ckd_add_iptr(t,(sp_int)d,&val))
      sp_raise_cls("RangeError",sp_sprintf("integer overflow parsing \"%s\"",s));
    any=1; prev_us=0; p++;
  }
  return sign*val;
}

/* Float#round(n) and its siblings at a positive digit count. Scaling by 10**n
   and rounding the product reads the product's OWN representation error as
   part of the value: 64.781995 * 1e5 is 6478199.4999999991, so the digit that
   should round up rounds down and the answer comes out a decimal short. CRuby
   compensates by asking whether the next step up is still <= the input, which
   is what makes 64.781995.round(5) answer 64.782 rather than 64.78199. The
   floor/ceil forms compensate the same way, from the other side.
   nd <= 0 answers an Integer and is the caller's own path; this is the Float
   half. (#3983) */
/* Float#round / #floor / #ceil / #truncate at a POSITIVE digit count, laid
   out as CRuby lays it out (numeric.c: flo_round, rb_float_floor,
   rb_float_ceil). Scaling by a power of ten and rounding the product reads the
   product's OWN representation error as part of the value -- 64.781995 * 1e5
   is 6478199.4999999991, so the digit that should round up rounds down and the
   answer comes out a decimal short (#3983). Each form compensates differently,
   and the asymmetry is deliberate:
     round  compensates on both sides, which is what makes 2.675.round(2)
            answer 2.68 rather than the exact value's 2.67;
     floor  compensates upward only;
     ceil   takes the scaled product as it stands.
   The two guards decide whether the digit being asked for is inside the
   double's reach at all: past it the value is unchanged, short of it the value
   rounds away to zero. A digit count a power of ten cannot represent exactly
   (>= DBL_DIG) goes through the decimal conversion, which is exact, in place
   of CRuby's rational arithmetic. */
/* The round family past DBL_DIG digits, decided on the value's own decimal
   expansion rather than on a scaled product. The cut digit and what follows it
   settle the tie the same way CRuby's rational arithmetic does, which the
   library conversion alone cannot: it breaks exact ties to even, and Ruby's
   default breaks them away from zero. */
static double sp_prec_decimal_round(double x, intptr_t nd, int op) {
  char buf[512];
  int extra = 25;
  snprintf(buf, sizeof buf, "%.*f", (int)nd + extra, x);
  char *dot = strchr(buf, '.');
  if (!dot) return x;
  char *cut = dot + 1 + (int)nd;      /* the first digit past the cut */
  char cutd = *cut;
  int tail_nonzero = 0;
  for (char *q = cut + 1; *q; q++) if (*q != '0') { tail_nonzero = 1; break; }
  *cut = '\0';                        /* the truncation toward zero */
  double r = strtod(buf, NULL);
  int away;
  if (cutd > '5') away = 1;
  else if (cutd < '5') away = 0;
  else if (tail_nonzero) away = 1;
  else if (op == SP_PREC_HALF_DOWN) away = 0;
  else if (op == SP_PREC_HALF_EVEN) {
    char last = cut[-1];
    away = (last >= '0' && last <= '9') ? ((last - '0') % 2 != 0) : 0;
  }
  else away = 1;                      /* the default: ties leave zero behind */
  if (!away) return r;
  double step = 1.0 / pow(10, (double)nd);
  snprintf(buf, sizeof buf, "%.*f", (int)nd, x < 0.0 ? r - step : r + step);
  return strtod(buf, NULL);
}
static int sp_prec_overflow(intptr_t nd, int binexp) {
  return nd >= (DBL_DIG + 2) - (binexp > 0 ? binexp / 4 : binexp / 3 - 1);
}
static int sp_prec_underflow(intptr_t nd, int binexp) {
  return nd < -(binexp > 0 ? binexp / 3 + 1 : binexp / 4);
}
double sp_float_prec_op(double x, intptr_t nd, int op) {
  if (x == 0.0 || !isfinite(x) || nd <= 0) return x;
  if (op == SP_PREC_TRUNC) return sp_float_prec_op(x, nd, signbit(x) ? SP_PREC_CEIL : SP_PREC_FLOOR);
  int binexp = 0;
  frexp(x, &binexp);
  if (sp_prec_overflow(nd, binexp)) return x;
  if (sp_prec_underflow(nd, binexp)) {
    /* round leaves zero whatever the sign; floor keeps a negative value's own
       first decimal step, and ceil keeps a positive one's */
    if (op == SP_PREC_FLOOR && x < 0.0) { /* fall through to the arithmetic */ }
    else if (op == SP_PREC_CEIL && x > 0.0) { /* likewise */ }
    else return 0.0;
  }
  /* Past DBL_DIG digits a power of ten is no longer exact, so the scaled form
     reads its own error as part of the value; CRuby rounds those by rational.
     The decimal conversion is the same exact answer. floor and ceil keep the
     scaled form -- that is what CRuby answers for them. */
  if (nd >= DBL_DIG && op != SP_PREC_FLOOR && op != SP_PREC_CEIL) return sp_prec_decimal_round(x, nd, op);
  double s = pow(10, (double)nd);
  double xs = x * s;
  if (!isfinite(s) || !isfinite(xs)) return x;
  double f;
  switch (op) {
    case SP_PREC_FLOOR: {
      f = floor(xs);
      double res = (f + 1) / s;
      return res > x ? f / s : res;
    }
    case SP_PREC_CEIL:
      return ceil(xs) / s;
    case SP_PREC_HALF_EVEN: {
      /* the tie is decided on the fractional part alone, so the integral part
         is split off first and only rejoined at the end */
      double u, v, us, vs, ff, d, uf;
      v = modf(x, &u);
      us = u * s; vs = v * s;
      if (x > 0.0) {
        ff = floor(vs); uf = us + ff; d = vs - ff;
        if (d > 0.5) d = 1.0;
        else if (d == 0.5 || (uf + 0.5) / s <= x) d = fmod(uf, 2.0);
        else d = 0.0;
        return (us + ff + d) / s;
      }
      ff = ceil(vs); uf = us + ff; d = ff - vs;
      if (d > 0.5) d = 1.0;
      else if (d == 0.5 || (uf - 0.5) / s >= x) d = fmod(-uf, 2.0);
      else d = 0.0;
      return (us + ff - d) / s;
    }
    case SP_PREC_HALF_DOWN:
      f = round(xs);
      if (x > 0) { if ((f - 0.5) / s >= x) f -= 1; }
      else { if ((f + 0.5) / s <= x) f += 1; }
      return f / s;
    default:
      f = round(xs);
      if (x > 0) { if ((f + 0.5) / s <= x) f += 1; }
      else { if ((f - 0.5) / s >= x) f -= 1; }
      return f / s;
  }
}
