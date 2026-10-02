/* codegen_call_array.c -- the builtin-op emitters of the Array family
   (BOP_ANY_ARRAY) that are more than one C text with its placeholders: an
   arm whose typed-array and poly-array forms differ in more than the
   variant's name ($A), or that builds its C in steps. The rest are
   templates in builtin_ops.c. emit_array_call looks the rows up after the
   arms every variant shares, so only an Int, Float, Str or poly array
   reaches these. Each answers 1 when it emitted the call, 0 (having
   emitted nothing) to leave it to the chain after the lookup. */

#include "codegen_internal.h"
#include "builtin_ops.h"

/* the element-kind name of the receiver: "Poly" or array_kind's */
static const char *arr_kind(TyKind rt) {
  return rt == TY_POLY_ARRAY ? "Poly" : array_kind(rt);
}

/* The inspect label of a blockless combinator's Enumerator, CRuby's
   `combination(2)` or an argless `permutation`; `tn` holds the count. */
static void emit_combinator_enum_label(const char *name, int argc, int tn, Buf *b) {
  if (argc == 1) buf_printf(b, "sp_sprintf(\"%s(%%lld)\", (long long)_t%d)", name, tn);
  else buf_printf(b, "SPL(\"%s\")", name);
}

/* shift(n) / pop(n): the removed subarray, via the slice! splice (pop takes
   the tail, shift the head; n clamps to the length) */
int emit_op_array_shift_n(Compiler *c, const BopCtx *x, Buf *b) {
  const char *name = x->name;
  int recv = x->recv, argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  if (x->rt == TY_POLY_ARRAY) {
    int t = ++g_tmp, tn2 = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_int _t%d = ", t, tn2); emit_int_expr(c, argv[0], b);
    buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\");", tn2);
    buf_printf(b, " if (_t%d > _t%d->len) _t%d = _t%d->len;", tn2, t, tn2, t);
    if (sp_streq(name, "pop"))
      buf_printf(b, " sp_PolyArray_slice_bang(_t%d, _t%d->len - _t%d, _t%d); })", t, t, tn2, tn2);
    else
      buf_printf(b, " sp_PolyArray_slice_bang(_t%d, 0, _t%d); })", t, tn2);
    return 1;
  }
  const char *k = array_kind(x->rt);
  int t = ++g_tmp, tn2 = ++g_tmp;
  /* rooted across the count, as the poly arm roots its receiver */
  buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_recv_rooted(c, recv, t, "SP_GC_ROOT", b);
  buf_printf(b, "sp_int _t%d = ", tn2); emit_int_expr(c, argv[0], b);
  buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\");", tn2);
  buf_printf(b, " if (_t%d > _t%d->len) _t%d = _t%d->len;", tn2, t, tn2, t);
  if (sp_streq(name, "pop"))
    buf_printf(b, " sp_%sArray_slice_bang(_t%d, _t%d->len - _t%d, _t%d); })", k, t, t, tn2, tn2);
  else
    buf_printf(b, " sp_%sArray_slice_bang(_t%d, 0, _t%d); })", k, t, tn2);
  return 1;
}

/* blockless cycle(n): the receiver repeated n times, materialized. A poly
   one typed as an Enumerator is one (emit_array_call, before the lookup). */
int emit_op_array_cycle_n(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv, argc;
  TyKind rt = x->rt;
  const int *argv = call_args(c->nt, x->id, &argc);
  if (rt == TY_POLY_ARRAY) {
    int t = ++g_tmp, tn2 = ++g_tmp, tr2 = ++g_tmp, tj = ++g_tmp, ti2 = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_int _t%d = ", t, tn2); emit_int_expr(c, argv[0], b);
    buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr2, tr2);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)", tj, tj, tn2, tj);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)", ti2, ti2, t, ti2);
    buf_printf(b, " sp_PolyArray_push(_t%d, _t%d->data[_t%d]);", tr2, t, ti2);
    buf_printf(b, " _t%d; })", tr2);
    return 1;
  }
  const char *k = array_kind(rt);
  int t = ++g_tmp, tn2 = ++g_tmp, tr2 = ++g_tmp, tj = ++g_tmp, ti2 = ++g_tmp;
  buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
  buf_printf(b, "; sp_int _t%d = ", tn2); emit_int_expr(c, argv[0], b);
  buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);", k, tr2, k, tr2);
  buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)", tj, tj, tn2, tj);
  buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)", ti2, ti2, t, ti2);
  buf_printf(b, " sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));", k, tr2, k, t, ti2);
  if (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY)   /* the receiver's nils, repeated */
    buf_printf(b, " sp_%sArray_nil_from(_t%d, _t%d);", k, tr2, t);
  buf_printf(b, " _t%d; })", tr2);
  return 1;
}

/* last: a self-contained statement expression, the receiver bound to a
   temp (needed twice, for the length and the index) inside `({ ... })`
   rather than spilled to g_pre. A g_pre decl leaks into an expression
   context when `.last` is itself hoisted -- e.g. as the receiver of a
   following `.call` (`pipe.last.call(x)`), where it landed mid-`_t = ...`
   (#2942). */
int emit_op_array_last(Compiler *c, const BopCtx *x, Buf *b) {
  int t = ++g_tmp;
  Buf rb = expr_buf(c, x->recv);
  if (x->rt == TY_POLY_ARRAY)
    buf_printf(b, "({ sp_PolyArray *_t%d = %s; sp_PolyArray_get(_t%d, sp_PolyArray_length(_t%d) - 1); })",
               t, rb.p ? rb.p : "", t, t);
  else {
    const char *k = array_kind(x->rt);
    buf_printf(b, "({ %s _t%d = %s; sp_%sArray_get(_t%d, sp_%sArray_length(_t%d) - 1); })",
               c_type_name(x->rt), t, rb.p ? rb.p : "", k, t, k, t);
  }
  free(rb.p);
  return 1;
}

/* join / join(sep): with a separator the receiver is held across it, since
   it may allocate; without one nothing runs between the two */
int emit_op_array_join(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv, argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  const char *k = arr_kind(x->rt);
  Buf rjn; memset(&rjn, 0, sizeof rjn); char tyj[32];
  snprintf(tyj, sizeof tyj, "sp_%sArray *", k);
  int cjn = argc == 1 && hold_recv_open(c, recv, 0, tyj, "SP_GC_ROOT", b, &rjn);
  buf_printf(b, "sp_%sArray_join(", k);
  if (argc == 1) buf_puts(b, rjn.p); else emit_expr(c, recv, b);
  buf_puts(b, ", ");
  /* the separator must be a const char*; a poly separator (e.g. a reader
     whose ivar widened to poly) is converted with sp_poly_to_s */
  if (argc == 1 && comp_ntype(c, argv[0]) == TY_POLY) {
    buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")");
  }
  /* nil is a legal separator (it means ""); false is not. The raw
     emit_expr passed both straight into the const char* slot, and the
     join then read a NULL as a string -- a segfault for either. */
  else if (argc == 1) emit_str_expr_nilable(c, argv[0], b);
  else buf_puts(b, "sp_str_empty");
  buf_puts(b, ")");
  free(rjn.p);
  if (cjn) buf_puts(b, "; })");
  return 1;
}

/* sort!: in place, answering self; a typed receiver that may hold the nil
   sentinel checks it first, as the comparison would raise */
int emit_op_array_sort_bang(Compiler *c, const BopCtx *x, Buf *b) {
  int t = ++g_tmp;
  if (x->rt == TY_POLY_ARRAY) {
    buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, x->recv, b);
    buf_printf(b, "; sp_PolyArray_sort_bang(_t%d); _t%d; })", t, t);
    return 1;
  }
  const char *k = array_kind(x->rt);
  buf_printf(b, "({ sp_%sArray *_t%d = ", k, t);
  emit_nil_ck_recv(c, x->recv, x->rt, "cmp", 0, b);
  buf_printf(b, "; sp_%sArray_sort_bang(_t%d); _t%d; })", k, t, t);
  return 1;
}

/* slice!(range): normalize begin/length against the live length; the
   receiver is rooted across the range, whose bounds may allocate. A
   beginless bound starts at 0 and an endless one runs to the end, the same
   sentinels Array#[] resolves (#3835). */
