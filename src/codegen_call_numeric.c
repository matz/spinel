/* codegen_call_numeric.c -- the builtin-op emitters of Complex and
   Rational that are more than one C expression around the receiver and
   its operands; the rest are templates in builtin_ops.c. Each answers 1
   when it emitted the call, 0 (having emitted nothing) to leave it to the
   chain after the lookup. */

#include "codegen_internal.h"
#include "builtin_ops.h"
#include "codegen_call_arms.h"
#include "repr.h"

/* Rational#round / #floor / #ceil / #truncate with a digit count, a
   `half:` keyword, or both. The arm reads the shape of the arguments (a
   keyword hash, an Integer literal), not their kinds. */
int emit_op_rational_round(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int recv = x->recv;
  int argc;
  const int *argv = call_args(nt, x->id, &argc);
  /* `round(half: mode)` on a Rational, with or without a digit count.
     The mode used to be read as a literal `:even` / `:down` / `:up` and
     nothing else, so a String, a Symbol out of a variable and a `**`
     source were all silently the half-up default, and a digit count
     alongside the keyword had no arm at all (#3047). One reader, shared
     with the Float, Integer and boxed arms, settles what the call said;
     the mode reaches the runtime as the value it was written as. */
  if ((argc == 1 || argc == 2) && nt_type(nt, argv[argc - 1]) &&
      sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode") &&
      is_round_family(name)) {
    RoundKw kw; round_kw_read(c, argv[argc - 1], &kw);
    /* the class the call answers, chosen exactly as infer_type's Rational
       rule chooses it once the keyword hash is peeled off */
    int nd_lit = (argc == 2 && nt_type(nt, argv[0]) &&
                  sp_streq(nt_type(nt, argv[0]), "IntegerNode"));
    int nd_val = nd_lit ? (int)nt_int(nt, argv[0], "value", 0) : 0;
    const char *fn = argc == 1  ? "sp_rational_round_half_i"
                   : !nd_lit    ? "sp_rational_round_half_v"
                   : nd_val > 0 ? "sp_rational_round_half_r"
                                : "sp_rational_round_half_i";
    const char *zero = argc == 1  ? "(sp_int)0"
                     : !nd_lit    ? "sp_box_nil()"
                     : nd_val > 0 ? "sp_rational_new(0, 1)"
                                  : "(sp_int)0";
    int tr = ++g_tmp, tn = -1;
    buf_printf(b, "({ sp_Rational _t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, "; ");
    if (argc == 2) {
      tn = ++g_tmp;
      buf_printf(b, "sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b); buf_puts(b, "; ");
    }
    /* only #round takes a tie-break mode. CRuby's words for a Rational
       are its own -- `not an integer`, not the Float and Integer paths'
       "no implicit conversion of Hash into Integer" -- and with a digit
       count as well it is the arity it complains about first. The
       receiver, the digit count and the keyword values are all evaluated
       before that: the hash is built before the call rejects it. */
    if (!sp_streq(name, "round")) {
      buf_printf(b, "(void)_t%d; ", tr);
      emit_round_kw_effects(c, &kw, b);
      if (argc == 2)
        buf_puts(b, "sp_raise_cls(\"ArgumentError\", \"wrong number of arguments"
                    " (given 2, expected 0..1)\"); ");
      else buf_puts(b, "sp_raise_cls(\"TypeError\", \"not an integer\"); ");
      buf_printf(b, "%s; })", zero);
      return 1;
    }
    int tm = emit_round_kw_binds(c, &kw, b);
    buf_printf(b, "%s(_t%d, ", fn, tr);
    if (tn >= 0) buf_printf(b, "_t%d", tn); else buf_puts(b, "0");
    if (tm >= 0) buf_printf(b, ", _t%d); })", tm);
    else buf_puts(b, ", sp_box_nil()); })");
    return 1;
  }
  /* round/truncate/floor/ceil with a literal precision: nd > 0 keeps a
     Rational, nd <= 0 realizes the Integer value (.num of the den-1 result). */
  if (is_round_family(name) && argc == 1 &&
      nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "IntegerNode")) {
    long long nd = nt_int(nt, argv[0], "value", 0);
    const char *fn = name[0] == 'r' ? "round"
                   : name[0] == 't' ? "truncate"
                   : name[0] == 'f' ? "floor" : "ceil";
    buf_printf(b, "%ssp_rational_%s_prec(", nd > 0 ? "" : "(", fn);
    emit_expr(c, recv, b);
    buf_printf(b, ", %lld)%s", nd, nd > 0 ? "" : ".num)");
    return 1;
  }
  /* Non-literal precision: the result class depends on the runtime value
     (Rational for nd > 0, Integer otherwise), so box to poly and choose at
     runtime. Both operands are value types -- nothing to GC-root. */
  if (is_round_family(name) && argc == 1) {
    const char *fn = name[0] == 'r' ? "round" : name[0] == 't' ? "truncate"
                   : name[0] == 'f' ? "floor" : "ceil";
    int tr = ++g_tmp, tn = ++g_tmp;
    buf_printf(b, "({ sp_Rational _t%d = ", tr); emit_expr(c, recv, b);
    /* A boxed precision is an sp_RbVal struct, which cannot be C-cast to
       an integer at all -- the generated C stopped compiling the moment
       the argument widened to poly (the same cast Rational()'s own
       constructor had to give up, #3184). */
    buf_printf(b, "; sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
    buf_printf(b, "; _t%d > 0 ? sp_box_rational(sp_rational_%s_prec(_t%d, _t%d))"
                  " : sp_box_int(sp_rational_%s_prec(_t%d, _t%d).num); })",
               tn, fn, tr, tn, fn, tr, tn);
    return 1;
  }
  return 0;
}

/* a Bignum receiver's methods */
int emit_call_bigint_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt) {
  /* bigint methods */
  if (recv >= 0 && rt == TY_BIGINT) {
    Buf rs = expr_buf(c, recv);
    const char *r = rs.p ? rs.p : "";
    if ((is_text_conversion(name)) && argc == 0) {
      /* NULL is this slot's nil: #to_s answers "" and #inspect "nil", the
         way they do for every other nullable pointer type (#4800). */
      int tsv = ++g_tmp;
      buf_printf(b, "({ sp_Bigint *_t%d = %s; _t%d ? sp_bigint_to_s(_t%d) : %s; })",
                 tsv, r, tsv, tsv,
                 sp_streq(name, "inspect") ? "(&(\"\\xff\" \"nil\")[1])" : "sp_str_empty");
      free(rs.p); return 1;
    }
    /* to_i / to_int on a Bignum is self -- returning the full value, not the
       64-bit-truncated sp_bigint_to_int (#2319) */
    if ((is_to_integer(name)) && argc == 0) {
      buf_printf(b, "(%s)", r); free(rs.p); return 1;
    }
    if ((sp_streq(name, "magnitude") || sp_streq(name, "abs")) && argc == 0) {
      buf_printf(b, "sp_bigint_abs_v(%s)", r); free(rs.p); return 1;   /* (#2418) */
    }
    if (sp_streq(name, "abs2") && argc == 0) {
      buf_printf(b, "sp_bigint_mul(%s, %s)", r, r); free(rs.p); return 1;   /* (#2424) */
    }
    /* Bignum#downto(hi)/#upto(hi) with no block: materialize the Bignum sequence
       as a poly array (a Bignum range has no lazy Enumerator type) (#2305). */
    if ((is_bounded_int_step(name)) && argc == 1 &&
        nt_ref(nt, id, "block") < 0) {
      int up = sp_streq(name, "upto");
      buf_printf(b, "sp_bigint_range_array(%s, ", r);
      Repr ar = repr_of(c, argv[0]);
      if (ar.big) emit_expr(c, argv[0], b);
      else { buf_puts(b, "sp_bigint_new_int("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_printf(b, ", %d)", up);
      free(rs.p); return 1;
    }
    /* Integer query / reflection on a Bignum receiver (#2318) */
    if (sp_streq(name, "zero?") && argc == 0) {
      buf_printf(b, "(sp_bigint_sign(%s) == 0)", r); free(rs.p); return 1;
    }
    if (sp_streq(name, "positive?") && argc == 0) {
      buf_printf(b, "(sp_bigint_sign(%s) > 0)", r); free(rs.p); return 1;
    }
    if (sp_streq(name, "negative?") && argc == 0) {
      buf_printf(b, "(sp_bigint_sign(%s) < 0)", r); free(rs.p); return 1;
    }
    if (sp_streq(name, "integer?") && argc == 0) {
      buf_printf(b, "((void)(%s), TRUE)", r); free(rs.p); return 1;
    }
    if ((is_succ_alias(name)) && argc == 0) {
      buf_printf(b, "sp_bigint_add(%s, sp_bigint_new_int(1))", r); free(rs.p); return 1;
    }
    if (sp_streq(name, "pred") && argc == 0) {
      buf_printf(b, "sp_bigint_sub(%s, sp_bigint_new_int(1))", r); free(rs.p); return 1;
    }
    if (sp_streq(name, "class") && argc == 0) {
      /* NULL is the slot's nil */
      if (node_may_be_null_nil(c, recv))
        buf_printf(b, "((sp_Class){(%s) ? -100 : %d})", r, builtin_class_id("NilClass"));
      else buf_printf(b, "((void)(%s), ((sp_Class){-100}))", r);  /* Integer */
      free(rs.p); return 1;
    }
    /* coerce(n): [n, self], both boxed (#3129), for an Integer n. Any other
       operand makes both Floats, as CRuby's Integer#coerce does through
       Float(): sp_poly_coerce answers that for a boxed Integer (a String
       parsed, nil's TypeError), where pushing the operand and the Bignum
       unchanged answered [2.5, 1180591620717411303424]. */
    if (sp_streq(name, "coerce") && argc == 1) {
      if (comp_ntype(c, argv[0]) != TY_INT && comp_ntype(c, argv[0]) != TY_BIGINT &&
          !repr_of(c, argv[0]).big) {
        int tcr = ++g_tmp, tco = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = sp_box_bigint(%s); SP_GC_ROOT_RBVAL(_t%d); sp_RbVal _t%d = ",
                   tcr, r, tcr, tco);
        emit_boxed(c, argv[0], b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_poly_to_poly_array(sp_poly_coerce(_t%d, _t%d)); })",
                   tco, tcr, tco);
        free(rs.p); return 1;
      }
      int tca = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " sp_PolyArray_push(_t%d, ", tca, tca, tca);
      emit_boxed(c, argv[0], b);
      buf_printf(b, "); sp_PolyArray_push(_t%d, sp_box_bigint(%s)); _t%d; })", tca, r, tca);
      free(rs.p); return 1;
    }
    /* clamp(lo, hi): compare in bigint; an sp_int bound promotes (#3129) */
    if (sp_streq(name, "clamp") && argc == 2) {
      int tcl = ++g_tmp, tch = ++g_tmp;
      /* each bound is held across the other's evaluation and the
         receiver's, which can allocate (#4049) */
      buf_printf(b, "({ sp_Bigint *_t%d = ", tcl); emit_bigint_operand(c, argv[0], b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); sp_Bigint *_t%d = ", tcl, tch); emit_bigint_operand(c, argv[1], b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); sp_bigint_cmp(%s, _t%d) < 0 ? _t%d"
                    " : sp_bigint_cmp(%s, _t%d) > 0 ? _t%d : (%s); })",
                 tch, r, tcl, tcl, r, tch, tch, r);
      free(rs.p); return 1;
    }
    if (sp_streq(name, "bit_length") && argc == 0) {
      buf_printf(b, "sp_bigint_bit_length(%s)", r); free(rs.p); return 1;
    }
    if (sp_streq(name, "even?") && argc == 0) {
      buf_printf(b, "sp_bigint_even_p(%s)", r); free(rs.p); return 1;
    }
    if (sp_streq(name, "odd?") && argc == 0) {
      buf_printf(b, "(!sp_bigint_even_p(%s))", r); free(rs.p); return 1;
    }
    if (sp_streq(name, "abs") && argc == 0) {
      buf_printf(b, "sp_bigint_abs_v(%s)", r); free(rs.p); return 1;
    }
    /* round/ceil/floor: no precision -> self; a precision arg rounds to
       10^(-ndigits) (a positive precision is a no-op on an integer) (#2303) */
    if (is_round_family(name) && argc == 0) {
      buf_printf(b, "(%s)", r); free(rs.p); return 1;
    }
    /* truncate(n) rounds toward zero: a negative Bignum's ceil, any other's
       floor. It had no arm and was refused. */
    if (sp_streq(name, "truncate") && argc == 1) {
      int tt = ++g_tmp;
      buf_printf(b, "({ sp_Bigint *_t%d = %s; SP_GC_ROOT(_t%d); sp_bigint_round_prec(_t%d, ({ sp_int _rnd = ",
                 tt, r, tt, tt);
      emit_int_expr(c, argv[0], b);
      buf_printf(b, "; sp_int_round_check_ndigits(_rnd); _rnd; }), sp_bigint_sign(_t%d) < 0 ? 2 : 1); })", tt);
      free(rs.p); return 1;
    }
    if ((sp_streq(name, "round") || sp_streq(name, "ceil") || sp_streq(name, "floor")) && argc == 1) {
      int mode = sp_streq(name, "floor") ? 1 : sp_streq(name, "ceil") ? 2 : 0;
      /* a precision past a C int is a RangeError, as on a Fixnum (#6702) */
      buf_printf(b, "sp_bigint_round_prec(%s, ({ sp_int _rnd = ", r); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; sp_int_round_check_ndigits(_rnd); _rnd; }), %d)", mode); free(rs.p); return 1;
    }
    if (sp_streq(name, "to_s") && argc == 1) {
      buf_printf(b, "sp_str_dup_external(sp_bigint_to_s_base(%s, ", r);
      emit_int_expr(c, argv[0], b); buf_puts(b, "))"); free(rs.p); return 1;
    }
    /* digits: kept only for the poly "face table" re-entry, not reached by
       any static concrete call site any more -- see the matching comment
       in codegen_call_recv.c and desugar_builtin_scalar_calls. */
    if (sp_streq(name, "digits") && argc <= 1) {
      /* least-significant first via repeated divmod -- any radix >= 2
         (the to_s(base) text path stops at 36) */
      int td = ++g_tmp, tb2 = ++g_tmp, tn2 = ++g_tmp, ti2 = ++g_tmp;
      buf_printf(b, "({ sp_int _t%d = ", tb2);
      if (argc == 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "10");
      buf_printf(b, "; if (_t%d < 2) sp_raise_cls(\"ArgumentError\", \"invalid radix\");", tb2);
      buf_printf(b, " sp_int *_t%d = NULL; sp_int _t%d = sp_bigint_digits_buf(%s, _t%d, &_t%d);", ti2, tn2, r, tb2, ti2);
      buf_printf(b, " if (_t%d < 0) sp_raise_cls(\"Math::DomainError\", \"out of domain\");", tn2);
      buf_printf(b, " sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);", td, td);
      buf_printf(b, " for (sp_int _i = 0; _i < _t%d; _i++) sp_IntArray_push(_t%d, _t%d[_i]);", tn2, td, ti2);
      buf_printf(b, " free(_t%d); _t%d; })", ti2, td);
      return 1;
    }
    if (sp_streq(name, "to_f") && argc == 0) {
      buf_printf(b, "sp_bigint_to_double(%s)", r); free(rs.p); return 1;
    }
    /* Bignum-receiver methods that return an Integer/Float/bool without a
       Rational (those need a bigint-backed Rational and stay unsupported)
       (#2469). */
    if (sp_streq(name, "~") && argc == 0) { buf_printf(b, "sp_bigint_not(%s)", r); free(rs.p); return 1; }
    /* numerator/ord of an Integer is the value itself; denominator is 1 */
    if ((sp_streq(name, "numerator") || sp_streq(name, "ord")) && argc == 0) {
      buf_printf(b, "(%s)", r); free(rs.p); return 1;
    }
    if (sp_streq(name, "denominator") && argc == 0) {
      buf_printf(b, "((void)(%s), (sp_int)1)", r); free(rs.p); return 1;
    }
    if (sp_streq(name, "size") && argc == 0) {
      /* Integer#size is the magnitude's byte count, at least an sp_int's
         (sp_bigint_int_size); sp_bigint_byte_len rounds up to whole limbs,
         which overcounts (2**100 -> 16 not 13), and ceil(bit_length / 8)
         answered 1 for a small value in a Bignum slot and 8 for -(2**64) */
      buf_printf(b, "sp_bigint_int_size(%s)", r); free(rs.p); return 1;
    }
    if (sp_streq(name, "nonzero?") && argc == 0) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_Bigint *_t%d = %s; sp_bigint_sign(_t%d) != 0 ? sp_box_bigint(_t%d) : sp_box_nil(); })", t, r, t, t);
      free(rs.p); return 1;
    }
    if (sp_streq(name, "fdiv") && argc == 1) {
      Repr ar = repr_of(c, argv[0]);
      TyKind at = ar.as_ty;
      buf_printf(b, "(sp_bigint_to_double(%s) / ", r);
      if (ar.big) { buf_puts(b, "sp_bigint_to_double("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (at == TY_FLOAT) { buf_puts(b, "("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else { buf_puts(b, "(double)("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_puts(b, ")"); free(rs.p); return 1;
    }
    if (sp_streq(name, "pow") && argc == 1) {
      buf_printf(b, "sp_bigint_pow(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      free(rs.p); return 1;
    }
    /* A Float operand: CRuby divides in floats, converting the Bignum to its
       nearest double. modulo and remainder answer a Float; div answers the
       quotient truncated toward zero, as CRuby's rb_dbl2big takes it (so
       (-2**64).div(2.0**65) is 0, not -1), and divmod CRuby's flodivmod pair;
       each quotient an Integer of whatever width (sp_box_f_to_int, which
       raises FloatDomainError for a NaN or an infinite one). The Bignum arms below
       took the Float through sp_bigint_new_int, which truncated it:
       (2**64).div(1.5) divided by 1. */
    if (argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT &&
        (sp_streq(name, "modulo") || sp_streq(name, "%") || sp_streq(name, "remainder") ||
         sp_streq(name, "div") || sp_streq(name, "divmod"))) {
      if (sp_streq(name, "div")) {
        int tx = ++g_tmp, ty = ++g_tmp;
        buf_printf(b, "({ sp_float _t%d = sp_bigint_to_double(%s); sp_float _t%d = ", tx, r, ty);
        emit_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d == 0.0) sp_raise_cls(\"ZeroDivisionError\", \"divided by 0\");"
                      " sp_box_f_to_int(_t%d / _t%d); })", ty, tx, ty);
      }
      else if (sp_streq(name, "divmod")) {
        int tx = ++g_tmp, ty = ++g_tmp, td = ++g_tmp, tm = ++g_tmp, tq = ++g_tmp, tp = ++g_tmp;
        buf_printf(b, "({ sp_float _t%d = sp_bigint_to_double(%s); sp_float _t%d = ", tx, r, ty);
        emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_float _t%d, _t%d;"
                      " if (isnan(_t%d)) _t%d = _t%d = _t%d;"
                      " else { if (_t%d == 0.0) sp_raise_cls(\"ZeroDivisionError\", \"divided by 0\");"
                      " _t%d = (_t%d == 0.0 || (isinf(_t%d) && !isinf(_t%d))) ? _t%d : fmod(_t%d, _t%d);"
                      " _t%d = (isinf(_t%d) && !isinf(_t%d)) ? _t%d : round((_t%d - _t%d) / _t%d);"
                      " if (_t%d * _t%d < 0) { _t%d += _t%d; _t%d -= 1.0; } }"
                      " sp_RbVal _t%d = sp_box_f_to_int(_t%d); SP_GC_ROOT_RBVAL(_t%d);"
                      " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, _t%d); sp_PolyArray_push(_t%d, sp_box_float(_t%d)); _t%d; })",
                   td, tm,
                   ty, td, tm, ty,
                   ty,
                   tm, tx, ty, tx, tx, tx, ty,
                   td, tx, ty, tx, tx, tm, ty,
                   ty, tm, tm, ty, td,
                   tq, td, tq,
                   tp, tp,
                   tp, tq, tp, tm, tp);
      }
      else {
        buf_printf(b, "%s(sp_bigint_to_double(%s), ", name[0] == 'r' ? "sp_fremainder" : "sp_fmod", r);
        emit_expr(c, argv[0], b);
        buf_puts(b, ")");
      }
      free(rs.p); return 1;
    }
    /* a divisor known only at run time: divmod and modulo answer what its
       kind makes them (a Rational's exactly, sp_rat_mod_v); the Bignum
       operand form read a Rational as 0 */
    if ((is_divmod_name(name) || is_modulo_alias(name)) && argc == 1 &&
        repr_of(c, argv[0]).kind == RK_BOXED &&
        (is_divmod_name(name) ? comp_ntype(c, id) == TY_POLY_ARRAY : repr_of(c, id).kind == RK_BOXED)) {
      int tr = ++g_tmp;
      buf_printf(b, "({ sp_Bigint *_t%d = %s; SP_GC_ROOT(_t%d); %s(sp_box_bigint(_t%d), ", tr, r, tr,
                 is_divmod_name(name) ? "sp_poly_to_poly_array(sp_poly_divmod"
                 : is_mod_operator(name) ? "sp_poly_mod" : "sp_poly_modulo", tr);
      emit_expr(c, argv[0], b);
      buf_puts(b, is_divmod_name(name) ? ")); })" : "); })");
      free(rs.p); return 1;
    }
    /* Bignum modulo/%/remainder/divmod/#[]/modular-pow (#2594) */
    if ((is_modulo_alias(name)) && argc == 1) {
      buf_printf(b, "sp_bigint_mod(%s, ", r); emit_bigint_operand(c, argv[0], b); buf_puts(b, ")");
      free(rs.p); return 1;
    }
    if (sp_streq(name, "remainder") && argc == 1) {
      buf_printf(b, "sp_bigint_remainder(%s, ", r); emit_bigint_operand(c, argv[0], b); buf_puts(b, ")");
      free(rs.p); return 1;
    }
    if (sp_streq(name, "divmod") && argc == 1) {
      int td = ++g_tmp, tb2 = ++g_tmp, to2 = ++g_tmp;
      /* both operands are held across the pair's allocation, which can
         collect: a fresh one (a literal divisor's sp_bigint_new_int, a
         computed receiver) was swept, and the division read its cleared
         value as a zero divisor */
      buf_printf(b, "({ sp_Bigint *_t%d = %s; SP_GC_ROOT(_t%d); sp_Bigint *_t%d = ", td, r, td, tb2);
      emit_bigint_operand(c, argv[0], b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " sp_PolyArray_push(_t%d, sp_box_bigint(sp_bigint_div(_t%d, _t%d)));"
                    " sp_PolyArray_push(_t%d, sp_box_bigint(sp_bigint_mod(_t%d, _t%d))); _t%d; })",
                 tb2, to2, to2, to2, td, tb2, to2, td, tb2, to2);
      free(rs.p); return 1;
    }
    if (sp_streq(name, "[]") && argc == 1 && repr_of(c, argv[0]).range == TY_INT) {
      /* Bignum bit-slice n[lo..hi]: shift down by lo, mask hi-lo+1 bits (or
         keep everything above lo for an endless range). Mirrors the int-
         receiver Range arm but over bigint ops (#3156). The slice may not fit
         sp_int for a very wide range; that truncates, like the int arm. */
      int tr = ++g_tmp, ts = ++g_tmp;
      /* an index span: each end through to_int (sp_range_ix), so an end
         written as a Float truncates and keeps its exclusivity */
      buf_printf(b, "({ sp_Range _t%d = sp_range_ix(", tr); emit_expr(c, argv[0], b); buf_puts(b, ")");
      buf_printf(b, "; sp_int _lo%d = _t%d.first == INTPTR_MIN"
                    " ? (sp_raise_cls(\"ArgumentError\","
                    " \"The beginless range for Integer#[] results in infinity\"), 0)"
                    " : _t%d.first;"
                    " sp_Bigint *_t%d = sp_bigint_shr(%s, (int64_t)_lo%d);"
                    " _t%d.last == INTPTR_MAX ? sp_bigint_to_int(_t%d)"
                    " : sp_bigint_to_int(sp_bigint_and(_t%d,"
                    " sp_bigint_sub(sp_bigint_shl(sp_bigint_new_int(1),"
                    " (int64_t)((uintptr_t)_t%d.last - (uintptr_t)_lo%d + (_t%d.excl ? 0 : 1))), sp_bigint_new_int(1)))); })",
                 tr, tr, tr,
                 ts, r, tr,
                 tr, ts,
                 ts,
                 tr, tr, tr);
      free(rs.p); return 1;
    }
    if (sp_streq(name, "[]") && argc == 1) {
      int tn = ++g_tmp;
      buf_printf(b, "({ sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; _t%d < 0 ? (sp_int)0"
                    " : sp_bigint_to_int(sp_bigint_and(sp_bigint_shr(%s, _t%d), sp_bigint_new_int(1))); })",
                 tn, r, tn);
      free(rs.p); return 1;
    }
    if (sp_streq(name, "[]") && argc == 2) {
      /* Bignum n[start, len]: the len-bit field starting at bit `start`. */
      int tst = ++g_tmp, tln = ++g_tmp, tsh = ++g_tmp;
      buf_printf(b, "({ sp_int _t%d = ", tst); emit_to_int_expr(c, argv[0], b);
      buf_printf(b, "; sp_int _t%d = ", tln); emit_to_int_expr(c, argv[1], b);
      buf_printf(b, "; sp_Bigint *_t%d = sp_bigint_shr(%s, (int64_t)_t%d);"
                    " (_t%d < 0 || _t%d < 0) ? (sp_int)0"
                    " : sp_bigint_to_int(sp_bigint_and(_t%d,"
                    " sp_bigint_sub(sp_bigint_shl(sp_bigint_new_int(1), (int64_t)_t%d), sp_bigint_new_int(1)))); })",
                 tsh, r, tst,
                 tst, tln,
                 tsh, tln);
      free(rs.p); return 1;
    }
    if (sp_streq(name, "pow") && argc == 2) {
      buf_printf(b, "sp_bigint_powmod(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_bigint_operand(c, argv[1], b); buf_puts(b, ")");
      free(rs.p); return 1;
    }
    /* Bignum#div(Rational): the floor of the exact quotient. The Bignum
       operand form below has no conversion for a Rational and refused it,
       and Integer#ceildiv(Rational) divides with it (builtins/integer.rb). */
    if (is_div_name(name) && argc == 1 && comp_ntype(c, argv[0]) == TY_RATIONAL) {
      int tr = ++g_tmp, ta = ++g_tmp;
      buf_printf(b, "({ sp_Bigint *_t%d = %s; SP_GC_ROOT(_t%d); sp_Rational _t%d = ", tr, r, tr, ta);
      emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_bigint_div_rat(_t%d, _t%d); })", tr, ta);
      free(rs.p); return 1;
    }
    /* a divisor known only at run time: a Float or a Rational divides as
       Numeric#div does; the Bignum operand form read one truncated, the
       other as 0 (sp_bigint_div_poly) */
    if (is_div_name(name) && argc == 1 && repr_of(c, argv[0]).kind == RK_BOXED) {
      /* the receiver is held across the divisor's evaluation, which can
         allocate */
      int tr = ++g_tmp;
      buf_printf(b, "({ sp_Bigint *_t%d = %s; SP_GC_ROOT(_t%d); sp_bigint_div_poly(_t%d, ", tr, r, tr, tr);
      emit_expr(c, argv[0], b); buf_puts(b, "); })");
      free(rs.p); return 1;
    }
    if ((sp_streq(name, "div") || sp_streq(name, "gcd") || sp_streq(name, "lcm")) && argc == 1) {
      const char *fn = sp_streq(name, "div") ? "div" : name;
      buf_printf(b, "sp_bigint_%s(%s, ", fn, r);
      emit_bigint_operand(c, argv[0], b); buf_puts(b, ")");
      free(rs.p); return 1;
    }
    if (sp_streq(name, "ceildiv") && argc == 1) {
      /* ceil division = -((-a) div b); div is floor division */
      buf_printf(b, "sp_bigint_sub(sp_bigint_new_int(0), sp_bigint_div(sp_bigint_sub(sp_bigint_new_int(0), %s), ", r);
      emit_bigint_operand(c, argv[0], b); buf_puts(b, "))");
      free(rs.p); return 1;
    }
    if (is_bits_query(name) && argc == 1) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_Bigint *_t%d = ", t); emit_bigint_operand(c, argv[0], b);
      /* the receiver expression below is unsequenced with this operand and can
         allocate; no reproducer, the rule (#4049) is the reason */
      buf_printf(b, "; SP_GC_ROOT(_t%d); ", t);
      if (sp_streq(name, "allbits?"))
        buf_printf(b, "(sp_bigint_cmp(sp_bigint_and(%s, _t%d), _t%d) == 0); })", r, t, t);
      else if (sp_streq(name, "anybits?"))
        buf_printf(b, "(sp_bigint_sign(sp_bigint_and(%s, _t%d)) != 0); })", r, t);
      else
        buf_printf(b, "(sp_bigint_sign(sp_bigint_and(%s, _t%d)) == 0); })", r, t);
      free(rs.p); return 1;
    }
    if (sp_streq(name, "gcdlcm") && argc == 1) {
      int t = ++g_tmp, ta = ++g_tmp, tr = ++g_tmp;
      buf_printf(b, "({ sp_Bigint *_t%d = %s; SP_GC_ROOT(_t%d); sp_Bigint *_t%d = ", tr, r, tr, t);
      emit_bigint_operand(c, argv[0], b);
      /* both operands are read after the array allocation below, and both are
         fresh bigints held by nothing else: unrooted they were swept and the
         pair came back as [0, 0]. The gcd arm above already roots its two. */
      buf_printf(b, "; SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " sp_PolyArray_push(_t%d, sp_box_bigint(sp_bigint_gcd(_t%d, _t%d)));"
                    " sp_PolyArray_push(_t%d, sp_box_bigint(sp_bigint_lcm(_t%d, _t%d))); _t%d; })",
                 t, ta, ta, ta, tr, t, ta, tr, t, ta);
      free(rs.p); return 1;
    }
    /* to_r / rationalize on a Bignum -> Rational(self, 1); quo -> Rational(self,
       arg). The numerator exceeds sp_int, so these produce a boxed big
       Rational (poly) rather than the by-value int Rational (#2469). */
    if ((is_to_rational(name)) && argc == 0) {
      buf_printf(b, "sp_brat_from_bigint(%s)", r); free(rs.p); return 1;
    }
    if (sp_streq(name, "quo") && argc == 1) {
      buf_printf(b, "sp_box_brat(%s, ", r); emit_bigint_operand(c, argv[0], b); buf_puts(b, ")");
      free(rs.p); return 1;
    }
    free(rs.p);
  }
  return 0;
}

