/* sp_format.c -- cold value-type display helpers (see sp_format.h).
   Self-contained: the shared value types (sp_types.h) + string allocator
   (sp_alloc.h) + libc formatting, and sp_time.c's Time renderer. */
#include "sp_format.h"
#include "sp_alloc.h"   /* sp_str_alloc_raw, sp_raise_cls, <math.h> for cos/sin/sqrt */
#include "sp_dtoa.h"    /* sp_format_float (locale-independent %g) */
#include <stdio.h>
#include <limits.h>    /* LLONG_MIN */
#include <string.h>
#include "sp_time.h"   /* sp_time_inspect_v / sp_time_to_s_v */
#include "sp_range.h"  /* sp_range_inspect */

/* Format a non-negative Complex-component magnitude the way MRI does: infinite
   and NaN values become the Ruby names Infinity/NaN (not C's inf/nan), a
   Float-classed component (is_f) renders Ruby-float style ("2.0"), a whole
   Integer-classed value stays integer-looking, else %g. Returns the length. */
static int sp_complex_mag(char *out, size_t sz, sp_float v, int is_f) {SP_GC_ROOT_STR(out);
  if (isinf(v)) return snprintf(out, sz, "Infinity");
  if (isnan(v)) return snprintf(out, sz, "NaN");
  if (is_f) return snprintf(out, sz, "%s", sp_float_to_s(v));
  /* a float outside sp_int's range makes (sp_int)v undefined behavior; only
     integer-print a whole value that fits, else fall through to %g. */
  if (v >= -(sp_float)INTPTR_MAX && v <= (sp_float)INTPTR_MAX && v == (sp_int)v)
    return snprintf(out, sz, "%lld", (long long)v);
  { char fb[32]; sp_format_float(v, fb, sizeof fb, 'g', -2, '\0'); return snprintf(out, sz, "%s", fb); }
}
/* Append the imaginary part ("+<mag>i" / "-<mag>i") to buf. MRI inserts a `*`
   before a non-numeric magnitude (Infinity/NaN) so `Infinity*i` stays readable,
   while a plain number is written as `10i`. */
static int sp_complex_imag(char *buf, int n, size_t sz, sp_float im, int is_f) {SP_GC_ROOT_STR(buf);
  if (n < 0 || (size_t)n >= sz) return 0;
  char mag[64];
  sp_complex_mag(mag, sizeof mag, im < 0 ? -im : im, is_f);
  const char *sep = (mag[0] >= '0' && mag[0] <= '9') ? "" : "*";
  return snprintf(buf + n, sz - (size_t)n, "%c%s%si", im < 0 ? '-' : '+', mag, sep);
}
const char *sp_complex_inspect(sp_Complex c) {
  char buf[128], re[64];
  sp_complex_mag(re, sizeof re, c.re < 0 ? -c.re : c.re, c.fl & SP_CPLX_RE_F);
  int n = snprintf(buf, sizeof buf, "(%s%s", c.re < 0 ? "-" : "", re);
  if (n < 0) n = 0; else if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
  n += sp_complex_imag(buf, n, sizeof buf, c.im, c.fl & SP_CPLX_IM_F);
  if (n < (int)sizeof buf) n += snprintf(buf + n, sizeof(buf) - (size_t)n, ")");
  if (n < 0) n = 0; else if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
  char *r = sp_str_alloc_raw(n + 1);
  memcpy(r, buf, n);
  r[n] = 0;
  return r;
}
/* Complex#to_s: bare `re+imi` (no surrounding parens, unlike #inspect). */
const char *sp_complex_to_s(sp_Complex c) {
  char buf[128], re[64];
  sp_complex_mag(re, sizeof re, c.re < 0 ? -c.re : c.re, c.fl & SP_CPLX_RE_F);
  int n = snprintf(buf, sizeof buf, "%s%s", c.re < 0 ? "-" : "", re);
  if (n < 0) n = 0; else if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
  n += sp_complex_imag(buf, n, sizeof buf, c.im, c.fl & SP_CPLX_IM_F);
  if (n < 0) n = 0; else if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
  char *r = sp_str_alloc_raw(n + 1);
  memcpy(r, buf, n);
  r[n] = 0;
  return r;
}

const char *sp_rational_inspect(sp_Rational r) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "(%lld/%lld)", (long long)r.num, (long long)r.den);
  if (n < 0) n = 0;
  char *o = sp_str_alloc_raw(n + 1);
  memcpy(o, buf, n);
  o[n] = 0;
  return o;
}
/* Rational#to_s: bare `num/den` (no parens, unlike #inspect). */
const char *sp_rational_to_s(sp_Rational r) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%lld/%lld", (long long)r.num, (long long)r.den);
  if (n < 0) n = 0;
  char *o = sp_str_alloc_raw(n + 1);
  memcpy(o, buf, n);
  o[n] = 0;
  return o;
}

/* A boxed Range renders as the typed one does: an open side is left out
   ("..3", "1.."). */
const char *sp_Range_inspect(sp_Range *r) {
  return sp_range_inspect(*r);
}

/* A boxed Time renders as an unboxed one does (lib/sp_time.c): its own
   zone kind and offset, the year in Ruby's form. A gmtime + strftime
   rendering here called every Time UTC, and C's %Y left a year below 1000
   unpadded on glibc. */