int emit_op_array_slice_bang_range(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv, argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  int ta = ++g_tmp, tr = ++g_tmp, tf = ++g_tmp, tn = ++g_tmp;
  if (x->rt == TY_POLY_ARRAY) {
    buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_recv_rooted(c, recv, ta, "SP_GC_ROOT", b);
    buf_printf(b, "sp_Range _t%d = ", tr); emit_expr(c, argv[0], b);
    buf_printf(b, "; sp_int _t%d = _t%d.first == INTPTR_MIN ? 0"
                  " : (_t%d.first < 0 ? _t%d.first + (_t%d ? _t%d->len : 0) : _t%d.first);",
               tf, tr, tr, tr, ta, ta, tr);
    buf_printf(b, " sp_int _t%d = _t%d.last == INTPTR_MAX ? ((_t%d ? _t%d->len : 0) - _t%d)"
                  " : ((_t%d.last < 0 ? _t%d.last + (_t%d ? _t%d->len : 0) : _t%d.last) - _t%d + (_t%d.excl ? 0 : 1));",
               tn, tr, ta, ta, tf, tr, tr, ta, ta, tr, tf, tr);
    /* a start still negative lies before the first element: passed as
       given, the runtime answers nil for it (after its frozen check) */
    buf_printf(b, " sp_PolyArray_slice_bang(_t%d, _t%d < 0 ? _t%d - (_t%d ? _t%d->len : 0) : _t%d,"
                  " _t%d < 0 ? 0 : _t%d); })", ta, tf, tf, ta, ta, tf, tn, tn);
    return 1;
  }
  const char *k = array_kind(x->rt);
  buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_recv_rooted(c, recv, ta, "SP_GC_ROOT", b);
  buf_printf(b, "sp_Range _t%d = ", tr); emit_expr(c, argv[0], b);
  buf_printf(b, "; sp_int _t%d = _t%d.first == INTPTR_MIN ? 0"
                " : (_t%d.first < 0 ? _t%d.first + (_t%d ? _t%d->len : 0) : _t%d.first);",
             tf, tr, tr, tr, ta, ta, tr);
  buf_printf(b, " sp_int _t%d = _t%d.last == INTPTR_MAX ? ((_t%d ? _t%d->len : 0) - _t%d)"
                " : ((_t%d.last < 0 ? _t%d.last + (_t%d ? _t%d->len : 0) : _t%d.last) - _t%d + (_t%d.excl ? 0 : 1));",
             tn, tr, ta, ta, tf, tr, tr, ta, ta, tr, tf, tr);
  buf_printf(b, " sp_%sArray_slice_bang(_t%d, _t%d, _t%d < 0 ? 0 : _t%d); })", k, ta, tf, tn, tn);
  return 1;
}

/* Array#+: the same kind concatenates; another kind boxes both sides into a poly array */
int emit_op_array_plus(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if (sp_streq(name, "+") && argc == 1 && a0 == TY_POLY_ARRAY) {
      /* Spill the receiver: evaluating the operand can allocate, and until
         the concat runs the receiver is in nothing but this temp. */
      int t = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); sp_PolyArray_concat(_t%d, ", t, t);
      emit_expr(c, argv[0], b); buf_puts(b, "); })");
      return 1;
    }
    /* poly_array + typed array: box the typed operand to poly, then concat. */
    if (sp_streq(name, "+") && argc == 1 && ty_is_array(a0) && a0 != TY_POLY_ARRAY) {
      const char *conv = a0 == TY_INT_ARRAY ? "sp_IntArray_to_poly" :
                         a0 == TY_FLOAT_ARRAY ? "sp_FloatArray_to_poly" :
                         a0 == TY_STR_ARRAY ? "sp_StrArray_to_poly_fmt" : NULL;
      if (conv) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_PolyArray_concat(_t%d, %s(", t, t, conv);
        emit_expr(c, argv[0], b); buf_puts(b, ")); })");
        return 1;
      }
    }
    return 0;
  }
  if (sp_streq(name, "+") && argc == 1 && a0 == rt) {
    /* array + array of the same kind -> a fresh concatenation */
    buf_printf(b, "sp_%sArray_concat(", k);
    emit_expr(c, recv, b); buf_puts(b, ", ");
    emit_expr(c, argv[0], b);
    buf_puts(b, ")");
    return 1;
  }
  if (sp_streq(name, "+") && argc == 1 && ty_is_array(a0) && a0 != rt) {
    /* array + different-kind array -> poly_array */
    const char *k2 = (a0 == TY_POLY_ARRAY) ? "Poly" : array_kind(a0);
    if (k2) {
      int tL = ++g_tmp, tR = ++g_tmp, tO = ++g_tmp, ti = ++g_tmp;
      Buf lbuf = expr_buf(c, recv);
      Buf rbuf = expr_buf(c, argv[0]);
      const char *box_l = (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY) ? typed_elem_box_fn(rt) :
                          (rt == TY_STR_ARRAY) ? "sp_box_str" : NULL;
      const char *box_r = (a0 == TY_INT_ARRAY || a0 == TY_FLOAT_ARRAY) ? typed_elem_box_fn(a0) :
                          (a0 == TY_STR_ARRAY) ? "sp_box_str" : NULL;
      /* an Integer or Float side's box takes its may_nil, read once */
      char nf_l[24] = "", nf_r[24] = "";
      const char *get_l = (rt == TY_POLY_ARRAY) ? "sp_PolyArray_get" :
                          NULL;
      const char *get_r = (a0 == TY_POLY_ARRAY) ? "sp_PolyArray_get" :
                          NULL;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);\n", k, tL, lbuf.p ? lbuf.p : "", tL); free(lbuf.p);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);\n", k2, tR, rbuf.p ? rbuf.p : "", tR); free(rbuf.p);
      if (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY) {
        int tn = ++g_tmp; snprintf(nf_l, sizeof nf_l, "_t%d, ", tn);
        char an[24]; snprintf(an, sizeof an, "_t%d", tL);
        emit_indent(g_pre, g_indent); buf_printf(g_pre, "int _t%d = ", tn); emit_may_nil_text(c, recv, rt, an, g_pre); buf_puts(g_pre, ";\n");
      }
      if (a0 == TY_INT_ARRAY || a0 == TY_FLOAT_ARRAY) {
        int tn = ++g_tmp; snprintf(nf_r, sizeof nf_r, "_t%d, ", tn);
        char an[24]; snprintf(an, sizeof an, "_t%d", tR);
        emit_indent(g_pre, g_indent); buf_printf(g_pre, "int _t%d = ", tn); emit_may_nil_text(c, argv[0], a0, an, g_pre); buf_puts(g_pre, ";\n");
      }
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tO, tO);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)\n", ti, ti, k, tL, ti);
      emit_indent(g_pre, g_indent + 1);
      if (rt == TY_POLY_ARRAY)
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));\n", tO, tL, ti);
      else if (box_l)
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s(%ssp_%sArray_get(_t%d, _t%d)));\n", tO, box_l, nf_l, k, tL, ti);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)\n", ti, ti, k2, tR, ti);
      emit_indent(g_pre, g_indent + 1);
      if (a0 == TY_POLY_ARRAY)
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));\n", tO, tR, ti);
      else if (box_r)
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s(%ssp_%sArray_get(_t%d, _t%d)));\n", tO, box_r, nf_r, k2, tR, ti);
      buf_printf(b, "_t%d", tO);
      (void)get_l; (void)get_r;
      return 1;
    }
  }
  return 0;
}

/* Array#& / #| / #- and intersection / union / difference with operands: the
   same kind runs the typed set op; another kind, or a boxed operand, the poly one */