/* a Range literal receiver that still types as the int TY_RANGE (a mixed or beginless / endless string range) */
int emit_call_range_literal_arms(Compiler *c, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind a0) {
  /* string-range literal methods: the int-only sp_Range struct can't hold
     string bounds, so inline strcmp / char-iteration for a literal
     `("a".."z")` receiver. */
  /* A string-range LITERAL is the distinct sp_StrRange value type now, served
     by emit_range_call; this arm is left for the shapes that still type as the
     int TY_RANGE (a mixed or beginless/endless string range). (#3064) */
  if (recv >= 0 && rt == TY_RANGE && nt_type(nt, unwrap_parens(c, recv)) &&
      sp_streq(nt_type(nt, unwrap_parens(c, recv)), "RangeNode")) {
    int rnode = unwrap_parens(c, recv);
    int lo = nt_ref(nt, rnode, "left"), hi = nt_ref(nt, rnode, "right");
    if (lo >= 0 && hi >= 0 && comp_ntype(c, lo) == TY_STRING && comp_ntype(c, hi) == TY_STRING) {
      int excl = (int)(nt_int(nt, rnode, "flags", 0) & 4) ? 1 : 0;
      if (is_range_membership(name) && argc == 1) {
        if (a0 != TY_STRING) {
          /* a non-string can't be in a string range: false (eval arg) */
          buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
        }
        else {
          int ta = ++g_tmp;
          buf_printf(b, "({ const char *_t%d = ", ta); emit_expr(c, argv[0], b);
          buf_puts(b, "; (sp_str_cmp_bytes("); emit_expr(c, lo, b); buf_printf(b, ", _t%d) <= 0 && sp_str_cmp_bytes(_t%d, ", ta, ta);
          emit_expr(c, hi, b); buf_printf(b, ") %s 0); })", excl ? "<" : "<=");
        }
        return 1;
      }
      if (sp_streq(name, "to_a") && argc == 0) {
        /* succ-based string range (handles multi-char: "aa".."ac" etc.) */
        buf_puts(b, "sp_StrArray_from_string_range("); emit_expr(c, lo, b);
        buf_puts(b, ", "); emit_expr(c, hi, b); buf_printf(b, ", %d)", excl);
        return 1;
      }
    }
  }
  return 0;
}