const char *sp_Time_inspect(sp_Time *t) {SP_GC_ROOT(t); return sp_time_inspect_v(*t); }
const char *sp_Time_to_s(sp_Time *t)    {SP_GC_ROOT(t); return sp_time_to_s_v(*t); }

/* ---- Complex arithmetic ---- */
/* Component-class (fl) propagation mirrors CRuby's numeric tower: add/sub act
   per component, so each Float bit carries through independently; mul/div mix
   all four components, so any Float input floats both results. */
/* Complex.polar: CRuby resolves an angle of exactly 0, pi/2, or pi without
   cos/sin (the magnitude keeps its class, the other component is 0.0);
   any other angle computes both components as Floats. m_is_f carries the
   magnitude's static class from codegen. */
sp_Complex sp_complex_polar(sp_float m, sp_float a, int m_is_f) {
  sp_Complex c;
  int mf = m_is_f ? 1 : 0;
  if (a == 0)      { c.re = m;  c.im = 0.0; c.fl = (unsigned char)((mf ? SP_CPLX_RE_F : 0) | SP_CPLX_IM_F); return c; }
  if (a == M_PI_2) { c.re = 0.0; c.im = m;  c.fl = (unsigned char)(SP_CPLX_RE_F | (mf ? SP_CPLX_IM_F : 0)); return c; }
  if (a == M_PI)   { c.re = -m; c.im = 0.0; c.fl = (unsigned char)((mf ? SP_CPLX_RE_F : 0) | SP_CPLX_IM_F); return c; }
  c.re = m * cos(a); c.im = m * sin(a); c.fl = SP_CPLX_RE_F | SP_CPLX_IM_F;
  return c;
}
sp_Complex sp_complex_add(sp_Complex a, sp_Complex b) { sp_Complex c; c.re = a.re + b.re; c.im = a.im + b.im; c.fl = a.fl | b.fl; return c; }
sp_Complex sp_complex_mul(sp_Complex a, sp_Complex b) { sp_Complex c; c.re = (a.re * b.re) - (a.im * b.im); c.im = (a.re * b.im) + (a.im * b.re); c.fl = (a.fl | b.fl) ? (SP_CPLX_RE_F | SP_CPLX_IM_F) : 0; return c; }
sp_Complex sp_complex_conjugate(sp_Complex a) { sp_Complex c; c.re = a.re; c.im = -a.im; c.fl = a.fl; return c; }
sp_Complex sp_complex_sub(sp_Complex a, sp_Complex b) { sp_Complex c; c.re = a.re - b.re; c.im = a.im - b.im; c.fl = a.fl | b.fl; return c; }
sp_Complex sp_complex_div(sp_Complex a, sp_Complex b) {
  sp_float d = (b.re * b.re) + (b.im * b.im); sp_Complex c;
  c.re = ((a.re * b.re) + (a.im * b.im)) / d; c.im = ((a.im * b.re) - (a.re * b.im)) / d;
  c.fl = (a.fl | b.fl) ? (SP_CPLX_RE_F | SP_CPLX_IM_F) : 0;
  return c;
}
/* Complex divided by a real scalar divides each component -- unlike the
   conjugate formula, this yields Infinity (not NaN) when the divisor is 0.0,
   matching MRI's Complex#/ against a Float. */
sp_Complex sp_complex_div_real(sp_Complex a, sp_float b) {
  sp_Complex c; c.re = a.re / b; c.im = a.im / b; c.fl = SP_CPLX_RE_F | SP_CPLX_IM_F; return c;
}
/* Complex divided by an Integer follows integer zero-division rules: dividing
   by 0 raises ZeroDivisionError, as in MRI (a Float divisor gives Infinity). */