int emit_op_array_setop(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if ((sp_streq(name, "&") || sp_streq(name, "intersection") ||
         sp_streq(name, "|") || sp_streq(name, "union") ||
         sp_streq(name, "-") || sp_streq(name, "difference")) && argc == 1 && (a0 == TY_POLY_ARRAY || a0 == TY_UNKNOWN)) {
      const char *fn = (sp_streq(name, "&") || sp_streq(name, "intersection")) ? "intersect" : (sp_streq(name, "|") || sp_streq(name, "union") ? "union" : "difference");
      buf_printf(b, "sp_PolyArray_%s(", fn);
      emit_expr(c, recv, b); buf_puts(b, ", ");
      if (a0 == TY_UNKNOWN) buf_puts(b, "NULL"); else emit_expr(c, argv[0], b);
      buf_puts(b, ")"); return 1;
    }
    /* poly-array set-op with a typed-array argument (different element type):
       box the argument to a poly array, then run the poly op. */
    if ((sp_streq(name, "&") || sp_streq(name, "intersection") ||
         sp_streq(name, "|") || sp_streq(name, "union") ||
         sp_streq(name, "-") || sp_streq(name, "difference")) && argc == 1 &&
        (a0 == TY_INT_ARRAY || a0 == TY_STR_ARRAY || a0 == TY_FLOAT_ARRAY)) {
      const char *fn = (sp_streq(name, "&") || sp_streq(name, "intersection")) ? "intersect" : (sp_streq(name, "|") || sp_streq(name, "union") ? "union" : "difference");
      const char *conv = a0 == TY_INT_ARRAY ? "sp_IntArray_to_poly" :
                         a0 == TY_STR_ARRAY ? "sp_StrArray_to_poly_fmt" : "sp_FloatArray_to_poly";
      buf_printf(b, "sp_PolyArray_%s(", fn);
      emit_expr(c, recv, b); buf_printf(b, ", %s(", conv); emit_expr(c, argv[0], b);
      buf_puts(b, "))"); return 1;
    }
    /* poly-array receiver, POLY argument: same run-time coercion (#3475) */
    if ((sp_streq(name, "&") || sp_streq(name, "intersection") ||
         sp_streq(name, "|") || sp_streq(name, "union") ||
         sp_streq(name, "-") || sp_streq(name, "difference")) && argc == 1 &&
        a0 == TY_POLY) {
      const char *fn = (sp_streq(name, "&") || sp_streq(name, "intersection")) ? "intersect" : (sp_streq(name, "|") || sp_streq(name, "union") ? "union" : "difference");
      buf_printf(b, "sp_PolyArray_%s(", fn);
      emit_expr(c, recv, b); buf_puts(b, ", sp_poly_set_operand(");
      emit_expr(c, argv[0], b); buf_puts(b, "))"); return 1;
    }
    /* variadic named set ops on a poly array: fold over each argument */
    if ((sp_streq(name, "intersection") || sp_streq(name, "union") ||
         sp_streq(name, "difference")) && argc >= 2) {
      int ok = 1;
      for (int j = 0; j < argc; j++) {
        TyKind atj = comp_ntype(c, argv[j]);
        if (atj != TY_POLY_ARRAY && atj != TY_UNKNOWN) { ok = 0; break; }
      }
      if (ok) {
        const char *fn = sp_streq(name, "intersection") ? "intersect" :
                         sp_streq(name, "union") ? "union" : "difference";
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d);", t);
        for (int j = 0; j < argc; j++) {
          buf_printf(b, " _t%d = sp_PolyArray_%s(_t%d, ", t, fn, t);
          if (comp_ntype(c, argv[j]) == TY_UNKNOWN) buf_puts(b, "NULL");
          else emit_expr(c, argv[j], b);
          buf_puts(b, ");");
        }
        buf_printf(b, " _t%d; })", t);
        return 1;
      }
    }
    return 0;
  }
  if ((sp_streq(name, "&") || sp_streq(name, "intersection") ||
       sp_streq(name, "|") || sp_streq(name, "union") ||
       sp_streq(name, "-") || sp_streq(name, "difference")) && argc == 1 && (a0 == rt || a0 == TY_UNKNOWN)) {
    const char *fn = (sp_streq(name, "&") || sp_streq(name, "intersection")) ? "intersect" : ((sp_streq(name, "|") || sp_streq(name, "union")) ? "union" : "difference");
    /* empty literal [] arg: use a null pointer (safe for all sp_*Array_* set ops) */
    if (a0 == TY_UNKNOWN) { buf_printf(b, "sp_%sArray_%s(", k, fn); emit_expr(c, recv, b); buf_puts(b, ", NULL)"); }
    else { buf_printf(b, "sp_%sArray_%s(", k, fn); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    return 1;
  }
  /* typed-array receiver, different-kind typed-array or poly-array argument:
     box both operands to poly and run the poly set op (result poly). */
  if ((sp_streq(name, "&") || sp_streq(name, "intersection") ||
       sp_streq(name, "|") || sp_streq(name, "union") ||
       sp_streq(name, "-") || sp_streq(name, "difference")) && argc == 1 &&
      (a0 == TY_INT_ARRAY || a0 == TY_STR_ARRAY || a0 == TY_FLOAT_ARRAY || a0 == TY_POLY_ARRAY) && a0 != rt) {
    const char *fn = (sp_streq(name, "&") || sp_streq(name, "intersection")) ? "intersect" : (sp_streq(name, "|") || sp_streq(name, "union") ? "union" : "difference");
    const char *conv_l = rt == TY_INT_ARRAY ? "sp_IntArray_to_poly" :
                         rt == TY_STR_ARRAY ? "sp_StrArray_to_poly_fmt" : "sp_FloatArray_to_poly";
    const char *conv_r = a0 == TY_INT_ARRAY ? "sp_IntArray_to_poly" :
                         a0 == TY_STR_ARRAY ? "sp_StrArray_to_poly_fmt" :
                         a0 == TY_FLOAT_ARRAY ? "sp_FloatArray_to_poly" : NULL;
    /* the boxed receiver is rooted while the argument is boxed: the two
       conversions allocate, and a nested-call operand is nobody's root
       between its evaluation and the call */
    int tl = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = %s(", tl, conv_l); emit_expr(c, recv, b); buf_printf(b, "); SP_GC_ROOT(_t%d); sp_PolyArray_%s(_t%d, ", tl, fn, tl);
    if (conv_r) { buf_printf(b, "%s(", conv_r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else emit_expr(c, argv[0], b);  /* already poly */
    buf_puts(b, "); })"); return 1;
  }
  /* typed-array receiver, POLY argument (a value whose static type widened,
     not a poly array): coerce it at run time -- an Array becomes the poly
     array the set-op primitives take, anything else raises the TypeError
     CRuby raises. Without this arm the call had nowhere to go and `&`/`|`
     failed to compile (#3475). */
  if ((sp_streq(name, "&") || sp_streq(name, "intersection") ||
       sp_streq(name, "|") || sp_streq(name, "union") ||
       sp_streq(name, "-") || sp_streq(name, "difference")) && argc == 1 &&
      a0 == TY_POLY) {
    const char *fn = (sp_streq(name, "&") || sp_streq(name, "intersection")) ? "intersect" : (sp_streq(name, "|") || sp_streq(name, "union") ? "union" : "difference");
    const char *conv_l = rt == TY_INT_ARRAY ? "sp_IntArray_to_poly" :
                         rt == TY_STR_ARRAY ? "sp_StrArray_to_poly_fmt" : "sp_FloatArray_to_poly";
    int tl = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = %s(", tl, conv_l); emit_expr(c, recv, b);
    buf_printf(b, "); SP_GC_ROOT(_t%d); sp_PolyArray_%s(_t%d, sp_poly_set_operand(", tl, fn, tl); emit_expr(c, argv[0], b);
    buf_puts(b, ")); })"); return 1;
  }
  /* variadic named set ops: union/intersection/difference(*others) fold the
     binary operator over each argument, accumulating in a rooted temp. */
  if ((sp_streq(name, "intersection") || sp_streq(name, "union") ||
       sp_streq(name, "difference")) && argc >= 2) {
    int ok = 1;
    for (int j = 0; j < argc; j++) {
      TyKind atj = comp_ntype(c, argv[j]);
      if (atj != rt && atj != TY_UNKNOWN) { ok = 0; break; }
    }
    if (ok) {
      const char *fn = sp_streq(name, "intersection") ? "intersect" :
                       sp_streq(name, "union") ? "union" : "difference";
      int t = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d);", t);
      for (int j = 0; j < argc; j++) {
        buf_printf(b, " _t%d = sp_%sArray_%s(_t%d, ", t, k, fn, t);
        if (comp_ntype(c, argv[j]) == TY_UNKNOWN) buf_puts(b, "NULL");
        else emit_expr(c, argv[j], b);
        buf_puts(b, ");");
      }
      buf_printf(b, " _t%d; })", t);
      return 1;
    }
  }
  return 0;
}

/* Array#intersect? */
int emit_op_array_intersect_p(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if (sp_streq(name, "intersect?") && argc == 1 &&
        (a0 == TY_POLY_ARRAY || a0 == TY_UNKNOWN || ty_is_array(a0) || a0 == TY_POLY)) {
      buf_puts(b, "sp_PolyArray_intersect_p("); emit_expr(c, recv, b); buf_puts(b, ", ");
      if (a0 == TY_UNKNOWN) buf_puts(b, "NULL");
      else if (a0 == TY_POLY_ARRAY) emit_expr(c, argv[0], b);
      else {
        /* a differently-stored Array argument coerces; Ruby has one Array */
        buf_puts(b, "sp_poly_to_poly_array(");
        Buf ab3; memset(&ab3, 0, sizeof ab3); emit_expr(c, argv[0], &ab3);
        if (a0 == TY_POLY) buf_puts(b, ab3.p ? ab3.p : "sp_box_nil()");
        else emit_boxed_text(c, a0, ab3.p ? ab3.p : "NULL", b);
        free(ab3.p);
        buf_puts(b, ")");
      }
      buf_puts(b, ")");
      return 1;
    }
    return 0;
  }
  if (sp_streq(name, "intersect?") && argc == 1 &&
      (a0 == rt || a0 == TY_UNKNOWN || ty_is_array(a0) || a0 == TY_POLY)) {
    /* Ruby has one Array; the storage kinds are ours. A receiver and an
       argument of different kinds -- a mapped String array against a
       poly-array constant, the shape this turned up in -- go through the
       generic comparison rather than declining to a NoMethodError. */
    if (a0 == rt) {
      buf_printf(b, "sp_%sArray_intersect_p(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    if (a0 == TY_UNKNOWN) {
      buf_printf(b, "sp_%sArray_intersect_p(", k); emit_expr(c, recv, b); buf_puts(b, ", NULL)");
      return 1;
    }
    buf_puts(b, "sp_PolyArray_intersect_p(sp_poly_to_poly_array(");
    { Buf rb2; memset(&rb2, 0, sizeof rb2); emit_expr(c, recv, &rb2);
      emit_boxed_text(c, rt, rb2.p ? rb2.p : "NULL", b); free(rb2.p); }
    buf_puts(b, "), sp_poly_to_poly_array(");
    { Buf ab2; memset(&ab2, 0, sizeof ab2); emit_expr(c, argv[0], &ab2);
      if (a0 == TY_POLY) buf_puts(b, ab2.p ? ab2.p : "sp_box_nil()");
      else emit_boxed_text(c, a0, ab2.p ? ab2.p : "NULL", b);
      free(ab2.p); }
    buf_puts(b, "))");
    return 1;
  }
  return 0;
}

/* Array#replace(other): in place, answering self */
int emit_op_array_replace(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if (sp_streq(name, "replace") && argc == 1 && a0 == TY_POLY_ARRAY) {
      buf_puts(b, "sp_PolyArray_replace("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* ...and a source of ANOTHER kind, which `[1, 2].replace(["x"])` is:
       the widening makes the receiver poly, and the source is read through
       the boxed accessors rather than needing an arm of its own (#4339). */
    if (sp_streq(name, "replace") && argc == 1 && ty_is_array(rt) &&
        (ty_is_array(a0) || a0 == TY_POLY)) {
      buf_puts(b, "sp_PolyArray_replace_from("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    return 0;
  }
  if (sp_streq(name, "replace") && argc == 1 && a0 == rt) {
    int t = ++g_tmp;
    buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
    buf_printf(b, "; sp_%sArray_replace(_t%d, ", k, t); emit_expr(c, argv[0], b);
    buf_printf(b, "); _t%d; })", t);
    return 1;
  }
  /* A source of another kind into a receiver that kept its own: only a
     true --rbs seed (`@storage: Array[Integer]`) pins it, since the
     mutation otherwise widens the receiver. The source converts to the
     receiver's kind, the way a seeded store converts it; with no arm the
     call fell to NoMethodError. nil or a non-Array is Ruby's TypeError. */
  if (sp_streq(name, "replace") && argc == 1 &&
      (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY || rt == TY_STR_ARRAY) &&
      (a0 == TY_POLY || a0 == TY_POLY_ARRAY || (ty_is_array(a0) && a0 != rt))) {
    int t = ++g_tmp, ts = ++g_tmp, tc = ++g_tmp;
    buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_recv_rooted(c, recv, t, "SP_GC_ROOT", b);
    buf_printf(b, "sp_RbVal _t%d = ", ts); emit_boxed(c, argv[0], b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", ts);
    buf_printf(b, "if (_t%d.tag != SP_TAG_OBJ || !sp_poly_is_array_kind(_t%d.cls_id))"
                  " sp_raise_cls(\"TypeError\", sp_sprintf(\"no implicit conversion of %%s into Array\","
                  " sp_poly_class_name(_t%d))); ", ts, ts, ts);
    char src[32]; snprintf(src, sizeof src, "_t%d", ts);
    emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", tc); emit_unbox_text(c, rt, src, b);
    buf_printf(b, "; sp_%sArray_replace(_t%d, _t%d); _t%d; })", k, t, tc, t);
    return 1;
  }
  return 0;
}

/* Array#minmax without a block */
int emit_op_array_minmax(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    /* minmax (no block): [min, max] via the poly comparator (user `<=>`
       through the cmp hook); incomparable raises the Comparable
       ArgumentError; empty -> [nil, nil]. Both temps rooted: min/max can
       allocate inside sp_poly_cmp (bigint temps) and push reallocs. */
    if (sp_streq(name, "minmax") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      int t = ++g_tmp, o = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " sp_PolyArray_push(_t%d, sp_PolyArray_min(_t%d));"
                    " sp_PolyArray_push(_t%d, sp_PolyArray_max(_t%d)); _t%d; })",
                 t, o, o, o, t, o, t, o);
      return 1;
    }
    return 0;
  }
  /* The extremes are read before the result is allocated: a fresh
     receiver (a Range's to_a) is held by nothing, and the allocation
     collected it before min/max read it. A number needs nothing held
     after that; a String extreme is one of the receiver's elements, so
     the receiver stays rooted across the allocation. Two pushes never
     grow a fresh array, so the result needs no root. */
  if (sp_streq(name, "minmax") && argc == 0 && block < 0) {
    int t = ++g_tmp, o = ++g_tmp;
    int is_str = rt == TY_STR_ARRAY;
    const char *et = is_str ? "const char *" : rt == TY_FLOAT_ARRAY ? "sp_float " : "sp_int ";
    buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_nil_ck_recv(c, recv, rt, "cmp", 0, b);
    buf_puts(b, ";");
    if (is_str) buf_printf(b, " SP_GC_ROOT(_t%d);", t);
    /* an empty receiver answers [nil, nil]: an Integer or Float pair
       notes those nils in may_nil */
    const char *ms = is_str ? "" : "_nilable";
    buf_printf(b, " %s_mn%d = sp_%sArray_min(_t%d); %s_mx%d = sp_%sArray_max(_t%d);"
                  " sp_%sArray *_t%d = sp_%sArray_new(); sp_%sArray_push%s(_t%d, _mn%d);"
                  " sp_%sArray_push%s(_t%d, _mx%d); _t%d; })",
               et, t, k, t, et, t, k, t, k, o, k, k, ms, o, t, k, ms, o, t, o);
    return 1;
  }
  return 0;
}

/* Array#sort: a typed receiver checks for its nil sentinel first; the
   poly form is the blockless one */
int emit_op_array_sort(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if (sp_streq(name, "sort") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      buf_puts(b, "sp_PolyArray_sort("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    return 0;
  }
  if (sp_streq(name, "sort") && argc == 0 &&
      (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY || rt == TY_STR_ARRAY)) {
    buf_printf(b, "sp_%sArray_sort(", k); emit_nil_ck_recv(c, recv, rt, "cmp", 0, b); buf_puts(b, ")");
    return 1;
  }
  return 0;
}

/* Array#uniq: the typed runtime call; a poly copy deduplicated in place */
int emit_op_array_uniq(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if (rt == TY_POLY_ARRAY && sp_streq(name, "uniq") && argc == 0 &&
        nt_ref(nt, id, "block") < 0) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_dup(", t); emit_expr(c, recv, b);
      buf_printf(b, "); sp_PolyArray_uniq_bang(_t%d); _t%d; })", t, t);
      return 1;
    }
    return 0;
  }
  if (sp_streq(name, "uniq") && argc == 0 && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY || rt == TY_FLOAT_ARRAY)) {
    buf_printf(b, "sp_%sArray_uniq(", k); emit_expr(c, recv, b); buf_puts(b, ")");
    return 1;
  }
  return 0;
}

/* Array#min(n) / #max(n) without a block */
int emit_op_array_nmin(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc == 1 && nt_ref(nt, id, "block") < 0) {
      /* as CRuby's nmin_run computes it (sp_PolyArray_nmin), which checks
         the size first */
      int t = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); sp_PolyArray_nmin(_t%d, ", t, t); emit_int_expr(c, argv[0], b);
      buf_printf(b, ", %d); })", sp_streq(name, "max"));
      return 1;
    }
    return 0;
  }
  /* min(n) / max(n) as CRuby's nmin_run computes them
     (sp_PolyArray_nmin): the size is checked first, and a boxed array
     is cut and sorted by its comparisons. A typed one of plain numbers
     cannot tell the two orders apart and keeps its sort; one that can
     hold nil runs the boxed cut first, which raises where CRuby does
     (an unmarked one only when its may_nil flag says so). */
  if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc == 1 && block < 0) {
    int want_max = sp_streq(name, "max");
    int t = ++g_tmp, tn = ++g_tmp;
    buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_int _t%d = ", t, tn); emit_int_expr(c, argv[0], b);
    buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", sp_sprintf(\"negative size (%%lld)\", (long long)_t%d));",
               tn, tn);
    if (rt == TY_POLY_ARRAY) {
      buf_printf(b, " sp_PolyArray_nmin(_t%d, _t%d, %d); })", t, tn, want_max);
      return 1;
    }
    if ((rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY) && elem_nil_sentinel(c, recv, rt)) {
      if (!elem_nil_marked(c, recv, rt)) buf_printf(b, " if (_t%d && SP_MAY_NIL(_t%d))", t, t);
      buf_printf(b, " (void)sp_PolyArray_nmin(%s(_t%d), _t%d, %d);",
                 rt == TY_INT_ARRAY ? "sp_IntArray_to_poly" : "sp_FloatArray_to_poly", t, tn, want_max);
    }
    buf_printf(b, " _t%d = sp_%sArray_sort(_t%d); SP_GC_ROOT(_t%d);", t, k, t, t);
    if (want_max) buf_printf(b, " sp_%sArray_reverse_bang(_t%d);", k, t);
    buf_printf(b, " sp_%sArray_slice(_t%d, 0, _t%d); })", k, t, tn);
    return 1;
  }
  return 0;
}

/* Array#sum without a seed or a block */
int emit_op_array_sum0(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if (rt == TY_POLY_ARRAY && sp_streq(name, "sum") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      /* fold via sp_poly_add so a Float (or Rational/Bignum) element promotes
         the result instead of being dropped by the int-only sum (#2627) */
      buf_puts(b, "sp_PolyArray_sum_poly("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    return 0;
  }
  /* A blockless SEEDLESS sum over Strings adds each element to the implied
     Integer 0, which CRuby rejects with "String can't be coerced into
     Integer". There is no sp_StrArray_sum, so the generic arms emitted a
     call to a function that does not exist and the C compiler stopped on
     its implicit declaration (#4327). An EMPTY receiver adds nothing and
     answers the 0, which is why the test is at run time. A seed of any
     class takes the boxed fold below, which reaches the same raise through
     the operator itself. */
  if (sp_streq(name, "sum") && rt == TY_STR_ARRAY && argc == 0 &&
      nt_ref(nt, id, "block") < 0) {
    int ts = ++g_tmp;
    buf_printf(b, "({ sp_StrArray *_t%d = ", ts); emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); if (sp_StrArray_length(_t%d) != 0)"
                  " sp_raise_cls(\"TypeError\", \"String can't be coerced into Integer\"); ", ts, ts);
    buf_puts(b, "sp_box_int(0); })");
    return 1;
  }
  if (sp_streq(name, "sum") && argc == 0 && nt_ref(nt, id, "block") < 0) {
    buf_printf(b, "sp_%sArray_sum(", k); emit_nil_ck_recv(c, recv, rt, "sum", 0, b); buf_puts(b, ", 0)");
    return 1;
  }
  return 0;
}