/* the Integer and Range iterators in expression position: range.step(n) { }, times / upto / downto / step with a block (answering the receiver), and the blockless forms that answer a Range */
int emit_call_iter_expr_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* range.step(n) { } in expression position: run the loop, evaluate to the
     receiver range (Ruby returns self) (#2415) */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 &&
      (comp_ntype(c, recv) == TY_RANGE || comp_ntype(c, recv) == TY_FLOAT_RANGE) &&
      sp_streq(name, "step")) {
    buf_puts(b, "({ ");
    emit_iteration_stmt(c, id, b, 0);
    emit_expr(c, recv, b); buf_puts(b, "; })");
    return 1;
  }
  /* n.times/upto/downto/step { ... } in expression position: run the loop
     (lowered to a statement) and evaluate to the receiver (Ruby returns self).
     A Rational receiver only steps. */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 &&
      ((comp_ntype(c, recv) == TY_INT &&
        (is_integer_iteration(name))) ||
       /* a Float steps too, and a boxed receiver's Float arm re-enters here
          in expression position (#4763) */
       ((comp_ntype(c, recv) == TY_RATIONAL || comp_ntype(c, recv) == TY_FLOAT ||
         comp_ntype(c, recv) == TY_BIGINT) &&
        sp_streq(name, "step")))) {
    /* the receiver is read twice, by the loop and as the answer: one that
       acts (`next_n.times { }`) is bound once */
    int bound = iter_recv_bind_once(c, recv);
    buf_puts(b, "({ ");
    emit_iteration_stmt(c, id, b, 0);
    emit_expr(c, recv, b); buf_puts(b, "; })");
    if (bound) view_unbind(g_n_argov - 1);
    return 1;
  }
  /* n.times / lo.upto(hi) / hi.downto(lo) without block: produce sp_Range for chaining */
  /* A boxed receiver (an Integer parameter under promote mode) is unboxed
     with the argument conversion, which is what the block forms do. */
  if (recv >= 0 && nt_ref(nt, id, "block") < 0 &&
      (comp_ntype(c, recv) == TY_INT || comp_ntype(c, recv) == TY_POLY) &&
      repr_of(c, id).range == TY_INT) {
    if (sp_streq(name, "times")) {
      buf_puts(b, "(sp_Range){ .first = 0, .last = "); emit_int_recv_named(c, recv, name, b); buf_puts(b, ", .excl = 1 }");
      return 1;
    }
    if (sp_streq(name, "upto") && argc == 1) {
      /* a Float limit is not truncated: n.upto(2.5) stops at 2, i.e. floor. */
      int lf = comp_ntype(c, argv[0]) == TY_FLOAT;
      buf_puts(b, "(sp_Range){ .first = "); emit_upto_recv(c, recv, argv[0], b);
      buf_puts(b, ", .last = ");
      if (lf) { buf_puts(b, "sp_flt_range_bound("); emit_expr(c, argv[0], b); buf_puts(b, ", 1)"); }
      else emit_int_expr(c, argv[0], b);
      buf_puts(b, ", .excl = 0 }");
      return 1;
    }
    if (sp_streq(name, "downto") && argc == 1) {
      /* descending: first=hi(recv), last=lo(arg), step=-1 -- an ascending range
         cannot carry the direction, which its .to_a would lose. A Float limit
         is not truncated: n.downto(1.5) stops at 2, i.e. ceil. */
      int lf = comp_ntype(c, argv[0]) == TY_FLOAT;
      buf_puts(b, "sp_range_new_step("); emit_int_recv_named(c, recv, name, b);
      buf_puts(b, ", ");
      if (lf) { buf_puts(b, "sp_flt_range_bound("); emit_expr(c, argv[0], b); buf_puts(b, ", 0)"); }
      else emit_int_expr(c, argv[0], b);
      buf_puts(b, ", 0, -1LL)");
      return 1;
    }
  }
  return 0;
}

