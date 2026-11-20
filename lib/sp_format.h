#ifndef SP_FORMAT_H
#define SP_FORMAT_H
/* sp_format.h -- cold value-type display helpers, split out of spinel_rt.h
   into libspinel_rt.a.

   Each formats a small value type (Complex / Rational / Range) into a freshly
   GC-allocated string. They need only the shared types (sp_types.h) and the
   shared string allocator (sp_alloc.h), and are only ever reached on a cold
   inspect/to_s path, so they compile once in the archive instead of being
   re-parsed and re-codegen'd in every generated translation unit. */
#include "sp_types.h"   /* sp_Complex, sp_Rational, sp_Range */
#include "sp_time.h"    /* sp_Time */
#include "sp_gc.h"      /* sp_RbVal, for the boxed Complex#abs/abs2 wrappers below */

const char *sp_complex_inspect(sp_Complex c);
const char *sp_complex_to_s(sp_Complex c);
const char *sp_rational_inspect(sp_Rational r);
const char *sp_rational_to_s(sp_Rational r);
const char *sp_Range_inspect(sp_Range *r);
const char *sp_Time_inspect(sp_Time *t);
const char *sp_Time_to_s(sp_Time *t);

/* Value-type arithmetic (cold: only reached when a program actually uses
   Complex / Rational; optcarrot touches Complex only under --nestopia-palette).
   Emitted by codegen via the sp_complex_%s / sp_rational_%s operator dispatch. */
sp_Complex sp_complex_polar(sp_float m, sp_float a, int m_is_f);
sp_Complex sp_complex_add(sp_Complex a, sp_Complex b);
sp_Complex sp_complex_sub(sp_Complex a, sp_Complex b);
sp_Complex sp_complex_mul(sp_Complex a, sp_Complex b);
sp_Complex sp_complex_mul_r(sp_Complex a, sp_float v, int is_f);
sp_Complex sp_complex_imul(int64_t n, sp_Complex a);
sp_Complex sp_complex_fmul(sp_float v, sp_Complex a);
sp_Complex sp_complex_div(sp_Complex a, sp_Complex b);
sp_Complex sp_complex_div_real(sp_Complex a, sp_float b);
sp_Complex sp_complex_div_int(sp_Complex a, sp_int b);
sp_Complex sp_complex_neg(sp_Complex a);
sp_Complex sp_complex_conjugate(sp_Complex a);
sp_Complex sp_complex_pow(sp_Complex a, sp_int e);
sp_Complex sp_complex_pow_c(sp_Complex z, sp_Complex w);   /* z ** w (general) */
sp_Complex sp_complex_pow_f(sp_Complex z, sp_float e);   /* z ** a real non-Integer e */
sp_Complex sp_complex_fdiv(sp_Complex a, sp_Complex b);  /* Complex#fdiv by a Complex */
sp_Complex sp_complex_pow_rational(sp_Complex z, sp_Rational w);  /* z ** Rational (#2962) */
sp_float sp_complex_abs(sp_Complex a);
sp_Complex sp_complex_math_sqrt(sp_Complex a);   /* Math.sqrt of a Complex */
sp_float sp_complex_abs2(sp_Complex a);
sp_bool sp_complex_eq(sp_Complex a, sp_Complex b);

sp_Rational sp_rational_new(sp_int n, sp_int d);
sp_Rational sp_rational_new_i64(int64_t n, int64_t d);   /* reduced before it is narrowed to sp_int: Time#to_r on a 32-bit sp_int */
sp_Rational sp_str_to_r(const char *s);
sp_Rational sp_str_to_r_strict(const char *s);   /* Kernel#Rational(String) */
sp_Rational sp_str_to_r_strict_ex(const char *s, int *ovf);
/* Kernel's `exception: false`: while set, an unparseable string sets
   sp_convert_failed instead of raising (defined in lib/sp_cold.c). */
extern sp_bool sp_convert_soft;
extern sp_bool sp_convert_failed;
sp_Rational sp_rational_add(sp_Rational a, sp_Rational b);
sp_Rational sp_rational_sub(sp_Rational a, sp_Rational b);
sp_Rational sp_rational_mul(sp_Rational a, sp_Rational b);
sp_Rational sp_rational_div(sp_Rational a, sp_Rational b);
sp_Rational sp_rational_neg(sp_Rational a);
sp_Rational sp_rational_abs(sp_Rational a);
sp_Rational sp_rational_pow(sp_Rational a, sp_int e);
int sp_rational_op_ck(int op, sp_Rational a, sp_Rational b, sp_Rational *out);
sp_int sp_rational_round_i(sp_Rational a);              /* Rational#round (no digits) */
sp_int sp_rational_round_i_even(sp_Rational a);         /* Rational#round(half: :even) */
sp_int sp_rational_round_i_down(sp_Rational a);         /* Rational#round(half: :down) */
sp_int sp_rational_idiv(sp_Rational a, sp_Rational b);  /* Rational#div (floor) */
sp_int sp_rational_floor_i(sp_Rational a);              /* Rational#floor (no digits) */
sp_int sp_rational_ceil_i(sp_Rational a);               /* Rational#ceil (no digits) */
sp_int sp_rational_round_i_mode(sp_Rational a, int md);    /* 0 even / 1 up / 2 down */
sp_Rational sp_rational_round_prec_mode(sp_Rational a, sp_int nd, int md);
sp_Rational sp_rational_round_prec(sp_Rational a, sp_int nd);
sp_Rational sp_rational_truncate_prec(sp_Rational a, sp_int nd);
sp_Rational sp_rational_mod(sp_Rational a, sp_Rational b);   /* Rational#% (floor) */
sp_Rational sp_rational_rem(sp_Rational a, sp_Rational b);   /* Rational#remainder */
sp_Rational sp_rational_floor_prec(sp_Rational a, sp_int nd);
sp_Rational sp_rational_ceil_prec(sp_Rational a, sp_int nd);
sp_int sp_rational_cmp(sp_Rational a, sp_Rational b);
sp_bool sp_rational_eq(sp_Rational a, sp_Rational b);
sp_float sp_rational_to_f(sp_Rational a);
sp_Rational sp_float_to_rational(sp_float f);          /* Float#to_r (exact) */
sp_Rational sp_float_rationalize(sp_float f, sp_float eps);  /* Float#rationalize(eps) */
sp_Rational sp_float_rationalize0(sp_float f);         /* Float#rationalize (no arg) */
/* ---- more Complex ops relocated from spinel_rt.h (sp_box_int/float in
   sp_alloc.h, sp_complex_abs/abs2 already declared above). ---- */
sp_RbVal sp_complex_comp_v(sp_float v, int is_f);
sp_RbVal sp_complex_abs_v(sp_Complex a);
sp_RbVal sp_complex_abs2_v(sp_Complex a);

#endif /* SP_FORMAT_H */