/* Array#compact! / #flatten! with no depth: self when changed, nil when a
   no-op (CRuby) */
int emit_op_array_compact_bang(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if (sp_streq(name, "compact!") && argc == 0) {
      /* value form: self when changed, nil when a no-op (CRuby) */
      buf_puts(b, "sp_PolyArray_compact_bangq("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "flatten!") && argc == 0) {
      /* value form: self when changed, nil when a no-op (CRuby) */
      buf_puts(b, "sp_PolyArray_flatten_bangq("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    return 0;
  }
  if (sp_streq(name, "compact!") && argc == 0 && elem_nil_sentinel(c, recv, rt)) {
    /* an Integer or Float array that can hold the sentinel -- its nil --
       drops it in place, answering self when it did; the no-op fold
       below is for one that cannot */
    int t = ++g_tmp;
    buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
    buf_printf(b, "; sp_%sArray_compact_bang(_t%d) ? sp_box_obj(_t%d, %s) : sp_box_nil(); })",
               k, t, t, rt == TY_INT_ARRAY ? "SP_BUILTIN_INT_ARRAY" : "SP_BUILTIN_FLT_ARRAY");
    return 1;
  }
  if ((sp_streq(name, "flatten!") || sp_streq(name, "compact!")) && argc == 0 &&
      (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY || rt == TY_FLOAT_ARRAY)) {
    /* a typed array can hold neither sub-arrays nor nils: both bangs are
       always a no-op, and CRuby's no-op contract is nil */
    buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_box_nil())");
    return 1;
  }
  return 0;
}