int emit_op_float_rationalize(Compiler *c, const BopCtx *x, Buf *b) {
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  int arg = argv[0];
  const char *r = x->rtext;
  /* The epsilon must reach sp_float_rationalize as a float. emit_float_expr
     casts a Rational arg with (sp_float)(<struct>), which the C compiler
     rejects; convert it through sp_rational_to_f instead (#3224). */
  TyKind et = comp_ntype(c, arg);
  /* an epsilon that is no number: CRuby asks it for its #abs, which
     it does not answer -- NoMethodError, not a conversion's TypeError */
  if (et != TY_RATIONAL && et != TY_COMPLEX && et != TY_INT && et != TY_FLOAT && et != TY_BIGINT && et != TY_POLY &&
      et != TY_UNKNOWN) {
    buf_printf(b, "({ (void)(%s); sp_raise_nomethod(sp_nomethod_msg(\"abs\", ", r);
    emit_boxed(c, arg, b);
    buf_puts(b, ")); sp_float_rationalize0(0.0); })");
  }
  else {
    buf_printf(b, "sp_float_rationalize(%s, ", r);
    if (et == TY_RATIONAL) { buf_puts(b, "sp_rational_to_f("); emit_expr(c, arg, b); buf_puts(b, ")"); }
    else if (et == TY_COMPLEX) { buf_puts(b, "sp_complex_abs("); emit_expr(c, arg, b); buf_puts(b, ")"); }
    else if (et == TY_POLY || et == TY_UNKNOWN) {
      int t = ++g_tmp;
      buf_puts(b, "({ "); t = hold_operand(c, arg, TY_POLY, 1, t, 1, " ", b);
      buf_printf(b, "sp_poly_to_f_with_rational(sp_poly_abs(_t%d)); })", t);
    }
    else emit_float_expr(c, arg, b);
    buf_puts(b, ")");
  }
  return 1;
}