sp_Complex sp_complex_div_int(sp_Complex a, sp_int b) {
  if (b == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  sp_Complex c; c.re = a.re / (sp_float)b; c.im = a.im / (sp_float)b; c.fl = a.fl; return c;
}
sp_Complex sp_complex_neg(sp_Complex a) { sp_Complex c; c.re = -a.re; c.im = -a.im; c.fl = a.fl; return c; }
sp_float sp_complex_abs2(sp_Complex a) { return (a.re * a.re) + (a.im * a.im); }
sp_float sp_complex_abs(sp_Complex a) { return sqrt((a.re * a.re) + (a.im * a.im)); }
sp_bool sp_complex_eq(sp_Complex a, sp_Complex b) { return a.re == b.re && a.im == b.im; }
/* z ** w for a non-integer exponent: exp(w * ln z); an integral real w
   defers to the exact integer power (preserving component classes). */
sp_Complex sp_complex_pow_c(sp_Complex z, sp_Complex w) {
  if (w.im == 0.0 && w.re == (sp_float)(sp_int)w.re && !(w.fl & SP_CPLX_RE_F))
    return sp_complex_pow(z, (sp_int)w.re);
  sp_float lr = 0.5 * log(z.re * z.re + z.im * z.im);
  sp_float th = atan2(z.im, z.re);
  sp_float xa = w.re * lr - w.im * th;
  sp_float xb = w.re * th + w.im * lr;
  sp_float m = exp(xa);
  sp_Complex r;
  r.re = m * cos(xb);
  r.im = m * sin(xb);
  r.fl = SP_CPLX_RE_F | SP_CPLX_IM_F;
  return r;
}
sp_Complex sp_complex_pow(sp_Complex a, sp_int e) {
  if (e < 0 && a.re == 0.0 && a.im == 0.0)
    sp_raise_cls("ZeroDivisionError", "divided by 0");   /* (#2965) */
  sp_Complex r; r.re = 1; r.im = 0; r.fl = 0;
  sp_int k = e < 0 ? -e : e;
  for (sp_int i = 0; i < k; i++) r = sp_complex_mul(r, a);
  if (e < 0) { sp_Complex one; one.re = 1; one.im = 0; one.fl = 0; r = sp_complex_div(one, r); }
  return r;
}
/* Complex ** Rational: a whole-number exponent (den 1) stays exact via the
   integer power; a fractional exponent computes in floats. (#2962) */
sp_Complex sp_complex_pow_rational(sp_Complex z, sp_Rational w) {
  if (w.den == 1) return sp_complex_pow(z, w.num);
  return sp_complex_pow_c(z, (sp_Complex){sp_rational_to_f(w), 0, SP_CPLX_RE_F | SP_CPLX_IM_F});
}

/* ---- Rational arithmetic ----
   Intermediate products use a wider type; a result that does not fit back into
   sp_int raises RangeError (mruby promotes to Bigint -- a later phase can too).
   A 128-bit integer covers the 64-bit build (see sp_rat_wide below); int64 covers two int32 operands losslessly. */
static sp_int sp_rational_gcd_i(sp_int a, sp_int b) {
  if (a < 0) a = -a;
  if (b < 0) b = -b;
  while (b) { sp_int t = b; b = a % b; a = t; }
  return a;
}
sp_Rational sp_rational_new(sp_int n, sp_int d) {
  sp_Rational r;
  if (d == 0) { r.num = n; r.den = 0; return r; }
  if (d < 0) { n = -n; d = -d; }
  sp_int g = sp_rational_gcd_i(n, d);
  if (g <= 0) g = 1;
  r.num = n / g;
  r.den = d / g;
  return r;
}
/* Kernel#Rational(String): unlike String#to_r, the whole string must be a
   rational literal -- trailing text, an empty string and a second `/` are all
   ArgumentError, and an exponent is honoured (#3720). */
sp_Rational sp_str_to_r_strict(const char *s) {SP_GC_ROOT_STR(s);
  const char *p = s;
  if (!p) sp_raise_cls("TypeError", "can't convert nil into Rational");
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') p++;
  sp_int sign = 1;
  if (*p == '+') p++;
  else if (*p == '-') { sign = -1; p++; }
  sp_int num = 0, den = 1;
  int any = 0;
  while ((*p >= '0' && *p <= '9') || *p == '_') { if (*p != '_') { num = num * 10 + (*p - '0'); any = 1; } p++; }
  if (*p == '.') {
    p++;
    while ((*p >= '0' && *p <= '9') || *p == '_') { if (*p != '_') { num = num * 10 + (*p - '0'); den *= 10; any = 1; } p++; }
  }
  if (!any) {
    if (sp_convert_soft) { sp_convert_failed = 1; return sp_rational_new(0, 1); }
    sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Rational(): \"%s\"", s));
  }
  if (*p == 'e' || *p == 'E') {
    const char *q = p + 1;
    sp_int esign = 1;
    if (*q == '+') q++; else if (*q == '-') { esign = -1; q++; }
    sp_int ev = 0; int anye = 0;
    while (*q >= '0' && *q <= '9') { ev = ev * 10 + (*q - '0'); q++; anye = 1; }
    if (anye) {
      for (sp_int k = 0; k < ev; k++) { if (esign > 0) num *= 10; else den *= 10; }
      p = q;
    }
  }
  if (*p == '/') {
    p++;
    sp_int d2 = 0; int anyd = 0;
    while ((*p >= '0' && *p <= '9') || *p == '_') { if (*p != '_') { d2 = d2 * 10 + (*p - '0'); anyd = 1; } p++; }
    if (!anyd) sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Rational(): \"%s\"", s));
    if (d2 == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
    den *= d2;
  }
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') p++;
  if (*p) sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Rational(): \"%s\"", s));
  return sp_rational_new(sign * num, den);
}
/* The products of two sp_int fit a 128-bit integer on the 64-bit build and a
   long long on the 32-bit one. A 64-bit build without a 128-bit type
   (sp_compat.h) computes in long long and checks every step, raising the
   RangeError an out-of-range result raises anyway -- earlier than the wide
   type would, for an intermediate the reduction would have brought back. */