/* Array#flatten / #flatten(depth) / #flatten!(depth) */
int emit_op_array_flatten(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if (sp_streq(name, "flatten") && argc <= 1) {
      if (argc == 1) {
        /* held across the depth, which may allocate */
        Buf rfl;
        int cfl = hold_recv_open(c, recv, 0, "sp_PolyArray *", "SP_GC_ROOT", b, &rfl);
        buf_printf(b, "sp_PolyArray_flatten_n(%s, ", rfl.p);
        /* a nil depth is legal and means "no limit" (flatten_n: < 0) */
        if (comp_ntype(c, argv[0]) == TY_NIL) { buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), (sp_int)-1)"); }
        else emit_int_expr(c, argv[0], b);
        buf_puts(b, ")");
        free(rfl.p);
        if (cfl) buf_puts(b, "; })");
      }
      else { buf_puts(b, "sp_PolyArray_flatten("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      return 1;
    }
    if (sp_streq(name, "flatten!") && argc == 1) {
      buf_puts(b, "sp_PolyArray_flatten_bangq_depth("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    return 0;
  }
  if (sp_streq(name, "flatten") && argc == 0) {
    /* a scalar-element array can't nest: flatten is identity, as
       to_a / to_ary / entries / deconstruct are (builtin-op rows) */
    emit_expr(c, recv, b); return 1;
  }
  if ((sp_streq(name, "flatten!") || sp_streq(name, "flatten")) && argc == 1) {
    /* a typed (scalar-element) array has no nesting: flatten(n) copies,
       flatten!(n) is a no-op returning nil */
    if (name[7] == '!') {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
      emit_int_expr(c, argv[0], b); buf_puts(b, "), sp_box_nil())");
    }
    else {
      buf_puts(b, "((void)(");
      emit_int_expr(c, argv[0], b);
      buf_printf(b, "), sp_%sArray_dup(", k);
      emit_expr(c, recv, b);
      buf_puts(b, "))");
    }
    return 1;
  }
  return 0;
}

/* Array#push / #<< / #append of one value onto a poly array */
int emit_op_array_push(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt != TY_POLY_ARRAY) return 0;
  if ((sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append")) && argc == 1) {
    buf_puts(b, "sp_PolyArray_push("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  return 0;
}

/* Array#insert(i, v...) on a poly array: the inserted values box into the
   sp_RbVal slots */
int emit_op_array_insert_n(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt != TY_POLY_ARRAY) return 0;
  if (sp_streq(name, "insert") && argc == 2 && rt == TY_POLY_ARRAY) {
    /* poly array (outside the typed-kind block -- array_kind(POLY_ARRAY) is
       NULL): the inserted value boxes into the sp_RbVal slot */
    int t = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
    /* rooted across the index and the value, as the typed arm is */
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_PolyArray_insert(_t%d, ", t, t); emit_int_expr(c, argv[0], b);
    buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_printf(b, "); _t%d; })", t);
    return 1;
  }
  if (sp_streq(name, "insert") && argc >= 2) {
    int t = ++g_tmp, ti2 = ++g_tmp, to2 = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_int _t%d = ", t, ti2); emit_int_expr(c, argv[0], b);
    buf_puts(b, ";");
    /* normalize ONCE (per-element normalization would drift as the array
       grows), keeping the too-negative IndexError the helper would raise */
    buf_printf(b, " sp_int _t%d = _t%d; if (_t%d < 0) { _t%d += (_t%d ? _t%d->len : 0) + 1;"
                  " if (_t%d < 0) sp_raise_cls(\"IndexError\","
                  " sp_sprintf(\"index %%lld too small for array; minimum: %%lld\","
                  " (long long)_t%d, (long long)(-((_t%d ? _t%d->len : 0) + 1)))); }",
               to2, ti2, ti2, ti2, t, t, ti2, to2, t, t);
    for (int a2 = 1; a2 < argc; a2++) {
      buf_printf(b, " sp_PolyArray_insert(_t%d, _t%d + %d, ", t, ti2, a2 - 1);
      emit_boxed(c, argv[a2], b); buf_puts(b, ");");
    }
    buf_printf(b, " _t%d; })", t);
    return 1;
  }
  return 0;
}

/* Array#transpose of a poly array */
int emit_op_array_transpose(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt != TY_POLY_ARRAY) return 0;
  if (sp_streq(name, "transpose") && argc == 0) {
    buf_puts(b, "sp_int_array_transpose("); emit_expr(c, recv, b); buf_puts(b, ")");
    return 1;
  }
  return 0;
}

/* Array#assoc / #rassoc on a poly array */
int emit_op_array_assoc(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt != TY_POLY_ARRAY) return 0;
  if ((sp_streq(name, "assoc") || sp_streq(name, "rassoc")) && argc == 1) {
    buf_printf(b, "sp_PolyArray_%s(", name); emit_expr(c, recv, b); buf_puts(b, ", ");
    emit_boxed(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  return 0;
}

/* Array#combination / #permutation and their repeated forms without a block */
int emit_op_array_combination(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if ((sp_streq(name, "repeated_combination") || sp_streq(name, "combination") ||
         sp_streq(name, "permutation") || sp_streq(name, "repeated_permutation")) &&
        (argc == 1 || (sp_streq(name, "permutation") && argc == 0)) &&
        nt_ref(nt, id, "block") < 0) {
      const char *combfn = sp_streq(name, "combination") ? "sp_PolyArray_combination"
                         : sp_streq(name, "permutation") ? "sp_PolyArray_permutation"
                         : sp_streq(name, "repeated_permutation") ? "sp_PolyArray_repeated_permutation"
                         : "sp_PolyArray_repeated_combination";
      int ta = ++g_tmp;
      /* a poly-array receiver keeps materializing the tuples: an Enumerator
         here would reach chain sites that read the array directly */
      buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); %s(_t%d, ", ta, combfn, ta);
      if (argc == 1) emit_expr(c, argv[0], b);
      else buf_printf(b, "_t%d ? _t%d->len : 0", ta, ta);
      buf_puts(b, "); })");
      return 1;
    }
    return 0;
  }
  if ((sp_streq(name, "repeated_combination") || sp_streq(name, "combination") ||
       sp_streq(name, "permutation") || sp_streq(name, "repeated_permutation")) &&
      (argc == 1 || (sp_streq(name, "permutation") && argc == 0)) &&
      rt == TY_INT_ARRAY && nt_ref(nt, id, "block") < 0) {
    const char *combfn = sp_streq(name, "combination") ? "sp_IntArray_combination"
                       : sp_streq(name, "permutation") ? "sp_IntArray_permutation"
                       : sp_streq(name, "repeated_permutation") ? "sp_IntArray_repeated_permutation"
                       : "sp_IntArray_repeated_combination";
    int ta = ++g_tmp, tc = ++g_tmp, tout = ++g_tmp, ti = ++g_tmp;
    int tn = ++g_tmp, te = ++g_tmp;
    buf_printf(b, "({ sp_IntArray *_t%d = ", ta); emit_recv_rooted(c, recv, ta, "SP_GC_ROOT", b);
    buf_printf(b, "sp_int _t%d = ", tn);
    if (argc == 1) emit_int_expr(c, argv[0], b);
    else buf_printf(b, "_t%d ? _t%d->len : 0", ta, ta);   /* argless permutation: full length */
    buf_printf(b, "; sp_PtrArray *_t%d = %s(_t%d, _t%d", tc, combfn, ta, tn);
    /* the combinations are only in this temp until the loop below boxes
       them, and the array it boxes them into allocates first */
    buf_printf(b, "); SP_GC_ROOT(_t%d);", tc);
    buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tout, tout);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)", ti, ti, tc, ti);
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int_array(_t%d->data[_t%d]));", tout, tc, ti);
    /* blockless: an Enumerator over those tuples (#3614) */
    buf_printf(b, " sp_Enumerator *_t%d = sp_Enumerator_new_from(sp_box_poly_array(_t%d)); SP_GC_ROOT(_t%d);", te, tout, te);
    buf_printf(b, " sp_enum_with_src(_t%d, sp_box_int_array(_t%d), ", te, ta);
    emit_combinator_enum_label(name, argc, tn, b);
    buf_puts(b, "); })");
    return 1;
  }
  if ((sp_streq(name, "repeated_combination") || sp_streq(name, "combination") ||
       sp_streq(name, "permutation") || sp_streq(name, "repeated_permutation")) &&
      (argc == 1 || (sp_streq(name, "permutation") && argc == 0)) &&
      nt_ref(nt, id, "block") < 0) {
    /* any other element kind rides the boxed PolyArray implementation */
    const char *combfn = sp_streq(name, "combination") ? "sp_PolyArray_combination"
                       : sp_streq(name, "permutation") ? "sp_PolyArray_permutation"
                       : sp_streq(name, "repeated_permutation") ? "sp_PolyArray_repeated_permutation"
                       : "sp_PolyArray_repeated_combination";
    int ta = ++g_tmp, ts = ++g_tmp, tn = ++g_tmp, te = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = ", ts);
    emit_boxed(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_PolyArray *_t%d = sp_poly_to_poly_array(_t%d); SP_GC_ROOT(_t%d); sp_int _t%d = ", ts, ta, ts, ta, tn);
    if (argc == 1) emit_expr(c, argv[0], b);
    else buf_printf(b, "_t%d ? _t%d->len : 0", ta, ta);
    buf_printf(b, "; sp_Enumerator *_t%d = ", te);
    buf_puts(b, "sp_Enumerator_new_from(sp_box_poly_array(");
    buf_printf(b, "%s(_t%d, _t%d", combfn, ta, tn);
    buf_puts(b, ")))");
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_enum_with_src(_t%d, _t%d, ", te, te, ts);
    emit_combinator_enum_label(name, argc, tn, b);
    buf_puts(b, ")");
    buf_puts(b, "; })");
    return 1;
  }
  return 0;
}

/* Array#product without a block: no operand on any array, one operand on
   a poly array */
int emit_op_array_product(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if (sp_streq(name, "product") && argc == 1 && nt_ref(nt, id, "block") < 0) {
      /* poly product with one list: all [x, y] pairs (an empty receiver or
         argument yields []) */
      int pta = ++g_tmp, ptb = ++g_tmp, ptr = ++g_tmp, pti = ++g_tmp, ptj = ++g_tmp, pte = ++g_tmp;
      Buf pra = expr_buf(c, recv);
      buf_printf(b, "({ sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);", pta, pra.p ? pra.p : "NULL", pta);
      free(pra.p);
      buf_printf(b, " sp_PolyArray *_t%d = sp_enum_items_from(", ptb);
      emit_boxed(c, argv[0], b);
      buf_printf(b, "); SP_GC_ROOT(_t%d);", ptb);
      buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ptr, ptr);
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)", pti, pti, pta, pti);
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ptj, ptj, ptb, ptj);
      buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));"
                    " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));"
                    " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d)); }",
                 pte, pte, pte, pta, pti, pte, ptb, ptj, ptr, pte);
      buf_printf(b, " _t%d; })", ptr);
      return 1;
    }
    if (sp_streq(name, "product") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      /* product with no arguments: each element wrapped in its own array */
      int ta = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, te = ++g_tmp;
      Buf ra = expr_buf(c, recv);
      buf_printf(b, "({ sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);", ta, ra.p ? ra.p : "NULL", ta);
      free(ra.p);
      buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, ta, ti);
      buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));"
                    " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d)); }",
                 te, te, te, ta, ti, tr, te);
      buf_printf(b, " _t%d; })", tr);
      return 1;
    }
    return 0;
  }
  if (sp_streq(name, "product") && argc == 0 && nt_ref(nt, id, "block") < 0) {
    /* product with no arguments: each element wrapped in its own array */
    int ta = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, te = ++g_tmp;
    Buf ra = expr_buf(c, recv);
    buf_printf(b, "({ sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);", k, ta, ra.p ? ra.p : "NULL", ta);
    free(ra.p);
    buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", ti, ti, k, ta, ti);
    buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); sp_PolyArray_push(_t%d, ", te, te, te);
    char ee[96]; snprintf(ee, sizeof ee, "sp_%sArray_get(_t%d, _t%d)", k, ta, ti);
    emit_boxed_text(c, ty_array_elem(rt), ee, b);
    buf_printf(b, "); sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d)); }", tr, te);
    buf_printf(b, " _t%d; })", tr);
    return 1;
  }
  return 0;
}