/* The keyword is structural, so the Range row delegates its emission here. */
int emit_op_range_clone(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  int args = nt_ref(nt, x->id, "arguments"), argc = 0;
  const int *argv = nt_arr(nt, args, "arguments", &argc);
  if (!argv || argc != 1) return 0;
  if (nt_kind(nt, argv[0]) != NK_KeywordHashNode) return 0;
  int fv = kwh_lookup(nt, argv[0], "freeze");
  int fk = fv >= 0 ? nt_kind(nt, fv) : NK_NilNode;
  if (fk != NK_TrueNode && fk != NK_FalseNode && fk != NK_NilNode) return 0;
  int t = ++g_tmp;
  buf_printf(b, "({ %s _t%d = ", c_type_name(x->rt), t);
  emit_expr(c, x->recv, b);
  buf_puts(b, "; ");
  if (fk != NK_NilNode) buf_printf(b, "_t%d.unfrozen = %d; ", t, fk == NK_FalseNode);
  buf_printf(b, "_t%d; })", t);
  return 1;
}

int emit_op_range_freeze(Compiler *c, const BopCtx *x, Buf *b) {
  int t = ++g_tmp;
  int rk = nt_kind(c->nt, x->recv);
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) {
    buf_printf(b, "({ %s *_t%d = ", c_type_name(x->rt), t);
    emit_constant_slot(c, x->recv, b);
    buf_printf(b, "; _t%d->unfrozen = 0; *_t%d; })", t, t);
    return 1;
  }
  buf_printf(b, "({ %s _t%d = ", c_type_name(x->rt), t);
  emit_expr(c, x->recv, b);
  buf_printf(b, "; _t%d.unfrozen = 0; ", t);
  if (rk == NK_LocalVariableReadNode || rk == NK_InstanceVariableReadNode ||
      rk == NK_ClassVariableReadNode || rk == NK_GlobalVariableReadNode) {
    emit_slot_expr(c, x->recv, b); buf_printf(b, " = _t%d; ", t);
  }
  buf_printf(b, "_t%d; })", t);
  return 1;
}
