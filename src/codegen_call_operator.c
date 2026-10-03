/* codegen_call_operator.c -- emit_call_body's operator arms: arithmetic, comparison and equality on the builtin kinds.
   Moved from emit_call_body (codegen_call.c) unchanged: each function holds
   a run of its arms, in their order, and answers 1 when one emitted the call
   (codegen_call_arms.h). */

#include "codegen_internal.h"
#include "codegen_poly.h"
#include "builtin_ops.h"
#include "call_plan.h"
#include "codegen_call_arms.h"

/* Integer shifts, <=>, the comparison and equality operators, and is_a? on a poly receiver */
int emit_call_compare_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt) {
  /* a literal `<<` whose result overflowed int64 (`1 << 64`): the node is typed
     bigint, but the int receiver would otherwise emit a UB C `1LL << 64LL`.
     Promote to a bigint shift. */
  if (recv >= 0 && argc == 1 && sp_streq(name, "<<") && rt == TY_INT &&
      comp_ntype(c, id) == TY_BIGINT) {
    buf_puts(b, "sp_bigint_shl(sp_bigint_new_int(");
    emit_expr(c, recv, b);
    buf_puts(b, "), ");
    emit_int_expr(c, argv[0], b);
    buf_puts(b, ")");
    return 1;
  }

  /* bitwise ops on a bignum receiver: arbitrary precision via sp_bigint_*.
     &/|/^ take a bigint second operand (an int/poly mask is promoted);
     <</>> take an int64 shift amount. The result stays a bignum -- a masked
     value can still exceed int64 (`bignum & MASK64`). */
  if (recv >= 0 && argc == 1 && rt == TY_BIGINT &&
      is_int_bit_op(name)) {
    TyKind at0 = comp_ntype(c, argv[0]);
    if (emit_int_operand_fail(c, id, recv, argv[0], sp_streq(name, "<<") || sp_streq(name, ">>"), b)) return 1;
    /* Both operands are heap Bignums, and either side may allocate (and so
       collect) while the other is being evaluated -- the C operand order is
       unspecified besides. Evaluate left then right into rooted temps. */
    int tbl = ++g_tmp, tbr = ++g_tmp;
    if (sp_streq(name, "<<") || sp_streq(name, ">>")) {
      buf_printf(b, "({ sp_Bigint *_t%d = ", tbl); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); int64_t _t%d = ", tbl, tbr);
      if (at0 == TY_BIGINT) { buf_puts(b, "sp_bigint_to_int("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else emit_int_expr(c, argv[0], b);
      buf_printf(b, "; sp_bigint_%s(_t%d, _t%d); })", sp_streq(name, "<<") ? "shl" : "shr", tbl, tbr);
    }
    else {
      const char *fn = sp_streq(name, "&") ? "and" : sp_streq(name, "|") ? "or" : "xor";
      buf_printf(b, "({ sp_Bigint *_t%d = ", tbl); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); sp_Bigint *_t%d = ", tbl, tbr);
      if (at0 == TY_BIGINT) emit_expr(c, argv[0], b);
      else if (at0 == TY_POLY) { buf_puts(b, "sp_poly_as_bigint("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else { buf_puts(b, "sp_bigint_new_int("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_printf(b, "; SP_GC_ROOT(_t%d); sp_bigint_%s(_t%d, _t%d); })", tbr, fn, tbl, tbr);
    }
    return 1;
  }

  /* integer bitwise operators. A poly receiver is coerced to int (the matching
     inference types these TY_INT); `<<` on a poly is handled earlier as the
     ambiguous shift/append via sp_poly_shl, so only &,|,^,>> reach here. */
  if (recv >= 0 && argc == 1 &&
      ((rt == TY_INT && is_int_bit_op(name)) ||
       (rt == TY_POLY && sp_streq(name, ">>")))) {
    TyKind at0 = comp_ntype(c, argv[0]);
    /* A `<<`/`>>` by a NEGATIVE (or >= word width) count is UB as a bare C shift
       -- Ruby shifts the other way for a negative count. Only a constant literal
       in that range takes the sp_int_shl/shr path; a non-constant count stays a
       direct C shift (the hot idiom, e.g. optcarrot's `hi << sweep_shift`, whose
       counts are always small and non-negative -- routing it through a branchy
       helper cost ~4% fps). */
    int is_shift = sp_streq(name, "<<") || sp_streq(name, ">>");
    /* a boxed receiver is read as the Integer it must be; nil has no such
       operator (NoMethodError) */
    char rcv_conv[48]; snprintf(rcv_conv, sizeof rcv_conv, "sp_poly_recv_i(\"%s\", ", name);
    /* a non-integer operand raises TypeError, as CRuby (#2421) -- except that
       the SHIFT operators accept a Float count and truncate it via to_int
       (`10 << 2.9` is 40); the bitwise &/|/^ still reject a Float. */
    if (emit_int_operand_fail(c, id, recv, argv[0], is_shift, b)) return 1;
    /* &, | and ^ with a Bignum operand promote (#2422). `&` too: a negative
       receiver is sign-extended forever, so `-1 & 0xFFFFFFFFFFFFFFFF` is that
       whole mask, not -1. */
    if (is_bit_op(name) && at0 == TY_BIGINT) {
      /* the promoted receiver is a fresh Bignum: root it while the operand
         (which may run arbitrary code, and allocate) is evaluated */
      int tpl = ++g_tmp, tpr = ++g_tmp;
      buf_printf(b, "({ sp_Bigint *_t%d = sp_bigint_new_int(", tpl);
      emit_poly_unboxed(c, recv, rt, rcv_conv, b);
      buf_printf(b, "); SP_GC_ROOT(_t%d); sp_Bigint *_t%d = ", tpl, tpr);
      emit_expr(c, argv[0], b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); sp_bigint_%s(_t%d, _t%d); })", tpr,
                 sp_streq(name, "|") ? "or" : sp_streq(name, "^") ? "xor" : "and", tpl, tpr);
      return 1;
    }
    /* --int-overflow=promote: an int `<<` the inference could not prove exact
       (either operand a runtime value) is typed TY_POLY -- the result can
       escape the word and must carry a Bignum. sp_poly_shl promotes exactly
       there and boxes the small results; sp_int_shl / sp_int_shl_ck below
       carry the raise/wrap contracts and must not serve this call (the _ck
       helper raised "use --int-overflow=promote" in promote mode itself, and
       an in-word count wrapped silently). Keyed on the cached node type so
       the two halves of the compiler cannot drift. */
    if (g_promote_mode && sp_streq(name, "<<") && rt == TY_INT &&
        comp_ntype(c, id) == TY_POLY) {
      buf_puts(b, "sp_poly_shl(");
      emit_boxed(c, recv, b);
      buf_puts(b, ", ");
      emit_boxed(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    const char *aty0 = nt_type(nt, argv[0]);
    int lit_shift = aty0 && sp_streq(aty0, "IntegerNode");
    long long litc = lit_shift ? nt_int(nt, argv[0], "value", 0) : 0;
    /* Every literal `<<` goes through sp_int_shl, whose overflow check is
       the only one there is: the raw C shift below has none, and `x << 9`
       wrapped to 0 past 2^55 (and past 2^23 on a 32-bit sp_int) in a mode
       whose contract is to raise. A literal `>>` cannot overflow and keeps
       the raw shift; a count outside the word joins the helper either way. */
    if (is_shift && lit_shift &&
        (litc < 0 || litc >= 64 || sp_streq(name, "<<"))) {
      buf_printf(b, "sp_int_%s(", sp_streq(name, "<<") ? "shl" : "shr");
      emit_poly_unboxed(c, recv, rt, rcv_conv, b);
      buf_printf(b, ", %lldLL)", litc);
      return 1;
    }
    if (is_shift && !lit_shift) {
      /* a runtime shift count: range-checked (negative shifts the other way,
         past-the-word raises) via a single-compare fast path (#2423) */
      buf_printf(b, "sp_int_%s_ck(", sp_streq(name, "<<") ? "shl" : "shr");
      emit_poly_unboxed(c, recv, rt, rcv_conv, b);
      buf_puts(b, ", ");
      if (at0 == TY_POLY) { buf_puts(b, "sp_poly_bit_operand("); emit_expr(c, argv[0], b); buf_puts(b, ", 1)"); }
      else if (at0 == TY_FLOAT) { buf_puts(b, "(sp_int)("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else emit_int_expr(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    /* A POLY receiver keeps its width: `sp_poly_to_i(x) >> n` truncated a
       bignum to int64 and then shifted arithmetically (#3371). */
    if (is_shift && rt == TY_POLY) {
      buf_printf(b, "sp_poly_%s(", sp_streq(name, "<<") ? "shl" : "shr");
      emit_boxed(c, recv, b);
      buf_puts(b, ", ");
      emit_boxed(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    /* `<<` on a signed value is undefined behavior when the receiver is
       negative (`-8 << 1`); do the shift on the unsigned bit pattern and
       reinterpret -- same two's-complement result, well-defined, no runtime
       cost (a bare reinterpret). `>>` stays a signed (arithmetic) shift, which
       matches Ruby and is only implementation-defined, not UB. */
    int shl_neg_safe = sp_streq(name, "<<");
    buf_puts(b, "(");
    if (shl_neg_safe) buf_puts(b, "(sp_int)((uint64_t)(");
    emit_poly_unboxed(c, recv, rt, rcv_conv, b);
    if (shl_neg_safe) buf_puts(b, ")");
    buf_printf(b, " %s ", name);
    if (at0 == TY_POLY) {
      buf_puts(b, "sp_poly_bit_operand("); emit_expr(c, argv[0], b); buf_printf(b, ", %d)", is_shift);
    }
    /* A literal wider than int64 (a 64-bit mask like 0xFFFFFFFFFFFFFFFF) is
       typed as a bigint; the result slot is int, so take its low-64 bit pattern
       (sp_bigint_to_int truncates) -- this is the xorshift/64-bit-mask idiom. */
    else if (at0 == TY_BIGINT) {
      buf_puts(b, "sp_bigint_to_int("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else emit_expr(c, argv[0], b);
    if (shl_neg_safe) buf_puts(b, ")");
    buf_puts(b, ")");
    return 1;
  }

  /* Blockless any?/all?/none?/one? on a BOXED receiver: walk the elements and
     count the truthy ones, the way the typed-array arms do. A widened array
     (an element read, a destructured multi-value return) had no arm at all and
     raised NoMethodError naming Array, the class that defines them (#3967). */
  if (recv >= 0 && argc == 0 && nt_ref(nt, id, "block") < 0 && rt == TY_POLY &&
      is_quantifier(name) &&
      !user_defines_or_reads(c, name)) {
    int ta = ++g_tmp, tn = ++g_tmp, tcnt = ++g_tmp, ti = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = ", ta); emit_boxed(c, recv, b);
    buf_puts(b, "; "); emit_poly_iter_obj_normalize(c, ta, b);
    emit_poly_iter_obj_reject(c, ta, name, b);
    /* the same receiver check the each emitter makes: nil is no collection,
       and a zero-length loop answered `nil.any?` false and `nil.all?` true
       (#4485) */
    buf_printf(b, "sp_poly_iter_check(_t%d, \"%s\"); ", ta, name);
    buf_printf(b, "sp_int _t%d = sp_poly_arr_len_ex(_t%d); sp_int _t%d = 0;"
                  " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)"
                  " if (sp_poly_truthy(sp_poly_each_elem(_t%d, _t%d))) _t%d++; ",
               tn, ta, tcnt, ti, ti, tn, ti, ta, ti, tcnt);
    if (sp_streq(name, "any?"))       buf_printf(b, "_t%d > 0; })", tcnt);
    else if (sp_streq(name, "all?"))  buf_printf(b, "_t%d == _t%d; })", tcnt, tn);
    else if (sp_streq(name, "none?")) buf_printf(b, "_t%d == 0; })", tcnt);
    else                              buf_printf(b, "_t%d == 1; })", tcnt);
    return 1;
  }

  /* any?/all?/none?/one?(Class) over an array: Class === element membership,
     a stage-5 builtin-op row (builtin_ops.c) */
  if (recv >= 0 && ty_is_array(rt) && emit_builtin_op_stage(c, id, recv, rt, name, 5, b)) return 1;

  if (recv >= 0 && argc == 1 && sp_streq(name, "<=>")) {
    /* Re-infer when stale cache has TY_POLY (e.g. block params temporarily pinned to element type). */
    TyKind lrt = (rt == TY_POLY || rt == TY_UNKNOWN) ? infer_type(c, recv) : rt;
    TyKind at = comp_ntype(c, argv[0]);
    TyKind lat = (at == TY_POLY || at == TY_UNKNOWN) ? infer_type(c, argv[0]) : at;
    /* nil <=> nil is 0; nil <=> anything-else is nil (#2383) */
    if (lrt == TY_NIL) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), (void)("); emit_boxed(c, argv[0], b);
      buf_printf(b, "), %s)", lat == TY_NIL ? "(sp_int)0" : "SP_INT_NIL");
      return 1;
    }
    /* Float <=> Rational: compare by float value, agreeing with the operators
       (<, <=, ...) that already coerce (#2596). The reverse direction works. */
    if (lrt == TY_FLOAT && lat == TY_RATIONAL) {
      int ta = ++g_tmp, tb = ++g_tmp;
      buf_printf(b, "({ double _t%d = ", ta); emit_expr(c, recv, b);
      buf_printf(b, "; double _t%d = sp_rational_to_f(", tb); emit_expr(c, argv[0], b);
      buf_printf(b, "); isnan(_t%d) ? SP_INT_NIL : (sp_int)((_t%d > _t%d) - (_t%d < _t%d)); })",
                 ta, ta, tb, ta, tb);
      return 1;
    }
    /* Float <=> Bignum (either side): compared by value, exactly
       (sp_bigint_cmp_f); a raw >/< on the sp_Bigint* pointer would be
       ill-typed C (#3009), and a double round-trip called a bignum equal to
       every Float within half an ulp of it. A NaN answers nil. */
    if ((lrt == TY_FLOAT && lat == TY_BIGINT) || (lrt == TY_BIGINT && lat == TY_FLOAT)) {
      int tc = ++g_tmp;
      buf_printf(b, "({ int _t%d = sp_bigint_cmp_f(", tc);
      emit_expr(c, lrt == TY_BIGINT ? recv : argv[0], b);
      buf_puts(b, ", ");
      emit_expr(c, lrt == TY_BIGINT ? argv[0] : recv, b);
      buf_printf(b, "); _t%d == 2 ? SP_INT_NIL : (sp_int)(%s_t%d); })", tc, lrt == TY_BIGINT ? "" : "-", tc);
      return 1;
    }
    /* Bignum <=> (either side): compare by value, not the pointer identity a
       raw `>`/`<` on the sp_Bigint* would give (always -1) (#2581) */
    if ((lrt == TY_BIGINT || lat == TY_BIGINT) &&
        (lrt == TY_INT || lrt == TY_BIGINT) && (lat == TY_INT || lat == TY_BIGINT)) {
      int tc = ++g_tmp;
      buf_printf(b, "({ int _t%d = sp_bigint_cmp(", tc);
      if (lrt == TY_BIGINT) emit_expr(c, recv, b);
      else { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      buf_puts(b, ", ");
      if (lat == TY_BIGINT) emit_expr(c, argv[0], b);
      else { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_printf(b, "); (sp_int)((_t%d > 0) - (_t%d < 0)); })", tc, tc);
      return 1;
    }
    if (ty_is_numeric(lrt) && ty_is_numeric(lat)) {
      int ta = ++g_tmp, tb = ++g_tmp;
      buf_puts(b, "({ "); emit_ctype(c, lrt, b); buf_printf(b, " _t%d = ", ta); emit_expr(c, recv, b);
      buf_puts(b, "; "); emit_ctype(c, lat, b); buf_printf(b, " _t%d = ", tb); emit_expr(c, argv[0], b);
      /* a NaN operand makes <=> nil, not 0 (#2315); only floats can be NaN */
      if (lrt == TY_FLOAT || lat == TY_FLOAT)
        buf_printf(b, "; (isnan((double)_t%d) || isnan((double)_t%d)) ? SP_INT_NIL"
                      " : (sp_int)((_t%d > _t%d) - (_t%d < _t%d)); })", ta, tb, ta, tb, ta, tb);
      /* an Integer slot's nil sentinel on either side: nil <=> n and n <=> nil
         are nil, as the NaN test above answers for a Float (#4567) */
      else if (lrt == TY_INT && lat == TY_INT &&
               (cmp_operand_may_be_nil(c, recv) || cmp_operand_may_be_nil(c, argv[0])))
        buf_printf(b, "; (_t%d == SP_INT_NIL || _t%d == SP_INT_NIL) ? SP_INT_NIL"
                      " : (_t%d > _t%d) - (_t%d < _t%d); })", ta, tb, ta, tb, ta, tb);
      else
        buf_printf(b, "; (_t%d > _t%d) - (_t%d < _t%d); })", ta, tb, ta, tb);
      return 1;
    }
    /* Symbol#<=> is defined only between Symbols; a String (or any other
       non-Symbol) operand is not comparable and answers nil (#3081). A Symbol
       receiver can reach here typed as a string (it prints as its name), so
       ask the inferred receiver type rather than trusting lrt alone. */
    if ((lrt == TY_SYMBOL || infer_type(c, recv) == TY_SYMBOL) &&
        lat != TY_SYMBOL && lat != TY_POLY && lat != TY_UNKNOWN) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), (void)("); emit_expr(c, argv[0], b);
      buf_puts(b, "), SP_INT_NIL)");
      return 1;
    }
    /* CRuby's String#<=> asks a non-String operand for #to_str
       (rb_check_string_type) and compares the strings. There was no arm for
       an object operand at all, so the call fell through to the object
       dispatch below and raised NoMethodError naming String#<=> itself.

       Only a CONVERTING class enters here, which is what makes the NULL
       fallback below mean one thing: a #to_str that answered nil. An operand
       whose class answers no usable #to_str keeps that NoMethodError. CRuby
       has an answer for it -- nil, or -(other <=> self) when the class
       defines #<=> (rb_invcmp) -- but that answer is Object#<=>'s rule, not
       this conversion, and giving it here would also silence the classes this
       rule deliberately declines (a #to_str with parameters, one returning an
       Integer, one that raises), each of which CRuby answers differently
       again. It goes as its own change. The ordered operators and #between?
       below DO reach their fallback for a class with no #to_str, because
       there a failed comparison is CRuby's own answer. */
    if (lrt == TY_STRING && str_cmp_conv_shape(c, argv[0])) {
      int tr, to, ts, tc = ++g_tmp;
      int boxed_out = comp_ntype(c, id) == TY_POLY;
      Buf rb = expr_buf(c, recv);
      if (boxed_out) buf_puts(b, "sp_box_int_or_nil(");
      emit_str_cmp_prologue(c, rb.p ? rb.p : "", argv[0], &tr, &to, &ts, b);
      buf_printf(b, "({ int _t%d = sp_str_cmp_bytes(_t%d, _t%d);"
                    " (sp_int)((_t%d > 0) - (_t%d < 0)); }) : SP_INT_NIL; })",
                 tc, tr, ts, tc, tc);
      if (boxed_out) buf_puts(b, ")");
      free(rb.p);
      return 1;
    }
    /* ... and the answer that comment deferred. An operand whose class has no
       usable #to_str does not enter the conversion above, and CRuby does not
       stop there either: rb_str_cmp_m falls back to rb_invcmp, which asks the
       OPERAND to compare itself against the string and negates the answer,
       nil when the class has no `<=>` of its own or answers nil with one.
       `"abc" <=> obj` raised NoMethodError naming String#<=>, a method String
       has, for every such class.

       sp_str_cmp_obj carries the rule, and the emitter is where it has to be
       decided: a Symbol receiver is boxed as its NAME, so the runtime cannot
       tell `:s <=> obj` -- which CRuby answers nil, as every non-String
       receiver does -- from this. The Symbol arm above declines a poly
       operand, which is exactly the pair that would be confused, so the
       receiver is asked for its own inferred type here too. */
    /* Is this receiver a Symbol wearing a String's type? A Symbol renders as
       its name, and by here the desugar has already turned `:s` into
       `:s.to_s`, a CallNode whose own type is TY_STRING -- so neither the
       node kind nor the inferred type says Symbol on its own. All three
       spellings are asked. */
    { const char *rvt_s = nt_type(nt, recv);
      int recv_is_sym = (infer_type(c, recv) == TY_SYMBOL) ||
                        (rvt_s && sp_streq(rvt_s, "SymbolNode"));
      if (!recv_is_sym && rvt_s && sp_streq(rvt_s, "CallNode")) {
        const char *rcn = nt_str(nt, recv, "name");
        int rr = nt_ref(nt, recv, "receiver");
        const char *rrt = rr >= 0 ? nt_type(nt, rr) : NULL;
        if (rcn && sp_streq(rcn, "to_s") &&
            ((rrt && sp_streq(rrt, "SymbolNode")) || (rr >= 0 && infer_type(c, rr) == TY_SYMBOL)))
          recv_is_sym = 1;
      }
    if (lrt == TY_STRING && !recv_is_sym &&
        !str_cmp_conv_shape(c, argv[0]) &&
        (ty_is_object(lat) || lat == TY_POLY || lat == TY_UNKNOWN)) {
      int boxed_out = comp_ntype(c, id) == TY_POLY;
      if (boxed_out) buf_puts(b, "sp_box_int_or_nil(");
      buf_puts(b, "sp_str_cmp_obj("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      if (boxed_out) buf_puts(b, ")");
      return 1;
    } }
    if (lrt == TY_STRING && lat == TY_STRING) {
      int tc = ++g_tmp;
      buf_printf(b, "({ int _t%d = sp_str_cmp_bytes(", tc); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_printf(b, "); (_t%d > 0) - (_t%d < 0); })", tc, tc);
      return 1;
    }
    if (lrt == TY_SYMBOL && lat == TY_SYMBOL) {
      int tc = ++g_tmp, ta = ++g_tmp, tb = ++g_tmp;
      buf_printf(b, "({ sp_sym _t%d = ", ta); emit_expr(c, recv, b);
      buf_printf(b, "; sp_sym _t%d = ", tb); emit_expr(c, argv[0], b);
      buf_printf(b, "; int _t%d = strcmp(sp_sym_to_s(_t%d), sp_sym_to_s(_t%d));"
                    " (_t%d > 0) - (_t%d < 0); })", tc, ta, tb, tc, tc);
      return 1;
    }
    if (lrt == TY_TIME) {
      TyKind a0t = comp_ntype(c, argv[0]);
      if (a0t == TY_TIME) {
        int ta = ++g_tmp, tb = ++g_tmp;
        buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = ", ta); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Time _t%d = ", tb); emit_expr(c, argv[0], b);
        buf_printf(b, "; (sp_int)sp_time_cmp(_t%d, _t%d); })", ta, tb);
        return 1;
      }
      /* Time <=> non-Time is nil (poly result). A poly operand is checked at
         run time; a concrete non-Time operand is unconditionally nil. */
      if (a0t == TY_POLY || a0t == TY_UNKNOWN) {
        int ta = ++g_tmp, tb = ++g_tmp;
        buf_printf(b, "({ sp_Time _t%d = ", ta); emit_expr(c, recv, b);
        buf_printf(b, "; sp_RbVal _t%d = ", tb); emit_boxed(c, argv[0], b);
        buf_printf(b, "; (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_TIME) ? "
                      "sp_box_int(sp_time_cmp(_t%d, *(sp_Time *)_t%d.v.p)) : sp_box_nil(); })",
                   tb, tb, ta, tb);
        return 1;
      }
      buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), sp_box_nil())");
      return 1;
    }
    /* Array <=> Array: lexicographic element-wise compare, or nil when an
       element pair is incomparable. Covers every builtin array kind via the
       boxed accessor. An empty `[]` literal is TY_UNKNOWN but still an array
       (`[] <=> []` is 0) (#2984). */
    int rlit0 = nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode");
    int alit0 = nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ArrayNode");
    if ((ty_is_array(lrt) || (rlit0 && lrt == TY_UNKNOWN)) &&
        (ty_is_array(lat) || (alit0 && lat == TY_UNKNOWN))) {
      int ta = ++g_tmp, tb = ++g_tmp, tk = ++g_tmp, tr = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", ta); emit_boxed(c, recv, b);
      buf_printf(b, "; sp_RbVal _t%d = ", tb); emit_boxed(c, argv[0], b);
      buf_printf(b, "; sp_bool _t%d; sp_int _t%d = sp_poly_arr_cmp(_t%d, _t%d, &_t%d);"
                    " _t%d ? _t%d : SP_INT_NIL; })", tk, tr, ta, tb, tk, tk, tr);
      return 1;
    }
    /* Poly operands (e.g. `@n <=> other.n` with int ivars widened to poly in
       promote mode): tag-dispatch via sp_poly_cmp rather than falling through
       to the object-receiver path, which would misread a boxed int's payload
       as a user-class pointer and recurse into this same `<=>`. */
    if (lrt == TY_POLY || lat == TY_POLY) {
      /* sp_poly_spaceship answers nil for incomparable runtime operands (the
         int-nil sentinel) but 0 for identical singletons -- `nil <=> nil` is 0
         even though the two are not "comparable" in the Comparable sense. */
      /* The helper answers an sp_int (the sentinel for nil). Where the
         expression's own type is poly -- a `<=>` whose method has a `return
         nil` guard, so the slot is sp_RbVal -- box it, or the raw compare goes
         out through an sp_RbVal signature and the build fails (#3498). */
      int cmp_poly = comp_ntype(c, id) == TY_POLY;
      if (cmp_poly) buf_puts(b, "sp_box_int_or_nil(");
      emit_poly_cmp_ordered(c, "sp_poly_spaceship", recv, argv[0], b);
      if (cmp_poly) buf_puts(b, ")");
      return 1;
    }
    /* Statically incomparable concrete operands (1 <=> "a"): Ruby answers
       nil. User objects fall through to their own #<=> dispatch. A nullable
       Integer or Float receiver against nil is the exception: holding its
       sentinel it is nil, and nil <=> nil is 0. */
    if ((lrt == TY_INT || lrt == TY_FLOAT) && lat == TY_NIL && call_returns_nullable_int(c, recv)) {
      char ref[24];
      buf_puts(b, "({ "); emit_sentinel_bind(c, lrt, recv, ref, sizeof ref, b);
      buf_puts(b, "(void)("); emit_expr(c, argv[0], b); buf_puts(b, "); ");
      emit_slot_truthy(lrt, ref, b);
      buf_puts(b, " ? SP_INT_NIL : (sp_int)0; })");
      return 1;
    }
    /* A Hash has no <=> of its own: Object#<=> answers 0 for the same object or
       an equal one, and nil otherwise. Equality is the == emitter's, reached
       through a temporary rename of the call. */
    if (ty_is_hash(lrt)) {
      Buf eqb; memset(&eqb, 0, sizeof eqb);
      nt_node_set_str((NodeTable *)nt, id, "name", "==");
      emit_expr(c, id, &eqb);
      nt_node_set_str((NodeTable *)nt, id, "name", "<=>");
      buf_printf(b, "((%s) ? (sp_int)0 : SP_INT_NIL)", eqb.p ? eqb.p : "0");
      free(eqb.p);
      return 1;
    }
    /* A Regexp, a Proc, a Thread and the other classes with no <=> of their
       own: 0 for the same object, nil for another. (Object#<=> also answers 0
       for an equal one, which needs each class's ==; that stays nil.) An
       operand of another concrete type is not the same object, except that an
       Exception held as the builtin type may be an instance of a class of the
       program. The receiver is rooted while the operand is evaluated, which
       may allocate. */
    if (obj_cmp_by_identity(lrt) && lat != TY_UNKNOWN && !object_defines_cmp(c) &&
        !(lrt == TY_EXCEPTION && exc_subclass_defines_cmp(c))) {
      int exc_inst = lrt == TY_EXCEPTION && ty_is_object(lat) && class_is_exc_subclass(c, ty_object_class(lat));
      if (lat != lrt && !exc_inst) {
        buf_puts(b, "((void)("); emit_expr(c, recv, b);
        buf_puts(b, "), (void)("); emit_expr(c, argv[0], b);
        buf_puts(b, "), SP_INT_NIL)");
        return 1;
      }
      int ta = ++g_tmp, tb = ++g_tmp;
      const char *ct = c_type_name(lrt);
      buf_printf(b, "({ %s_t%d = ", ct, ta); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); ", ta);
      buf_printf(b, "%s_t%d = ", exc_inst ? "void *" : ct, tb);
      if (exc_inst) buf_puts(b, "(void *)(");
      emit_expr(c, argv[0], b);
      if (exc_inst) buf_puts(b, ")");
      buf_printf(b, "; (void *)_t%d == (void *)_t%d ? (sp_int)0 : SP_INT_NIL; })", ta, tb);
      return 1;
    }
    if (lrt != TY_UNKNOWN && lat != TY_UNKNOWN &&
        !ty_is_object(lrt) && !ty_is_object(lat)) {
      buf_puts(b, "((void)(");
      emit_expr(c, recv, b);
      buf_puts(b, "), (void)(");
      emit_expr(c, argv[0], b);
      buf_puts(b, "), SP_INT_NIL)");
      return 1;
    }
  }

  if (recv >= 0 && argc == 1 &&
      is_cmp_op(name)) {
    if ((rt == TY_BIGINT || comp_ntype(c, argv[0]) == TY_BIGINT) &&
        emit_float_bigint_cmp(c, recv, argv[0], name, b)) return 1;
    if ((rt == TY_BIGINT || comp_ntype(c, argv[0]) == TY_BIGINT) &&
        bigint_cmp_operand_ok(rt) && bigint_cmp_operand_ok(comp_ntype(c, argv[0]))) {
      buf_printf(b, "(sp_bigint_cmp(");
      emit_bigint_operand(c, recv, b);
      buf_puts(b, ", ");
      emit_bigint_operand(c, argv[0], b);
      buf_printf(b, ") %s 0)", name);
      return 1;
    }
    if (ty_is_numeric(rt)) {
      /* a statically non-numeric operand (nil, a string, ...) raises the
         Comparable ArgumentError instead of comparing against a coerced 0 */
      TyKind cat = comp_ntype(c, argv[0]);
      if (cat == TY_NIL || cat == TY_STRING || cat == TY_BOOL || cat == TY_SYMBOL) {
        const char *cn9 = rt == TY_FLOAT ? "Float" : rt == TY_RATIONAL ? "Rational" : "Integer";
        const char *an9 = cat == TY_NIL ? "nil" : cat == TY_STRING ? "String"
                        : cat == TY_SYMBOL ? "Symbol" : "boolean";
        buf_puts(b, "((void)("); emit_expr(c, recv, b);
        buf_puts(b, "), (void)("); emit_expr(c, argv[0], b);
        buf_printf(b, "), sp_raise_cls(\"ArgumentError\", \"comparison of %s with %s failed\"), 0)",
                   cn9, an9);
        return 1;
      }
      TyKind rht9 = (rt == TY_FLOAT || rt == TY_RATIONAL) ? TY_FLOAT : TY_INT;
      /* An Integer or Float slot can hold its nil sentinel (an ivar written
         nil, a `--rbs` `Integer?`, a hash read). Every arithmetic helper
         tests for it, but the comparison compared it as a number: `nil > 0`
         answered false where CRuby raises. The test is emitted only when an
         operand can carry the sentinel: a literal, an arithmetic result (its
         helper raised already) and a length never do, which keeps a loop's
         `i < n` as it was when `i` counts from a literal (#4567). */
      /* A read the nil narrowing proves non-nil drops only its own half of
         the test: the other operand keeps the half it had wherever the
         variable can hold nil at all. That half is what told gcc the other
         value is no nil either, which `best.nil? || x > best` needs to split
         its loop after the first element; without it the loop retests
         `best` on every pass (18% more instructions). */
      int rawl9 = nullable_int_value_raw(c, recv), rawr9 = cat == rt && nullable_int_value_raw(c, argv[0]);
      int narl9 = rawl9 && !cmp_operand_may_be_nil(c, recv);
      int narr9 = rawr9 && !cmp_operand_may_be_nil(c, argv[0]);
      int guard9 = rt == rht9 && (rt == TY_INT || rt == TY_FLOAT) &&
                   (cat == rt || cat == TY_POLY) && (rawl9 || rawr9) &&
                   !((narl9 || nt_kind(c->nt, recv) == NK_IntegerNode || nt_kind(c->nt, recv) == NK_FloatNode) &&
                     (narr9 || nt_kind(c->nt, argv[0]) == NK_IntegerNode || nt_kind(c->nt, argv[0]) == NK_FloatNode));
      /* An Integer against a Float compares as C does, each side its own
         kind, so each is tested by its own sentinel: `nil > 1` on a Float
         slot, `nil < 2.0` on an Integer one, answered as numbers. */
      int mixed9 = (rt == TY_INT && cat == TY_FLOAT) || (rt == TY_FLOAT && cat == TY_INT);
      int mln = mixed9 && cmp_operand_may_be_nil(c, recv);
      int mrn = mixed9 && cmp_operand_may_be_nil(c, argv[0]);
      if (mln || mrn) {
        int tg = ++g_tmp;
        char lv[32], rv[32], ln[64] = "0", rn[64] = "0";
        snprintf(lv, sizeof lv, "_t%d", tg);
        snprintf(rv, sizeof rv, "_t%d_r", tg);
        if (mln) scalar_nil_test(rt, lv, ln, sizeof ln);
        if (mrn) scalar_nil_test(cat, rv, rn, sizeof rn);
        Buf ap; memset(&ap, 0, sizeof ap);
        Buf av; memset(&av, 0, sizeof av);
        emit_split_pre(c, argv[0], emit_expr, &ap, &av);
        buf_printf(b, "({ %s %s = ", rt == TY_FLOAT ? "sp_float" : "sp_int", lv);
        emit_expr(c, recv, b);
        buf_printf(b, "; %s%s %s = %s", ap.p ? ap.p : "", cat == TY_FLOAT ? "sp_float" : "sp_int", rv, av.p ? av.p : "0");
        free(ap.p); free(av.p);
        buf_printf(b, "; if (SP_UNLIKELY(%s || %s)) sp_raise_nil_cmp(%s, \"%s\", \"%s\"); %s %s %s; })",
                   ln, rn, ln, name, rt == TY_FLOAT ? "Float" : "Integer", lv, name, rv);
        return 1;
      }
      if (guard9) {
        int tg = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", rt == TY_FLOAT ? "sp_float" : "sp_int", tg);
        emit_expr(c, recv, b);
        buf_printf(b, ", _t%d_r = ", tg);
        if (cat == TY_POLY) {
          buf_printf(b, "%s(", rht9 == TY_FLOAT ? "sp_poly_to_f" : "sp_poly_to_i");
          emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else emit_expr(c, argv[0], b);
        char l9[32], r9[32];
        if (narl9) snprintf(l9, sizeof l9, "%s", rt == TY_FLOAT ? "0.0" : "0"); else snprintf(l9, sizeof l9, "_t%d", tg);
        if (narr9) snprintf(r9, sizeof r9, "%s", rt == TY_FLOAT ? "0.0" : "0"); else snprintf(r9, sizeof r9, "_t%d_r", tg);
        buf_printf(b, "; %s(%s, %s, \"%s\"); _t%d %s _t%d_r; })",
                   rt == TY_FLOAT ? "SP_FLOAT_NIL_CMP_CK" : "SP_INT_NIL_CMP_CK", l9, r9, name, tg, name, tg);
        return 1;
      }
      buf_puts(b, "(");
      emit_expr(c, recv, b);
      buf_printf(b, " %s ", name);
      /* a poly or unresolved-call (raise-all token) right operand against a
         numeric left: coerce it to the comparison's numeric type so the C `>=`
         does not compare an sp_int with an sp_RbVal (`len >= x.megabytes`, an
         unresolved Rails method). */
      if (cat == TY_POLY) {
        buf_printf(b, "%s(", rht9 == TY_FLOAT ? "sp_poly_to_f" : "sp_poly_to_i");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (cat == TY_UNKNOWN || cat == TY_VOID)
        emit_unresolved_coerced(c, argv[0], rht9, b);
      else emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    if (rt == TY_STRING) {
      TyKind sat = comp_ntype(c, argv[0]);
      /* Comparable's relational operators are built on <=>, which answers nil
         against a non-String: that is an ArgumentError, not a comparison
         against the operand reinterpreted as a char pointer (#3592) */
      if (sat == TY_INT || sat == TY_FLOAT || sat == TY_NIL || sat == TY_BOOL ||
          sat == TY_SYMBOL) {
        buf_puts(b, "((void)("); emit_expr(c, recv, b);
        buf_puts(b, "), sp_raise_cls(\"ArgumentError\", sp_sprintf("
                    "\"comparison of String with %s failed\", sp_poly_inspect(");
        emit_boxed(c, argv[0], b);
        buf_puts(b, "))), FALSE)");
        return 1;
      }
      /* A user object: the <=> these are built on asks it for #to_str first,
         so one that answers a string compares, and one that answers none is
         the same failed comparison as above -- which CRuby names by its class
         rather than its inspect (sp_cmperr_desc makes that distinction).
         Lowered as a byte compare, the object's pointer went to
         sp_str_cmp_bytes and the build failed -- or, before that, compared as
         bytes (found by matz reviewing #4265). Both halves are one arm: the
         refusal is the conversion answering NULL, so it is raised where the
         <=> that failed is, not eagerly in front of it. */
      if (ty_is_object(sat)) {
        int tr, to, ts;
        Buf rb = expr_buf(c, recv);
        emit_str_cmp_prologue(c, rb.p ? rb.p : "", argv[0], &tr, &to, &ts, b);
        buf_printf(b, "sp_str_cmp_bytes(_t%d, _t%d) %s 0"
                      " : (sp_raise_cls(\"ArgumentError\", sp_sprintf("
                      "\"comparison of String with %%s failed\","
                      " sp_cmperr_desc(_t%d))), FALSE); })",
                   tr, ts, name, to);
        free(rb.p);
        return 1;
      }
      buf_puts(b, "(sp_str_cmp_bytes(");
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_printf(b, ") %s 0)", name);
      return 1;
    }
    /* Time comparison via sp_time_cmp; a relational against a non-Time operand
       raises ArgumentError (CRuby's Comparable, its <=> having returned nil). */
    if (rt == TY_TIME) {
      if (comp_ntype(c, argv[0]) == TY_TIME) {
        int tt = ++g_tmp, tu = ++g_tmp;
        buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = ", tt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Time _t%d = ", tu); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_time_cmp(_t%d, _t%d) %s 0; })", tt, tu, name);
        return 1;
      }
      /* a poly operand is a Time or not at run time (#4465) */
      if (comp_ntype(c, argv[0]) == TY_POLY || comp_ntype(c, argv[0]) == TY_UNKNOWN) {
        int tt = ++g_tmp, tu = ++g_tmp;
        buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = ", tt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_RbVal _t%d = ", tu); emit_boxed(c, argv[0], b);
        buf_printf(b, "; sp_poly_time_cmp_arg(_t%d, _t%d) %s 0; })", tt, tu, name);
        return 1;
      }
      buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
      buf_puts(b, "); sp_raise_cls(\"ArgumentError\", \"comparison of Time with an incompatible value failed\"); 0; })");
      return 1;
    }
    /* An object whose class defines the comparison operator itself (e.g.
       Set#< aliased to proper_subset?): dispatch to the user method. */
    if (ty_is_object(rt)) {
      int cid5 = ty_object_class(rt);
      if (comp_method_in_chain(c, cid5, name, NULL) >= 0) {
        Buf selfb = emit_cmp_self(c, recv, rt);
        emit_dispatch(c, cid5, name, selfb.p, nt_ref(nt, id, "arguments"), -1, b);
        free(selfb.p);
        return 1;
      }
    }
    /* Comparable: object with a user `<=>` method but no direct `<` etc. */
    if (ty_is_object(rt)) {
      int cid4 = ty_object_class(rt);
      if (comp_method_in_chain(c, cid4, name, NULL) < 0 &&
          comp_method_in_chain(c, cid4, "<=>", NULL) >= 0) {
        if (user_cmp_invalid_ret(c, cid4) != TY_UNKNOWN)
          unsupported_feature(c, id, "Comparable operator on an object whose #<=> returns a non-Integer (protocol requires Integer or nil)");
        /* a `<=>` that can return nil: check it -- incomparable raises the
           Comparable ArgumentError instead of comparing a garbage value */
        if (user_cmp_needs_check(c, cid4)) {
          int ta = hoist_boxed_rooted(c, recv), tb2 = hoist_boxed_rooted(c, argv[0]);
          buf_printf(b, "(sp_poly_cmp_ck(_t%d, _t%d) %s 0)", ta, tb2, name);
          return 1;
        }
        Buf selfb = emit_cmp_self(c, recv, rt);
        buf_puts(b, "(");
        emit_dispatch(c, cid4, "<=>", selfb.p, nt_ref(nt, id, "arguments"), -1, b);
        buf_printf(b, " %s 0)", name);
        free(selfb.p);
        return 1;
      }
    }
    /* Hash subset/superset: < <= > >= over any hash-variant pairing. An empty
       `{}` literal operand (TY_UNKNOWN) is an empty hash all the same (#2399). */
    {
      TyKind h_rt = rt, h_a0 = comp_ntype(c, argv[0]);
      if (h_rt == TY_UNKNOWN && nt_type(nt, recv) &&
          (sp_streq(nt_type(nt, recv), "HashNode") || sp_streq(nt_type(nt, recv), "KeywordHashNode")) &&
          ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; })) h_rt = TY_STR_POLY_HASH;
      if (h_a0 == TY_UNKNOWN && nt_type(nt, argv[0]) &&
          (sp_streq(nt_type(nt, argv[0]), "HashNode") || sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) &&
          ({ int _n = 0; nt_arr(nt, argv[0], "elements", &_n); _n == 0; })) h_a0 = TY_STR_POLY_HASH;
      if (ty_is_hash(h_rt) && argc == 1 && ty_is_hash(h_a0)) {
        int strict = sp_streq(name, "<") || sp_streq(name, ">");
        int flip = sp_streq(name, ">") || sp_streq(name, ">=");
        buf_puts(b, "sp_poly_hash_subset(");
        if (flip) { emit_boxed(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, recv, b); }
        else { emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); }
        buf_printf(b, ", %d)", strict);
        return 1;
      }
    }
    unsupported(c, id, "comparison");
  }

  /* concrete builtin receiver: is_a?/kind_of?/instance_of? is known at compile
     time (evaluate the receiver for side effects, then yield the constant). */
  if (recv >= 0 && argc == 1 &&
      is_kind_query(name) &&
      isa_const_name(nt, argv[0])) {
    /* `[]` and a bare `Array.new` are arrays even when their element type (and
       so the inferred type) is still UNKNOWN -- treat them as such for the fold. */
    TyKind eff_rt = rt;
    if (eff_rt == TY_UNKNOWN) {
      const char *rvt = nt_type(nt, recv);
      if (rvt && sp_streq(rvt, "ArrayNode")) eff_rt = TY_POLY_ARRAY;
      else if (rvt && sp_streq(rvt, "CallNode") && nt_str(nt, recv, "name") &&
               sp_streq(nt_str(nt, recv, "name"), "new")) {
        int rr = nt_ref(nt, recv, "receiver");
        if (rr >= 0 && nt_type(nt, rr) && sp_streq(nt_type(nt, rr), "ConstantReadNode") &&
            nt_str(nt, rr, "name") && sp_streq(nt_str(nt, rr, "name"), "Array")) eff_rt = TY_POLY_ARRAY;
      }
    }
    /* a bool receiver's class depends on its VALUE: true is a TrueClass,
       false a FalseClass (ty_matches_class carries only the type) */
    {
      const char *bcn = nt_str(nt, argv[0], "name");
      if (rt == TY_BOOL && bcn &&
          (sp_streq(bcn, "TrueClass") || sp_streq(bcn, "FalseClass"))) {
        buf_puts(b, "(("); emit_expr(c, recv, b);
        buf_printf(b, ") %s 0)", sp_streq(bcn, "TrueClass") ? "!=" : "==");
        return 1;
      }
    }
    /* Queue and SizedQueue are one runtime object told apart by its bound, so
       these two cannot be answered statically from TY_QUEUE alone -- a
       SizedQueue reported false for its own class (#3466). */
    { const char *qcn = nt_str(nt, argv[0], "name");
      if (eff_rt == TY_QUEUE && qcn &&
          (sp_streq(qcn, "SizedQueue") || sp_streq(qcn, "Queue"))) {
        int tq3 = ++g_tmp;
        int want_sized = sp_streq(qcn, "SizedQueue");
        buf_printf(b, "({ sp_queue *_t%d = ", tq3); emit_expr(c, recv, b);
        /* is_a?(Queue) is true for both (SizedQueue < Queue); instance_of? and
           is_a?(SizedQueue) read the bound. */
        if (!want_sized && !sp_streq(name, "instance_of?"))
          buf_printf(b, "; (void)_t%d; (sp_bool)1; })", tq3);
        else
          buf_printf(b, "; (sp_bool)((sp_Queue_max(_t%d) > 0) == %d); })", tq3, want_sized ? 1 : 0);
        return 1;
      } }
    int yes = ty_matches_class(eff_rt, nt_str(nt, argv[0], "name"), sp_streq(name, "instance_of?"));
    /* an Integer or a Float reads its nil sentinel at run time, as nil?
       does: the nullable-value analysis does not see every way nil reaches
       one (a method's parameter only nil is passed to, a splat of a boxed
       Array), and folded, a nil answered true to is_a?(Integer) */
    if (emit_scalar_class_test(c, recv, eff_rt, nt_str(nt, argv[0], "name"),
                               sp_streq(name, "instance_of?"), b)) return 1;
    /* A nullable String slot answers at run time too: nil is a NilClass and
       is not a String, whatever the slot's kind says. Object and its
       ancestors hold for nil too. */
    if (yes >= 0 && eff_rt == TY_STRING) {
      const char *kn = nt_str(nt, argv[0], "name");
      int nilcls = kn && sp_streq(kn, "NilClass");
      int univ = kn && is_object_root(kn);
      if (nilcls || (yes && !univ)) {
        int tn = ++g_tmp;
        buf_puts(b, "({ "); emit_ctype(c, eff_rt, b); buf_printf(b, " _t%d = ", tn); emit_expr(c, recv, b);
        buf_printf(b, "; %s(_t%d == NULL); })", nilcls ? "" : "!", tn);
        return 1;
      }
    }
    if (yes >= 0) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), %d)", yes); return 1; }
  }

  /* poly.is_a?(class_var) where the argument is a TY_CLASS typed expression.
     Skip if argv[0] is a ConstantReadNode: the fast-path below handles builtins. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 &&
      is_kind_query(name) &&
      comp_ntype(c, argv[0]) == TY_CLASS &&
      !(nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ConstantReadNode"))) {
    int t = ++g_tmp, k = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_expr(c, recv, b); buf_printf(b, "; ");
    buf_printf(b, "sp_Class _t%d = ", k); emit_expr(c, argv[0], b); buf_printf(b, "; ");
    if (sp_streq(name, "instance_of?"))
      buf_printf(b, "sp_poly_get_class(_t%d).cls_id == _t%d.cls_id; })", t, k);
    else
      buf_printf(b, "sp_poly_is_a(_t%d, _t%d); })", t, k);
    return 1;
  }

  /* poly.is_a?(Class) / kind_of?: runtime tag/cls_id check */
  if (recv >= 0 && rt == TY_POLY && argc == 1 &&
      is_kind_query(name)) {
    const char *cty = nt_type(nt, argv[0]);
    char cnq[192];
    const char *cn = isa_match_name(nt, argv[0], cnq, sizeof cnq);
    if (cn) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_expr(c, recv, b); buf_printf(b, "; ");
      char v[32]; snprintf(v, sizeof v, "_t%d", t);
      emit_poly_isa_test(c, cn, v, sp_streq(name, "instance_of?"), b);
      buf_puts(b, "; })");
      return 1;
    }
  }

  /* nil receiver: nil.inspect -> "nil", nil.to_s -> "", nil.nil? -> true.
     Evaluate the receiver for side effects, then yield the constant. */
  if (recv >= 0 && rt == TY_NIL) {
    /* nil & / | / ^ are BOOLEAN ops (nil is false): & is always false, | and ^
       are the argument's truthiness -- not integer bitwise (#2401) */
    if (argc == 1 && is_bit_op(name)) {
      if (sp_streq(name, "&")) {
        buf_puts(b, "((void)("); emit_expr(c, recv, b);
        buf_puts(b, "), (void)("); emit_boxed(c, argv[0], b); buf_puts(b, "), 0)");
      }
      else {
        buf_puts(b, "((void)("); emit_expr(c, recv, b);
        buf_puts(b, "), sp_poly_truthy("); emit_boxed(c, argv[0], b); buf_puts(b, "))");
      }
      return 1;
    }
    if (argc == 0 && sp_streq(name, "inspect")) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), SPL(\"nil\"))"); return 1; }
    if (argc == 0 && sp_streq(name, "to_s"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_str_frozen_empty)"); return 1; }
    if (argc == 0 && sp_streq(name, "nil?"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 1)"); return 1; }
    if (argc == 0 && sp_streq(name, "to_i"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (sp_int)0)"); return 1; }
    if (argc == 0 && sp_streq(name, "to_f"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0.0)"); return 1; }
    if (argc == 0 && (sp_streq(name, "to_r") || sp_streq(name, "rationalize"))) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_rational_new(0, 1))"); return 1; }
    /* nil.rationalize takes an epsilon of any kind and ignores it; it is
       still evaluated, after the receiver */
    if (argc == 1 && sp_streq(name, "rationalize")) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
      emit_expr(c, argv[0], b); buf_puts(b, "), sp_rational_new(0, 1))");
      return 1;
    }
    /* nil =~ anything is nil; nil !~ anything is true (#2385) */
    if (argc == 1 && sp_streq(name, "=~")) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), (void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
      return 1;
    }
    if (argc == 1 && sp_streq(name, "!~")) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), (void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 1)");
      return 1;
    }
    if (argc == 0 && sp_streq(name, "to_c"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (sp_Complex){0, 0})"); return 1; }
    if (argc == 0 && sp_streq(name, "to_a"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_PolyArray_new())"); return 1; }
    if (argc == 0 && sp_streq(name, "to_h"))    {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), sp_SymPolyHash_new())");
      return 1;
    }
    if (argc == 1 && is_kind_query(name)) {
      const char *cn = isa_const_name(nt, argv[0]);
      int yes = cn ? (sp_streq(cn, "NilClass") || sp_streq(name, "instance_of?") ? sp_streq(cn, "NilClass") : (sp_streq(cn, "Object") || sp_streq(cn, "BasicObject"))) : 0;
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), %d)", yes);
      return 1;
    }
    /* NilClass boolean operators: & is always false, | and ^ test the
       operand's truthiness */
    if (argc == 1 && is_bit_op(name)) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), ");
      if (sp_streq(name, "&")) { buf_puts(b, "(void)("); emit_boxed(c, argv[0], b); buf_puts(b, "), 0)"); }
      else { buf_puts(b, "sp_poly_truthy("); emit_boxed(c, argv[0], b); buf_puts(b, "))"); }
      return 1;
    }
    if (argc == 1 && (sp_streq(name, "===") || sp_streq(name, "equal?") || sp_streq(name, "eql?"))) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (");
      emit_boxed(c, argv[0], b);
      buf_puts(b, ").tag == SP_TAG_NIL)");
      return 1;
    }
  }
  /* boolean receiver & | ^ with a non-boolean operand: logical ops on the
     operand's truthiness (an Integer operand included -- 0 is truthy) */
  if (recv >= 0 && rt == TY_BOOL && argc == 1 &&
      is_bit_op(name)) {
    TyKind bat = comp_ntype(c, argv[0]);
    if (bat != TY_BOOL) {
      int tb3 = ++g_tmp;
      buf_printf(b, "({ sp_bool _t%d = ", tb3); emit_expr(c, recv, b);
      buf_printf(b, "; sp_bool _a%d = sp_poly_truthy(", tb3);
      emit_boxed(c, argv[0], b);
      buf_puts(b, "); ");
      if (sp_streq(name, "&")) buf_printf(b, "(_t%d && _a%d); })", tb3, tb3);
      else if (sp_streq(name, "|")) buf_printf(b, "(_t%d || _a%d); })", tb3, tb3);
      else buf_printf(b, "(_t%d != _a%d); })", tb3, tb3);
      return 1;
    }
  }
  /* `Comparable === x` / `Enumerable === x`: the module names no class object
     spinel carries, so the call fell through to the poly dispatch and raised
     NoMethodError. It is the is_a? question with the operands swapped (#3871). */
  if (recv >= 0 && argc == 1 && sp_streq(name, "===") &&
      nt_kind(nt, recv) == NK_ConstantReadNode && nt_str(nt, recv, "name")) {
    const char *mcn = nt_str(nt, recv, "name");
    if ((sp_streq(mcn, "Comparable") || sp_streq(mcn, "Enumerable")) &&
        comp_class_index(c, mcn) < 0) {
      TyKind at = comp_ntype(c, argv[0]);
      int yes;
      if (ty_is_object(at))
        yes = class_includes_module_named(c, ty_object_class(at), mcn);
      else if (sp_streq(mcn, "Comparable"))
        yes = at == TY_INT || at == TY_FLOAT || at == TY_BIGINT || at == TY_STRING ||
              at == TY_SYMBOL || at == TY_TIME || at == TY_RATIONAL;
      else
        yes = ty_is_array(at) || ty_is_hash(at) || at == TY_RANGE ||
              at == TY_FLOAT_RANGE || at == TY_STR_RANGE || at == TY_ENUMERATOR ||
              at == TY_DIR;
      /* nil is neither, and a nullable Integer or Float is nil where it
         holds its sentinel */
      if (yes && (at == TY_INT || at == TY_FLOAT) && call_returns_nullable_int(c, argv[0])) {
        char ref[24];
        buf_puts(b, "({ "); emit_sentinel_bind(c, at, argv[0], ref, sizeof ref, b);
        emit_slot_truthy(at, ref, b); buf_puts(b, "; })");
        return 1;
      }
      buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_printf(b, "), %d)", yes);
      return 1;
    }
  }
  /* true/false receiver: equal?/eql?/=== are value identity */
  if (recv >= 0 && rt == TY_BOOL && argc == 1 &&
      (sp_streq(name, "equal?") || sp_streq(name, "eql?") || sp_streq(name, "==="))) {
    int tb2 = ++g_tmp;
    buf_printf(b, "({ sp_bool _t%d = ", tb2); emit_expr(c, recv, b);
    buf_printf(b, "; sp_RbVal _tb%d = ", tb2); emit_boxed(c, argv[0], b);
    buf_printf(b, "; (_tb%d.tag == SP_TAG_BOOL && _tb%d.v.b == (_t%d != 0)); })", tb2, tb2, tb2);
    return 1;
  }
  /* Symbol receiver: equal?/eql? compare the interned id */
  /* A statically-typed user object is_a? the universal ancestors */
  if (recv >= 0 && ty_is_object(rt) && argc == 1 &&
      (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?"))) {
    const char *acn = nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ConstantReadNode")
                        ? nt_str(nt, argv[0], "name") : NULL;
    if (acn && is_object_root(acn)) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 1)");
      return 1;
    }
    /* a builtin mixin: fold from the class's own `include` declarations (#2363) */
    if (acn && (sp_streq(acn, "Comparable") || sp_streq(acn, "Enumerable") ||
                sp_streq(acn, "Math")) && comp_class_index(c, acn) < 0) {
      int yes = class_includes_module_named(c, ty_object_class(rt), acn);
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), %d)", yes);
      return 1;
    }
  }
  return 0;
}

