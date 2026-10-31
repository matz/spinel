/* codegen_call_array.c -- the builtin-op emitters of the Array family
   (BOP_ANY_ARRAY) that are more than one C text with its placeholders: an
   arm whose typed-array and poly-array forms differ in more than the
   variant's name ($A), or that builds its C in steps. The rest are
   templates in builtin_ops.c. emit_array_call looks the rows up after the
   arms every variant shares, so only an Int, Float, Str or poly array
   reaches these. Each answers 1 when it emitted the call, 0 (having
   emitted nothing) to leave it to the chain after the lookup. */

#include "codegen_internal.h"
#include "repr.h"
#include "holder.h"
#include "codegen_poly.h"
#include "builtin_ops.h"
#include "codegen_call_arms.h"

/* ---- nil out of band: an emitter whose C result is an sp_oint / sp_ofloat
   leaves it bare when the node is one (node_is_oint: the dispatcher's
   consumer takes the oint) and otherwise reads it as the plain scalar through
   sp_oint_arg (TypeError for nil) -- the same wrap the dispatcher applies to an
   oint producer, applied here because this emitter decides the form. */
static void oint_open(Compiler *c, int id, TyKind t, Buf *b) { if (!node_is_oint(c, id)) buf_printf(b, "%s(", oint_arg(t)); }
static void oint_close(Compiler *c, int id, Buf *b) { if (!node_is_oint(c, id)) buf_puts(b, ")"); }

/* the element-kind name of the receiver: "Poly" or array_kind's */
static const char *arr_kind(TyKind rt) {
  return rt == TY_POLY_ARRAY ? "Poly" : array_kind(rt);
}

/* A merged keyword hash copies its entries. The last source of buffer:
   still owns a value-form String's write-back. The ordinary argument
   bindings keep every source at the value it had when evaluated. */
static void emit_pack_kw_writeback(Compiler *c, int kwh, int tr, Buf *b) {
  const NodeTable *nt = c->nt;
  int en = 0, close = 0;
  const int *el = nt_arr(nt, kwh, "elements", &en);
  int sym = comp_sym_intern(c, "buffer");
  for (int e = en - 1; e >= 0; e--) {
    int v = nt_ref(nt, el[e], "value"), key = nt_ref(nt, el[e], "key");
    if (nt_kind(nt, el[e]) == NK_AssocSplatNode) {
      int th = hold_operand(c, v, TY_POLY, 1, ++g_tmp, 1, " ", b);
      buf_printf(b, "if (!sp_poly_nil_p(_t%d) && sp_poly_has_key(_t%d, sp_box_sym((sp_sym)%d))) { sp_poly_set_sym(_t%d, (sp_sym)%d, _t%d); }\nelse { ", th, th, sym, th, sym, tr);
      close++;
      continue;
    }
    if (nt_kind(nt, key) == NK_SymbolNode && !sp_streq(nt_str(nt, key, "value"), "buffer")) continue;
    if (nt_kind(nt, key) == NK_StringNode) continue;
    int dynamic = nt_kind(nt, key) != NK_SymbolNode;
    if (dynamic) {
      buf_puts(b, "if (sp_poly_eq("); emit_boxed(c, key, b);
      buf_printf(b, ", sp_box_sym((sp_sym)%d))) { ", sym);
    }
    HolderRef h; Buf ref; char val[32];
    memset(&ref, 0, sizeof ref);
    int vslot = nt_paren_single(c->nt, v), rebound = 0;
    for (int j = e + 1; j < en; j++)
      rebound |= read_rebound_by(c, v, nt_ref(nt, el[j], "value"));
    if (!rebound && holder_of_node(c, vslot, &h) && holder_slot_buf(c, &h, &ref) &&
        (h.r.kind == RK_BOXED || h.r.as_ty == TY_STRING)) {
      snprintf(val, sizeof val, "_t%d", tr);
      buf_printf(b, "%s = ", ref.p); emit_unbox_text(c, h.r.as_ty, val, b); buf_puts(b, "; ");
    }
    free(ref.p);
    if (!dynamic) break;
    buf_puts(b, "}\nelse { "); close++;
  }
  while (close-- > 0) buf_puts(b, "} ");
}

/* A keyword buffer is an operand, then the destination and the answer.
   Keep a carried handle; value slots take the packed bytes back. */
int emit_op_array_pack_buffer(Compiler *c, const BopCtx *x, Buf *b) {
  int argc, en = 0;
  const int *argv = call_args(c->nt, x->id, &argc);
  if (nt_kind(c->nt, argv[1]) != NK_KeywordHashNode) return 0;
  int ob = struct_kwarg_value(c, argv[1], "buffer");
  const int *el = nt_arr(c->nt, argv[1], "elements", &en);
  if (x->rt == TY_POLY && poly_name_user_claimed(c, x->name, argc)) return 0;
  buf_puts(b, "({ ");
  int ta = hold_operand(c, x->recv, TY_POLY, 1, ++g_tmp, 1, " ", b);
  TyKind ft = arg_ran_first(argv[0], 0) && repr_of(c, argv[0]).as_ty == TY_STRBUF ? TY_STRBUF : TY_POLY;
  int tf = hold_operand(c, argv[0], ft, ft == TY_POLY, ++g_tmp, 1, " ", b);
  int tb = ++g_tmp, tr = ++g_tmp;
  int kh = -1, kwmark = g_n_argov;
  if (ob < 0 || en != 1) {
    kh = ++g_tmp;
    if (en == 1 && nt_kind(c->nt, el[0]) == NK_AssocSplatNode) {
      int src = nt_ref(c->nt, el[0], "value");
      kh = hold_operand(c, src, TY_POLY, 1, kh, 1, " ", b);
      char hash[32]; snprintf(hash, sizeof hash, "_t%d", kh);
      Buf *save = g_pre; g_pre = b;
      emit_kw_splat_conv_temp(c, hash);
      g_pre = save;
    }
    else {
      emit_args_in_source_order(c, &argv[1], 1, b);
      int th = ++g_tmp;
      emit_poly_kw_all(c, argv[1], th, 1, 1, b);
      buf_printf(b, "sp_RbVal _t%d = sp_box_obj(_t%d, SP_BUILTIN_POLY_POLY_HASH); SP_GC_ROOT_RBVAL(_t%d); ", kh, th, kh);
    }
    buf_printf(b, "sp_poly_ary_chk(_t%d, \"pack\", 0); sp_kwargs_verify(_t%d, (const char *const[]){\"buffer\", NULL}, (const char *const[]){NULL}, (const char *const[]){NULL}, 1); ", ta, kh);
    int sym = comp_sym_intern(c, "buffer");
    buf_printf(b, "sp_RbVal _t%d = sp_poly_nil_p(_t%d) || !sp_poly_has_key(_t%d, sp_box_sym((sp_sym)%d)) ? sp_box_nil() : sp_poly_get_sym(_t%d, (sp_sym)%d); SP_GC_ROOT_RBVAL(_t%d); ", tb, kh, kh, sym, kh, sym, tb);
  }
  Buf ref; memset(&ref, 0, sizeof ref);
  HolderRef h;
  int source = nt_paren_single(c->nt, ob), slot = kh < 0 && holder_of_node(c, source, &h);
  int handle = kh < 0 && strbuf_slot_text(c, slot ? source : ob, &ref);
  slot = !handle && slot && holder_slot_buf(c, &h, &ref);
  if (handle)
    buf_printf(b, "sp_RbVal _t%d = sp_box_nullable_obj(%s, SP_BUILTIN_STRBUF); SP_GC_ROOT_RBVAL(_t%d); ", tb, ref.p, tb);
  else if (kh < 0) tb = hold_operand(c, ob, TY_POLY, 1, tb, 1, " ", b);
  if (slot && h.r.kind == RK_BOXED)
    buf_printf(b, "_t%d = sp_poly_strbuf_lift(_t%d); %s = _t%d; ", tb, tb, ref.p, tb);
  buf_printf(b, "sp_RbVal _t%d = sp_poly_pack_buffer(_t%d, ", tr, ta);
  char fmt[32]; snprintf(fmt, sizeof fmt, "_t%d", tf);
  emit_boxed_text(c, ft, fmt, b);
  buf_printf(b, ", _t%d); ", tb);
  if (kh >= 0) {
    buf_printf(b, "if (!sp_poly_nil_p(_t%d) && !sp_poly_is_strbuf(_t%d)) { SP_GC_ROOT_RBVAL(_t%d); ", tb, tb, tr);
    if (en == 1 && nt_kind(c->nt, el[0]) == NK_AssocSplatNode)
      buf_printf(b, "sp_poly_set_sym(_t%d, (sp_sym)%d, _t%d); ", kh, comp_sym_intern(c, "buffer"), tr);
    else emit_pack_kw_writeback(c, argv[1], tr, b);
    buf_puts(b, "} ");
  }
  view_unbind(kwmark);
  if (slot && (h.r.kind == RK_BOXED || h.r.as_ty == TY_STRING)) {
    char val[32]; snprintf(val, sizeof val, "_t%d", tr);
    buf_printf(b, "if (!sp_poly_nil_p(_t%d)) %s = ", tb, ref.p);
    emit_unbox_text(c, h.r.as_ty, val, b);
    buf_puts(b, "; ");
  }
  free(ref.p);
  buf_printf(b, "_t%d; })", tr);
  return 1;
}

/* Array.new's poly fill keeps an existing shared slot's handle. Static
   reads retain String dispatch even when their storage is the handle. */
int array_fill_boxes_handle(Compiler *c, int id, int fill) {
  if (repr_of(c, id).as_ty != TY_POLY_ARRAY) return 0;
  return repr_of(c, fill).kind == RK_STRBUF;
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
  /* an Integer or Float element is copied with its nil */
  if (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY)
    buf_printf(b, " sp_%sArray_push_o(_t%d, sp_%sArray_oget(_t%d, _t%d));", k, tr2, k, t, ti2);
  else buf_printf(b, " sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));", k, tr2, k, t, ti2);
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
  /* a receiver that can be nil raises under last, not under the [] its read names */
  int nil_ok = recv_never_nil(c, x->recv);
  const char *pre = nil_ok ? "" : "sp_nil_recv_named(", *post = nil_ok ? "" : ", \"last\")";
  if (x->rt == TY_POLY_ARRAY)
    buf_printf(b, "({ sp_PolyArray *_t%d = %s%s%s; sp_PolyArray_get(_t%d, sp_PolyArray_length(_t%d) - 1); })",
               t, pre, rb.p ? rb.p : "", post, t, t);
  else if (oint_kind(ty_array_elem(x->rt))) {
    /* an Integer or Float element is read with its nil (an empty array,
       a nil element), unwrapped where the consumer wants it plain */
    const char *k = array_kind(x->rt);
    oint_open(c, x->id, ty_array_elem(x->rt), b);
    buf_printf(b, "({ %s _t%d = %s; sp_%sArray_oget(_t%d, sp_%sArray_length(_t%d) - 1); })",
               c_type_name(x->rt), t, rb.p ? rb.p : "", k, t, k, t);
    oint_close(c, x->id, b);
  }
  else {
    const char *k = array_kind(x->rt);
    buf_printf(b, "({ %s _t%d = %s%s%s; sp_%sArray_get(_t%d, sp_%sArray_length(_t%d) - 1); })",
               c_type_name(x->rt), t, pre, rb.p ? rb.p : "", post, k, t, k, t);
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
  if (argc == 1 && repr_of(c, argv[0]).kind == RK_BOXED) {
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

/* sort!: in place, answering self; a typed receiver that may hold nil
   checks for it first, as the comparison would raise */
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
    buf_printf(b, "sp_Range _t%d = sp_range_ix(", tr); emit_expr(c, argv[0], b); buf_puts(b, ")");
    buf_printf(b, "; sp_int _t%d = _t%d.nobeg ? 0"
                  " : (_t%d.first < 0 ? _t%d.first + (_t%d ? _t%d->len : 0) : _t%d.first);",
               tf, tr, tr, tr, ta, ta, tr);
    buf_printf(b, " sp_int _t%d = _t%d.noend ? ((_t%d ? _t%d->len : 0) - _t%d)"
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
  buf_printf(b, "sp_Range _t%d = sp_range_ix(", tr); emit_expr(c, argv[0], b); buf_puts(b, ")");
  buf_printf(b, "; sp_int _t%d = _t%d.nobeg ? 0"
                " : (_t%d.first < 0 ? _t%d.first + (_t%d ? _t%d->len : 0) : _t%d.first);",
             tf, tr, tr, tr, ta, ta, tr);
  buf_printf(b, " sp_int _t%d = _t%d.noend ? ((_t%d ? _t%d->len : 0) - _t%d)"
                " : ((_t%d.last < 0 ? _t%d.last + (_t%d ? _t%d->len : 0) : _t%d.last) - _t%d + (_t%d.excl ? 0 : 1));",
             tn, tr, ta, ta, tf, tr, tr, ta, ta, tr, tf, tr);
  buf_printf(b, " sp_%sArray_slice_bang(_t%d, _t%d, _t%d < 0 ? 0 : _t%d); })", k, ta, tf, tn, tn);
  return 1;
}

/* Array#+ / - / & / | with an operand that can never be an Array (nil, a
   boolean, or a builtin with no #to_ary): CRuby raises TypeError "no implicit
   conversion of X into Array". The arms below take only an Array, so the call
   fell through to the unresolved gate and raised NoMethodError for a method
   Array has. Both sides are evaluated first, as CRuby does; a boolean's class
   is read at run time. A boxed operand keeps the runtime check, and a user
   object its #to_ary path, as concat's arm leaves them (codegen_call_recv.c). */
static int emit_array_operand_type_error(Compiler *c, const BopCtx *x, Buf *b) {
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  if (argc != 1) return 0;
  TyKind at = comp_ntype(c, argv[0]);
  if (!conv_to_ary_misfit(c, at)) return 0;
  /* a nullable Integer or Float slot (a parameter one call site passes nil)
     names nil or its class by what it holds */
  int nil_rt = repr_opt_kind(c, argv[0]) != TY_UNKNOWN;
  /* and a String slot's nil is NULL */
  int str_rt = at == TY_STRING;
  int tb = ++g_tmp;
  buf_puts(b, "({ ");
  int tr = emit_fold_recv_hold(c, x->id, x->recv, b);
  if (nil_rt) {
    buf_printf(b, "%s _t%d = (", oint_ctype(at), tb);
    emit_oint_expr(c, argv[0], at, b); buf_puts(b, "); ");
  }
  /* an object names its class at run time (a subclass instance its own,
     a nil one nil) */
  else if (ty_is_object(at)) {
    buf_printf(b, "sp_RbVal _t%d = ", tb); emit_boxed(c, argv[0], b); buf_puts(b, "; ");
  }
  else if (at == TY_BOOL || str_rt) {
    buf_printf(b, "%s _t%d = (", at == TY_BOOL ? "int" : "const char *", tb);
    emit_expr(c, argv[0], b); buf_puts(b, "); ");
  }
  else { buf_puts(b, "(void)("); emit_expr(c, argv[0], b); buf_puts(b, "); "); }
  emit_fold_recv_test(c, x->id, tr, b);
  if (at == TY_NIL)
    buf_puts(b, "sp_raise_cls(\"TypeError\", \"no implicit conversion of nil into Array\");");
  else if (at == TY_BOOL)
    buf_printf(b, "sp_raise_cls(\"TypeError\", _t%d"
                  " ? \"no implicit conversion of true into Array\""
                  " : \"no implicit conversion of false into Array\");", tb);
  else if (str_rt)
    buf_printf(b, "sp_raise_cls(\"TypeError\", _t%d == NULL"
                  " ? \"no implicit conversion of nil into Array\""
                  " : \"no implicit conversion of String into Array\");", tb);
  else if (ty_is_object(at))
    buf_printf(b, "sp_raise_cls(\"TypeError\", sp_sprintf(\"no implicit conversion of %%s into Array\", sp_convert_src_name(_t%d)));", tb);
  else if (nil_rt)
    buf_printf(b, "sp_raise_cls(\"TypeError\", _t%d.nil"
                  " ? \"no implicit conversion of nil into Array\""
                  " : \"no implicit conversion of %s into Array\");",
               tb, conv_builtin_class_name(at));
  else
    buf_printf(b, "sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into Array\");",
               conv_builtin_class_name(at));
  buf_printf(b, " %s; })", raise_tail_value(repr_of(c, x->id).as_ty));
  return 1;
}

/* Array#+: the same kind concatenates; another kind boxes both sides into a poly array */
int emit_op_array_plus(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  Repr a0r = repr_of(c, argc >= 1 ? argv[0] : -1);
  TyKind a0 = a0r.as_ty;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (emit_array_operand_type_error(c, x, b)) return 1;
  if (rt == TY_POLY_ARRAY) {
    if (sp_streq(name, "+") && argc == 1 && a0r.elem == TY_POLY) {
      /* Spill the receiver: evaluating the operand can allocate, and until
         the concat runs the receiver is in nothing but this temp. */
      int t = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); sp_PolyArray_concat(_t%d, ", t, t);
      emit_expr(c, argv[0], b); buf_puts(b, "); })");
      return 1;
    }
    /* poly_array + typed array: box the typed operand to poly, then concat. */
    if (sp_streq(name, "+") && argc == 1 && ty_is_array(a0) && a0r.elem != TY_POLY) {
      const char *conv = a0r.elem == TY_INT ? "sp_IntArray_to_poly" :
                         a0r.elem == TY_FLOAT ? "sp_FloatArray_to_poly" :
                         a0r.elem == TY_STRING ? "sp_StrArray_to_poly_fmt" : NULL;
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
  if (sp_streq(name, "+") && argc == 1 && a0r.elem == ty_array_elem(rt)) {
    /* array + array of the same kind -> a fresh concatenation */
    buf_printf(b, "sp_%sArray_concat(", k);
    emit_expr(c, recv, b); buf_puts(b, ", ");
    emit_expr(c, argv[0], b);
    buf_puts(b, ")");
    return 1;
  }
  if (sp_streq(name, "+") && argc == 1 && ty_is_array(a0) && a0r.elem != ty_array_elem(rt)) {
    /* array + different-kind array -> poly_array */
    const char *k2 = (a0r.elem == TY_POLY) ? "Poly" : array_kind(a0);
    if (k2) {
      int tL = ++g_tmp, tR = ++g_tmp, tO = ++g_tmp, ti = ++g_tmp;
      Buf lbuf = expr_buf(c, recv);
      Buf rbuf = expr_buf(c, argv[0]);
      const char *box_l = (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY) ? typed_elem_box_fn(rt) :
                          (rt == TY_STR_ARRAY) ? "sp_box_str" : NULL;
      const char *box_r = (a0r.elem == TY_INT || a0r.elem == TY_FLOAT) ? typed_elem_box_fn(a0) :
                          (a0r.elem == TY_STRING) ? "sp_box_str" : NULL;
      /* an Integer or Float side boxes each element with its nil bit:
         sp_IntArray_box_elem / sp_FloatArray_box_elem */
      const char *box_l_elem = rt == TY_INT_ARRAY ? "sp_IntArray_box_elem" : rt == TY_FLOAT_ARRAY ? "sp_FloatArray_box_elem" : NULL;
      const char *box_r_elem = a0r.elem == TY_INT ? "sp_IntArray_box_elem" : a0r.elem == TY_FLOAT ? "sp_FloatArray_box_elem" : NULL;
      const char *get_l = (rt == TY_POLY_ARRAY) ? "sp_PolyArray_get" :
                          NULL;
      const char *get_r = (a0r.elem == TY_POLY) ? "sp_PolyArray_get" :
                          NULL;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);\n", k, tL, lbuf.p ? lbuf.p : "", tL); free(lbuf.p);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);\n", k2, tR, rbuf.p ? rbuf.p : "", tR); free(rbuf.p);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tO, tO);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)\n", ti, ti, k, tL, ti);
      emit_indent(g_pre, g_indent + 1);
      if (rt == TY_POLY_ARRAY)
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));\n", tO, tL, ti);
      else if (box_l_elem)
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s(_t%d, _t%d));\n", tO, box_l_elem, tL, ti);
      else if (box_l)
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s(sp_%sArray_get(_t%d, _t%d)));\n", tO, box_l, k, tL, ti);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)\n", ti, ti, k2, tR, ti);
      emit_indent(g_pre, g_indent + 1);
      if (a0r.elem == TY_POLY)
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));\n", tO, tR, ti);
      else if (box_r_elem)
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s(_t%d, _t%d));\n", tO, box_r_elem, tR, ti);
      else if (box_r)
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s(sp_%sArray_get(_t%d, _t%d)));\n", tO, box_r, k2, tR, ti);
      buf_printf(b, "_t%d", tO);
      (void)get_l; (void)get_r;
      return 1;
    }
  }
  return 0;
}