/* Array#fetch_values with no keys on a typed array (a poly one is answered
   ahead of the lookup, where a program defining fetch_values can take it) */
int emit_op_array_fetch_values0(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) return 0;
  /* fetch_values with no keys reads nothing: an empty Array */
  if (sp_streq(name, "fetch_values") && argc == 0) {
    buf_printf(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), sp_%sArray_new())", k);
    return 1;
  }
  return 0;
}

/* Array#all? / #any? / #none? / #one? with no pattern and no block */
int emit_op_array_pred0(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if ((sp_streq(name, "all?") || sp_streq(name, "any?") ||
         sp_streq(name, "none?") || sp_streq(name, "one?")) &&
        argc == 0 && nt_ref(nt, id, "block") < 0) {
      /* count truthy elements; a poly element may be nil/false */
      int t = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; sp_int _t%d = 0; for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                    " if (sp_poly_truthy(sp_PolyArray_get(_t%d, _t%d))) _t%d++;",
                 tn, ti, ti, t, ti, t, ti, tn);
      const char *expr = sp_streq(name, "all?") ? "_t%d == sp_PolyArray_length(_t%d)"
                       : sp_streq(name, "any?") ? "_t%d > 0"
                       : sp_streq(name, "none?") ? "_t%d == 0" : "_t%d == 1";
      buf_puts(b, " (");
      if (sp_streq(name, "all?")) buf_printf(b, expr, tn, t);
      else buf_printf(b, expr, tn);
      buf_puts(b, "); })");
      return 1;
    }
    return 0;
  }
  if ((sp_streq(name, "all?") || sp_streq(name, "any?") ||
       sp_streq(name, "none?") || sp_streq(name, "one?")) &&
      argc == 0 && nt_ref(nt, id, "block") < 0) {
    /* scalar-element arrays never hold nil/false: predicate is length-based.
       One that can hold the sentinel counts its truthy (non-nil) elements
       instead. */
    if (elem_nil_sentinel(c, recv, rt)) {
      /* sp_*Array_truthy_count: the length unless may_nil is set */
      int ta = ++g_tmp, tn = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
      buf_printf(b, "; sp_int _t%d = sp_%sArray_truthy_count(_t%d, %d); ", tn, k, ta, elem_nil_marked(c, recv, rt));
      if (sp_streq(name, "all?"))       buf_printf(b, "_t%d == sp_%sArray_length(_t%d); })", tn, k, ta);
      else if (sp_streq(name, "any?"))  buf_printf(b, "_t%d > 0; })", tn);
      else if (sp_streq(name, "none?")) buf_printf(b, "_t%d == 0; })", tn);
      else                              buf_printf(b, "_t%d == 1; })", tn);
      return 1;
    }
    const char *op = sp_streq(name, "all?") ? ">= 0" : sp_streq(name, "any?") ? "> 0"
                   : sp_streq(name, "none?") ? "== 0" : "== 1";
    buf_printf(b, "(sp_%sArray_length(", k); emit_expr(c, recv, b); buf_printf(b, ") %s)", op);
    return 1;
  }
  return 0;
}