/* String concatenation, unary -@ +@ ~ !, element stores and the arithmetic on a poly operand */
int emit_call_operator_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind a0) {
  /* String#concat with no arguments returns the receiver unchanged (#2309):
     a stage-1 builtin-op row (builtin_ops.c); the shared handle's arm stays */
  if (recv >= 0 && rt == TY_STRING && emit_builtin_op_stage(c, id, recv, rt, name, 1, b)) return 1;
  if (recv >= 0 && rt == TY_STRBUF && sp_streq(name, "concat") && argc == 0) {
    /* zero-argument concat returns the receiver, but CRuby checks frozen
       first -- the empty append is still a mutation attempt (#3339). The
       receiver once: a call with effects must not run twice. */
    int tcc = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = ", tcc); emit_expr(c, recv, b);
    buf_printf(b, "; sp_str_check_mutable(_t%d); _t%d; })", tcc, tcc);
    return 1;
  }
  /* String#clear consumed as a value: empty the assignable receiver in place
     and yield the now-empty string (#2332) */
  /* TY_STRBUF too: a reader handing out the shared handle is still a String
     receiver, and the strbuf_slot_ref arm just below is the one written for
     it. Gated on TY_STRING alone, the value form of `obj.buf.clear` matched
     nothing, cleared nothing and answered nil. */
  if (recv >= 0 && (rt == TY_STRING || rt == TY_STRBUF) && sp_streq(name, "clear") && argc == 0) {
    /* A shared-mutable receiver owns a buffer: empty it in place and answer
       the same string, as CRuby does. Its read is a copy out of the handle,
       not an lvalue, so the reassignment below did not even compile. */
    { char srefC[1024];
      if (strbuf_slot_ref(c, recv, srefC, sizeof srefC)) {
        int tC2 = ++g_tmp;
        /* marked to hand out the handle (`r = obj.buf.clear`): the
           receiver itself, as for the appends */
        buf_printf(b, "({ sp_String *_t%d = %s; sp_String_set_bin(_t%d, (&(\"\\xff\")[1]));",
                   tC2, srefC, tC2);
        if (c->strbuf_box[id]) buf_printf(b, " _t%d; })", tC2);
        else buf_printf(b, " sp_String_cstr(_t%d); })", tC2);
        return 1;
      } }
    const char *rty = nt_type(nt, recv);
    if (str_mut_var_recv(c, recv)) {
      /* a fresh unfrozen empty: the shared frozen "" would make a later
         mutation of the cleared receiver raise */
      buf_puts(b, "({ sp_str_check_mutable("); emit_expr(c, recv, b);
      buf_puts(b, "); "); emit_expr(c, recv, b); buf_puts(b, " = sp_str_from_bytes(\"\", 0); ");
      emit_expr(c, recv, b); buf_puts(b, "; })");
      return 1;
    }
    /* a direct literal receiver is frozen: FrozenError, as CRuby */
    if (rty && sp_streq(rty, "StringNode")) {
      buf_puts(b, "((const char *)(sp_raise_frozen_str(");
      emit_expr(c, recv, b);
      buf_puts(b, "), (&(\"\\xff\")[1])))");
      return 1;
    }
  }
  if ((sp_streq(name, "-@") || sp_streq(name, "+@")) && recv >= 0 && argc == 0 && !ty_is_object(rt)) {
    if (rt == TY_POLY) {
      if (name[0] == '-') { buf_puts(b, "sp_poly_neg("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else { buf_puts(b, "sp_poly_uplus("); emit_expr(c, recv, b); buf_puts(b, ")"); }
    }
    else if (rt == TY_STRING) {
      /* +str is str itself unless it is frozen, else a mutable copy (so a
         later <</concat/upcase! mutates it); -str returns a FROZEN string
         (#2331). The copy is sp_str_dup, not sp_str_dup_external: the latter
         sizes it with strlen, which truncated an embedded NUL (#3473). */
      if (name[0] == '+') { buf_puts(b, "sp_str_uplus("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else { buf_puts(b, "sp_str_uminus_val("); emit_expr(c, recv, b); buf_puts(b, ")"); }  /* frozen recv: identity (#2369) */
    }
    else if (rt == TY_BIGINT) {
      /* -@ negates via 0 - b (no unary neg on a bigint pointer); +@ is self (#2304) */
      if (name[0] == '-') { buf_puts(b, "sp_bigint_sub(sp_bigint_new_int(0), "); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else emit_expr(c, recv, b);
    }
    else {
      /* `-(-7)` puts the two signs side by side, and C reads `--7LL` as a
         pre-decrement of a literal and rejects it. A space separates them;
         the common `-x` keeps its tight spelling (#4008). */
      Buf ub; memset(&ub, 0, sizeof ub); emit_expr(c, recv, &ub);
      const char *ut = ub.p ? ub.p : "";
      /* a nullable Integer or Float slot's nil has no -@: negating the Float
         sentinel flipped its sign bit into a NaN that was no longer nil, and
         the int sentinel is INTPTR_MIN, whose negation overflows */
      if (name[0] == '-' && (rt == TY_FLOAT || rt == TY_INT) && cmp_operand_may_be_nil(c, recv)) {
        int tn = ++g_tmp;
        buf_printf(b, "({ %s _t%d = %s; if (SP_UNLIKELY(%s(_t%d))) sp_nil_recv(\"%s\"); %c_t%d; })",
                   rt == TY_FLOAT ? "sp_float" : "sp_int", tn, ut,
                   rt == TY_FLOAT ? "sp_float_is_nil" : "SP_INT_NIL ==", tn, name, name[0], tn);
      }
      else buf_printf(b, "(%c%s%s)", name[0], ut[0] == name[0] ? " " : "", ut);
      free(ub.p); }
    return 1;
  }
  /* h.default_proc = <a Proc value>: install a trampoline that drives the
     Proc, so any callable works (#3563) -- a lambda literal included, which
     is a Proc of its own like any other and keeps its own body's typing,
     `return` and preludes. */
  if (recv >= 0 && sp_streq(name, "default_proc=") && argc == 1 &&
      (comp_ntype(c, recv) == TY_STR_POLY_HASH || comp_ntype(c, recv) == TY_SYM_POLY_HASH ||
       comp_ntype(c, recv) == TY_POLY_POLY_HASH) &&
      comp_ntype(c, argv[0]) == TY_PROC) {
    TyKind hrt = comp_ntype(c, recv);
    const char *hn2 = ty_hash_cname(hrt);
    const char *keyct = hrt == TY_SYM_POLY_HASH ? "sp_sym"
                      : hrt == TY_STR_POLY_HASH ? "const char *" : "sp_RbVal";
    const char *kbox = hrt == TY_SYM_POLY_HASH ? "sp_box_sym(_key)"
                     : hrt == TY_STR_POLY_HASH ? "sp_box_str(_key)" : "_key";
    int dn2 = ++g_proc_counter;
    buf_printf(&g_procs,
      "static sp_RbVal _sp_hash_dproc_%d(sp_%sHash *_self_h, %s _key, void *_dproc_self) {\n"
      "  return sp_penum_call2((sp_Proc *)_dproc_self,"
      " sp_box_nullable_obj((void *)_self_h, %s), %s);\n}\n",
      dn2, hn2, keyct, hash_box_cls(hrt), kbox);
    int th2 = ++g_tmp, tp2 = ++g_tmp;
    buf_printf(b, "({ sp_%sHash *_t%d = ", hn2, th2); emit_expr(c, recv, b);
    buf_printf(b, "; sp_Proc *_t%d = ", tp2); emit_expr(c, argv[0], b);
    buf_printf(b, "; if (_t%d && sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);",
               th2, th2, th2, hash_box_cls(hrt));
    /* An assignment's value in Ruby is the RIGHT-HAND SIDE: `h.default_proc
       = p` answers the proc, not the hash, which would land a sp_XHash * in
       whatever slot the expression feeds -- a Proc * one here (#3833). */
    TyKind vt2 = comp_ntype(c, id);
    int ans = (vt2 == TY_PROC || vt2 == TY_POLY || vt2 == TY_UNKNOWN) ? tp2 : th2;
    buf_printf(b, " _t%d->dproc = _sp_hash_dproc_%d; _t%d->dproc_self = (void *)_t%d;"
                  " sp_gc_wb((void *)_t%d); _t%d; })",
               th2, dn2, th2, tp2, th2, ans);
    return 1;
  }
  /* value-position String#[]= (s[i] = v / s[i, n] = v / s[range] = v / s["sub"]
     = v on an assignable receiver): run the mutate statement, the expression's
     value is the assigned string (#2370). */
  /* A reader call handing out the shared handle takes the same statement,
     through its shim (#3227). */
  if (recv >= 0 && sp_streq(name, "[]=") && (argc == 2 || argc == 3) &&
      ((comp_ntype(c, recv) == TY_STRING &&
        nt_type(nt, recv) && (sp_streq(nt_type(nt, recv), "LocalVariableReadNode") ||
                              sp_streq(nt_type(nt, recv), "InstanceVariableReadNode"))) ||
       (comp_ntype(c, recv) == TY_STRBUF && nt_kind(nt, recv) == NK_CallNode))) {
    Buf mb; memset(&mb, 0, sizeof mb);
    if (emit_array_mutate_stmt(c, id, &mb, 0)) {
      buf_puts(b, "({ ");
      buf_puts(b, mb.p ? mb.p : "");
      emit_expr(c, argv[argc - 1], b);
      buf_puts(b, "; })");
      free(mb.p);
      return 1;
    }
    free(mb.p);
  }
  /* poly `<<` in expression position: sp_poly_shl dispatches on the runtime tag
     (Integer#<< shift -> boxed int, Array#push append -> the array) and returns
     a poly either way, matching the statement-level path. */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "<<") && argc == 1) {
    buf_puts(b, "sp_poly_shl("); emit_expr(c, recv, b); buf_puts(b, ", ");
    emit_boxed(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  /* unary bitwise complement: ~int -> (~x); ~poly -> coerce to int first */
  if (sp_streq(name, "~") && recv >= 0 && argc == 0 && (rt == TY_INT || rt == TY_POLY)) {
    if (rt == TY_POLY) { buf_puts(b, "(~sp_poly_recv_i(\"~\", "); emit_expr(c, recv, b); buf_puts(b, "))"); }
    else { buf_puts(b, "(~"); emit_expr(c, recv, b); buf_puts(b, ")"); }
    return 1;
  }
  /* poly parity predicates: Integer-only in Ruby, so truncation cannot lose a
     value that legally reaches them and the int coercion stays correct.
     zero?/positive?/negative? are NOT handled here: this arm ran ahead of
     emit_poly_call and shadowed sp_poly_zero_p and friends, which dispatch on
     the runtime tag. Truncating first answered every |v| < 1 as zero (0.004
     came back zero? -> true, positive? -> false), read a bigint through a
     wrapped int64, and hid a user class's own zero?. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 &&
      (sp_streq(name, "even?") || sp_streq(name, "odd?"))) {
    int t = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = sp_poly_recv_i(\"%s\", ", t, name); emit_expr(c, recv, b); buf_puts(b, "); ");
    if (sp_streq(name, "even?")) buf_printf(b, "(_t%d %% 2 == 0); })", t);
    else buf_printf(b, "(_t%d %% 2 != 0); })", t);
    return 1;
  }

  if (sp_streq(name, "!") && recv >= 0 && argc == 0) {
    /* A user-defined #! wins over truthiness. The generic arms below cast the
       receiver to a pointer, which for a value-type object is not even a
       conversion the C accepts (#3819). */
    if (ty_is_object(rt)) {
      int bmi = comp_method_in_chain(c, ty_object_class(rt), "!", NULL);
      if (bmi >= 0) {
        buf_puts(b, "(");
        emit_method_cname(c, &c->scopes[bmi], b);
        buf_puts(b, "(");
        emit_expr(c, recv, b);
        buf_puts(b, "))");
        return 1;
      }
    }
    /* Ruby truthiness: only nil and false are falsy. `!x` negates the same
       per-type truthiness emit_cond uses -- a poly / nullable scalar / nullable
       pointer can be falsy, so the result is not unconditionally false. */
    if (rt == TY_BOOL) { buf_puts(b, "(!"); emit_expr(c, recv, b); buf_puts(b, ")"); }
    else if (rt == TY_NIL) { buf_puts(b, "1"); }
    else if (rt == TY_POLY) { buf_puts(b, "(!sp_poly_truthy("); emit_expr(c, recv, b); buf_puts(b, "))"); }
    else if (rt == TY_INT) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == SP_INT_NIL)"); }
    else if (rt == TY_FLOAT) { buf_puts(b, "sp_float_is_nil("); emit_expr(c, recv, b); buf_puts(b, ")"); }
    else if (rt == TY_CLASS) { buf_puts(b, "sp_class_nil_p("); emit_expr(c, recv, b); buf_puts(b, ")"); }
    /* a by-value object has no pointer to null-check and is never falsy (#2633) */
    else if (ty_is_object(rt) && comp_ty_value_obj(c, rt)) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), 0)");
    }
    else if (rt == TY_STRING || ty_is_array(rt) || ty_is_hash(rt) || ty_is_object(rt) ||
             rt == TY_PROC ||
             rt == TY_MATCHDATA || rt == TY_EXCEPTION || rt == TY_FIBER || rt == TY_IO) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == 0)");  /* NULL pointer is falsy */
    }
    else { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), 0)"); }  /* always-truthy -> false */
    return 1;
  }

  /* default Object#<=>: 0 when the operands are the same object, nil otherwise
     (identity). Only for a reference user object with no user `<=>` (#2686). */
  if (sp_streq(name, "<=>") && argc == 1 && recv >= 0 && ty_is_object(rt) &&
      !comp_ty_value_obj(c, rt) && comp_method_in_chain(c, ty_object_class(rt), "<=>", NULL) < 0 &&
      comp_native_method_find(c, ty_object_class(rt), "<=>", 1, 0) < 0) {
    const char *cn = c->classes[ty_object_class(rt)].c_name;
    if (comp_ntype(c, argv[0]) == rt) {
      int ta = ++g_tmp, tb = ++g_tmp;
      buf_printf(b, "({ sp_%s *_t%d = ", cn, ta); emit_expr(c, recv, b);
      buf_printf(b, "; sp_%s *_t%d = ", cn, tb); emit_expr(c, argv[0], b);
      buf_printf(b, "; _t%d == _t%d ? sp_box_int(0) : sp_box_nil(); })", ta, tb);
    }
    else {
      /* a different-class operand is never the same object: nil */
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
      emit_boxed(c, argv[0], b); buf_puts(b, "), sp_box_nil())");
    }
    return 1;
  }

  /* Numeric#fdiv on a boxed receiver: both operands as Floats, a Float out
     (#3767). Ahead of the poly arithmetic below, whose ops all answer boxed. */
  if (recv >= 0 && argc == 1 && rt == TY_POLY && sp_streq(name, "fdiv") &&
      comp_ntype(c, id) == TY_FLOAT) {
    buf_puts(b, "sp_poly_fdiv("); emit_boxed(c, recv, b); buf_puts(b, ", ");
    emit_boxed(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  /* poly arithmetic: sp_poly_<op>(boxed, boxed) -> a (poly) result.
     `str + poly` / `str * poly` are string concat/repeat (handled below as
     sp_str_concat/sp_str_repeat with the poly operand coerced), not poly
     arithmetic, so let them fall through. */
  if (recv >= 0 && argc == 1 && (rt == TY_POLY || a0 == TY_POLY) &&
      rt != TY_TIME &&  /* Time +/- a poly is Time arithmetic (emit_array_arith_call, #2456) */
      !(rt == TY_STRING && (sp_streq(name, "+") || sp_streq(name, "*"))) &&
      !((ty_is_array(rt) || rt == TY_POLY_ARRAY) && sp_streq(name, "*")) &&
      /* `arr - x` is a set difference, served by the set-op arms with their
         run-time Array coercion; routing it here reached sp_poly_sub, which
         has no array case and answered "no implicit conversion of Array into
         Array" on two real Arrays (#3475) */
      !((ty_is_array(rt) || rt == TY_POLY_ARRAY) && sp_streq(name, "-"))) {
    const char *pfn = NULL;
    if (sp_streq(name, "+")) pfn = "sp_poly_add";
    else if (sp_streq(name, "-")) pfn = "sp_poly_sub";
    else if (sp_streq(name, "*")) pfn = "sp_poly_mul";
    else if (sp_streq(name, "/")) pfn = "sp_poly_div";
    else if (sp_streq(name, "%")) pfn = "sp_poly_mod";
    else if (sp_streq(name, "**")) pfn = "sp_poly_pow";
    /* The named divisions belong here too -- but only for a receiver that has
       no arm of its own. A Rational answered NoMethodError for `quo` the
       moment the other operand was boxed, for a name its own `/` already
       served; a typed Integer or Float, by contrast, already reaches a
       direct scalar helper (`17.remainder(x)` is sp_iremainder over an
       unboxed argument), and routing it through the boxed dispatch instead
       would box the receiver, call the generic helper and unbox the result
       -- correct, and three operations worse, on a path that was fine. */
    else if (rt != TY_INT && rt != TY_FLOAT && rt != TY_BIGINT && !user_defines_or_reads(c, name)) {
      if (sp_streq(name, "quo")) pfn = "sp_poly_quo";
      else if (sp_streq(name, "fdiv")) pfn = "sp_poly_fdiv";
      else if (sp_streq(name, "div")) pfn = "sp_poly_div_m";
      else if (sp_streq(name, "divmod")) pfn = "sp_poly_divmod";
      else if (sp_streq(name, "modulo")) pfn = "sp_poly_modulo";
      else if (sp_streq(name, "remainder")) pfn = "sp_poly_remainder";
    }

    if (pfn) {
      /* The receiver's value is a C temporary until the call runs, and the
         ARGUMENT is evaluated in between. When that argument can allocate, a
         collection lands with the receiver held nowhere the root scan can see
         it -- and a chained `a + b + c` makes the receiver a freshly built
         string, so it is swept and the concat reads freed memory (#3396).
         Hoist it into a rooted temp first. Only when the argument can
         allocate: otherwise nothing can collect in the window. */
      /* A user operator whose OTHER operand is poly runs through the boxed
         path, but the fixpoint may still have settled this call's own type on
         the one class that defines it (`(a % o).value` with `%` answering an
         Int64). The value is an sp_RbVal here, so unbox it to the type the
         reader on it expects (#3781). */
      TyKind pres = comp_ntype(c, id);
      /* --plan-check: the boxed operator dispatches at run time to every
         class's own operator of the name: each is an arm, compared with the
         operator inference bound the call to (a call it bound none for is a
         poly operation, not a binding) */
      if (g_plan_check && c->ucall_inf[id].via != UC_NONE)
        for (int k = 0; k < c->nclasses; k++) {
          int kmi = comp_method_in_class(c, k, name);
          if (kmi >= 0) ucall_observe(c, id, kmi, k, 1);
        }
      Buf pcall; memset(&pcall, 0, sizeof pcall);
      if (subtree_may_allocate(nt, argv[0])) {
        int th = ++g_tmp;
        Buf rb; memset(&rb, 0, sizeof rb);
        emit_boxed(c, recv, &rb);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
                   th, rb.p ? rb.p : "sp_box_nil()", th);
        free(rb.p);
        buf_printf(&pcall, "%s(_t%d, ", pfn, th); emit_boxed(c, argv[0], &pcall); buf_puts(&pcall, ")");
      }
      else {
        buf_printf(&pcall, "%s(", pfn); emit_boxed(c, recv, &pcall);
        buf_puts(&pcall, ", "); emit_boxed(c, argv[0], &pcall); buf_puts(&pcall, ")");
      }
      if (ty_is_object(pres)) emit_unbox_text(c, pres, pcall.p ? pcall.p : "sp_box_nil()", b);
      /* The named divisions all answer boxed, while inference gives the call
         whatever class the receiver's own arm promises -- an Integer for
         `div` and for `remainder` on an Integer receiver, a pair for
         `divmod`, a Float where a Float operand decides it. Coerce the boxed
         answer to that, or the generated C is handed an sp_RbVal where a
         scalar belongs. The operators above never needed this: they are
         typed poly wherever they reach here. */
      else if (sp_streq(name, "div") || sp_streq(name, "divmod") ||
               sp_streq(name, "modulo") || sp_streq(name, "remainder")) {
        const char *unbox = pres == TY_INT ? "sp_poly_to_i("
                          : pres == TY_FLOAT ? "sp_poly_to_f("
                          : pres == TY_POLY_ARRAY ? "sp_poly_to_poly_array("
                          : NULL;
        if (unbox) {
          buf_puts(b, unbox); buf_puts(b, pcall.p ? pcall.p : "sp_box_nil()"); buf_puts(b, ")");
        }
        else buf_puts(b, pcall.p ? pcall.p : "sp_box_nil()");
      }
      else buf_puts(b, pcall.p ? pcall.p : "sp_box_nil()");
      free(pcall.p);
      return 1;
    }
    const char *cfn = NULL;
    if (sp_streq(name, "<")) cfn = "sp_poly_lt";
    else if (sp_streq(name, ">")) cfn = "sp_poly_gt";
    else if (sp_streq(name, "<=")) cfn = "sp_poly_le";
    else if (sp_streq(name, ">=")) cfn = "sp_poly_ge";
    /* a user operator that answers a non-bool goes through the dispatch */
    if (cfn && comp_ntype(c, id) == TY_POLY && user_defines_or_reads(c, name)) {
      buf_printf(b, "sp_poly_relop_v(\"%s\", ", name); emit_boxed(c, recv, b); buf_puts(b, ", ");
      emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    if (cfn) {
      buf_printf(b, "%s(", cfn); emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
  }

  /* Array#* (join): arr * sep_str -> elements joined by the separator: a
     stage-4 builtin-op row (builtin_ops.c) */
  if (recv >= 0 && ty_is_array(rt) && emit_builtin_op_stage(c, id, recv, rt, name, 4, b)) return 1;
  return 0;
}