/* the poly runtime's name for a named set operator */
static const char *fn_of_named_setop(const char *name) {
  return is_intersection_alias(name) ? "intersect" : is_union_alias(name) ? "union" : "difference";
}

/* The poly-array form of a set-op operand of kind `t` held in `v`: a poly
   array as it is, a typed one converted, a boxed value coerced (an Array, or
   CRuby's TypeError), an empty literal NULL. 0 for a kind it cannot take. */
static int setop_poly_operand(TyKind t, const char *v, Buf *b) {
  switch (t) {
    case TY_POLY_ARRAY: buf_puts(b, v); break;
    case TY_INT_ARRAY: buf_printf(b, "sp_IntArray_to_poly(%s)", v); break;
    case TY_STR_ARRAY: buf_printf(b, "sp_StrArray_to_poly_fmt(%s)", v); break;
    case TY_FLOAT_ARRAY: buf_printf(b, "sp_FloatArray_to_poly(%s)", v); break;
    case TY_POLY: buf_printf(b, "sp_poly_set_operand(%s)", v); break;
    case TY_UNKNOWN: buf_puts(b, "NULL"); break;
    default: return 0;
  }
  return 1;
}

/* union / intersection / difference(*others) whose operands are not all of
   the receiver's kind (`[1].union(["a"], [2])`, or a boxed element under
   --int-overflow=promote): every operand evaluated in order into a rooted
   temp, then the poly op folded over them from the receiver as a poly
   array. 0 when an operand's kind has no poly form. */
static int emit_setop_fold_poly(Compiler *c, int id, int recv, TyKind rt, const char *fn, Buf *b) {
  int argc; const int *argv = call_args(c->nt, id, &argc);
  Buf chk; memset(&chk, 0, sizeof chk);
  int ok = setop_poly_operand(rt, "", &chk);
  for (int j = 0; j < argc && ok; j++) ok = setop_poly_operand(repr_of(c, argv[j]).as_ty, "", &chk);
  free(chk.p);
  if (!ok || rt == TY_POLY || rt == TY_UNKNOWN) return 0;
  int t = ++g_tmp, t0 = g_tmp + 1;
  g_tmp += argc;
  buf_puts(b, "({ ");
  for (int j = -1; j < argc; j++) {
    int n = j < 0 ? recv : argv[j];
    TyKind at = j < 0 ? rt : repr_of(c, n).as_ty;
    if (at == TY_UNKNOWN) continue;
    char v[24]; snprintf(v, sizeof v, "_t%d", j < 0 ? t : t0 + j);
    emit_ctype(c, at, b); buf_printf(b, " %s = ", v); emit_expr(c, n, b);
    buf_printf(b, at == TY_POLY ? "; SP_GC_ROOT_RBVAL(%s); " : "; SP_GC_ROOT(%s); ", v);
  }
  int ta = ++g_tmp;
  char v[24]; snprintf(v, sizeof v, "_t%d", t);
  buf_printf(b, "sp_PolyArray *_t%d = ", ta); setop_poly_operand(rt, v, b);
  buf_printf(b, "; SP_GC_ROOT(_t%d);", ta);
  for (int j = 0; j < argc; j++) {
    snprintf(v, sizeof v, "_t%d", t0 + j);
    buf_printf(b, " _t%d = sp_PolyArray_%s(_t%d, ", ta, fn, ta);
    setop_poly_operand(repr_of(c, argv[j]).as_ty, v, b);
    buf_puts(b, ");");
  }
  buf_printf(b, " _t%d; })", ta);
  return 1;
}

/* union / intersection / difference with a `*splat` among the operands
   (`a.union(*others)`): how many operands it gives is known only at run time.
   The per-arity arms read the splat as one operand -- a poly array where an
   sp_RbVal goes, which failed to compile, or a call that raised
   NoMethodError. 1 when argv holds a splat this fold can spread (an
   anonymous `*` needs its rest parameter). */
static int setop_splat_args(Compiler *c, const int *argv, int argc) {
  const NodeTable *nt = c->nt;
  int splat = 0;
  for (int j = 0; j < argc; j++) {
    if (nt_kind(nt, argv[j]) != NK_SplatNode) continue;
    splat = 1;
    if (nt_ref(nt, argv[j], "expression") >= 0) continue;
    Buf ab; memset(&ab, 0, sizeof ab);
    int ok = emit_anon_rest_ref(c, argv[j], &ab);
    free(ab.p);
    if (!ok) return 0;
  }
  return splat;
}

/* The operands gathered, then the poly op folded over each into the rooted
   poly array _t<tr>, which holds the receiver's elements; one that is no
   Array is CRuby's TypeError. None at all leaves the receiver's copy,
   deduplicated by union alone (difference_eql, whose nil operand is no
   operand: the public difference raises on one). */
static void emit_setop_splat_fold(Compiler *c, const int *argv, int argc, const char *name, int tr, Buf *b) {
  int ta = ++g_tmp, ti = ++g_tmp;
  const char *fn = fn_of_named_setop(name);
  emit_gather_call_args(c, argv, argc, ta, b);
  buf_printf(b, " if (_t%d->len == 0) _t%d = sp_PolyArray_%s(_t%d, NULL);"
                " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                " _t%d = sp_PolyArray_%s(_t%d, sp_poly_set_operand(_t%d->data[_t%d]));",
             ta, tr, sp_streq(fn, "union") ? "union" : "difference_eql", tr,
             ti, ti, ta, ti, tr, fn, tr, ta, ti);
}

/* a typed array's set op over a splat (setop_splat_args): the receiver taken
   as a poly array. 0 for a call not typed a poly array. */
static int emit_setop_splat(Compiler *c, int id, int recv, TyKind rt, const char *name, Buf *b) {
  int argc; const int *argv = call_args(c->nt, id, &argc);
  if (!is_named_set_operator(name) || !setop_splat_args(c, argv, argc)) return 0;
  if (repr_of(c, id).as_ty != TY_POLY_ARRAY || rt == TY_UNKNOWN || rt == TY_POLY) return 0;
  Buf rb; memset(&rb, 0, sizeof rb);
  int ok = setop_poly_operand(rt, "", &rb);
  free(rb.p);
  if (!ok) return 0;
  memset(&rb, 0, sizeof rb);
  emit_expr(c, recv, &rb);
  int tr = ++g_tmp;
  buf_printf(b, "({ sp_PolyArray *_t%d = ", tr);
  setop_poly_operand(rt, rb.p ? rb.p : "", b);
  buf_printf(b, "; SP_GC_ROOT(_t%d); ", tr);
  free(rb.p);
  emit_setop_splat_fold(c, argv, argc, name, tr, b);
  buf_printf(b, " _t%d; })", tr);
  return 1;
}

/* Array#& / #| / #- and intersection / union / difference with operands: the
   same kind runs the typed set op; another kind, or a boxed operand, the poly one */