#if INTPTR_MAX > 0x7fffffff && SP_HAVE_INT128
typedef sp_int128 sp_rat_wide;
# define SP_RAT_MUL(a, b) ((sp_rat_wide)(a) * (b))
# define SP_RAT_ADD(a, b) ((a) + (b))
# define SP_RAT_SUB(a, b) ((a) - (b))
#elif INTPTR_MAX > 0x7fffffff
typedef long long sp_rat_wide;
static void sp_rat_ovf(void) { sp_raise_cls("RangeError", "Rational out of sp_int range"); }
static sp_rat_wide sp_rat_mul_ck(sp_rat_wide a, sp_rat_wide b) {
  intptr_t r; if (sp_ckd_mul_iptr((intptr_t)a, (intptr_t)b, &r)) sp_rat_ovf(); return r;
}
static sp_rat_wide sp_rat_add_ck(sp_rat_wide a, sp_rat_wide b) {
  intptr_t r; if (sp_ckd_add_iptr((intptr_t)a, (intptr_t)b, &r)) sp_rat_ovf(); return r;
}
static sp_rat_wide sp_rat_sub_ck(sp_rat_wide a, sp_rat_wide b) {
  intptr_t r; if (sp_ckd_sub_iptr((intptr_t)a, (intptr_t)b, &r)) sp_rat_ovf(); return r;
}
# define SP_RAT_MUL(a, b) sp_rat_mul_ck((sp_rat_wide)(a), (sp_rat_wide)(b))
# define SP_RAT_ADD(a, b) sp_rat_add_ck(a, b)
# define SP_RAT_SUB(a, b) sp_rat_sub_ck(a, b)
#else
typedef long long sp_rat_wide;
# define SP_RAT_MUL(a, b) ((sp_rat_wide)(a) * (b))
# define SP_RAT_ADD(a, b) ((a) + (b))
# define SP_RAT_SUB(a, b) ((a) - (b))
#endif
/* -2**63 fits (a numerator or an answer of exactly that value, as CRuby);
   the negations below raise for it rather than overflow */
static sp_int sp_rat_fit(sp_rat_wide v) {
  if (v > (sp_rat_wide)INTPTR_MAX || v < (sp_rat_wide)INTPTR_MIN)
    sp_raise_cls("RangeError", "Rational out of sp_int range");
  return (sp_int)v;
}
/* A rational from a 64-bit numerator and denominator, reduced before it is
   narrowed: Time#to_r's nanoseconds do not fit a 32-bit sp_int, its seconds
   do (1577923200/1), and only a value that stays too wide after the
   reduction raises. */
