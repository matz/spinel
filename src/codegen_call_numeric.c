/* codegen_call_numeric.c -- the builtin-op emitters of Complex and
   Rational that are more than one C expression around the receiver and
   its operands; the rest are templates in builtin_ops.c. Each answers 1
   when it emitted the call, 0 (having emitted nothing) to leave it to the
   chain after the lookup. */

#include "codegen_internal.h"
#include "builtin_ops.h"

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
      (sp_streq(name, "round") || sp_streq(name, "floor") ||
       sp_streq(name, "ceil") || sp_streq(name, "truncate"))) {
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
  if ((sp_streq(name, "round") || sp_streq(name, "truncate") ||
       sp_streq(name, "floor") || sp_streq(name, "ceil")) && argc == 1 &&
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
  if ((sp_streq(name, "round") || sp_streq(name, "truncate") ||
       sp_streq(name, "floor") || sp_streq(name, "ceil")) && argc == 1) {
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