int emit_op_array_setop(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  Repr a0r = repr_of(c, argc >= 1 ? argv[0] : -1);
  TyKind a0 = a0r.as_ty;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (emit_setop_splat(c, id, recv, rt, name, b)) return 1;
  if (emit_array_operand_type_error(c, x, b)) return 1;
  /* difference on a receiver that can be nil raises under difference, not
     under the - the functions name (#9347), once every operand has run: the
     receiver and the operands are evaluated into temps, the receiver is
     tested (and seen as tested, nil_tested), and the set op is emitted over
     the temps, so an operand of another element kind is boxed after the test
     and not turned into an empty Array from a NULL receiver */
  if (is_difference_spelling(name) && argc >= 1 && rt != TY_POLY_ARRAY && k &&
      !recv_never_nil(c, recv) && !repr_of(c, recv).nil_tested) {
    int tr = ++g_tmp;
    Buf rx = expr_buf(c, recv);
    Buf pre; memset(&pre, 0, sizeof pre);
    buf_printf(&pre, "({ sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d); ", k, tr, rx.p ? rx.p : "", tr);
    free(rx.p);
    int sv = g_n_argov;
    view_bind(recv, "_t%d", tr);
    for (int j = 0; j < argc; j++) {
      Repr ar = repr_of(c, argv[j]);
      if (ar.untyped) continue;
      int ta = ++g_tmp;
      emit_ctype(c, ar.as_ty, &pre);
      buf_printf(&pre, " _t%d = ", ta);
      emit_expr(c, argv[j], &pre);
      buf_puts(&pre, "; ");
      emit_gc_root_tmp(c, ar.as_ty, ta, &pre);
      buf_puts(&pre, " ");
      view_bind(argv[j], "_t%d", ta);
    }
    nil_recv_check(name, tr, &pre);
    int tok = view_push_repr(c, recv, VR_NIL_TESTED, 1);
    Buf inner; memset(&inner, 0, sizeof inner);
    int ok = emit_op_array_setop(c, x, &inner);
    view_pop(c, tok);
    view_unbind(sv);
    if (ok) buf_printf(b, "%s %s; })", pre.p ? pre.p : "", inner.p ? inner.p : "");
    free(pre.p); free(inner.p);
    return ok;
  }
  if (rt == TY_POLY_ARRAY) {
    if (is_set_op(name) && argc == 1 && (a0r.elem == TY_POLY || a0r.untyped)) {
      const char *fn = (is_intersection_alias(name)) ? "intersect" : (is_union_alias(name) ? "union" : "difference");
      buf_printf(b, "sp_PolyArray_%s(", fn);
      emit_expr(c, recv, b); buf_puts(b, ", ");
      if (a0r.untyped) buf_puts(b, "NULL"); else emit_expr(c, argv[0], b);
      buf_puts(b, ")"); return 1;
    }
    /* poly-array set-op with a typed-array argument (different element type):
       box the argument to a poly array, then run the poly op. */
    if (is_set_op(name) && argc == 1 &&
        (a0r.elem == TY_INT || a0r.elem == TY_STRING || a0r.elem == TY_FLOAT)) {
      const char *fn = (is_intersection_alias(name)) ? "intersect" : (is_union_alias(name) ? "union" : "difference");
      const char *conv = a0r.elem == TY_INT ? "sp_IntArray_to_poly" :
                         a0r.elem == TY_STRING ? "sp_StrArray_to_poly_fmt" : "sp_FloatArray_to_poly";
      buf_printf(b, "sp_PolyArray_%s(", fn);
      emit_expr(c, recv, b); buf_printf(b, ", %s(", conv); emit_expr(c, argv[0], b);
      buf_puts(b, "))"); return 1;
    }
    /* poly-array receiver, POLY argument: same run-time coercion (#3475) */
    if (is_set_op(name) && argc == 1 &&
        a0r.kind == RK_BOXED) {
      const char *fn = (is_intersection_alias(name)) ? "intersect" : (is_union_alias(name) ? "union" : "difference");
      buf_printf(b, "sp_PolyArray_%s(", fn);
      emit_expr(c, recv, b); buf_puts(b, ", sp_poly_set_operand(");
      emit_expr(c, argv[0], b); buf_puts(b, "))"); return 1;
    }
    /* variadic named set ops on a poly array: fold over each argument */
    if ((is_named_set_operator(name)) && argc >= 2) {
      int ok = 1;
      for (int j = 0; j < argc; j++) {
        Repr atr = repr_of(c, argv[j]);
        if (atr.elem != TY_POLY && !atr.untyped) { ok = 0; break; }
      }
      if (ok) {
        const char *fn = sp_streq(name, "intersection") ? "intersect" :
                         sp_streq(name, "union") ? "union" : "difference";
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d);", t);
        for (int j = 0; j < argc; j++) {
          buf_printf(b, " _t%d = sp_PolyArray_%s(_t%d, ", t, fn, t);
          if (repr_of(c, argv[j]).untyped) buf_puts(b, "NULL");
          else emit_expr(c, argv[j], b);
          buf_puts(b, ");");
        }
        buf_printf(b, " _t%d; })", t);
        return 1;
      }
      return emit_setop_fold_poly(c, id, recv, rt, fn_of_named_setop(name), b);
    }
    return 0;
  }
  if (is_set_op(name) && argc == 1 && (a0r.elem == ty_array_elem(rt) || a0r.untyped)) {
    const char *fn = (is_intersection_alias(name)) ? "intersect" : ((is_union_alias(name)) ? "union" : "difference");
    /* empty literal [] arg: use a null pointer (safe for all sp_*Array_* set ops) */
    if (a0r.untyped) { buf_printf(b, "sp_%sArray_%s(", k, fn); emit_expr(c, recv, b); buf_puts(b, ", NULL)"); }
    else { buf_printf(b, "sp_%sArray_%s(", k, fn); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    return 1;
  }
  /* typed-array receiver, different-kind typed-array or poly-array argument:
     box both operands to poly and run the poly set op (result poly). */
  if (is_set_op(name) && argc == 1 &&
      (a0r.elem == TY_INT || a0r.elem == TY_STRING || a0r.elem == TY_FLOAT || a0r.elem == TY_POLY) && a0r.elem != ty_array_elem(rt)) {
    const char *fn = (is_intersection_alias(name)) ? "intersect" : (is_union_alias(name) ? "union" : "difference");
    const char *conv_l = rt == TY_INT_ARRAY ? "sp_IntArray_to_poly" :
                         rt == TY_STR_ARRAY ? "sp_StrArray_to_poly_fmt" : "sp_FloatArray_to_poly";
    const char *conv_r = a0r.elem == TY_INT ? "sp_IntArray_to_poly" :
                         a0r.elem == TY_STRING ? "sp_StrArray_to_poly_fmt" :
                         a0r.elem == TY_FLOAT ? "sp_FloatArray_to_poly" : NULL;
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
  if (is_set_op(name) && argc == 1 &&
      a0r.kind == RK_BOXED) {
    const char *fn = (is_intersection_alias(name)) ? "intersect" : (is_union_alias(name) ? "union" : "difference");
    const char *conv_l = rt == TY_INT_ARRAY ? "sp_IntArray_to_poly" :
                         rt == TY_STR_ARRAY ? "sp_StrArray_to_poly_fmt" : "sp_FloatArray_to_poly";
    int tl = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = %s(", tl, conv_l); emit_expr(c, recv, b);
    buf_printf(b, "); SP_GC_ROOT(_t%d); sp_PolyArray_%s(_t%d, sp_poly_set_operand(", tl, fn, tl); emit_expr(c, argv[0], b);
    buf_puts(b, ")); })"); return 1;
  }
  /* variadic named set ops: union/intersection/difference(*others) fold the
     binary operator over each argument, accumulating in a rooted temp. */
  if ((is_named_set_operator(name)) && argc >= 2) {
    int ok = 1;
    for (int j = 0; j < argc; j++) {
      Repr atr = repr_of(c, argv[j]);
      if (atr.elem != ty_array_elem(rt) && !atr.untyped) { ok = 0; break; }
    }
    if (ok) {
      const char *fn = sp_streq(name, "intersection") ? "intersect" :
                       sp_streq(name, "union") ? "union" : "difference";
      int t = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d);", t);
      for (int j = 0; j < argc; j++) {
        buf_printf(b, " _t%d = sp_%sArray_%s(_t%d, ", t, k, fn, t);
        if (repr_of(c, argv[j]).untyped) buf_puts(b, "NULL");
        else emit_expr(c, argv[j], b);
        buf_puts(b, ");");
      }
      buf_printf(b, " _t%d; })", t);
      return 1;
    }
    return emit_setop_fold_poly(c, id, recv, rt, fn_of_named_setop(name), b);
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
  Repr a0r = repr_of(c, argc >= 1 ? argv[0] : -1);
  TyKind a0 = a0r.as_ty;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if (is_intersect_name(name) && argc == 1 &&
        (a0r.elem == TY_POLY || a0r.untyped || ty_is_array(a0) || a0r.kind == RK_BOXED)) {
      buf_puts(b, "sp_PolyArray_intersect_p_eql("); emit_expr(c, recv, b); buf_puts(b, ", ");
      if (a0r.untyped) buf_puts(b, "NULL");
      else if (a0r.elem == TY_POLY) emit_expr(c, argv[0], b);
      else {
        /* a differently-stored Array argument coerces; Ruby has one Array */
        buf_puts(b, "sp_poly_to_poly_array(");
        Buf ab3; memset(&ab3, 0, sizeof ab3); emit_expr(c, argv[0], &ab3);
        if (a0r.kind == RK_BOXED) buf_puts(b, ab3.p ? ab3.p : "sp_box_nil()");
        else emit_boxed_text(c, a0, ab3.p ? ab3.p : "NULL", b);
        free(ab3.p);
        buf_puts(b, ")");
      }
      buf_puts(b, ")");
      return 1;
    }
    return 0;
  }
  if (is_intersect_name(name) && argc == 1 &&
      (a0r.elem == ty_array_elem(rt) || a0r.untyped || ty_is_array(a0) || a0r.kind == RK_BOXED)) {
    /* Ruby has one Array; the storage kinds are ours. A receiver and an
       argument of different kinds -- a mapped String array against a
       poly-array constant, the shape this turned up in -- go through the
       generic comparison rather than declining to a NoMethodError. */
    if (a0r.elem == ty_array_elem(rt)) {
      buf_printf(b, "sp_%sArray_intersect_p(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    if (a0r.untyped) {
      buf_printf(b, "sp_%sArray_intersect_p(", k); emit_expr(c, recv, b); buf_puts(b, ", NULL)");
      return 1;
    }
    /* sp_poly_intersect_p_eql coerces only the argument and reads the receiver
       in place: two coercions as sibling arguments left the first one
       unrooted while the second allocated, and the set it builds allocates
       again. */
    buf_puts(b, "sp_poly_intersect_p_eql(");
    { Buf rb2; memset(&rb2, 0, sizeof rb2); emit_expr(c, recv, &rb2);
      emit_boxed_text(c, rt, rb2.p ? rb2.p : "NULL", b); free(rb2.p); }
    buf_puts(b, ", ");
    { Buf ab2; memset(&ab2, 0, sizeof ab2); emit_expr(c, argv[0], &ab2);
      if (a0r.kind == RK_BOXED) buf_puts(b, ab2.p ? ab2.p : "sp_box_nil()");
      else emit_boxed_text(c, a0, ab2.p ? ab2.p : "NULL", b);
      free(ab2.p); }
    buf_puts(b, ")");
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
  Repr a0r = repr_of(c, argc >= 1 ? argv[0] : -1);
  TyKind a0 = a0r.as_ty;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)a0; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if (sp_streq(name, "replace") && argc == 1 && a0r.elem == TY_POLY) {
      buf_puts(b, "sp_PolyArray_replace("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* ...and a source of ANOTHER kind, which `[1, 2].replace(["x"])` is:
       the widening makes the receiver poly, and the source is read through
       the boxed accessors rather than needing an arm of its own (#4339). */
    if (sp_streq(name, "replace") && argc == 1 && ty_is_array(rt) &&
        (ty_is_array(a0) || a0r.kind == RK_BOXED)) {
      buf_puts(b, "sp_PolyArray_replace_from("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    return 0;
  }
  if (sp_streq(name, "replace") && argc == 1 && a0r.elem == ty_array_elem(rt)) {
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
      (a0r.kind == RK_BOXED || a0r.elem == TY_POLY || (ty_is_array(a0) && a0r.elem != ty_array_elem(rt)))) {
    int t = ++g_tmp, ts = ++g_tmp, tc = ++g_tmp;
    buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_recv_rooted(c, recv, t, "SP_GC_ROOT", b);
    ts = hold_operand(c, argv[0], TY_POLY, 1, ts, 1, " ", b);
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
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
    const char *et = is_str ? "const char *" : rt == TY_FLOAT_ARRAY ? "sp_ofloat " : "sp_oint ";
    buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_nil_ck_recv(c, recv, rt, "cmp", 0, b);
    buf_puts(b, ";");
    if (is_str) buf_printf(b, " SP_GC_ROOT(_t%d);", t);
    /* an empty receiver answers [nil, nil]: an Integer or Float pair
       carries those nils in its bitmap */
    const char *ms = is_str ? "" : "_o", *mo = is_str ? "" : "_o";
    buf_printf(b, " %s_mn%d = sp_%sArray_min%s(_t%d); %s_mx%d = sp_%sArray_max%s(_t%d);"
                  " sp_%sArray *_t%d = sp_%sArray_new(); sp_%sArray_push%s(_t%d, _mn%d);"
                  " sp_%sArray_push%s(_t%d, _mx%d); _t%d; })",
               et, t, k, mo, t, et, t, k, mo, t, k, o, k, k, ms, o, t, k, ms, o, t, o);
    return 1;
  }
  return 0;
}

/* Array#sort: a typed receiver checks for a nil element first; the
   poly form is the blockless one */
int emit_op_array_sort(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if (rt == TY_POLY_ARRAY && sp_streq(name, "uniq") && argc == 0 &&
        nt_ref(nt, id, "block") < 0) {
      /* builds the answer in one pass, rather than uniq! on a copy */
      buf_puts(b, "sp_PolyArray_uniq("); emit_expr(c, recv, b); buf_puts(b, ")");
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if ((is_minmax_query(name)) && argc == 1 && nt_ref(nt, id, "block") < 0) {
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
  if ((is_minmax_query(name)) && argc == 1 && block < 0) {
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
      buf_printf(b, " if (_t%d && SP_MAY_NIL(_t%d))", t, t);
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
  /* promote typed an Integer sum poly: the total can leave the word */
  if (rt == TY_INT_ARRAY && argc == 0 && block < 0 && repr_of(c, id).kind == RK_BOXED) {
    buf_puts(b, "sp_IntArray_fold_v("); emit_expr(c, recv, b); buf_puts(b, ", sp_box_int(0), 0)");
    return 1;
  }
  if (rt == TY_POLY_ARRAY) {
    if (rt == TY_POLY_ARRAY && sp_streq(name, "sum") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      /* fold via sp_poly_add so a Float (or Rational/Bignum) element promotes
         the result instead of being dropped by the int-only sum (#2627) */
      buf_puts(b, "sp_PolyArray_sum_poly("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    return 0;
  }
  if (rt == TY_FLOAT_ARRAY) {
    int t = ++g_tmp;
    buf_printf(b, "({ sp_FloatArray *_t%d = ", t);
    emit_nil_ck_recv(c, recv, rt, "sum", 0, b);
    buf_printf(b, "; _t%d->len == 0 ? sp_box_int(0) : sp_box_float(sp_FloatArray_sum(_t%d, 0.0)); })", t, t);
    return 1;
  }
  if (rt == TY_INT_ARRAY && g_promote_mode) {
    buf_puts(b, "sp_IntArray_sum_promote("); emit_nil_ck_recv(c, recv, rt, "sum", 0, b); buf_puts(b, ", 0)");
    return 1;
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if (sp_streq(name, "compact!") && argc == 0) {
      /* value form: self when changed, nil when a no-op (CRuby) */
      buf_puts(b, "sp_PolyArray_compact_bangq("); emit_array_frozen_checked(c, recv, rt, b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "flatten!") && argc == 0) {
      /* value form: self when changed, nil when a no-op (CRuby) */
      buf_puts(b, "sp_PolyArray_flatten_bangq("); emit_array_frozen_checked(c, recv, rt, b); buf_puts(b, ")");
      return 1;
    }
    return 0;
  }
  if (sp_streq(name, "compact!") && argc == 0 && elem_nil_sentinel(c, recv, rt)) {
    /* an Integer or Float array that can hold nil drops its nil
       elements in place, answering self when it did; the no-op fold
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
    buf_puts(b, "((void)("); emit_array_frozen_checked(c, recv, rt, b); buf_puts(b, "), sp_box_nil())");
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
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
       to_a / to_ary / entries / deconstruct are (builtin-op rows). Those
       names run through the generic builtin-op row emitter, which already
       rebuilds a typed-array identity into a poly array when the call's
       own slot (a yield mixed with another site's poly-array kind) asks
       for one; this arm is this op's own emitter, so it has to do the same
       check itself, or a typed array's raw pointer gets assigned straight
       into the shared poly slot -- a C compile error the other names never
       hit, since no typed-array identity at a second site is ever a bare
       `sp_PolyArray *` and a bare typed-array pointer at once.
       The shared helper for that rebuild, emit_array_into_poly_slot, copies
       the receiver's own frozen flag and turns a NULL receiver into an
       empty poly array -- both right for the paths it already serves
       (a widening read, never meant to look like a fresh value), both
       wrong for flatten, which CRuby always returns a fresh, mutable copy
       from (even a frozen receiver's), and which raises NoMethodError on a
       nil receiver (reachable through an out-of-range slice) rather than
       answering an empty array (CodeRabbit, yosefbennywidyo/spinel#43).
       So this arm builds its own conversion instead of reusing that
       helper, with the nil check first and the frozen flag dropped. */
    if (comp_ntype(c, id) == TY_POLY_ARRAY) {
      const char *lk = rt == TY_INT_ARRAY ? "int" : rt == TY_FLOAT_ARRAY ? "float" : "str";
      int t = ++g_tmp, t2 = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
      buf_printf(b, "; if (!_t%d) sp_raise_nomethod(sp_nomethod_msg(\"flatten\", sp_box_nil()));"
                    " sp_PolyArray *_t%d = sp_PolyArray_from_%s_array(_t%d); _t%d->frozen = 0; _t%d; })",
                 t, t2, lk, t, t2, t2);
      return 1;
    }
    emit_expr(c, recv, b); return 1;
  }
  if ((sp_streq(name, "flatten!") || sp_streq(name, "flatten")) && argc == 1) {
    /* a typed (scalar-element) array has no nesting: flatten(n) copies,
       flatten!(n) is a no-op returning nil */
    if (name[7] == '!') {
      /* the receiver, then the depth, then the frozen check: the depth's
         side effects run before the raise, as in CRuby */
      int t = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); (void)(", t); emit_int_expr(c, argv[0], b);
      buf_printf(b, "); if (_t%d && _t%d->frozen) sp_raise_frozen_array_at(_t%d, %s); sp_box_nil(); })",
                 t, t, t, array_frozen_cls(rt));
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
  if (rt != TY_POLY_ARRAY) return 0;
  if (is_push_alias(name) && argc == 1) {
    /* a receiver that can be nil raises under the name as written (push, << or
       append), once the argument has run */
    if (recv_never_nil(c, recv)) {
      buf_puts(b, "sp_PolyArray_push("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    Buf rx = expr_buf(c, recv);
    int t = nil_recv_open(c, recv, rx.p ? rx.p : "", "sp_PolyArray *", NULL, subtree_may_allocate(c->nt, argv[0]), b);
    free(rx.p);
    buf_printf(b, "sp_PolyArray_push(_t%d, ", t); emit_boxed(c, argv[0], b); buf_puts(b, ")");
    nil_recv_close(name, t, 0, b);
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
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

/* Is receiver node recv an Array (or Hash) that is never nil: the nil fact
   (analyze_nil.c, #7444) says it cannot be. */
int recv_never_nil(Compiler *c, int recv) {
  /* ARGV is the program's own Array of Strings (a call to the runtime) */
  if (nt_kind(c->nt, recv) == NK_ConstantReadNode) {
    if (is_argv_const(nt_str(c->nt, recv, "name"))) return 1;
  }
  return !repr_of(c, recv).may_nil;
}

/* A call on a container that can be nil whose Ruby name its function does not
   carry: the compiler's own reads take a NULL container for an empty one, and
   the spellings that share a function (size and count with length; at, first,
   last and dig with []; << and append with push) are not one method to CRuby,
   which says `undefined method 'size' for nil` (#9347). A call with no
   argument passes its receiver through sp_nil_recv_named, which raises under
   the name as written. A call with arguments holds the receiver in a temp, runs
   the tolerant function on it, and tests the temp once the arguments have run,
   as CRuby tests a receiver after evaluating them: nil_recv_open begins the
   statement expression, with `<vt> _vN = ` ahead of the call when it answers a
   value of C type vt (NULL for a call that answers none), and roots the temp
   when root is set, as it must be while an argument can allocate; the caller
   writes the call on `_tN`, and nil_recv_close ends it. A function that does
   not take a NULL (a Hash's has_value, difference) is tested first, with
   nil_recv_check alone, once the arguments have been evaluated into temps. */
int nil_recv_open(Compiler *c, int recv, const char *rtext, const char *ctype, const char *vt, int root, Buf *b) {
  /* the argument-order rewrite may hold the receiver in a rooted temp
     already (`_tN`): that one is the test's, and is not copied and rooted again */
  int t = -1, held = 0, n = 0;
  for (int i = 0; i < g_n_argov; i++)
    if (g_argov_node[i] == recv) held = 1;
  if (held && (sscanf(rtext, "_t%d%n", &t, &n) != 1 || rtext[n])) t = -1;
  buf_puts(b, "({ ");
  if (t < 0) {
    t = ++g_tmp;
    buf_printf(b, "%s _t%d = %s; ", ctype, t, rtext);
    if (root) buf_printf(b, "SP_GC_ROOT(_t%d); ", t);
  }
  if (vt) buf_printf(b, "%s _v%d = ", vt, t);
  return t;
}

void nil_recv_check(const char *name, int t, Buf *b) {
  buf_printf(b, "if (SP_UNLIKELY(!_t%d)) sp_nil_recv(\"%s\");", t, name);
}

void nil_recv_close(const char *name, int t, int has_value, Buf *b) {
  buf_puts(b, "; ");
  nil_recv_check(name, t, b);
  if (has_value) buf_printf(b, " _v%d;", t);
  buf_puts(b, " })");
}

/* Array#length, size, count, empty?, to_a and entries (#9347). nil has no
   length, size, count or empty?, nil.to_a is [], and nil has no entries, so a
   receiver that can be nil is read through what says so (sp_*Array_length_m,
   or sp_nil_recv_named under the name as written, _to_a, _entries); one that
   cannot keeps the plain read
   the compiler's own loops use (sp_*Array_length, tolerant of NULL as it
   always was, and to_a's receiver itself), so a program over an Array known
   not to be nil keeps its C and the folding gcc does on it. entries copies,
   whatever the receiver. */
int emit_op_array_nil_recv(Compiler *c, const BopCtx *x, Buf *b) {
  TyKind rt = x->rt;
  const char *k = rt == TY_POLY_ARRAY ? "Poly" : ty_is_ptr_array(rt) ? "Ptr" : array_kind(rt);
  if (!k) return 0;
  /* the Array of shared String handles is an sp_PtrArray for its length */
  const char *lk = k;
  int entries = sp_streq(x->name, "entries"), to_a = sp_streq(x->name, "to_a");
  /* a safe navigation has tested the receiver already */
  int plain = recv_never_nil(c, x->recv) || nt_safe_nav(c->nt, x->id);
  /* Integer and Float Arrays have no tolerant length to tell apart from */
  int len_m = !plain && (rt == TY_POLY_ARRAY || rt == TY_STR_ARRAY || ty_is_ptr_array(rt));
  /* the raising length names length: size, count and empty? raise under their own name */
  int named = len_m && is_length_spelling(x->name);
  /* an object Array's raiser already names the call */
  if (!plain && sp_streq(x->name, "empty?") && ty_is_ptr_array(rt)) {
    buf_puts(b, "sp_PtrArray_empty(");
    if (x->rtext) buf_puts(b, x->rtext);
    else emit_expr(c, x->recv, b);
    buf_puts(b, ")");
    return 1;
  }
  /* a length where the call's slot holds its nil is lifted, as the template did */
  int lift = !to_a && !entries && !sp_streq(x->name, "empty?") && node_is_oint(c, x->id);
  if (lift) buf_printf(b, "%s(", oint_of(TY_INT));
  if (to_a && plain) { /* the receiver itself */ }
  else if (to_a || entries) buf_printf(b, "sp_%sArray_%s(", k, entries ? "entries" : "to_a");
  else {
    if (sp_streq(x->name, "empty?")) buf_puts(b, "(");
    buf_printf(b, "sp_%sArray_length%s(", lk, len_m && !named ? "_m" : "");
    if (named) buf_puts(b, "sp_nil_recv_named(");
  }
  if (x->rtext) buf_puts(b, x->rtext);
  else emit_expr(c, x->recv, b);
  if (named) buf_printf(b, ", \"%s\")", x->name);
  if (!(to_a && plain)) buf_puts(b, ")");
  if (sp_streq(x->name, "empty?")) buf_puts(b, " == 0)");
  if (lift) buf_puts(b, ")");
  return 1;
}

/* Array#transpose of a poly array */
int emit_op_array_transpose(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
  /* transpose of an Array of Integers, Floats or Strings: CRuby converts
     each element to an Array, which a scalar cannot -- TypeError, and an
     empty one answers [] */
  if (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY || rt == TY_STR_ARRAY) {
    const char *k = array_kind(rt);
    int t = ++g_tmp;
    const char *en = rt == TY_INT_ARRAY ? "Integer" : rt == TY_FLOAT_ARRAY ? "Float" : "String";
    buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
    buf_printf(b, "; if (!_t%d) sp_raise_nomethod(sp_nomethod_msg(\"transpose\", sp_box_nil()));"
                  " if (sp_%sArray_length(_t%d) > 0) sp_raise_cls(\"TypeError\","
                  " \"no implicit conversion of %s into Array\"); sp_%sArray_new(); })", t, k, t, en, k);
    return 1;
  }
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
  /* an array of numbers, Strings, Symbols or booleans holds no Array for
     assoc or rassoc to match: nil, once the receiver and the key are
     evaluated, as CRuby answers */
  if (rt != TY_POLY_ARRAY && argc == 1 && (sp_streq(name, "assoc") || sp_streq(name, "rassoc"))) {
    TyKind et = ty_array_elem(rt);
    if (et == TY_INT || et == TY_FLOAT || et == TY_STRING || et == TY_SYMBOL || et == TY_BOOL) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
      emit_boxed(c, argv[0], b); buf_puts(b, "), sp_box_nil())");
      return 1;
    }
  }
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if (is_combination_family(name) &&
        (argc == 1 || (sp_streq(name, "permutation") && argc == 0)) &&
        nt_ref(nt, id, "block") < 0) {
      const char *combfn = sp_streq(name, "combination") ? "sp_PolyArray_combination"
                         : sp_streq(name, "permutation") ? "sp_PolyArray_permutation"
                         : sp_streq(name, "repeated_permutation") ? "sp_PolyArray_repeated_permutation"
                         : "sp_PolyArray_repeated_combination";
      int ta = ++g_tmp;
      /* a poly-array receiver materializes the tuples where the call on them
         answers alike (an_blockless_tuples_kept); elsewhere it answers the
         Enumerator over them, labeled with the receiver and the call */
      if (comp_ntype(c, id) == TY_ENUMERATOR) {
        int tn = ++g_tmp, te = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_int _t%d = ", ta, tn);
        if (argc == 1) emit_int_expr(c, argv[0], b);
        else buf_printf(b, "_t%d ? _t%d->len : 0", ta, ta);
        buf_printf(b, "; sp_Enumerator *_t%d = sp_Enumerator_new_from(sp_box_poly_array(%s(_t%d, _t%d)));", te, combfn, ta, tn);
        buf_printf(b, " SP_GC_ROOT(_t%d); sp_enum_with_src(_t%d, sp_box_poly_array(_t%d), ", te, te, ta);
        emit_combinator_enum_label(name, argc, tn, b);
        buf_puts(b, "); })");
        return 1;
      }
      buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); %s(_t%d, ", ta, combfn, ta);
      if (argc == 1) emit_int_expr(c, argv[0], b);
      else buf_printf(b, "_t%d ? _t%d->len : 0", ta, ta);
      buf_puts(b, "); })");
      return 1;
    }
    return 0;
  }
  if (is_combination_family(name) &&
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
  if (is_combination_family(name) &&
      (argc == 1 || (sp_streq(name, "permutation") && argc == 0)) &&
      nt_ref(nt, id, "block") < 0) {
    /* any other element kind rides the boxed PolyArray implementation */
    const char *combfn = sp_streq(name, "combination") ? "sp_PolyArray_combination"
                       : sp_streq(name, "permutation") ? "sp_PolyArray_permutation"
                       : sp_streq(name, "repeated_permutation") ? "sp_PolyArray_repeated_permutation"
                       : "sp_PolyArray_repeated_combination";
    int ta = ++g_tmp, ts = ++g_tmp, tn = ++g_tmp, te = ++g_tmp;
    buf_puts(b, "({ "); ts = hold_operand(c, recv, TY_POLY, 1, ts, 1, " ", b);
    buf_printf(b, "sp_PolyArray *_t%d = sp_poly_to_poly_array(_t%d); SP_GC_ROOT(_t%d); sp_int _t%d = ", ta, ts, ta, tn);
    if (argc == 1) emit_int_expr(c, argv[0], b);
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) {
    if (is_quantifier(name) &&
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
  if (is_quantifier(name) &&
      argc == 0 && nt_ref(nt, id, "block") < 0) {
    /* scalar-element arrays never hold false: predicate is length-based.
       One that can hold nil counts its truthy (non-nil) elements
       instead. */
    if (rt == TY_STR_ARRAY) {
      /* any String Array can hold nil (sp_StrArray_truthy_count); count
         only as far as the answer needs: any?/none? stop at the first
         String, one? at the second */
      int ta = ++g_tmp;
      buf_printf(b, "({ sp_StrArray *_t%d = ", ta); emit_expr(c, recv, b);
      if (sp_streq(name, "all?"))       buf_printf(b, "; sp_StrArray_truthy_count(_t%d, sp_StrArray_length(_t%d)) == sp_StrArray_length(_t%d); })", ta, ta, ta);
      else if (sp_streq(name, "any?"))  buf_printf(b, "; sp_StrArray_truthy_count(_t%d, 1) > 0; })", ta);
      else if (sp_streq(name, "none?")) buf_printf(b, "; sp_StrArray_truthy_count(_t%d, 1) == 0; })", ta);
      else                              buf_printf(b, "; sp_StrArray_truthy_count(_t%d, 2) == 1; })", ta);
      return 1;
    }
    if (elem_nil_sentinel(c, recv, rt)) {
      /* sp_*Array_truthy_count: the length unless the array has a nil bitmap */
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
  if (rt == TY_POLY_ARRAY) return 0;
  if (sp_streq(name, "dig") && argc >= 2) {
    /* multi-step (one step is arr[i], a builtin-op row): hand the whole
       key list to the runtime walk, which stops at nil and raises
       TypeError on a step that cannot be dug. Chaining index reads
       instead read the scalar the first step answered as if it were an
       array, so `[1].dig(0, 0)` answered 1 where Ruby raises (#3825). */
    Buf rb = {0}; emit_boxed(c, recv, &rb);
    emit_rooted_key_call(c, "sp_poly_dig_n", rb.p, argv, argc, b);
    free(rb.p);
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
  if (rt != TY_POLY_ARRAY) return 0;
  if (rt == TY_POLY_ARRAY && sp_streq(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0) {
    TyKind init_t = comp_ntype(c, argv[0]);
    /* an Array initial value concatenates one level ([[1],[2]].sum([])) */
    if (ty_is_array(init_t)) {
      emit_poly_sum_seed(c, recv, argv[0], b);
      return 1;
    }
    /* a String initial value folds by concatenation ([str].sum("")) */
    if (init_t == TY_STRING) {
      emit_poly_sum_seed(c, recv, argv[0], b);
      return 1;
    }
    /* a Float initial value folds to a Float (bare sp_float, not boxed),
       through sp_poly_sum_seed, the seeded fold the boxed receiver already
       takes: the receiver evaluated before the seed, the seed added first.
       Adding the seed to a plain fold of the elements summed in the wrong
       order and skipped an element that is no number, where CRuby raises
       the seed's own TypeError. */
    if (init_t == TY_FLOAT) {
      /* typed boxed (analyze: a Complex element answers a Complex), the fold's
         own value; a Float slot, if anything still types it so, unboxed */
      if (repr_of(c, id).kind == RK_BOXED) { emit_poly_sum_seed(c, recv, argv[0], b); return 1; }
      buf_puts(b, "sp_poly_to_f("); emit_poly_sum_seed(c, recv, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* an Integer (or poly) seed folds via sp_poly_add so Float/Rational/
       Bignum elements promote the result instead of being dropped by the
       int-only sum (matches the no-arg poly fold above) (#2959).
       The seed starts that fold: an empty array returns it untouched, and
       every element is added to the running total in order. */
    emit_poly_sum_seed(c, recv, argv[0], b);
    return 1;
  }
  return 0;
}

typedef struct {
  Repr repr;
  int boxed, snapshot;
} ConcatOperand;

/* Capture before any later conversion this lowering may perform. Direct
   synthetic to_ary calls have already run as argument expressions. */
static int prepare_concat_operands(Compiler *c, const int *argv, int argc, ConcatOperand *out) {
  int later_conversion = 0;
  for (int i = argc - 1; i >= 0; i--) {
    Repr r = repr_array_operand(c, argv[i]);
    /* Several concrete converters have no per-occurrence planned calls:
       the global hook would select a later definition at an earlier call.
       Leave that unsupported surface to the ordinary call path. */
    if (ty_is_object(r.as_ty) && r.kind != RK_BOXED && !conv_to_ary_misfit(c, r.as_ty)) return 0;
    out[i].repr = r;
    out[i].boxed = r.kind == RK_BOXED || !ty_is_array(r.as_ty);
    out[i].snapshot = later_conversion;
    if (out[i].boxed) later_conversion = 1;
  }
  return 1;
}

/* A literal or a nested call may put construction in g_pre. Keep that
   prelude beside this input's value, ahead of the next input, rather
   than hoisting all argument construction ahead of the receiver hold. */
static void emit_concat_hold(Compiler *c, int node, TyKind type, int boxed, int fresh, int t, Buf *b) {
  Buf pre = {0}, value = {0};
  Buf *saved_pre = g_pre;
  int saved_indent = g_indent;
  g_pre = &pre;
  g_indent = 0;
  if (fresh) emit_fresh_lift_recv(c, node, &value);
  else if (boxed) emit_boxed(c, node, &value);
  else emit_expr(c, node, &value);
  g_pre = saved_pre;
  g_indent = saved_indent;
  if (pre.p) buf_puts(b, pre.p);
  if (boxed) buf_puts(b, " sp_RbVal");
  else emit_ctype(c, type, b);
  buf_printf(b, " _t%d = %s; SP_GC_ROOT%s(_t%d);", t,
             value.p ? value.p : boxed ? "sp_box_nil()" : "NULL", boxed ? "_RBVAL" : "", t);
  free(pre.p);
  free(value.p);
}

/* The caller has admitted builtin Array#concat, through its typed route
   or a guarded builtin arm. Hold arguments, then guard before the boxed
   conversions performed here; already-desugared direct conversions are
   argument effects upstream. Capture each operand before later callbacks
   and append only when all conversions succeed. bulk retains the existing
   Integer statement append choice; it does not change capture policy. */
int emit_builtin_array_concat(Compiler *c, int id, int recv, TyKind rt, const int *argv, int argc, int bulk, Buf *b) {
  int fresh = an_push_widens_fresh(c, id);
  if (fresh) rt = TY_POLY_ARRAY;
  const char *k = arr_kind(rt);
  if (!k || nt_argv_has_splat(c->nt, argv, argc)) return 0;
  ConcatOperand *op = (ConcatOperand *)calloc(argc ? argc : 1, sizeof *op);
  if (!op) { perror("calloc"); exit(1); }
  if (!prepare_concat_operands(c, argv, argc, op)) { free(op); return 0; }
  int int_bulk = bulk && argc == 1 && rt == TY_INT_ARRAY && !op[0].boxed && op[0].repr.as_ty == rt;
  int single_length = argc == 1 && !op[0].boxed && !op[0].repr.may_nil && !int_bulk;
  int tr = ++g_tmp, base = g_tmp + 1;
  g_tmp += argc;
  int lengths = g_tmp + 1; g_tmp += argc;
  buf_puts(b, "({ ");
  emit_concat_hold(c, recv, rt, 0, fresh, tr, b);
  for (int i = 0; i < argc; i++)
    emit_concat_hold(c, argv[i], op[i].repr.as_ty, op[i].boxed, 0, base + i, b);
  /* A sole nonnil typed operand has no later conversion. Keep its pure
     length read beside its hold, as on the typed route. A nullable operand
     keeps its length behind the entry-frozen and TypeError checks. */
  if (single_length)
    buf_printf(b, " sp_int _t%d = sp_%sArray_length(_t%d);", lengths, arr_kind(op[0].repr.as_ty), base);
  /* Argument expressions have run. This guard precedes the boxed
     conversions below, even for an empty concat. Nonempty appends test
     frozen again if a callback has frozen the receiver. */
  if (repr_of(c, recv).may_nil) buf_printf(b, " if (!_t%d) sp_nil_recv(\"concat\");", tr);
  buf_printf(b, " if (_t%d && _t%d->frozen) sp_raise_frozen_array_at(_t%d, %s);", tr, tr, tr, array_frozen_cls(rt));
  for (int i = 0; i < argc; i++) {
    int tv = base + i;
    const char *ak = arr_kind(op[i].repr.as_ty);
    if (op[i].boxed) {
      buf_printf(b, " _t%d = sp_poly_ary_arg_ck(_t%d);", tv, tv);
      if (op[i].snapshot) {
        int tc = ++g_tmp;
        buf_printf(b, " { sp_PolyArray *_t%d = sp_poly_to_poly_array(_t%d);"
                      " if (_t%d == (sp_PolyArray *)_t%d.v.p) _t%d = sp_PolyArray_dup(_t%d);"
                      " _t%d = sp_box_poly_array(_t%d); }", tc, tv, tc, tv, tc, tc, tv, tc);
      }
      buf_printf(b, " sp_int _t%d = sp_poly_length(_t%d);", lengths + i, tv);
    }
    else {
      /* A typed pointer's NULL is Ruby's nil, not an empty operand. */
      if (op[i].repr.may_nil)
        buf_printf(b, " if (!_t%d) sp_raise_cls(\"TypeError\", \"no implicit conversion of nil into Array\");", tv);
      if (op[i].snapshot) buf_printf(b, " _t%d = sp_%sArray_dup(_t%d);", tv, ak, tv);
      if (!int_bulk && !single_length) buf_printf(b, " sp_int _t%d = sp_%sArray_length(_t%d);", lengths + i, ak, tv);
    }
  }
  for (int i = 0; i < argc; i++) {
    int tv = base + i;
    TyKind at = op[i].repr.as_ty;
    const char *ak = arr_kind(at);
    if (int_bulk) {
      buf_printf(b, " sp_IntArray_append_o(_t%d, _t%d);", tr, tv);
      continue;
    }
    int ti = ++g_tmp;
    int same = !op[i].boxed && at == rt;
    int num = rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY;
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) sp_%sArray_push%s(_t%d, ",
               ti, ti, lengths + i, ti, k, num ? (same ? "_o" : "_nilable") : "", tr);
    Buf el = {0};
    if (same) buf_printf(&el, "sp_%sArray_%s(_t%d, _t%d)", ak, num ? "oget" : "get", tv, ti);
    else if (op[i].boxed) {
      /* The checked Array's numeric kinds retain the iterator's inline
         nil-aware reads. Other kinds need the Array getter: the generic
         iterator does not handle Symbol Arrays. */
      /* Keep the compatible Integer read on the hot path of an Integer
         destination, rather than the Float/error arms of the boxed read. */
      const char *likely = rt == TY_INT_ARRAY ? "SP_LIKELY" : "";
      buf_printf(&el, "(%s(_t%d.cls_id == SP_BUILTIN_INT_ARRAY) ? sp_IntArray_box_elem((sp_IntArray *)_t%d.v.p, _t%d)"
                      " : _t%d.cls_id == SP_BUILTIN_FLT_ARRAY ? sp_FloatArray_box_elem((sp_FloatArray *)_t%d.v.p, _t%d)"
                      " : sp_poly_arr_get(_t%d, _t%d))", likely, tv, tv, ti, tv, tv, ti, tv, ti);
    }
    else if (at == TY_POLY_ARRAY) buf_printf(&el, "sp_PolyArray_get(_t%d, _t%d)", tv, ti);
    else {
      char get[96];
      int onum = at == TY_INT_ARRAY || at == TY_FLOAT_ARRAY;
      snprintf(get, sizeof get, "sp_%sArray_%s(_t%d, _t%d)", ak, onum ? "oget" : "get", tv, ti);
      if (onum) buf_printf(&el, "%s(%s)", at == TY_INT_ARRAY ? "sp_box_oint" : "sp_box_ofloat", get);
      else emit_boxed_text(c, ty_array_elem(at), get, &el);
    }
    if (same || rt == TY_POLY_ARRAY) buf_puts(b, el.p);
    else if (rt == TY_INT_ARRAY) buf_printf(b, "sp_poly_elem_i(%s)", el.p);
    else if (rt == TY_FLOAT_ARRAY) buf_printf(b, "sp_poly_elem_f(%s)", el.p);
    else emit_unbox_text(c, ty_array_elem(rt), el.p, b);
    free(el.p);
    buf_puts(b, ");");
  }
  free(op);
  buf_printf(b, " _t%d; })", tr);
  return 1;
}

int emit_op_array_concat(Compiler *c, const BopCtx *x, Buf *b) {
  /* Inference answers a fresh receiver's widening outside the rows. The
     typed route below the row calls the same lowering for that case. */
  if (an_push_widens_fresh(c, x->id)) return 0;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  /* emit_builtin_op_ex selected the builtin row in the caller's arm. */
  return emit_builtin_array_concat(c, x->id, x->recv, x->rt, argv, argc, 0, b);
}

/* Array#index(v) / #find_index(v) / #rindex(v) without a block on a poly
   array (a typed one asks for a user ==, after the lookup) */
int emit_op_array_index_v(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
  if (rt != TY_POLY_ARRAY) return 0;
  /* rindex(obj): last matching index, or nil (an sp_oint, matching
     the index/find_index int-or-nil convention). */
  if (rt == TY_POLY_ARRAY && sp_streq(name, "rindex") && argc == 1) {
    Buf rb; int ch = hold_recv_open(c, recv, 0, "sp_PolyArray *", "SP_GC_ROOT", b, &rb);
    int t = ++g_tmp;
    oint_open(c, id, TY_INT, b);
    buf_printf(b, "({ sp_int _t%d = sp_PolyArray_rindex(%s, ", t, rb.p); free(rb.p);
    emit_boxed(c, argv[0], b);
    buf_printf(b, "); _t%d < 0 ? sp_oint_nil() : sp_oint_of(_t%d); })", t, t);
    if (ch) buf_puts(b, "; })");
    oint_close(c, id, b);
    return 1;
  }
  /* index(v) / find_index(v) on a poly array (no block) -> the first
     position whose element == v (sp_poly_eq), or nil (an sp_oint),
     mirroring the count(v)/any?(v) idiom (doom: @map.sectors.index(sector)). */
  if (rt == TY_POLY_ARRAY && (sp_streq(name, "index") || sp_streq(name, "find_index")) &&
      argc == 1) {
    int trecv = ++g_tmp, ta = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
    Buf ra = expr_buf(c, recv);
    /* Root the receiver and the boxed needle: sp_poly_eq can allocate
       (bigint promotion), so a collection may run mid-loop. */
    buf_printf(b, "({ sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);", trecv, ra.p ? ra.p : "NULL", trecv); free(ra.p);
    buf_printf(b, " sp_RbVal _t%d = ", ta); emit_boxed(c, argv[0], b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", ta);
    buf_printf(b, " sp_oint _t%d = sp_oint_nil();", tres);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)", ti, ti, trecv, ti);
    buf_printf(b, " if (sp_poly_rb_equal(sp_PolyArray_get(_t%d, _t%d), _t%d)) { _t%d = sp_oint_of(_t%d); break; }",
               trecv, ti, ta, tres, ti);
    buf_puts(b, " "); oint_open(c, id, TY_INT, b); buf_printf(b, "_t%d", tres); oint_close(c, id, b); buf_puts(b, "; })");
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
  /* --share-strings: the to_a the compiler routes an Enumerator's cycle
     through reads the Enumerator where it is made; one over a String's
     handle cycles as it is pulled instead, each round from the String as
     it is then (sp_poly_cycle, sp_Enumerator_cycle_gen) */
  const NodeTable *nt = c->nt;
  int r = x->recv, rr = r >= 0 && nt_kind(nt, r) == NK_CallNode ? nt_ref(nt, r, "receiver") : -1;
  if (rr >= 0 && nt_int(nt, r, "made_to_a", 0) && is_to_array_alias(nt_str(nt, r, "name")) &&
      comp_ntype(c, rr) == TY_ENUMERATOR && str_enum_h_made(c)) {
    buf_puts(b, "sp_poly_cycle(sp_box_obj(");
    emit_expr(c, rr, b);
    buf_puts(b, ", SP_BUILTIN_ENUMERATOR))");
    return 1;
  }
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
  buf_puts(b, "({ "); ta = hold_operand(c, x->recv, TY_POLY, 1, ta, 1, " ", b);
  buf_printf(b, "sp_Class _t%d = ", tc2); emit_expr(c, argv[0], b);
  buf_puts(b, "; "); emit_poly_iter_obj_reject(c, x->id, ta, name, b);
  emit_poly_iter_obj_normalize(c, ta, b);
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

/* `boxed << x` dispatched at run time from receiver temp _t<t> with the boxed
   argument text arg: sp_poly_shl, which knows the builtin values and the user
   classes, after an arm for each native class the box can hold (any the
   program makes, where the walk cannot bound the box) whose binding takes
   `<<` (StringIO#<<, as its typed receiver calls it), answering the
   receiver */
static void emit_poly_shl_call(Compiler *c, int id, const int *argv, int t, const char *arg, Buf *b) {
  int nk = 0, ta = -1, tr = -1;
  const int *ks = poly_recv_classes(c, id, &nk);
  TyKind aty = TY_POLY;
  Buf arms; memset(&arms, 0, sizeof arms);
  for (int i = 0; i < (ks ? nk : c->nclasses); i++) {
    int k = ks ? ks[i] : i;
    Buf cb; TyKind mret;
    /* (a reopening's own `<<` is sp_poly_shl's user arm) */
    if (k < 0 || k >= c->nclasses || !c->classes[k].is_native_class || !c->classes[k].instantiated ||
        comp_method_in_chain(c, k, "<<", NULL) >= 0)
      continue;
    if (ta < 0) { ta = ++g_tmp; tr = ++g_tmp; }
    if (!poly_native_arm_call(c, k, "<<", 1, argv, &ta, &aty, t, &cb, &mret)) continue;
    buf_printf(&arms, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == %d) { %s; _t%d = _t%d; }\nelse ", t, t, k, cb.p, tr, t);
    free(cb.p);
  }
  if (arms.p) buf_printf(b, "({ sp_RbVal _t%d = %s; sp_RbVal _t%d; %s_t%d = sp_poly_shl(_t%d, _t%d); _t%d; })",
                         ta, arg, tr, arms.p, tr, t, ta, tr);
  else buf_printf(b, "sp_poly_shl(_t%d, %s)", t, arg);
  free(arms.p);
}

/* Is an operand an Array, a Hash or an object, which an Integer, Float or
   String array's element store refuses? */
static int container_or_object_operand(Compiler *c, int node) {
  TyKind t = repr_of(c, node).as_ty;
  return ty_is_array(t) || ty_is_obj_array(t) || ty_is_hash(t) || ty_is_object(t);
}

/* << and push on a poly or array receiver, a splat into an Array mutator, and their poly element stores */
int emit_call_append_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* `poly_val << x`: runtime dispatch via sp_poly_shl. For an array receiver it
     appends and returns the (same) array; for an integer it returns the shifted
     value. Use sp_poly_shl's RESULT -- returning the receiver would discard the
     shift (e.g. peek16's `hi << 8`). */
  if (recv >= 0 && sp_streq(name, "<<") && argc == 1 &&
      repr_of(c, recv).kind == RK_BOXED) {
    /* the hoisted receiver is rooted across its argument and the dispatch:
       sp_poly_shl can reach a user-defined <<, which can reassign the slot
       the receiver was read from, so the slot-keeping rule of the array
       arms (push_recv_in_slot) does not hold here */
    /* A capture wrapper's parameter the append is made to takes the result
       back, as the statement form does for any local (#3325): a plain String
       there appends by concat, and sp_poly_shl answers the new box. In value
       position -- `-> { a << x }` over the block parameter the wrapper
       (desugar_block_capture_wrap) renamed `__cap_N_a` -- the cell kept the
       old String and the append was lost. Only a plain String takes the
       result back: any other receiver answers itself, or, through a user
       class's own #<<, something that is not the receiver at all. */
    /* The same holds for any boxed local or ivar a chain starts at: a
       block's last line (its value goes nowhere, but it is a value), or
       `r = (x << y)`, left the variable without the append. It takes the
       result only while it still holds the receiver: an argument that
       stored a new String there (`@buf << take`) keeps it, as in CRuby.
       Each step of a chain comes through here, so each stores back before
       the next argument runs. */
    const char *rvn = nt_kind(nt, recv) == NK_LocalVariableReadNode ? nt_str(nt, recv, "name") : NULL;
    int slot = rvn && strncmp(rvn, "__cap_", 6) == 0 ? recv : poly_shl_root_slot(c, recv);
    int se = 0;
    int t = poly_binop_recv_temp(c, recv, argv[0], b, &se);
    Buf ab; memset(&ab, 0, sizeof ab);
    if (slot < 0) {
      emit_boxed(c, argv[0], &ab);
      emit_poly_shl_call(c, id, argv, t, ab.p, b);
      free(ab.p);
      buf_puts(b, se ? "; })" : "");
      c->args_in_call = recv;
      return 1;
    }
    int got = ++g_tmp, cur = ++g_tmp;
    if (!se) buf_puts(b, "({ ");
    buf_printf(b, "sp_RbVal _t%d = ", got);
    emit_boxed(c, argv[0], &ab);
    emit_poly_shl_call(c, id, argv, t, ab.p, b);
    free(ab.p);
    buf_printf(b, "; if (_t%d.tag == SP_TAG_STR) { sp_RbVal _t%d = ", t, cur);
    emit_expr(c, slot, b);
    buf_printf(b, "; if (_t%d.tag == SP_TAG_STR && _t%d.v.s == _t%d.v.s) ", cur, cur, t);
    emit_expr(c, slot, b);
    buf_printf(b, " = _t%d; } _t%d; })", got, got);
    c->args_in_call = recv;
    return 1;
  }
  /* poly_val >> int: unbox recv to int, apply op. & | ^ dispatch on the
     runtime tag instead (nil/bool are boolean ops, ints bitwise; #2401). */
  if (recv >= 0 && argc == 1 && is_bit_op(name) &&
      repr_of(c, recv).kind == RK_BOXED) {
    int bop = sp_streq(name, "&") ? 0 : sp_streq(name, "|") ? 1 : 2;
    /* the hoisted receiver is rooted across its argument and the dispatch:
       a heap receiver (an array, a Bignum, a user object whose own operator
       sp_poly_bitop reaches) is held by nothing else while the argument runs */
    int se = 0;
    int t = poly_binop_recv_temp(c, recv, argv[0], b, &se);
    buf_printf(b, "sp_poly_bitop(_t%d, ", t);
    emit_boxed(c, argv[0], b);
    buf_printf(b, ", %d)%s", bop, se ? "; })" : "");
    c->args_in_call = recv;
    return 1;
  }
  /* poly.difference / union / intersection: the named forms of - | & over
     one or more arrays, folded left to right, for an Array receiver only --
     another kind's - or | is no set operation, and it has no such method */
  if (recv >= 0 && argc >= 1 && is_named_set_operator(name) &&
      repr_of(c, recv).kind == RK_BOXED && !user_defines_or_reads(c, name)) {
    int splat = nt_argv_has_splat(nt, argv, argc);
    if (splat && setop_splat_args(c, argv, argc)) {
      int t = ++g_tmp, tr = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_boxed(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);"
                    " if (!(_t%d.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_t%d.cls_id)))"
                    " sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d));"
                    " sp_PolyArray *_t%d = sp_poly_to_poly_array(_t%d); SP_GC_ROOT(_t%d); ",
                 t, t, t, name, t, tr, t, tr);
      emit_setop_splat_fold(c, argv, argc, name, tr, b);
      buf_printf(b, " sp_box_poly_array(_t%d); })", tr);
      return 1;
    }
    if (!splat) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_boxed(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);"
                    " if (!(_t%d.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_t%d.cls_id)))"
                    " sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d));", t, t, t, name, t);
      for (int a = 0; a < argc; a++) {
        if (sp_streq(name, "difference")) buf_printf(b, " _t%d = sp_poly_sub(_t%d, ", t, t);
        else buf_printf(b, " _t%d = sp_poly_bitop(_t%d, ", t, t);
        emit_boxed(c, argv[a], b);
        if (sp_streq(name, "difference")) buf_puts(b, ");");
        else buf_printf(b, ", %d);", sp_streq(name, "union") ? 1 : 0);
      }
      buf_printf(b, " _t%d; })", t);
      return 1;
    }
  }
  /* `poly >> n`: through sp_poly_shr, which keeps a bignum receiver in bignum
     space. Truncating to int64 here made a positive value past 2^63 negative,
     turning the shift arithmetic and diverging a masked xorshift from CRuby for
     good (#3371). sp_poly_shr also carries the Proc#>> composition arm. */
  if (recv >= 0 && argc == 1 && sp_streq(name, ">>") && repr_of(c, recv).kind == RK_BOXED) {
    /* as the bit-operator arm above: a Bignum, Proc or user-object receiver
       is held by nothing else while the argument runs */
    int se = 0;
    int t = poly_binop_recv_temp(c, recv, argv[0], b, &se);
    buf_printf(b, "sp_poly_shr(_t%d, ", t);
    emit_boxed(c, argv[0], b);
    buf_puts(b, se ? "); })" : ")");
    c->args_in_call = recv;
    return 1;
  }

  /* `arr << x` / push / append in value position: mutate, then yield the array
     (statement position is handled earlier by emit_array_mutate_stmt). */
  if (recv >= 0 && emit_array_splat_mutator(c, id, b)) return 1;
  if (recv >= 0 && is_push_alias(name) &&
      argc >= 1 && ty_is_array(repr_of(c, recv).as_ty)) {
    Repr arr = repr_of(c, recv);
    TyKind art = arr.as_ty;
    /* A narrowed pointer array (int-array-array): push the element pointer
       (an sp_IntArray*) directly into the sp_PtrArray, no boxing. */
    if (ty_is_ptr_array(arr.as_ty)) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_PtrArray *_t%d = ", t);
      if (push_recv_in_slot(c, recv, argc, argv, art)) { emit_expr(c, recv, b); buf_puts(b, "; "); }
      else emit_recv_rooted(c, recv, t, "SP_GC_ROOT", b);
      for (int a = 0; a < argc; a++) {
        buf_printf(b, "sp_PtrArray_push(_t%d, ", t); emit_expr(c, argv[a], b); buf_puts(b, "); ");
      }
      buf_printf(b, "_t%d; })", t);
      return 1;
    }
    /* Lift: when a typed-array literal is pushed a heterogeneous element,
       rebuild the receiver as a PolyArray rather than emitting a type mismatch. */
    int needs_lift = 0;
    if (arr.elem != TY_POLY && array_kind(art)) {
      TyKind elem_t = arr.elem;
      NodeKind rk = nt_kind(nt, recv);
      if (rk == NK_ArrayNode) {
        for (int a = 0; a < argc; a++) {
          Repr ar = repr_of(c, argv[a]);
          TyKind at = ar.as_ty;
          if (!ar.untyped && at != elem_t) { needs_lift = 1; break; }
        }
      }
    }
    /* ...and so is one a builtin call just built, which no variable holds
       to widen (an_push_widens_fresh): its elements, boxed, then the push */
    if (!needs_lift && an_push_widens_fresh(c, id)) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", t);
      emit_fresh_lift_recv(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); ", t);
      /* every argument runs before a frozen receiver raises, as in CRuby */
      int vbase = g_tmp + 1; g_tmp += argc;
      for (int a = 0; a < argc; a++) {
        buf_printf(b, "sp_RbVal _t%d = ", vbase + a); emit_boxed(c, argv[a], b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", vbase + a);
      }
      for (int a = 0; a < argc; a++) buf_printf(b, "sp_PolyArray_push(_t%d, _t%d); ", t, vbase + a);
      buf_printf(b, "_t%d; })", t);
      return 1;
    }
    if (needs_lift) {
      int en = 0;
      const int *els = nt_arr(nt, recv, "elements", &en);
      int t = ++g_tmp;
      buf_puts(b, "({ ");
      /* the fresh array is held by nothing else while the boxed elements
         and arguments run, so it is rooted, as the array-literal emitters
         root theirs */
      buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", t, t);
      for (int j = 0; j < en; j++) {
        /* a splat element (`[*a]`) spreads its items, as the literal would */
        if (emit_poly_concat_splat(c, t, els[j], b)) { buf_puts(b, " "); continue; }
        Buf el; memset(&el, 0, sizeof el);
        emit_boxed(c, els[j], &el);
        buf_printf(b, "sp_PolyArray_push(_t%d, %s); ", t, el.p ? el.p : "sp_box_nil()");
        free(el.p);
      }
      for (int a = 0; a < argc; a++) {
        buf_printf(b, "sp_PolyArray_push(_t%d, ", t);
        emit_boxed(c, argv[a], b);
        buf_puts(b, "); ");
      }
      buf_printf(b, "_t%d; })", t);
      return 1;
    }
    const char *k = (art == TY_POLY_ARRAY) ? "Poly" : array_kind(art);
    int t = ++g_tmp;
    buf_puts(b, "({ ");
    emit_ctype(c, art, b); buf_printf(b, " _t%d = ", t);
    /* The receiver sits in `_tN`, which is not a root, while the arguments
       run. A fresh array from a call is held by nothing else; a local or an
       ivar is held by its slot, but only until an argument overwrites the
       slot, which an argument that writes a variable or runs user code can
       do. So `_tN` is rooted, as the hoisting arms in codegen_call_recv.c
       root theirs, unless the receiver is a slot read and every argument
       keeps the slot (push_recv_in_slot above). The pointer-array form
       above follows the same rule. */
    if (push_recv_in_slot(c, recv, argc, argv, art)) { emit_expr(c, recv, b); buf_puts(b, "; "); }
    else emit_recv_rooted(c, recv, t, "SP_GC_ROOT", b);
    TyKind elem = ty_array_elem(art);
    for (int a = 0; a < argc; a++) {
      buf_printf(b, "sp_%sArray_push%s(_t%d, ", k, nil_store_sfx(c, k, argv[a]), t);
      if (art == TY_POLY_ARRAY) emit_boxed(c, argv[a], b);
      /* a poly value into a String, Integer or Float array: its nil is the
         slot's nil, and a value of another kind is refused, as the statement
         form takes them (sp_poly_elem_*). The sp_poly_to_* conversions read
         a nil as 0, 0.0 or "", and turned 1 into "1" and 2.5 into 2. */
      else if (repr_of(c, argv[a]).kind == RK_BOXED && elem == TY_STRING) {
        buf_puts(b, "sp_poly_elem_s("); emit_expr(c, argv[a], b); buf_puts(b, ")");
      }
      else if (repr_of(c, argv[a]).kind == RK_BOXED && elem == TY_INT) {
        buf_puts(b, "sp_poly_elem_i("); emit_expr(c, argv[a], b); buf_puts(b, ")");
      }
      else if (repr_of(c, argv[a]).kind == RK_BOXED && elem == TY_FLOAT) {
        buf_puts(b, "sp_poly_elem_f("); emit_expr(c, argv[a], b); buf_puts(b, ")");
      }
      else if (repr_of(c, argv[a]).untyped) {
        /* the _nilable store of an unresolved value takes an oint */
        if (oint_kind(elem)) buf_printf(b, "%s(", oint_of(elem));
        emit_unresolved_coerced(c, argv[a], elem, b);
        if (oint_kind(elem)) buf_puts(b, ")");
      }
      /* an Array, a Hash or an object into an Integer, Float or String
         array is refused at run time, as the statement form refuses it
         (the plain store here unwraps the oint the refusal never answers) */
      else if ((elem == TY_INT || elem == TY_FLOAT || elem == TY_STRING) &&
               container_or_object_operand(c, argv[a])) {
        if (oint_kind(elem)) buf_printf(b, "%s(", oint_arg(elem));
        buf_puts(b, elem == TY_INT ? "sp_poly_elem_i(" : elem == TY_FLOAT ? "sp_poly_elem_f(" : "sp_poly_elem_s(");
        emit_boxed(c, argv[a], b); buf_puts(b, ")");
        if (oint_kind(elem)) buf_puts(b, ")");
      }
      /* an Integer or Float value that can be nil stores with it; any other
         value keeps the push's own coercion (and its refusal wording) */
      else if (oint_kind(elem) && nil_store_sfx(c, k, argv[a])[0]) emit_elem_store_value(c, k, argv[a], b);
      else emit_coerce(c, argv[a], elem, CO_HOLD, "an Array push", b);
      buf_puts(b, "); ");
    }
    /* a nil mixed Array took none of the pushes: it raises, under the name
       as written, once every argument has run */
    if (art == TY_POLY_ARRAY && !recv_never_nil(c, recv))
      buf_printf(b, "if (SP_UNLIKELY(!_t%d)) sp_nil_recv(\"%s\"); ", t, name);
    buf_printf(b, "_t%d; })", t);
    return 1;
  }
  return 0;
}

/* an element store in expression position, answering the stored value: Fiber[:k] = v, and h[k] = v / a[i] = v */
int emit_call_store_value_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt) {
  /* Fiber[:k] = v (expression form) */
  if (sp_streq(name, "[]=") && argc == 2 && recv >= 0) {
    if (fiber_storage_recv(nt, recv)) {
      Repr fvtr = repr_of(c, argv[1]);
      TyKind fvt = fvtr.as_ty;
      /* Fiber storage is poly-valued. A nil/void/untyped value has no scalar
         C slot -- carry it boxed (`void _t = nil` is otherwise a type error). */
      int fval_poly = (fvtr.kind == RK_BOXED || fvt == TY_UNKNOWN || fvt == TY_NIL || fvt == TY_VOID);
      int tf = ++g_tmp;
      buf_puts(b, "({ ");
      emit_ctype(c, fval_poly ? TY_POLY : fvt, b);
      buf_printf(b, " _t%d = ", tf);
      if (fval_poly) emit_boxed(c, argv[1], b);
      else emit_expr(c, argv[1], b);
      buf_puts(b, "; sp_Fiber_storage_set(sp_fiber_current, ");
      emit_fiber_storage_key(c, argv[0], b);
      buf_puts(b, ", ");
      if (!fval_poly) {
        char tfs[32]; snprintf(tfs, sizeof tfs, "_t%d", tf);
        emit_boxed_text(c, fvt, tfs, b);
      }
      else buf_printf(b, "_t%d", tf);
      buf_printf(b, "); _t%d; })", tf);
      return 1;
    }
  }

  /* `[]=` in expression position: mutate and return the assigned value.
     Ruby's `(h[k] = v)` and `(a[i] = v)` evaluate to v. */
  if (sp_streq(name, "[]=") && argc == 2 && recv >= 0) {
    Repr vr = repr_of(c, argv[1]);
    TyKind vt = vr.as_ty;
    if (ty_is_hash(rt)) {
      const char *hn = ty_hash_cname(rt);
      if (hn) {
        int tv = ++g_tmp;
        int is_poly_hash = (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH);
        TyKind hvt = ty_hash_val(rt);
        /* A poly value into a typed-value hash (e.g. a String? guarded non-nil
           stored into a Hash[String, String]): unbox to the value type. */
        int unbox_poly_val = (!is_poly_hash && vr.kind == RK_BOXED &&
                              (hvt == TY_STRING || hvt == TY_INT || hvt == TY_FLOAT));
        /* An UNRESOLVED rhs (a NameError/NoMethodError raise token, emitted
           as a diverging sp_RbVal expression) into a typed-value hash:
           declare the temp with the slot's type and coerce the token, so the
           set argument and the expression's own value typecheck. The raise
           fires before either is read (#3256). */
        int coerce_unknown_val = (!is_poly_hash && vt == TY_UNKNOWN &&
                                  (hvt == TY_STRING || hvt == TY_INT || hvt == TY_FLOAT));
        /* a value that can be nil into an Integer-valued typed hash: held as
           its oint and stored with its nil (_oset, DESIGN.md D3b-ii); the
           expression answers it in the form node_is_oint gives the store */
        int voset = !is_poly_hash && hvt == TY_INT && !unbox_poly_val &&
                    (nt_kind(c->nt, argv[1]) == NK_NilNode || node_has_oint_form(c, argv[1]));
        /* ...and a boxed one, which may hold nil at run time: unboxed with its
           nil, and the expression answers it boxed again */
        int vboxo = unbox_poly_val && hvt == TY_INT;
        /* ...and one that can be nil into a Hash of boxed values: held as
           its oint, boxed with its nil (a Float parameter that holds one) */
        TyKind vpt = vt == TY_FLOAT || vt == TY_INT ? vt : TY_UNKNOWN;
        int vpolyo = is_poly_hash && vpt != TY_UNKNOWN && node_has_oint_form(c, argv[1]);
        buf_puts(b, "({ ");
        /* A receiver or a key that can allocate goes into a temp ahead of the
           value, in Ruby's order; the receiver is rooted when the key or the
           value can drop the hash, and the key when it can allocate, as the
           statement form roots them. */
        int tr = -1, tk = -1;
        if (subtree_may_allocate(c->nt, recv) || subtree_may_allocate(c->nt, argv[0])) {
          TyKind kt = ty_hash_key(rt);
          tr = ++g_tmp; tk = ++g_tmp;
          buf_printf(b, "%s _t%d = ", c_type_name(rt), tr); emit_expr(c, recv, b); buf_puts(b, "; ");
          if (subtree_may_allocate(c->nt, recv) || subtree_has_side_effect(c, argv[0]) || subtree_has_side_effect(c, argv[1])) { emit_gc_root_tmp(c, rt, tr, b); buf_puts(b, " "); }
          buf_printf(b, "%s _t%d = ", c_type_name(kt), tk); emit_hash_store_key(c, argv[0], rt, b); buf_puts(b, "; ");
          if (operand_may_allocate(c, argv[0]) && needs_root(kt)) { emit_gc_root_tmp(c, kt, tk, b); buf_puts(b, " "); }
        }
        /* For poly hashes with scalar values, store the scalar and box it for the hash call.
           A nil/void rhs (`return @cache[k] = nil`) has no C storage type --
           emit_ctype would print `void` -- so hold it boxed. */
        TyKind vt_eff = (vt == TY_NIL || vt == TY_VOID) ? TY_POLY : vt;
        TyKind decl_type = unbox_poly_val ? hvt
                         : coerce_unknown_val ? hvt
                         : (is_poly_hash && vt_eff != TY_UNKNOWN && vt_eff != TY_POLY) ? vt_eff
                         : (vt_eff != TY_UNKNOWN ? vt_eff : TY_POLY);
        /* a String stored as the shared handle (`hh[k] = +"d"` in a Hash's
           default block, whose value is what the read answers): boxed once,
           so the store and the expression's value are the one handle */
        if (is_poly_hash && repr_of(c, argv[1]).handle) decl_type = TY_POLY;
        if (vpolyo) buf_puts(b, oint_ctype(vpt));
        else if (voset || vboxo) buf_puts(b, "sp_oint"); else emit_ctype(c, decl_type, b);
        buf_printf(b, " _t%d = ", tv);
        /* When the slot is poly but the rhs has no type yet (e.g. `{}`),
           emit a boxed value so the sp_RbVal temp initialises correctly. */
        if (vpolyo) emit_oint_expr(c, argv[1], vpt, b);
        else if (voset) emit_oint_expr(c, argv[1], TY_INT, b);
        else if (vboxo) { buf_puts(b, "sp_poly_hval_oi("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
        else if (unbox_poly_val) {
          const char *fn = hvt == TY_STRING ? "sp_poly_hval_s" : hvt == TY_INT ? "sp_poly_hval_i" : "sp_poly_hval_f";
          buf_printf(b, "%s(", fn); emit_expr(c, argv[1], b); buf_puts(b, ")");
        }
        else if (coerce_unknown_val) emit_unresolved_coerced(c, argv[1], hvt, b);
        else if (decl_type == TY_POLY) emit_boxed(c, argv[1], b);
        else emit_expr_face(c, argv[1], decl_type, b);
        /* a key that is a shared String slot's read is a fresh copy, read
           in the set after the value: the value is held across it */
        if (tk < 0 && strbuf_read_copies(c, argv[0]) && operand_may_allocate(c, argv[1])) {
          buf_puts(b, "; "); emit_gc_root_tmp(c, decl_type, tv, b);
        }
        buf_puts(b, "; if (sp_gc_is_frozen("); emit_node_or_tmp(c, recv, tr, b);
        buf_puts(b, ")) sp_raise_frozen_hash_at("); emit_node_or_tmp(c, recv, tr, b); buf_printf(b, ", %s); ", hash_box_cls(rt));
        buf_printf(b, "sp_%sHash_%s(", hn, voset || vboxo ? "oset" : "set"); emit_node_or_tmp(c, recv, tr, b); buf_puts(b, ", ");
        if (tk >= 0) buf_printf(b, "_t%d", tk);
        else emit_hash_store_key(c, argv[0], rt, b);  /* unbox a poly key to the hash's key type */
        buf_puts(b, ", ");
        char tvn[32]; snprintf(tvn, sizeof tvn, "_t%d", tv);
        if (voset || vboxo) {
          buf_printf(b, "_t%d); ", tv);
          if (vboxo) buf_printf(b, "sp_box_oint(_t%d); })", tv);
          else if (node_is_oint(c, id)) buf_printf(b, "_t%d; })", tv);
          else buf_printf(b, "sp_oint_arg(_t%d); })", tv);
          return 1;
        }
        if (vpolyo) {
          buf_printf(b, "%s(_t%d)); %s(_t%d); })", oint_box(vpt), tv, oint_box(vpt), tv);
          return 1;
        }
        if (is_poly_hash && decl_type != TY_POLY) {
          emit_boxed_text(c, decl_type, tvn, b);
        }
        else if (!is_poly_hash) emit_coerce_text(c, argv[1], decl_type, hvt, CO_HOLD, tvn, "a Hash element store", b);
        else buf_printf(b, "_t%d", tv);
        /* For poly-hash receivers the expression returns the boxed value
           (sp_RbVal); for typed-hash receivers return the raw typed value. */
        if (is_poly_hash) {
          buf_puts(b, "); ");
          if (decl_type == TY_POLY) buf_printf(b, "_t%d; })", tv);
          else { Buf _bx; memset(&_bx, 0, sizeof _bx); emit_boxed_text(c, decl_type, tvn, &_bx); buf_printf(b, "%s; })", _bx.p ? _bx.p : tvn); free(_bx.p); }
        }
        else if (unbox_poly_val) {
          /* value was poly (the `[]=` expression's value type); box the
             unboxed temp back so the expression result stays poly. */
          buf_puts(b, "); "); emit_boxed_text(c, hvt, tvn, b); buf_puts(b, "; })");
        }
        /* (one representation: a value held as the handle answers the
           String the expression is typed as) */
        else if (decl_type == TY_STRBUF && repr_of(c, id).as_ty == TY_STRING) {
          buf_printf(b, "); SP_STR_B(_t%d); })", tv);
        }
        else {
          buf_printf(b, "); _t%d; })", tv);
        }
        return 1;
      }
    }
    if (ty_is_array(rt) || rt == TY_POLY_ARRAY) {
      const char *k = rt == TY_POLY_ARRAY ? "Poly" : array_kind(rt);
      if (k) {
        int tv = ++g_tmp;
        const char *vsfx = nil_store_sfx(c, k, argv[1]);
        /* a number that can be nil travels as its oint into the _nilable store */
        int voint = rt != TY_POLY_ARRAY && oint_kind(vt) && vsfx[0];
        buf_puts(b, "({ ");
        if (voint) buf_puts(b, oint_ctype(vt)); else emit_ctype(c, vr.kind == RK_BOXED || vt == TY_UNKNOWN ? TY_POLY : vt, b);
        buf_printf(b, " _t%d = ", tv);
        if (rt == TY_POLY_ARRAY && vr.kind != RK_BOXED) emit_boxed(c, argv[1], b);
        else if (voint) emit_oint_expr(c, argv[1], vt, b);
        else emit_expr(c, argv[1], b);
        buf_printf(b, "; sp_%sArray_set%s(", k, vsfx); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b); buf_printf(b, ", _t%d); ", tv);
        /* the store answers the value: with its nil where the consumer takes the oint */
        if (voint) { oint_open(c, id, vt, b); buf_printf(b, "_t%d", tv); oint_close(c, id, b); buf_puts(b, "; })"); }
        else buf_printf(b, "_t%d; })", tv);
        return 1;
      }
    }
  }
  return 0;
}
/* Is the untyped receiver `recv` an empty Array: the `[]` literal, or a
   local every write of which is one? Only then does an untyped `sum` have
   the empty answer; an untyped call's value (`big.times` under
   --int-overflow=promote) is no empty literal. */
static int untyped_empty_array(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  recv = nt_paren_single(c->nt, recv);
  if (recv < 0) return 0;
  if (nt_kind(nt, recv) == NK_ArrayNode) {
    int en = 0;
    nt_arr(nt, recv, "elements", &en);
    return en == 0;
  }
  if (nt_kind(nt, recv) != NK_LocalVariableReadNode) return 0;
  const char *ln = nt_str(nt, recv, "name");
  Scope *sc = ln ? comp_scope_of(c, recv) : NULL;
  return sc && local_all_writes_empty_array(c, sc, ln);
}


/* shuffle / shuffle! / sample with `random: g`: the draws come from g, where
   the keyword was dropped (sample) or the call refused (shuffle). A typed
   Array, or a boxed receiver, which is checked to be an Array at run time;
   sample also takes a count before the keyword. */
int emit_array_random_kw(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt) {
  int sample = sp_streq(name, "sample");
  if (recv < 0 || !(ty_is_array(rt) || rt == TY_POLY) || nt_ref(nt, id, "block") >= 0) return 0;
  if (!(argc == 1 || (argc == 2 && sample))) return 0;
  if (!(sample || sp_streq(name, "shuffle") || sp_streq(name, "shuffle!"))) return 0;
  int kw = argv[argc - 1];
  if (nt_kind(nt, kw) != NK_KeywordHashNode) return 0;
  if (rt == TY_POLY && poly_name_user_claimed(c, name, argc)) return 0;
  int g = struct_kwarg_value(c, kw, "random");
  int nel = 0; nt_arr(nt, kw, "elements", &nel);
  Repr gtr = repr_of(c, g >= 0 ? g : -1);
  TyKind gt = g >= 0 ? gtr.as_ty : TY_UNKNOWN;
  if (g < 0 || nel != 1 || (gt != TY_RANDOM && gt != TY_POLY)) return 0;
  /* a boxed receiver answers in the slot the call was given */
  TyKind et = rt == TY_POLY ? repr_of(c, id).as_ty : rt;
  int ta = ++g_tmp, tg = ++g_tmp;
  buf_puts(b, "({ "); ta = hold_operand(c, recv, TY_POLY, 1, ta, 1, " ", b);
  if (rt == TY_POLY) buf_printf(b, "sp_poly_ary_chk(_t%d, \"%s\", 0); ", ta, name);
  int tn = -1;
  if (argc == 2) { tn = ++g_tmp; buf_printf(b, "sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b); buf_puts(b, "; "); }
  buf_printf(b, "sp_Random *_t%d = %s", tg, gtr.kind == RK_BOXED ? "sp_poly_random_recv(" : ""); emit_expr(c, g, b);
  buf_puts(b, gtr.kind == RK_BOXED ? ", \"rand\"); " : "; ");
  char call[96];
  if (sp_streq(name, "shuffle!")) {
    buf_printf(b, "sp_poly_shuffle_bang_r(_t%d, _t%d); ", ta, tg);
    snprintf(call, sizeof call, "_t%d", ta);
    emit_unbox_text(c, et, call, b);
  }
  else if (sp_streq(name, "shuffle")) {
    /* the copy keeps the receiver's array kind; a boxed call's slot is a
       poly array */
    if (rt == TY_POLY) snprintf(call, sizeof call, "sp_box_poly_array(sp_poly_to_a_arr(sp_poly_shuffle_r(_t%d, _t%d)))", ta, tg);
    else snprintf(call, sizeof call, "sp_poly_shuffle_r(_t%d, _t%d)", ta, tg);
    emit_unbox_text(c, et, call, b);
  }
  else {
    if (tn >= 0) snprintf(call, sizeof call, "sp_poly_sample_n_r(_t%d, _t%d, _t%d)", ta, tn, tg);
    else snprintf(call, sizeof call, "sp_poly_sample_r(_t%d, _t%d)", ta, tg);
    TyKind st = rt == TY_POLY ? et : repr_of(c, id).as_ty;
    if (st == TY_POLY) buf_puts(b, call);
    /* an element that can be nil (an empty receiver's sample) answers its
       oint where the call is one */
    else if (oint_kind(st) && node_is_oint(c, id)) buf_printf(b, "%s(%s)", oint_unbox(st), call);
    else emit_unbox_text(c, st, call, b);
  }
  buf_puts(b, "; })");
  return 1;
}

/* `arr[i] = v` whose boxed index may be a Range (value_may_be_range), and
   whose value both the splice and the element store
   take: a Range splices, anything else stores. Read as an Integer, a Range
   stored at index 0. */
int aset_range_held(Compiler *c, TyKind rt, const int *argv) {
  if (repr_of(c, argv[0]).kind != RK_BOXED || !value_may_be_range(c, argv[0])) return 0;
  if (rt == TY_POLY_ARRAY) return 1;
  const char *k = array_kind(rt);
  TyKind et = ty_array_elem(rt);
  if (!k || (et != TY_INT && et != TY_FLOAT && et != TY_STRING)) return 0;
  /* a boxed value (promote's Integers) takes the same two arms: the
     splice unboxes it as an element, the store as it stores one */
  if (repr_of(c, argv[1]).kind == RK_BOXED) return 1;
  /* an Integer or Float that can be nil splices and stores with its nil */
  TyKind vt = comp_ntype(c, argv[1]);
  if (vt == TY_STRBUF) vt = TY_STRING;   /* (one-String mode: a String held as its handle) */
  return vt == et || (vt == TY_NIL && et != TY_STRING);
}
void emit_aset_range_held(Compiler *c, int id, int recv, TyKind rt, const int *argv, Buf *b) {
  buf_puts(b, "({ ");
  int tr = hold_operand(c, recv, rt, 0, ++g_tmp, 1, " ", b);
  int rbind = view_bind(recv, "_t%d", tr);
  int ti = ++g_tmp;
  buf_printf(b, "sp_RbVal _t%d = ", ti); emit_expr(c, argv[0], b);
  /* A boxed value (promote's Integers): held once, each arm takes it boxed
     and is a statement, and the store answers it as the call is read --
     the two arms' own answers (the splice's sp_RbVal, the store's oint)
     do not meet in one conditional */
  if (rt != TY_POLY_ARRAY && repr_of(c, argv[1]).kind == RK_BOXED) {
    int tv = ++g_tmp;
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_RbVal _t%d = ", ti, tv); emit_boxed(c, argv[1], b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); if ((_t%d = sp_poly_frange_ix(_t%d)).tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_RANGE) { (void)", tv, ti, ti, ti);
    int vbind = view_bind(argv[1], "_t%d", tv);
    int vv = view_push(c, argv[1], TY_POLY);
    int bind = view_bind(argv[0], "(*(sp_Range *)_t%d.v.p)", ti);
    int v = view_push(c, argv[0], TY_RANGE);
    emit_array_splice(c, id, recv, rt, -1, -1, argv[0], argv[1], b);
    view_pop(c, v);
    view_unbind(bind);
    buf_puts(b, "; }\nelse { (void)");
    bind = view_bind(argv[0], "sp_poly_arg_int_chk(_t%d)", ti);
    v = view_push(c, argv[0], TY_INT);
    emit_call_array_arms(c, id, b, c->nt, "[]=", recv, 2, argv, rt);
    view_pop(c, v);
    view_unbind(bind);
    view_pop(c, vv);
    view_unbind(vbind);
    view_unbind(rbind);
    buf_puts(b, "; } ");
    Repr ir = repr_of(c, id);
    TyKind et = ty_array_elem(rt), it = ir.as_ty;
    char tn[32]; snprintf(tn, sizeof tn, "_t%d", tv);
    if (oint_kind(et) && node_is_oint(c, id)) buf_printf(b, "%s(%s)", oint_unbox(et), tn);
    else if (ir.kind == RK_BOXED || it == TY_UNKNOWN) buf_puts(b, tn);
    else emit_unbox_text(c, it, tn, b);
    buf_puts(b, "; })");
    return;
  }
  buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ((_t%d = sp_poly_frange_ix(_t%d)).tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_RANGE) ? ", ti, ti, ti, ti);
  /* (one-String mode: the splice answers the element, the handle, and the
     store the String's bytes; both arms answer the bytes) */
  int one_s = ty_array_elem(rt) == TY_STRING;
  if (one_s) buf_puts(b, "SP_STR_B(");
  int bind = view_bind(argv[0], "(*(sp_Range *)_t%d.v.p)", ti);
  int v = view_push(c, argv[0], TY_RANGE);
  emit_array_splice(c, id, recv, rt, -1, -1, argv[0], argv[1], b);
  view_pop(c, v);
  view_unbind(bind);
  buf_puts(b, one_s ? ") : SP_STR_B(" : " : ");
  bind = view_bind(argv[0], "sp_poly_arg_int_chk(_t%d)", ti);
  v = view_push(c, argv[0], TY_INT);
  emit_call_array_arms(c, id, b, c->nt, "[]=", recv, 2, argv, rt);
  view_pop(c, v);
  view_unbind(bind);
  view_unbind(rbind);
  buf_puts(b, one_s ? "); })" : "; })");
}

/* `arr.slice!(i)` whose boxed index may be a Range (value_may_be_range),
   typed boxed (infer_array_call): a Range removes the subarray, anything
   else the element. Read as an Integer, a Range raised TypeError. */
static int slice_bang_range_held(Compiler *c, int id, TyKind rt, int argc, const int *argv) {
  if (argc != 1 || repr_of(c, argv[0]).kind != RK_BOXED || repr_of(c, id).kind != RK_BOXED) return 0;
  TyKind et = ty_array_elem(rt);
  if (rt != TY_POLY_ARRAY && et != TY_INT && et != TY_FLOAT && et != TY_STRING) return 0;
  return value_may_be_range(c, argv[0]);
}
static void emit_slice_bang_range_held(Compiler *c, int id, int recv, TyKind rt, const int *argv, Buf *b) {
  buf_puts(b, "({ ");
  int tr = hold_operand(c, recv, rt, 0, ++g_tmp, 1, " ", b);
  int rbind = view_bind(recv, "_t%d", tr);
  int ti = ++g_tmp;
  buf_printf(b, "sp_RbVal _t%d = ", ti); emit_expr(c, argv[0], b);
  buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ((_t%d = sp_poly_frange_ix(_t%d)).tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_RANGE) ? ", ti, ti, ti, ti);
  Buf ab; memset(&ab, 0, sizeof ab);
  int bind = view_bind(argv[0], "(*(sp_Range *)_t%d.v.p)", ti);
  int v = view_push(c, argv[0], TY_RANGE);
  emit_call_array_arms(c, id, &ab, c->nt, "slice!", recv, 1, argv, rt);
  view_pop(c, v);
  view_unbind(bind);
  emit_boxed_text(c, rt, ab.p ? ab.p : "NULL", b);
  free(ab.p); memset(&ab, 0, sizeof ab);
  buf_puts(b, " : ");
  bind = view_bind(argv[0], "sp_poly_arg_int_chk(_t%d)", ti);
  v = view_push(c, argv[0], TY_INT);
  /* the row that guards no index kind: the Integer index's row states the
     element, and this branch takes the element's nil beside it (oint_box) */
  if (!emit_builtin_op_unguarded(c, id, recv, rt, "slice!", &ab))
    emit_call_array_arms(c, id, &ab, c->nt, "slice!", recv, 1, argv, rt);
  view_pop(c, v);
  view_unbind(bind);
  view_unbind(rbind);
  TyKind et = ty_array_elem(rt);
  const char *e = ab.p ? ab.p : "";
  if (rt == TY_POLY_ARRAY) buf_puts(b, e);
  else if (et == TY_STRING) buf_printf(b, "SP_STR_BOX(%s)", e);
  else buf_printf(b, "%s(%s)", oint_box(et), e);
  free(ab.p);
  buf_puts(b, "; })");
}

/* An Array's or a String's index that is a Float Range (`a[r]`, `a[r] = v`,
   `s.slice!(r)`, `a.values_at(r, 1)`) converts each end with to_int, as
   CRuby does: the call reads it as the Integer Range sp_frange_ix makes of
   it, viewed as one. A program's own method of the name takes the Range as
   it is. A conversion that runs code is hoisted, so the receiver and the
   earlier operands are held before it, and a literal's ends are held for
   the arms that read them; under `&.` it waits for the nil
   guard's re-entry. Answers the view mark to unwind to, or -1 for no such
   call. */
int frange_index_view(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  const char *nm = nt_str(nt, id, "name");
  if (recv < 0 || !nm || sn_guard_pending(c, id)) return -1;
  TyKind rt = comp_ntype(c, recv);
  int vals = sp_streq(nm, "values_at");
  int str = rt == TY_STRING || rt == TY_STRBUF;
  if (str ? !is_seq_index_call(nm) : !ty_is_array(rt) || !(vals || (is_seq_index_call(nm) && !sp_streq(nm, "byteslice"))))
    return -1;
  int args = nt_ref(nt, id, "arguments"), argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int n = vals ? argc : argc >= 1 ? 1 : 0, any = 0;
  for (int i = 0; i < n; i++) any |= comp_ntype(c, argv[i]) == TY_FLOAT_RANGE;
  if (!any) return -1;
  int ci = comp_core_class(c, str ? CORE_STRING : CORE_ARRAY);
  if (ci >= 0 && call_target(c, id, ci, nm, 0, NULL) >= 0) return -1;
  int mark = view_mark();
  int held = subtree_is_pure_read(c, recv), done = 0;
  for (int i = 0; i < n; i++) {
    if (comp_ntype(c, argv[i]) != TY_FLOAT_RANGE) continue;
    if (!subtree_is_pure_read(c, argv[i]) && g_pre) {
      if (!held) view_bind(recv, "_t%d", hold_operand_pre(c, recv, repr_of(c, recv).as_ty, 0, ++g_tmp, 1));
      held = 1;
      for (; done < i; done++)
        if (comp_ntype(c, argv[done]) != TY_FLOAT_RANGE &&
            (!subtree_is_pure_read(c, argv[done]) || read_rebound_by(c, argv[done], argv[i])))
          view_bind(argv[done], "_t%d", hold_operand_pre(c, argv[done], repr_of(c, argv[done]).as_ty, 0, ++g_tmp, 1));
      done = i + 1;
      /* (a literal in parentheses too: `(f(1.5)..f(2.5))` left its ends
         to the C argument order, which runs the last first under gcc) */
      int rn = nt_paren_single(nt, argv[i]);
      if (rn >= 0 && nt_kind(nt, rn) == NK_RangeNode)
        for (int e = 0; e < 2; e++) {
          int x = nt_ref(nt, rn, e ? "right" : "left");
          if (x >= 0 && !subtree_is_pure_read(c, x))
            view_bind(x, "_t%d", hold_operand_pre(c, x, repr_of(c, x).as_ty, 0, ++g_tmp, 1));
        }
    }
    Buf ab = expr_buf(c, argv[i]);
    const char *at = ab.p ? ab.p : "";
    if (subtree_is_pure_read(c, argv[i]) && strlen(at) < ARGOV_TEXT_LEN - 16) view_bind(argv[i], "sp_frange_ix(%s)", at);
    else if (g_pre) {
      int t = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Range _t%d = sp_frange_ix(%s);\n", t, at);
      view_bind(argv[i], "_t%d", t);
    }
    else { free(ab.p); view_unwind(mark); return -1; }
    free(ab.p);
    view_push(c, argv[i], TY_RANGE);
  }
  return mark;
}

/* an Array receiver: a store in expression position (a[i] = v, a[i, n] = src, a[range] = src),
   sum and the other methods of an empty literal, then the Array emitters (emit_array_call) */
int emit_call_array_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt) {
  if (emit_array_random_kw(c, id, b, nt, name, recv, argc, argv, rt)) return 1;
  /* `arr[i] = v` in expression position: do the store, evaluate to the rhs
     (Ruby []= returns the assigned value). The statement form is emitted
     elsewhere; this covers rvalue chains like `b = arr[i] = v`. */
  /* a[i, n] = src  --  slice assignment */
  /* arr[start, len] = rhs : a splice (remove `len` at `start`, insert rhs). */
  if (recv >= 0 && ty_is_array(rt) && sp_streq(name, "[]=") && argc == 3) {
    emit_array_splice(c, id, recv, rt, argv[0], argv[1], -1, argv[2], b);
    return 1;
  }
  if (recv >= 0 && ty_is_array(rt) && sp_streq(name, "[]=") && argc == 2 && aset_range_held(c, rt, argv)) {
    emit_aset_range_held(c, id, recv, rt, argv, b);
    return 1;
  }
  if (recv >= 0 && ty_is_array(rt) && sp_streq(name, "slice!") && slice_bang_range_held(c, id, rt, argc, argv)) {
    emit_slice_bang_range_held(c, id, recv, rt, argv, b);
    return 1;
  }
  if (recv >= 0 && ty_is_array(rt) && sp_streq(name, "[]=") && argc == 2) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    /* arr[range] = rhs : a splice over the range's (start, length). */
    if (k && repr_of(c, argv[0]).range == TY_INT) {
      emit_array_splice(c, id, recv, rt, -1, -1, argv[0], argv[1], b);
      return 1;
    }
    if (k) {
      int t = ++g_tmp, ti = ++g_tmp, tv = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
      buf_printf(b, "; sp_int _t%d = ", ti); emit_int_expr(c, argv[0], b); buf_puts(b, "; ");
      if (rt == TY_POLY_ARRAY) {
        buf_printf(b, "sp_RbVal _t%d = ", tv); emit_boxed(c, argv[1], b);
      }
      else {
        TyKind et = ty_array_elem(rt);
        int vboxed = repr_of(c, argv[1]).kind == RK_BOXED;
        /* a number that can be nil (a boxed one: sp_poly_elem_i / _f answer
           an oint; a nullable read, a bound oint temp) travels as its oint
           into the _nilable store; a plain one into the plain store */
        int voint = oint_kind(et) && nil_store_sfx(c, k, argv[1])[0];
        if (voint) buf_puts(b, oint_ctype(et)); else emit_ctype(c, et, b);
        buf_printf(b, " _t%d = ", tv);
        if (vboxed && et == TY_INT) { buf_puts(b, "sp_poly_elem_i("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
        else if (vboxed && et == TY_STRING) { buf_puts(b, "sp_poly_elem_s("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
        else if (vboxed && et == TY_FLOAT) { buf_puts(b, "sp_poly_elem_f("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
        else if (voint) emit_oint_expr(c, argv[1], et, b);
        else emit_coerce(c, argv[1], et, CO_HOLD, "an Array element store", b);
        buf_printf(b, "; sp_%sArray_set%s(_t%d, _t%d, _t%d); ", k, nil_store_sfx(c, k, argv[1]), t, ti, tv);
        /* the store answers the value: with its nil where the consumer takes the oint */
        if (voint) { oint_open(c, id, et, b); buf_printf(b, "_t%d", tv); oint_close(c, id, b); buf_puts(b, "; })"); }
        else buf_printf(b, "_t%d; })", tv);
        return 1;
      }
      buf_printf(b, "; sp_%sArray_set%s(_t%d, _t%d, _t%d); _t%d; })", k, nil_store_sfx(c, k, argv[1]), t, ti, tv, tv);
      return 1;
    }
  }

  /* array value methods */
  /* empty array literal [] has TY_UNKNOWN; sum returns init or 0. A local bound
     to [] that no push narrowed also stays TY_UNKNOWN, so `a = []; a.sum(0.0)`
     reaches here with a non-literal receiver -- still an empty array. Any
     other untyped receiver is not one: `big.times.sum` under
     --int-overflow=promote answered 0 here (untyped_empty_array). */
  if (recv >= 0 && rt == TY_UNKNOWN && sp_streq(name, "sum") && untyped_empty_array(c, recv)) {
    int call_boxed = repr_of(c, id).kind == RK_BOXED;
    if (argc == 1) {
      if (call_boxed) emit_boxed(c, argv[0], b);
      else emit_expr(c, argv[0], b);
    }
    else {
      if (call_boxed) buf_puts(b, "sp_box_int(0)");
      else buf_puts(b, "0");
    }
    return 1;
  }
  /* take_while/drop_while/each_index/set-ops on empty array literal [] (TY_UNKNOWN receiver) */
  if (recv >= 0 && rt == TY_UNKNOWN &&
      (sp_streq(name, "take_while") || sp_streq(name, "drop_while") || sp_streq(name, "each_index") ||
       sp_streq(name, "difference") || sp_streq(name, "-") || sp_streq(name, "&") || sp_streq(name, "|") ||
       sp_streq(name, "intersection") || sp_streq(name, "union") || sp_streq(name, "+") ||
       sp_streq(name, "zip") || sp_streq(name, "flatten") || sp_streq(name, "compact") ||
       sp_streq(name, "uniq") || sp_streq(name, "sort") || sp_streq(name, "reverse") ||
       sp_streq(name, "shuffle")) &&
      nt_kind(nt, recv) == NK_ArrayNode) {
    int en = 0; nt_arr(nt, recv, "elements", &en);
    if (en == 0) {
      if (sp_streq(name, "each_index")) {
        /* each_index on [] is a no-op; evaluate receiver for side-effects */
        emit_expr(c, recv, b);
      }
      else if (sp_streq(name, "take_while") || sp_streq(name, "drop_while")) {
        buf_puts(b, "sp_PolyArray_new()");
      }
      else {
        /* set/transform ops on [] receiver: call the runtime with NULL first arg */
        Repr akr = repr_of(c, argc > 0 ? argv[0] : -1);
        TyKind akt = akr.as_ty;
        const char *ek = ty_is_array(akt) ? ((akr.elem == TY_POLY) ? "Poly" : array_kind(akt)) : NULL;
        if (!ek) ek = "Poly";
        if (argc > 0 && !akr.untyped &&
            (sp_streq(name, "union") || sp_streq(name, "|") ||
             sp_streq(name, "difference") || sp_streq(name, "-") ||
             sp_streq(name, "intersection") || sp_streq(name, "&") ||
             sp_streq(name, "+") || sp_streq(name, "zip"))) {
          /* call the real function with NULL receiver (handles empty-self case) */
          const char *fn = (is_intersection_alias(name)) ? "intersect"
                         : (is_union_alias(name)) ? "union"
                         : (sp_streq(name, "+")) ? "concat"
                         : "difference";
          buf_printf(b, "sp_%sArray_%s(NULL, ", ek, fn); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else {
          buf_printf(b, "sp_%sArray_new()", ek);
        }
      }
      return 1;
    }
  }
  if (emit_or_take_back(c, id, b, emit_array_call)) return 1;
  return 0;
}

/* methods on an empty array literal or an argument-less Array.new, answered
   against an empty poly array. The receiver's type is unknown when it never
   settled; one whose use gave it a kind (a marked receiver, mark_empty_array_operands)
   answers the counts, empty?, frozen? and class the same way, since it
   is the same empty container whatever its inferred type. */
int emit_call_untyped_array_arms(Compiler *c, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, TyKind rt) {
  /* the common methods, answered directly against an empty (poly) array */
  if (recv >= 0 && (rt == TY_UNKNOWN || ty_is_array(rt))) {
    /* an argument-less `Array.new` is the same empty array, and carries the
       same unknown element type (#3613) */
    int empty_recv = node_is_empty_container(c, recv) && an_empty_container_kind(c, recv) == 1;
    /* a receiver typed for its use answers only what no type changes */
    int typed = rt != TY_UNKNOWN;
    {
      if (empty_recv) {
        if (is_count_alias(name) && argc == 0) { buf_puts(b, "0"); return 1; }
        if (sp_streq(name, "empty?") && argc == 0) { buf_puts(b, "1"); return 1; }
        if (sp_streq(name, "frozen?") && argc == 0) { buf_puts(b, "0"); return 1; }
        if (sp_streq(name, "class") && argc == 0) { buf_puts(b, "((sp_Class){(sp_int)-1, SPL(\"Array\")})"); return 1; }
        if (typed) return 0;
        /* first/last type poly (boxed nil, printable as nil); min/max/pop/
           shift answer the Integer slot's nil */
        if ((is_endpoint_query(name)) && argc == 0) { buf_puts(b, "sp_box_nil()"); return 1; }
        /* pop / shift are oint producers (node_is_oint); min / max of the
           empty literal are read plain here, where this arm has no node to
           ask -- their nil is the TypeError of a plain read */
        if ((sp_streq(name, "pop") || sp_streq(name, "shift")) && argc == 0) { buf_puts(b, "sp_oint_nil()"); return 1; }
        if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc == 0) { buf_puts(b, "sp_oint_arg(sp_oint_nil())"); return 1; }
        if (sp_streq(name, "sample") && argc == 0) { buf_puts(b, "sp_box_nil()"); return 1; }  /* #2322 */
        if ((is_text_conversion(name)) && argc == 0) { buf_puts(b, "\"[]\""); return 1; }
        if ((sp_streq(name, "join") || sp_streq(name, "pack")) && argc <= 1) { buf_puts(b, "(&(\"\\xff\")[1])"); return 1; }
        if ((sp_streq(name, "union")) && argc == 0) { buf_puts(b, "sp_IntArray_new()"); return 1; }
        if ((sp_streq(name, "flatten") || sp_streq(name, "compact") || sp_streq(name, "uniq") ||
             sp_streq(name, "sort") || sp_streq(name, "reverse") || sp_streq(name, "dup") ||
             sp_streq(name, "clone") || sp_streq(name, "to_a") || sp_streq(name, "to_ary") ||
             sp_streq(name, "deconstruct") || sp_streq(name, "entries")) && argc <= 1) {
          buf_puts(b, "sp_PolyArray_new()"); return 1;
        }
      }
    }
  }
  return 0;
}

/* The scalar case precedes the structural Array arms, as it did before
   joining the transpose row's emitter. */
int emit_scalar_array_transpose(Compiler *c, int id, int recv, TyKind rt,
                                const char *name, int argc, Buf *b) {
  if (rt != TY_INT_ARRAY && rt != TY_FLOAT_ARRAY && rt != TY_STR_ARRAY) return 0;
  const BuiltinOp *op = bop_find(BOP_ANY_ARRAY, name, argc, nt_ref(c->nt, id, "block") >= 0);
  if (!op || op->emit != BOPE_ARRAY_TRANSPOSE) return 0;
  BopCtx x = { id, recv, argc, rt, name, op, NULL, 0 };
  return emit_op_array_transpose(c, &x, b);
}

/* ---- Array subclass instances (#7449) ----
   A call Array answers on an Array subclass instance (comp_arysub_call) is
   Array's call: the receiver is bound to its Array -- the same pointer, since
   the instance starts with its Array -- and the call re-enters the emitters
   under the kind the inference pinned it to (infer_arysub_call), as a boxed
   receiver's face does (emit_face_arm). A call whose answer is its receiver
   answers the instance. */
typedef struct { int bound, vr, vf, vi, nv, views[16]; TyKind nat; int copy, copy_class; } ArysubView;

/* Bind node n, an Array subclass instance, to its Array -- the same pointer
   cast -- evaluating anything but a variable once, ahead of the call. */
static int arysub_bind(Compiler *c, int n, TyKind k) {
  int cid = ty_object_class(repr_of(c, n).as_ty);
  const char *at = arysub_array_ctype(c, cid);
  NodeKind nk = nt_kind(c->nt, n);
  Buf rb; memset(&rb, 0, sizeof rb);
  emit_expr(c, n, &rb);
  /* a String subclass instance's String: boxed as its handle, or a copy of
     its bytes (comp_arysub_call) */
  if (comp_ary_is_str(c, cid)) {
    int t = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_String *_t%d = (sp_String *)(%s); SP_GC_ROOT(_t%d);\n", t, rb.p ? rb.p : "NULL", t);
    free(rb.p);
    if (k == TY_POLY) {
      /* an lvalue: the poly String arms write a changed String back */
      int bx = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nullable_obj((void *)_t%d, SP_BUILTIN_STRBUF); SP_GC_ROOT_RBVAL(_t%d);\n", bx, t, bx);
      return view_bind(n, "_t%d", bx);
    }
    /* (one representation: the read face is the payload itself, which a
       String slot takes back to the instance; the copy is made here, or
       `m.to_s` answered m) */
    return view_bind(n, "sp_strbuf_read_own(_t%d)", t);
  }
  /* a variable is read through the cast where the call reads it; one whose
     cast is longer than a bound text holds (a long name: a fixed buffer cut
     the cast and bound what was left) goes through a temp, as any other
     receiver does, holding the same pointer */
  int slot;
  Buf cast; memset(&cast, 0, sizeof cast);
  buf_printf(&cast, "((%s *)(%s))", at, rb.p ? rb.p : "NULL");
  if ((nk == NK_LocalVariableReadNode || nk == NK_SelfNode) && cast.len < sizeof g_argov_text[0])
    slot = view_bind(n, "%s", cast.p);
  else {
    int t = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "%s *_t%d = (%s *)(%s); SP_GC_ROOT(_t%d);\n", at, t, at, rb.p ? rb.p : "NULL", t);
    slot = view_bind(n, "_t%d", t);
  }
  free(cast.p);
  free(rb.p);
  return slot;
}

/* Bind and retype the receiver of call `id` when the call is Array's on an
   Array subclass instance, and the arguments the call reads as Arrays
   (comp_arysub_args_viewed); 0 when there is neither. */
static int arysub_view_open(Compiler *c, int id, ArysubView *v) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN, k = TY_UNKNOWN;
  v->bound = -1; v->vr = v->vf = v->vi = -1; v->nv = 0; v->nat = TY_UNKNOWN; v->copy = 0; v->copy_class = 0;
  int str = recv >= 0 && ty_is_object(rt) && comp_ary_is_str(c, ty_object_class(rt));
  if (comp_arysub_call(c, id, rt, &k) && (array_new_copies(k) || ty_is_hash(k) || str)) {
    int hash = str ? 2 : ty_is_hash(k) ? 1 : 0;
    v->bound = arysub_bind(c, recv, k);
    v->vr = view_push(c, recv, k);
    v->vf = view_push_face(recv, k);
    /* a call that answers its receiver: the Array emitter's answer -- the
       Array itself, or boxed where a `!` method answers nil when it
       changed nothing (BOPF_SELF_OR_NIL) -- is turned back into the
       instance below */
    if (comp_arysub_self_result(c, id, hash)) {
      v->nat = (comp_arysub_answer(c, id, hash) & BOPF_SELF_OR_NIL) || k == TY_POLY ? TY_POLY : k;
      v->vi = view_push(c, id, v->nat);
    }
    /* a conversion answering its receiver only when the receiver's class
       is exactly Array (to_a, BOPF_SELF_EXACT) answers a new plain Array
       of the elements: the Array emitter's answer is the instance's own
       Array, copied below */
    else if (comp_arysub_answer(c, id, hash) & BOPF_SELF_EXACT) v->copy = 1;
    /* a Hash method answering a copy of the receiver (merge, compact:
       BOPF_COPY_CLASS) answers an instance of the receiver's class, with its
       instance variables: the Hash emitter's answer is put into a dup */
    else if (hash == 1 && (comp_arysub_answer(c, id, hash) & BOPF_COPY_CLASS)) v->copy_class = 1;
    rt = k;
  }
  int args = nt_ref(nt, id, "arguments"), an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  int want = 0;
  for (int i = 0; i < an; i++) want |= comp_ty_ary_root(c, repr_of(c, av[i]).as_ty) >= 0;
  if (want && comp_arysub_args_viewed(c, id, rt)) {
    for (int i = 0; i < an && v->nv < 16; i++) {
      TyKind at = repr_of(c, av[i]).as_ty;
      if (comp_ty_ary_root(c, at) < 0) continue;
      TyKind ak = comp_ary_kind(c, ty_object_class(at));
      if (ak == TY_STRBUF) ak = TY_STRING;
      int slot = arysub_bind(c, av[i], ak);
      if (v->bound < 0) v->bound = slot;
      v->views[v->nv++] = view_push(c, av[i], ak);
    }
  }
  return v->bound >= 0;
}
static void arysub_view_close(Compiler *c, ArysubView *v) {
  for (int i = v->nv - 1; i >= 0; i--) view_pop(c, v->views[i]);
  if (v->vi >= 0) view_pop(c, v->vi);
  if (v->vf >= 0) view_pop(c, v->vf);
  if (v->vr >= 0) view_pop(c, v->vr);
  view_unbind(v->bound);
}

int emit_arysub_call(Compiler *c, int id, Buf *b) {
  if (!c->has_arysub) return 0;
  int ka = comp_arysub_kernel_array(c, id);
  if (ka >= 0 && comp_ty_arysub_array(c, repr_of(c, ka).as_ty)) { emit_expr(c, ka, b); return 1; }
  int recv = nt_ref(c->nt, id, "receiver");
  TyKind rt = recv >= 0 ? repr_of(c, recv).as_ty : TY_UNKNOWN;
  /* the view below retypes the call as the Array's own: a name that answers
     its oint there (last, pop, ...) is plain to the consumer outside the
     view, which sees the subclass receiver, so the answer is unwrapped */
  int outer_o = node_is_oint(c, id);
  ArysubView v;
  if (!arysub_view_open(c, id, &v)) return 0;
  int inner_o = node_is_oint(c, id) && oint_kind(repr_of(c, id).as_ty) && !outer_o;
  const char *cn = recv >= 0 && ty_is_object(rt) ? c->classes[ty_object_class(rt)].c_name : NULL;
  /* an unwrap's offer for this call (emit_oint_unwrap_ck, `a[0] + 1`): the
     Array's read takes it and answers the plain value, with the unwrap's
     own error for nil, so it is not unwrapped again */
  Buf *ob = b, side; memset(&side, 0, sizeof side);
  int offered = inner_o && g_ck_node == id && !g_ck_done;
  if (offered) ob = &side;
  else if (inner_o) buf_printf(b, "%s(", oint_arg(repr_of(c, id).as_ty));
  if (v.vi >= 0 && v.nat == TY_POLY) {
    int t = ++g_tmp;
    buf_printf(ob, "({ sp_RbVal _t%d = ", t);
    emit_call(c, id, ob);
    buf_printf(ob, "; _t%d.tag == SP_TAG_NIL ? NULL : (sp_%s *)_t%d.v.p; })", t, cn, t);
  }
  else if (v.copy && cn && comp_ary_is_str(c, ty_object_class(rt))) {
    /* to_s / to_str of a String subclass instance: a String of its bytes,
       which the view already copied */
    emit_call(c, id, ob);
  }
  else if (v.copy) {
    buf_printf(ob, "%s_dup(", arysub_array_ctype(c, ty_object_class(rt)));
    emit_call(c, id, ob);
    buf_puts(ob, ")");
  }
  else if (v.copy_class) {
    const char *at = arysub_array_ctype(c, ty_object_class(rt));
    int t = ++g_tmp, d = ++g_tmp;
    buf_printf(ob, "({ %s *_t%d = ", at, t);
    emit_call(c, id, ob);
    buf_printf(ob, "; SP_GC_ROOT(_t%d); sp_%s *_t%d = (sp_%s *)sp_%s__dup((void *)%s, 0); SP_GC_ROOT(_t%d); "
                  "%s_replace(&_t%d->ary, _t%d); _t%d; })",
               t, cn, d, cn, cn, g_argov_text[v.bound], d, at, d, t, d);
  }
  else {
    if (v.vi >= 0) buf_printf(ob, "((sp_%s *)(", cn);
    emit_call(c, id, ob);
    if (v.vi >= 0) buf_puts(ob, "))");
  }
  if (offered) {
    if (g_ck_done) buf_puts(b, side.p ? side.p : "0");
    else buf_printf(b, "%s(%s)", oint_arg(repr_of(c, id).as_ty), side.p ? side.p : "0");
    free(side.p);
  }
  else if (inner_o) buf_puts(b, ")");
  arysub_view_close(c, &v);
  return 1;
}

/* The statement form: the statement emitters' own Array paths (the in-place
   mutators, the loops) take it. */
int emit_arysub_call_stmt(Compiler *c, int id, Buf *b, int indent) {
  if (!c->has_arysub) return 0;
  ArysubView v;
  if (!arysub_view_open(c, id, &v)) return 0;
  emit_stmt_inner(c, id, b, indent);
  arysub_view_close(c, &v);
  return 1;
}


/* The receiver of an operation on a String Array of handles: its text, or
   CRuby's NoMethodError for a nil one where the nil fact says it can be
   (a method that answers the Array or nil, a parameter some call gives nil,
   an instance variable not set yet). A safe navigation guards it itself. */
/* Can the receiver be nil, and is the call no safe navigation (which guards
   it itself) and its receiver not tested already? A receiver the call plan
   tests ahead of the call (emit_nil_target_head: cplan_nil, the arguments
   run first) is seen as tested (nil_tested) and read bare; any other, such
   as an instance variable the plan leaves untested, is read through
   sp_StrhArray_rnn. */
static int strh_recv_nilable(Compiler *c, int id, int recv) {
  return !nt_safe_nav(c->nt, id) && repr_of(c, recv).may_nil && !repr_of(c, recv).nil_tested;
}
void strh_emit_recv(Compiler *c, int id, int recv, Buf *b) {
  const char *nm = nt_str(c->nt, id, "name");
  int nilable = nm && !is_nil_method(nm) && strh_recv_nilable(c, id, recv);
  if (nilable) buf_printf(b, "sp_StrhArray_rnn(");
  emit_expr(c, recv, b);
  if (nilable) buf_printf(b, ", \"%s\")", nm);
}