sp_Rational sp_rational_new_i64(int64_t n, int64_t d);
static sp_Rational sp_rational_new_wide(sp_rat_wide n, sp_rat_wide d) {
  if (d == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  if (d < 0) {
    if (n == LLONG_MIN || d == LLONG_MIN) sp_raise_cls("RangeError", "Rational out of sp_int range");
    n = -n; d = -d;
  }
  /* the gcd over magnitudes taken unsigned: |-2**63| is no sp_rat_wide */
  unsigned long long a = n < 0 ? 0ULL - (unsigned long long)n : (unsigned long long)n, b = (unsigned long long)d;
  while (b) { unsigned long long t = b; b = a % b; a = t; }
  if (a == 0) a = 1;
  sp_Rational r;
  r.num = sp_rat_fit(n / (sp_rat_wide)a);
  r.den = sp_rat_fit(d / (sp_rat_wide)a);
  return r;
}
sp_Rational sp_rational_new_i64(int64_t n, int64_t d) { return sp_rational_new_wide((sp_rat_wide)n, (sp_rat_wide)d); }
/* ---- String#to_r ----
   A number is read as CRuby's read_num reads it: digits, an optional `.` and
   digits, an optional exponent (`e`, a sign, digits), a single `_` allowed
   between two digits (and after the zeros that begin a run); a `.` with no
   digit after it is consumed, and so are an `e` and its sign with no digit
   after them. The text is [ws][sign]number[/number]; it stops at the first
   byte that fits nothing, and an unparseable String is 0/1.

   A number is kept as a magnitude m in the wide type and a power of ten e,
   the trailing zeros of its digits counted into e rather than multiplied in,
   so 1.000000000000000000000 and 1000000000000000000000e-21 are 1. The two
   sides meet as (m1/m2) * 10^(e1 - e2) with the gcd of the magnitudes taken
   first and the powers of 2 and 5 of the ten cancelled against the side they
   divide, so 5e40/1e40 is 5 and 5e-19 is 1/(2*10^18). A magnitude past the
   wide type, an exponent past a quarter of it and a result that does not fit
   64 bits are a RangeError (CRuby answers a Bignum Rational or a Float, or
   raises for an exponent it cannot use), raised unless the other side is
   zero: 0/<huge> is 0/1 and <huge>/0 is a ZeroDivisionError. */
#define SP_RATW_TOP ((sp_rat_wide)1 << (sizeof(sp_rat_wide) * 8 - 2))
#define SP_RATW_MAX (SP_RATW_TOP - 1 + SP_RATW_TOP)
typedef struct { sp_rat_wide m; sp_rat_wide e; int ovf; } sp_str_to_r_q;
static sp_rat_wide sp_str_to_r_step(sp_rat_wide m, sp_int add, sp_int mul, int *ovf) {
  if (*ovf) return m;
  if (m > (SP_RATW_MAX - add) / mul) { *ovf = 1; return m; }
  return m * mul + add;
}
static sp_rat_wide sp_str_to_r_gcd(sp_rat_wide a, sp_rat_wide b) {
  while (b) { sp_rat_wide t = a % b; a = b; b = t; }
  return a;
}
/* a run of digits into the magnitude *m, trailing zeros held back in *z */
static const char *sp_str_to_r_run(const char *p, sp_rat_wide *m, sp_int *z, sp_int *nd, int *ovf) {
  if (*p == '0') {
    /* the zeros that begin a run, and a `_` between them, are read together
       (CRuby squeezes them at the start of the integer, the fraction and the
       exponent alike) */
    int us = 0;
    while (*p == '0' || *p == '_') {
      if (*p == '_') {
        if (++us >= 2) break;
      }
      else {
        us = 0;
        (*nd)++;
        if (*m != 0) (*z)++;
      }
      p++;
    }
  }
  while (*p >= '0' && *p <= '9') {
    sp_int d = (sp_int)(*p - '0');
    if (d == 0) {
      if (*m != 0) (*z)++;
    }
    else {
      for (; *z > 0 && !*ovf; (*z)--) *m = sp_str_to_r_step(*m, 0, 10, ovf);
      *z = 0;
      *m = sp_str_to_r_step(*m, d, 10, ovf);
    }
    (*nd)++; p++;
    if (*p == '_' && p[1] >= '0' && p[1] <= '9') p++;
  }
  return p;
}
static int sp_str_to_r_number(const char **pp, sp_str_to_r_q *q) {
  const char *p = *pp;
  sp_rat_wide m = 0;
  sp_int z = 0, nd = 0, fd = 0;
  sp_rat_wide e = 0;
  int ovf = 0, eneg = 0, ehuge = 0;
  if (*p != '.') {
    p = sp_str_to_r_run(p, &m, &z, &nd, &ovf);
    if (!nd) return 0;
  }
  if (*p == '.') {
    p++;
    p = sp_str_to_r_run(p, &m, &z, &fd, &ovf);
    if (!fd) goto scaled;
  }
  if ((*p == 'e' || *p == 'E') && p[1]) {
    p++;
    if (*p == '+') p++;
    else if (*p == '-') { eneg = 1; p++; }
    sp_rat_wide ev = 0;
    sp_int ez = 0, ed = 0;
    int eovf = 0;
    const char *t = sp_str_to_r_run(p, &ev, &ez, &ed, &eovf);
    if (ed) {
      p = t;
      for (; ez > 0 && !eovf; ez--) ev = sp_str_to_r_step(ev, 0, 10, &eovf);
      if (eovf || ev > (SP_RATW_TOP >> 1)) {
        ehuge = 1;
      }
      else {
        e = ev;
      }
    }
  }
scaled:
  *pp = p;
  q->m = 0; q->e = 0; q->ovf = 0;
  if (m == 0 && !ovf) return 1;
  if (ovf || ehuge) { q->ovf = 1; return 1; }
  q->m = m;
  q->e = (sp_rat_wide)z - fd + (eneg ? -e : e);
  return 1;
}
sp_Rational sp_str_to_r(const char *s) {SP_GC_ROOT_STR(s);
  if (!s) return sp_rational_new(0, 1);
  const char *p = s;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') p++;
  int neg = 0;
  if (*p == '+') p++;
  else if (*p == '-') { neg = 1; p++; }
  sp_str_to_r_q a, b;
  int hb = 0;
  if (!sp_str_to_r_number(&p, &a)) return sp_rational_new(0, 1);
  if (*p == '/') { const char *t = p + 1; hb = sp_str_to_r_number(&t, &b); }
  if (hb && !b.ovf && b.m == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  if (!a.ovf && a.m == 0) return sp_rational_new(0, 1);
  if (a.ovf || (hb && b.ovf)) sp_raise_cls("RangeError", "Rational out of sp_int range");
  sp_rat_wide n = a.m, d = 1;
  sp_rat_wide k = a.e;
  if (hb) {
    sp_rat_wide g = sp_str_to_r_gcd(n, b.m);
    n /= g;
    d = b.m / g;
    k -= b.e;
  }
  int ovf = 0;
  sp_rat_wide c2 = 0, c5 = 0;
  if (k >= 0) {
    while (c2 < k && d % 2 == 0) { d /= 2; c2++; }
    while (c5 < k && d % 5 == 0) { d /= 5; c5++; }
    for (sp_rat_wide i = c2; i < k && !ovf; i++) n = sp_str_to_r_step(n, 0, 2, &ovf);
    for (sp_rat_wide i = c5; i < k && !ovf; i++) n = sp_str_to_r_step(n, 0, 5, &ovf);
  }
  else {
    sp_rat_wide kk = -k;
    while (c2 < kk && n % 2 == 0) { n /= 2; c2++; }
    while (c5 < kk && n % 5 == 0) { n /= 5; c5++; }
    for (sp_rat_wide i = c2; i < kk && !ovf; i++) d = sp_str_to_r_step(d, 0, 2, &ovf);
    for (sp_rat_wide i = c5; i < kk && !ovf; i++) d = sp_str_to_r_step(d, 0, 5, &ovf);
  }
  if (ovf) sp_raise_cls("RangeError", "Rational out of sp_int range");
  /* -INTPTR_MAX - 1 fits sp_int but not sp_rat_fit's symmetric range */
  sp_Rational r;
  if (sizeof(sp_rat_wide) > sizeof(sp_int) && neg && n - 1 == (sp_rat_wide)INTPTR_MAX) {
    if (d > (sp_rat_wide)INTPTR_MAX) sp_raise_cls("RangeError", "Rational out of sp_int range");
    r.num = (sp_int)(-INTPTR_MAX - 1); r.den = (sp_int)d;
    return r;
  }
  if (n > (sp_rat_wide)INTPTR_MAX || d > (sp_rat_wide)INTPTR_MAX) sp_raise_cls("RangeError", "Rational out of sp_int range");
  r.num = neg ? -(sp_int)n : (sp_int)n; r.den = (sp_int)d;
  return r;
}
sp_Rational sp_rational_add(sp_Rational a, sp_Rational b) {
  return sp_rational_new_wide(SP_RAT_ADD(SP_RAT_MUL(a.num, b.den), SP_RAT_MUL(b.num, a.den)),
                              SP_RAT_MUL(a.den, b.den));
}
sp_Rational sp_rational_sub(sp_Rational a, sp_Rational b) {
  return sp_rational_new_wide(SP_RAT_SUB(SP_RAT_MUL(a.num, b.den), SP_RAT_MUL(b.num, a.den)),
                              SP_RAT_MUL(a.den, b.den));
}
sp_Rational sp_rational_mul(sp_Rational a, sp_Rational b) {
  return sp_rational_new_wide(SP_RAT_MUL(a.num, b.num), SP_RAT_MUL(a.den, b.den));
}
sp_Rational sp_rational_div(sp_Rational a, sp_Rational b) {
  if (b.num == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  return sp_rational_new_wide(SP_RAT_MUL(a.num, b.den), SP_RAT_MUL(a.den, b.num));
}
sp_Rational sp_rational_neg(sp_Rational a) { if (a.num == INTPTR_MIN) sp_raise_cls("RangeError", "Rational out of sp_int range"); a.num = -a.num; return a; }
sp_Rational sp_rational_abs(sp_Rational a) { if (a.num == INTPTR_MIN) sp_raise_cls("RangeError", "Rational out of sp_int range"); if (a.num < 0) a.num = -a.num; return a; }
sp_int sp_rational_cmp(sp_Rational a, sp_Rational b) {
  sp_rat_wide l = SP_RAT_MUL(a.num, b.den), r = SP_RAT_MUL(b.num, a.den);
  return l < r ? -1 : (l > r ? 1 : 0);
}
sp_bool sp_rational_eq(sp_Rational a, sp_Rational b) {
  return a.num == b.num && a.den == b.den;
}
sp_float sp_rational_to_f(sp_Rational a) {
  return (sp_float)a.num / (sp_float)a.den;
}
/* 2^e as a wide integer, raising RangeError past the sp_int range. */
static sp_rat_wide sp_rat_pow2(int e) {
  sp_rat_wide r = 1;
  for (int i = 0; i < e; i++) {
    r <<= 1;
    if (r > (sp_rat_wide)INTPTR_MAX) sp_raise_cls("RangeError", "Rational out of sp_int range");
  }
  return r;
}
/* Float#to_r : the exact rational value of the double. A finite double equals
   m * 2^e for a 53-bit integer mantissa m, so the conversion is exact. When the
   exact numerator/denominator does not fit in sp_int (huge magnitude or a tiny
   subnormal), it raises RangeError -- spinel's Rational is int64/int64 and does
   not promote to a Bignum (matching the existing Rational-overflow behavior). */
sp_Rational sp_float_to_rational(sp_float f) {
  if (isnan(f) || isinf(f)) sp_raise_cls("FloatDomainError", isnan(f) ? "NaN" : "Infinity");
  if (f == 0.0) { sp_Rational z; z.num = 0; z.den = 1; return z; }
  int e;
  double m = frexp((double)f, &e);          /* f = m * 2^e, 0.5 <= |m| < 1 */
  for (int i = 0; i < 53 && m != floor(m); i++) { m *= 2.0; e--; }
  sp_rat_wide num = (sp_rat_wide)m;          /* integer mantissa (fits in 54 bits) */
  if (e >= 0) {
    /* num * 2^e can exceed sp_rat_wide on a 32-bit build (long long); guard the
       product first. p > INTPTR_MAX/|num| is exactly "num*p won't fit sp_int",
       so this raises rather than overflowing. */
    sp_rat_wide p = sp_rat_pow2(e), an = num < 0 ? -num : num;
    if (an != 0 && p > (sp_rat_wide)INTPTR_MAX / an)
      sp_raise_cls("RangeError", "Rational out of sp_int range");
    return sp_rational_new_wide(num * p, 1);
  }
  return sp_rational_new_wide(num, sp_rat_pow2(-e));
}
/* Simplest p/q with lo <= p/q <= hi, for 0 < lo <= hi. Classic continued-fraction
   "simplest rational in an interval" recursion; the convergent depth is bounded by
   the interval, so q stays small for sane epsilons. */
static void sp_simplest_pos(double lo, double hi, sp_rat_wide *np, sp_rat_wide *dp) {
  double fl = floor(lo);
  if (fl == lo) { *np = (sp_rat_wide)fl; *dp = 1; return; }   /* lo is an integer */
  if (fl == floor(hi)) {                                       /* no integer in (lo,hi) */
    sp_rat_wide n, d;
    sp_simplest_pos(1.0 / (hi - fl), 1.0 / (lo - fl), &n, &d);
    *np = SP_RAT_ADD(SP_RAT_MUL(fl, n), d);
    *dp = n;
    return;
  }
  *np = (sp_rat_wide)fl + 1; *dp = 1;                          /* fl+1 lies in [lo,hi] */
}
static sp_Rational sp_rationalize_interval(double lo, double hi) {
  if (lo > hi) { double t = lo; lo = hi; hi = t; }
  if (lo <= 0.0 && hi >= 0.0) { sp_Rational z; z.num = 0; z.den = 1; return z; }
  int neg = 0;
  if (hi < 0.0) { double t = -lo; lo = -hi; hi = t; neg = 1; } /* fold to positives */
  /* Any p/q in [lo,hi] has p >= lo*q >= lo, so lo past sp_int can't fit and
     floor(lo) would also overflow the (double->sp_rat_wide) cast below. */
  if (lo > (double)INTPTR_MAX) sp_raise_cls("RangeError", "Rational out of sp_int range");
  sp_rat_wide n, d;
  sp_simplest_pos(lo, hi, &n, &d);
  if (neg) n = -n;
  return sp_rational_new_wide(n, d);
}
sp_Rational sp_float_rationalize(sp_float f, sp_float eps) {
  if (isnan(f) || isinf(f)) sp_raise_cls("FloatDomainError", isnan(f) ? "NaN" : "Infinity");
  double e = eps < 0 ? -eps : eps;
  return sp_rationalize_interval((double)f - e, (double)f + e);
}
/* No-arg rationalize: simplest rational that round-trips to this exact double,
   i.e. lying in the half-ulp interval around f. */
/* No-arg rationalize: the simplest rational whose nearest double is exactly f.
   The continued-fraction convergents of f are, by construction, the simplest
   rationals approximating it in increasing complexity, so the FIRST convergent
   that round-trips to f is the answer. This is robust where a double-precision
   half-ulp interval search is not: computing the interval bounds in `double`
   rounds them back to f for an exactly-representable value (collapsing the
   interval) and amplifies rounding at the ulp scale for others. */
sp_Rational sp_float_rationalize0(sp_float f) {
  if (isnan(f) || isinf(f)) sp_raise_cls("FloatDomainError", isnan(f) ? "NaN" : "Infinity");
  if (f == 0.0) { sp_Rational z; z.num = 0; z.den = 1; return z; }
  int neg = f < 0.0;
  double x = neg ? -(double)f : (double)f;
  double v = x;
  sp_rat_wide h0 = 1, h1 = 0, k0 = 0, k1 = 1;  /* convergent num/den recurrences */
  sp_rat_wide num = 0, den = 1;
  int found = 0;
  for (int i = 0; i < 64 && !found; i++) {
    /* floor(v) out of sp_rat_wide range would make the cast below UB; any such
       convergent exceeds INTPTR_MAX anyway, so bail to the exact-ratio fallback. */
    if (v > (double)INTPTR_MAX) break;
    sp_rat_wide a = (sp_rat_wide)floor(v);
    /* guard the convergent against sp_int overflow before forming it (all in
       integer arithmetic: a*h0 + h1 <= INTPTR_MAX). */
    if (a > 0 && (h0 > ((sp_rat_wide)INTPTR_MAX - h1) / a ||
                  k0 > ((sp_rat_wide)INTPTR_MAX - k1) / a))
      break;
    sp_rat_wide h = a * h0 + h1;
    sp_rat_wide k = a * k0 + k1;
    num = h; den = k;
    if (k != 0 && (double)h / (double)k == x) { found = 1; break; }
    h1 = h0; h0 = h; k1 = k0; k0 = k;
    double frac = v - a;
    if (frac == 0.0) break;
    v = 1.0 / frac;
  }
  if (!found) return sp_float_to_rational(f);  /* fallback: exact bit ratio */
  return sp_rational_new_wide(neg ? -num : num, den);
}
/* Rational#round with no digits: nearest integer, ties away from zero
   (CRuby's Rational#round default; den > 0 by construction). */
sp_int sp_rational_round_i(sp_Rational a) {
  sp_rat_wide q = a.num / a.den, r = a.num % a.den;
  if (r != 0) {
    sp_rat_wide r2 = (r < 0 ? -r : r) * 2;
    if (r2 >= a.den) q += a.num < 0 ? -1 : 1;
  }
  return sp_rat_fit(q);
}
/* Rational#round(half: :even): ties go to the even neighbor (#3047). */
sp_int sp_rational_round_i_even(sp_Rational a) {
  sp_rat_wide q = a.num / a.den, r = a.num % a.den;
  if (r != 0) {
    sp_rat_wide r2 = (r < 0 ? -r : r) * 2;
    if (r2 > a.den) q += a.num < 0 ? -1 : 1;           /* past the half: round away */
    else if (r2 == a.den && (q % 2 != 0)) q += a.num < 0 ? -1 : 1;  /* tie: to even */
  }
  return sp_rat_fit(q);
}
/* Rational#round(half: :down): ties go toward zero (#3047). */
sp_int sp_rational_round_i_down(sp_Rational a) {
  sp_rat_wide q = a.num / a.den, r = a.num % a.den;
  if (r != 0) {
    sp_rat_wide r2 = (r < 0 ? -r : r) * 2;
    if (r2 > a.den) q += a.num < 0 ? -1 : 1;           /* past the half: round away */
    /* an exact tie keeps the toward-zero truncated quotient */
  }
  return sp_rat_fit(q);
}
/* Rational#div: floor division to an Integer (CRuby Numeric#div). */
sp_int sp_rational_idiv(sp_Rational a, sp_Rational b) {
  if (b.num == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  sp_rat_wide n = (sp_rat_wide)a.num * b.den, d = (sp_rat_wide)a.den * b.num;
  if (d < 0) { n = -n; d = -d; }
  sp_rat_wide q = n / d;
  if (n % d != 0 && n < 0) q--;
  return sp_rat_fit(q);
}
/* 10^e as a wide integer, raising RangeError past the sp_int range. */
static sp_rat_wide sp_rat_pow10(sp_int e) {
  sp_rat_wide r = 1;
  for (sp_int i = 0; i < e; i++) {
    if (r > (sp_rat_wide)INTPTR_MAX / 10)
      sp_raise_cls("RangeError", "Rational out of sp_int range");
    r *= 10;
  }
  return r;
}
/* Rational#round/#truncate with a precision: scale by 10^nd, round or chop to
   an integer, scale back. nd <= 0 yields an integer-valued Rational (den 1);
   codegen realizes the Integer class from the literal precision. */
sp_int sp_rational_floor_i(sp_Rational a) {
  return a.num >= 0 ? a.num / a.den : -((-a.num + a.den - 1) / a.den);
}
sp_int sp_rational_ceil_i(sp_Rational a) {
  return a.num >= 0 ? (a.num + a.den - 1) / a.den : -((-a.num) / a.den);
}
/* Rational#round's tie-break rule as the 0 even / 1 up / 2 down code the
   other numeric paths take, so a mode read at run time reaches the same
   three answers the named forms give. */
sp_int sp_rational_round_i_mode(sp_Rational a, int md) {
  if (md == 0) return sp_rational_round_i_even(a);
  if (md == 2) return sp_rational_round_i_down(a);
  return sp_rational_round_i(a);
}
sp_Rational sp_rational_round_prec_mode(sp_Rational a, sp_int nd, int md) {
  sp_rat_wide p = sp_rat_pow10(nd < 0 ? -nd : nd);
  sp_Rational s = nd >= 0 ? sp_rational_new_wide((sp_rat_wide)a.num * p, a.den)
                          : sp_rational_new_wide(a.num, (sp_rat_wide)a.den * p);
  sp_int q = sp_rational_round_i_mode(s, md);
  return nd >= 0 ? sp_rational_new_wide(q, p) : sp_rational_new_wide((sp_rat_wide)q * p, 1);
}
sp_Rational sp_rational_round_prec(sp_Rational a, sp_int nd) {
  return sp_rational_round_prec_mode(a, nd, 1);
}
sp_Rational sp_rational_mod(sp_Rational a, sp_Rational b) {
  sp_int q = sp_rational_idiv(a, b);
  return sp_rational_sub(a, sp_rational_mul(b, sp_rational_new(q, 1)));
}
sp_Rational sp_rational_rem(sp_Rational a, sp_Rational b) {
  sp_Rational d = sp_rational_div(a, b);
  sp_int q = d.num / d.den;   /* toward zero */
  return sp_rational_sub(a, sp_rational_mul(b, sp_rational_new(q, 1)));
}
sp_Rational sp_rational_floor_prec(sp_Rational a, sp_int nd) {
  sp_rat_wide p = sp_rat_pow10(nd < 0 ? -nd : nd);
  sp_Rational s = nd >= 0 ? sp_rational_new_wide((sp_rat_wide)a.num * p, a.den)
                          : sp_rational_new_wide(a.num, (sp_rat_wide)a.den * p);
  sp_int q = sp_rational_floor_i(s);
  return nd >= 0 ? sp_rational_new_wide(q, p) : sp_rational_new_wide((sp_rat_wide)q * p, 1);
}
sp_Rational sp_rational_ceil_prec(sp_Rational a, sp_int nd) {
  sp_rat_wide p = sp_rat_pow10(nd < 0 ? -nd : nd);
  sp_Rational s = nd >= 0 ? sp_rational_new_wide((sp_rat_wide)a.num * p, a.den)
                          : sp_rational_new_wide(a.num, (sp_rat_wide)a.den * p);
  sp_int q = sp_rational_ceil_i(s);
  return nd >= 0 ? sp_rational_new_wide(q, p) : sp_rational_new_wide((sp_rat_wide)q * p, 1);
}
sp_Rational sp_rational_truncate_prec(sp_Rational a, sp_int nd) {
  sp_rat_wide p = sp_rat_pow10(nd < 0 ? -nd : nd);
  sp_Rational s = nd >= 0 ? sp_rational_new_wide((sp_rat_wide)a.num * p, a.den)
                          : sp_rational_new_wide(a.num, (sp_rat_wide)a.den * p);
  sp_int q = s.num / s.den;  /* toward zero */
  return nd >= 0 ? sp_rational_new_wide(q, p) : sp_rational_new_wide((sp_rat_wide)q * p, 1);
}
static sp_rat_wide sp_rat_ipow(sp_rat_wide base, sp_int e) {
  sp_rat_wide r = 1;
  for (sp_int i = 0; i < e; i++) {
    r *= base;
    if (r > (sp_rat_wide)INTPTR_MAX || r < (sp_rat_wide)(-INTPTR_MAX))
      sp_raise_cls("RangeError", "Rational out of sp_int range");
  }
  return r;
}
sp_Rational sp_rational_pow(sp_Rational a, sp_int e) {
  if (e >= 0) return sp_rational_new_wide(sp_rat_ipow(a.num, e), sp_rat_ipow(a.den, e));
  if (a.num == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  return sp_rational_new_wide(sp_rat_ipow(a.den, -e), sp_rat_ipow(a.num, -e));
}