/* Array#dig with two or more keys on a typed array (a poly one is answered
   ahead of the lookup, which reads a splatted key list) */
int emit_op_array_dig_n(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) return 0;
  if (sp_streq(name, "dig") && argc >= 2) {
    /* multi-step (one step is arr[i], a builtin-op row): hand the whole
       key list to the runtime walk, which stops at nil and raises
       TypeError on a step that cannot be dug. Chaining index reads
       instead read the scalar the first step answered as if it were an
       array, so `[1].dig(0, 0)` answered 1 where Ruby raises (#3825). */
    buf_puts(b, "sp_poly_dig_n(sp_box_obj(");
    emit_expr(c, recv, b);
    buf_printf(b, ", SP_BUILTIN_%s_ARRAY), %d, (sp_RbVal[]){",
               rt == TY_INT_ARRAY ? "INT" : rt == TY_FLOAT_ARRAY ? "FLT" : "STR", argc);
    for (int di = 0; di < argc; di++) { if (di) buf_puts(b, ", "); emit_boxed(c, argv[di], b); }
    buf_puts(b, "})");
    return 1;
  }
  return 0;
}

/* Array#sum(seed) without a block on a poly array (a typed one reads the
   seed's literal, after the lookup) */
int emit_op_array_sum1(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt != TY_POLY_ARRAY) return 0;
  if (rt == TY_POLY_ARRAY && sp_streq(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0) {
    TyKind init_t = comp_ntype(c, argv[0]);
    /* an Array initial value concatenates one level ([[1],[2]].sum([])) */
    if (ty_is_array(init_t)) {
      buf_puts(b, "sp_PolyArray_sum_concat("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* a String initial value folds by concatenation ([str].sum("")) */
    if (init_t == TY_STRING) {
      buf_puts(b, "sp_PolyArray_sum_str("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* a Float initial value folds to a Float (bare sp_float, not boxed) */
    if (init_t == TY_FLOAT) {
      buf_puts(b, "("); emit_float_expr(c, argv[0], b);
      buf_puts(b, " + sp_PolyArray_sum_float("); emit_expr(c, recv, b); buf_puts(b, "))");
      return 1;
    }
    /* an Integer (or poly) seed folds via sp_poly_add so Float/Rational/
       Bignum elements promote the result instead of being dropped by the
       int-only sum (matches the no-arg poly fold above) (#2959) */
    buf_puts(b, "sp_poly_add(");
    if (init_t == TY_POLY) emit_expr(c, argv[0], b);
    else emit_boxed(c, argv[0], b);
    buf_puts(b, ", sp_PolyArray_sum_poly("); emit_expr(c, recv, b); buf_puts(b, "))");
    return 1;
  }
  return 0;
}

/* Array#concat(*arrays) on a poly array (a typed one reads its operand nodes,
   after the lookup) */
int emit_op_array_concat(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt != TY_POLY_ARRAY) return 0;
  /* concat(*arrays): append each argument array's elements onto the receiver
     in place, return the receiver. Coerce a typed-array argument to poly. */
  if (rt == TY_POLY_ARRAY && sp_streq(name, "concat")) {
    int t = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b); buf_puts(b, ";");
    /* evaluate (and root) every argument left-to-right BEFORE any append, so a
       side-effecting argument or one that reads the receiver sees pre-mutation
       state, per Ruby's arg-before-call evaluation order. */
    int base = g_tmp + 1; g_tmp += argc;
    for (int ai = 0; ai < argc; ai++) {
      TyKind at = comp_ntype(c, argv[ai]);
      const char *from = at == TY_INT_ARRAY   ? "sp_PolyArray_from_int_array"
                       : at == TY_STR_ARRAY   ? "sp_PolyArray_from_str_array"
                       : at == TY_FLOAT_ARRAY ? "sp_PolyArray_from_float_array" : NULL;
      buf_printf(b, " sp_PolyArray *_t%d = ", base + ai);
      if (from) { buf_printf(b, "%s(", from); emit_expr(c, argv[ai], b); buf_puts(b, ")"); }
      else if (at == TY_POLY || at == TY_UNKNOWN) {
        /* a boxed argument (a rest param widened to poly): unbox to the
           working array through the runtime kind dispatch (#3317); one
           that is no Array is CRuby's TypeError, not an empty list */
        buf_puts(b, "sp_poly_set_operand("); emit_boxed(c, argv[ai], b); buf_puts(b, ")");
      }
      else emit_expr(c, argv[ai], b);   /* already a poly array */
      buf_printf(b, "; SP_GC_ROOT(_t%d);", base + ai);
    }
    for (int ai = 0; ai < argc; ai++)
      buf_printf(b, " sp_PolyArray_append_all(_t%d, _t%d);", t, base + ai);
    buf_printf(b, " _t%d; })", t);
    return 1;
  }
  return 0;
}

/* Array#index(v) / #find_index(v) / #rindex(v) without a block on a poly
   array (a typed one asks for a user ==, after the lookup) */
int emit_op_array_index_v(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt != TY_POLY_ARRAY) return 0;
  /* rindex(obj): last matching index, or nil (SP_INT_NIL sentinel, matching
     the index/find_index int-or-nil convention). */
  if (rt == TY_POLY_ARRAY && sp_streq(name, "rindex") && argc == 1 && nt_ref(nt, id, "block") < 0) {
    Buf rb; int ch = hold_recv_open(c, recv, 0, "sp_PolyArray *", "SP_GC_ROOT", b, &rb);
    int t = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = sp_PolyArray_rindex(%s, ", t, rb.p); free(rb.p);
    emit_boxed(c, argv[0], b);
    buf_printf(b, "); _t%d < 0 ? SP_INT_NIL : _t%d; })", t, t);
    if (ch) buf_puts(b, "; })");
    return 1;
  }
  /* index(v) / find_index(v) on a poly array (no block) -> the first
     position whose element == v (sp_poly_eq), or nil (SP_INT_NIL),
     mirroring the count(v)/any?(v) idiom (doom: @map.sectors.index(sector)). */
  if (rt == TY_POLY_ARRAY && (sp_streq(name, "index") || sp_streq(name, "find_index")) &&
      argc == 1 && nt_ref(nt, id, "block") < 0) {
    int trecv = ++g_tmp, ta = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
    Buf ra = expr_buf(c, recv);
    /* Root the receiver and the boxed needle: sp_poly_eq can allocate
       (bigint promotion), so a collection may run mid-loop. */
    buf_printf(b, "({ sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);", trecv, ra.p ? ra.p : "NULL", trecv); free(ra.p);
    buf_printf(b, " sp_RbVal _t%d = ", ta); emit_boxed(c, argv[0], b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", ta);
    buf_printf(b, " sp_int _t%d = SP_INT_NIL;", tres);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)", ti, ti, trecv, ti);
    buf_printf(b, " if (sp_poly_eq(sp_PolyArray_get(_t%d, _t%d), _t%d)) { _t%d = _t%d; break; }",
               trecv, ti, ta, tres, ti);
    buf_printf(b, " _t%d; })", tres);
    return 1;
  }
  return 0;
}

/* The Array arms of emit_call_body's chain that run long before
   emit_array_call, each a row of its own stage looked up where its arm sat:
   stage 3 in emit_blockless_enumerator, stage 4 (Array#* with a String) and
   stage 5 (any?/all?/none?/one? with a Class) in emit_call_body. */

/* arr.cycle with no count and no block: an Enumerator over the elements
   (stage 3) */
int emit_op_array_cycle_endless(Compiler *c, const BopCtx *x, Buf *b) {
  int tcy = ++g_tmp;
  buf_printf(b, "({ sp_Enumerator *_t%d = sp_Enumerator_new_cycle_endless(", tcy);
  emit_boxed(c, x->recv, b);
  buf_printf(b, "); _t%d->meth = SPL(\"cycle\"); _t%d; })", tcy, tcy);
  return 1;
}

/* arr.slice_before(pat) / slice_after(pat) with no block -> a materialized
   Enumerator over the groups (stage 3). CRuby's pattern form matches with
   `pattern === element`: the boxed pattern dispatches through
   sp_poly_case_eq (Range cover / Class is_a / Regexp match / value
   equality, #2847). A Proc pattern would need a stored-proc call per element
   and stays a loud reject. */
int emit_op_array_slice_groups(Compiler *c, const BopCtx *x, Buf *b) {
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  TyKind spat = comp_ntype(c, argv[0]);
  if (spat == TY_PROC)
    unsupported(c, x->id, "slice_before/slice_after with a Proc pattern; use the block form");
  buf_printf(b, "sp_Enumerator_new_from_items(sp_poly_slice_groups(");
  emit_boxed(c, x->recv, b); buf_puts(b, ", ");
  emit_boxed(c, argv[0], b);
  buf_printf(b, ", %d))", sp_streq(x->name, "slice_after") ? 1 : 0);
  return 1;
}

/* Array#* (join): arr * sep_str -> the elements joined by the separator
   (stage 4); the receiver is held across the separator */
int emit_op_array_join_str(Compiler *c, const BopCtx *x, Buf *b) {
  TyKind rt = x->rt;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) k = "Str";
  Buf rb; char tyj[32]; snprintf(tyj, sizeof tyj, "sp_%sArray *", k);
  int ch = hold_recv_open(c, x->recv, 0, tyj, "SP_GC_ROOT", b, &rb);
  buf_printf(b, "sp_%sArray_join(%s, ", k, rb.p); free(rb.p);
  emit_expr(c, argv[0], b); buf_puts(b, ")");
  if (ch) buf_puts(b, "; })");
  return 1;
}

/* any?/all?/none?/one?(Class) over an array: Class === element membership
   (the value-argument arms compare ==), stage 5. Walks the boxed elements so
   every array kind is covered. The receiver is rooted for the walk: a
   method's return or a chain is held by nothing else, and a collection
   during the loop handed its slot on, so the elements it read were another
   object's. */
int emit_op_array_pred_class(Compiler *c, const BopCtx *x, Buf *b) {
  const char *name = x->name;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  int ta = ++g_tmp, tc2 = ++g_tmp, tn = ++g_tmp, tcnt = ++g_tmp, ti = ++g_tmp;
  buf_printf(b, "({ sp_RbVal _t%d = ", ta); emit_boxed(c, x->recv, b);
  buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_Class _t%d = ", ta, tc2); emit_expr(c, argv[0], b);
  buf_puts(b, "; "); emit_poly_iter_obj_normalize(c, ta, b);
  emit_poly_iter_obj_reject(c, ta, name, b);
  buf_printf(b, "sp_poly_iter_check(_t%d, \"%s\"); ", ta, name);
  buf_printf(b, "sp_int _t%d = sp_poly_arr_len_ex(_t%d); sp_int _t%d = 0;"
                " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)"
                " if (sp_poly_is_a(sp_poly_each_elem(_t%d, _t%d), _t%d)) _t%d++; ",
             tn, ta, tcnt, ti, ti, tn, ti, ta, ti, tc2, tcnt);
  if (sp_streq(name, "any?"))       buf_printf(b, "_t%d > 0; })", tcnt);
  else if (sp_streq(name, "all?"))  buf_printf(b, "_t%d == _t%d; })", tcnt, tn);
  else if (sp_streq(name, "none?")) buf_printf(b, "_t%d == 0; })", tcnt);
  else                              buf_printf(b, "_t%d == 1; })", tcnt);
  return 1;
}
