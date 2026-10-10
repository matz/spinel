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
    HolderRef h; char ref[1024], val[32];
    int vslot = unwrap_parens(c, v), rebound = 0;
    for (int j = e + 1; j < en; j++)
      rebound |= read_rebound_by(c, v, nt_ref(nt, el[j], "value"));
    if (!rebound && holder_of_node(c, vslot, &h) && holder_slot_text(c, &h, ref, sizeof ref) &&
        (h.r.kind == RK_BOXED || h.r.as_ty == TY_STRING)) {
      snprintf(val, sizeof val, "_t%d", tr);
      buf_printf(b, "%s = ", ref); emit_unbox_text(c, h.r.as_ty, val, b); buf_puts(b, "; ");
    }
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
  char ref[1024];
  HolderRef h;
  int source = unwrap_parens(c, ob), slot = kh < 0 && holder_of_node(c, source, &h);
  int handle = kh < 0 && strbuf_slot_ref(c, slot ? source : ob, ref, sizeof ref);
  slot = !handle && slot && holder_slot_text(c, &h, ref, sizeof ref);
  if (handle)
    buf_printf(b, "sp_RbVal _t%d = sp_box_nullable_obj(%s, SP_BUILTIN_STRBUF); SP_GC_ROOT_RBVAL(_t%d); ", tb, ref, tb);
  else if (kh < 0) tb = hold_operand(c, ob, TY_POLY, 1, tb, 1, " ", b);
  if (repr_share_rule(c) && slot && h.r.kind == RK_BOXED)
    buf_printf(b, "_t%d = sp_poly_strbuf_lift(_t%d); %s = _t%d; ", tb, tb, ref, tb);
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
    buf_printf(b, "if (!sp_poly_nil_p(_t%d)) %s = ", tb, ref);
    emit_unbox_text(c, h.r.as_ty, val, b);
    buf_puts(b, "; ");
  }
  buf_printf(b, "_t%d; })", tr);
  return 1;
}

/* Array.new's poly fill keeps an existing shared slot's handle. Static
   reads retain String dispatch even when their storage is the handle. */
int array_fill_boxes_handle(Compiler *c, int id, int fill) {
  if (repr_of(c, id).as_ty != TY_POLY_ARRAY || !repr_share_rule(c)) return 0;
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
  if (x->rt == TY_POLY_ARRAY)
    buf_printf(b, "({ sp_PolyArray *_t%d = %s; sp_PolyArray_get(_t%d, sp_PolyArray_length(_t%d) - 1); })",
               t, rb.p ? rb.p : "", t, t);
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
  if (at != TY_NIL && at != TY_BOOL && !conv_to_ary_impossible(at)) return 0;
  /* a nullable Integer or Float slot (a parameter one call site passes nil)
     names nil or its class by what it holds */
  int nil_rt = oint_kind(at) && node_has_oint_form(c, argv[0]);
  /* and a String slot's nil is NULL */
  int str_rt = at == TY_STRING;
  int tb = ++g_tmp;
  buf_puts(b, "({ (void)("); emit_expr(c, x->recv, b); buf_puts(b, "); ");
  if (nil_rt) {
    buf_printf(b, "%s _t%d = (", oint_ctype(at), tb);
    emit_oint_expr(c, argv[0], at, b); buf_puts(b, "); ");
  }
  else if (at == TY_BOOL || str_rt) {
    buf_printf(b, "%s _t%d = (", at == TY_BOOL ? "int" : "const char *", tb);
    emit_expr(c, argv[0], b); buf_puts(b, "); ");
  }
  else { buf_puts(b, "(void)("); emit_expr(c, argv[0], b); buf_puts(b, "); "); }
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
  if (t == TY_POLY_ARRAY) buf_puts(b, v);
  else if (t == TY_INT_ARRAY) buf_printf(b, "sp_IntArray_to_poly(%s)", v);
  else if (t == TY_STR_ARRAY) buf_printf(b, "sp_StrArray_to_poly_fmt(%s)", v);
  else if (t == TY_FLOAT_ARRAY) buf_printf(b, "sp_FloatArray_to_poly(%s)", v);
  else if (t == TY_POLY) buf_printf(b, "sp_poly_set_operand(%s)", v);
  else if (t == TY_UNKNOWN) buf_puts(b, "NULL");
  else return 0;
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
  for (int j = 0; j < argc && ok; j++) ok = setop_poly_operand(comp_ntype(c, argv[j]), "", &chk);
  free(chk.p);
  if (!ok || rt == TY_POLY || rt == TY_UNKNOWN) return 0;
  int t = ++g_tmp, t0 = g_tmp + 1;
  g_tmp += argc;
  buf_puts(b, "({ ");
  for (int j = -1; j < argc; j++) {
    int n = j < 0 ? recv : argv[j];
    TyKind at = j < 0 ? rt : comp_ntype(c, n);
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
    setop_poly_operand(comp_ntype(c, argv[j]), v, b);
    buf_puts(b, ");");
  }
  buf_printf(b, " _t%d; })", ta);
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
  if (emit_array_operand_type_error(c, x, b)) return 1;
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
        a0 == TY_POLY) {
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
      a0 == TY_POLY) {
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
    if (sp_streq(name, "intersect?") && argc == 1 &&
        (a0r.elem == TY_POLY || a0r.untyped || ty_is_array(a0) || a0 == TY_POLY)) {
      buf_puts(b, "sp_PolyArray_intersect_p_eql("); emit_expr(c, recv, b); buf_puts(b, ", ");
      if (a0r.untyped) buf_puts(b, "NULL");
      else if (a0r.elem == TY_POLY) emit_expr(c, argv[0], b);
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
      (a0r.elem == ty_array_elem(rt) || a0r.untyped || ty_is_array(a0) || a0 == TY_POLY)) {
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
      if (a0 == TY_POLY) buf_puts(b, ab2.p ? ab2.p : "sp_box_nil()");
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
        (ty_is_array(a0) || a0 == TY_POLY)) {
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
      (a0 == TY_POLY || a0r.elem == TY_POLY || (ty_is_array(a0) && a0r.elem != ty_array_elem(rt)))) {
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
  if (rt != TY_POLY_ARRAY) return 0;
  if (is_push_alias(name) && argc == 1) {
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
      /* a poly-array receiver keeps materializing the tuples: an Enumerator
         here would reach chain sites that read the array directly */
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
    /* scalar-element arrays never hold nil/false: predicate is length-based.
       One that can hold nil counts its truthy (non-nil) elements
       instead. */
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

/* Array#concat(*arrays) on a poly array (a typed one reads its operand nodes,
   after the lookup) */
int emit_op_array_concat(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = x->name;
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = x->rt;
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
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
      Repr ar = repr_of(c, argv[ai]);
      const char *from = ar.elem == TY_INT   ? "sp_PolyArray_from_int_array"
                       : ar.elem == TY_STRING   ? "sp_PolyArray_from_str_array"
                       : ar.elem == TY_FLOAT ? "sp_PolyArray_from_float_array" : NULL;
      buf_printf(b, " sp_PolyArray *_t%d = ", base + ai);
      if (from) { buf_printf(b, "%s(", from); emit_expr(c, argv[ai], b); buf_puts(b, ")"); }
      else if (ar.as_ty == TY_POLY || ar.untyped) {
        /* a boxed argument (a rest param widened to poly): unbox to the
           working array through the runtime kind dispatch (#3317); one
           that is no Array is CRuby's TypeError, not an empty list */
        buf_puts(b, "sp_poly_set_operand("); emit_boxed(c, argv[ai], b); buf_puts(b, ")");
      }
      else emit_expr(c, argv[ai], b);   /* already a poly array */
      buf_printf(b, "; SP_GC_ROOT(_t%d);", base + ai);
    }
    /* ... and its length too: one aliasing the receiver is appended as it
       was, not as an earlier append grew it (CRuby) */
    int lb = g_tmp + 1; g_tmp += argc;
    for (int ai = 0; ai < argc; ai++)
      buf_printf(b, " sp_int _t%d = sp_PolyArray_length(_t%d);", lb + ai, base + ai);
    for (int ai = 0; ai < argc; ai++) {
      int ti = ++g_tmp;
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));",
                 ti, ti, lb + ai, ti, t, base + ai, ti);
    }
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
  const char *k = array_kind(rt);
  int block = nt_ref(nt, id, "block");
  (void)name; (void)k; (void)block; (void)argv;
  if (rt != TY_POLY_ARRAY) return 0;
  /* rindex(obj): last matching index, or nil (an sp_oint, matching
     the index/find_index int-or-nil convention). */
  if (rt == TY_POLY_ARRAY && sp_streq(name, "rindex") && argc == 1 && nt_ref(nt, id, "block") < 0) {
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
      argc == 1 && nt_ref(nt, id, "block") < 0) {
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
    if (slot < 0) {
      buf_printf(b, "sp_poly_shl(_t%d, ", t);
      emit_boxed(c, argv[0], b);
      buf_puts(b, se ? "); })" : ")");
      c->args_in_call = recv;
      return 1;
    }
    int got = ++g_tmp, cur = ++g_tmp;
    if (!se) buf_puts(b, "({ ");
    buf_printf(b, "sp_RbVal _t%d = sp_poly_shl(_t%d, ", got, t);
    emit_boxed(c, argv[0], b);
    buf_printf(b, "); if (_t%d.tag == SP_TAG_STR) { sp_RbVal _t%d = ", t, cur);
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
    int splat = 0;
    for (int a = 0; a < argc; a++) if (nt_kind(nt, argv[a]) == NK_SplatNode) splat = 1;
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
      argc >= 1 && ty_is_array(comp_ntype(c, recv))) {
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
      const char *rty = nt_type(nt, recv);
      if (rty && sp_streq(rty, "ArrayNode")) {
        for (int a = 0; a < argc; a++) {
          Repr ar = repr_of(c, argv[a]);
          TyKind at = ar.as_ty;
          if (!ar.untyped && at != elem_t) { needs_lift = 1; break; }
        }
      }
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
               (ty_is_array(comp_ntype(c, argv[a])) || ty_is_obj_array(comp_ntype(c, argv[a])) ||
                ty_is_hash(comp_ntype(c, argv[a])) || ty_is_object(comp_ntype(c, argv[a])))) {
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
      TyKind fvt = repr_of(c, argv[1]).as_ty;
      /* Fiber storage is poly-valued. A nil/void/untyped value has no scalar
         C slot -- carry it boxed (`void _t = nil` is otherwise a type error). */
      int fval_poly = (fvt == TY_POLY || fvt == TY_UNKNOWN || fvt == TY_NIL || fvt == TY_VOID);
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
    TyKind vt = repr_of(c, argv[1]).as_ty;
    if (ty_is_hash(rt)) {
      const char *hn = ty_hash_cname(rt);
      if (hn) {
        int tv = ++g_tmp;
        int is_poly_hash = (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH);
        TyKind hvt = ty_hash_val(rt);
        /* A poly value into a typed-value hash (e.g. a String? guarded non-nil
           stored into a Hash[String, String]): unbox to the value type. */
        int unbox_poly_val = (!is_poly_hash && vt == TY_POLY &&
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
        if (voset || vboxo) buf_puts(b, "sp_oint"); else emit_ctype(c, decl_type, b);
        buf_printf(b, " _t%d = ", tv);
        /* When the slot is poly but the rhs has no type yet (e.g. `{}`),
           emit a boxed value so the sp_RbVal temp initialises correctly. */
        if (voset) emit_oint_expr(c, argv[1], TY_INT, b);
        else if (vboxo) { buf_puts(b, "sp_poly_hval_oi("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
        else if (unbox_poly_val) {
          const char *fn = hvt == TY_STRING ? "sp_poly_hval_s" : hvt == TY_INT ? "sp_poly_hval_i" : "sp_poly_hval_f";
          buf_printf(b, "%s(", fn); emit_expr(c, argv[1], b); buf_puts(b, ")");
        }
        else if (coerce_unknown_val) emit_unresolved_coerced(c, argv[1], hvt, b);
        else if (decl_type == TY_POLY) emit_boxed(c, argv[1], b);
        else emit_expr(c, argv[1], b);
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
        if (voint) buf_puts(b, oint_ctype(vt)); else emit_ctype(c, vt != TY_UNKNOWN ? vt : TY_POLY, b);
        buf_printf(b, " _t%d = ", tv);
        if (rt == TY_POLY_ARRAY && vt != TY_POLY) emit_boxed(c, argv[1], b);
        else if (voint) emit_oint_expr(c, argv[1], vt, b);
        else emit_expr(c, argv[1], b);
        buf_printf(b, "; sp_%sArray_set%s(", k, vsfx); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b); buf_printf(b, ", _t%d); ", tv);
        /* the store answers the value; this arm has no node to ask for the
           oint form, so a nil here raises as a plain consumer's would */
        if (voint) buf_printf(b, "%s(_t%d); })", oint_arg(vt), tv);
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
  recv = unwrap_parens(c, recv);
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
  TyKind gt = g >= 0 ? comp_ntype(c, g) : TY_UNKNOWN;
  if (g < 0 || nel != 1 || (gt != TY_RANDOM && gt != TY_POLY)) return 0;
  /* a boxed receiver answers in the slot the call was given */
  TyKind et = rt == TY_POLY ? repr_of(c, id).as_ty : rt;
  int ta = ++g_tmp, tg = ++g_tmp;
  buf_puts(b, "({ "); ta = hold_operand(c, recv, TY_POLY, 1, ta, 1, " ", b);
  if (rt == TY_POLY) buf_printf(b, "sp_poly_ary_chk(_t%d, \"%s\", 0); ", ta, name);
  int tn = -1;
  if (argc == 2) { tn = ++g_tmp; buf_printf(b, "sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b); buf_puts(b, "; "); }
  buf_printf(b, "sp_Random *_t%d = %s", tg, gt == TY_POLY ? "sp_poly_random_recv(" : ""); emit_expr(c, g, b);
  buf_puts(b, gt == TY_POLY ? ", \"rand\"); " : "; ");
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
  if (recv >= 0 && ty_is_array(rt) && sp_streq(name, "[]=") && argc == 2) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    /* arr[range] = rhs : a splice over the range's (start, length). */
    if (k && comp_ntype(c, argv[0]) == TY_RANGE) {
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
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
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

/* methods on an array literal whose type never settled (an empty [] or an argument-less Array.new), answered against an empty poly array */
int emit_call_untyped_array_arms(Buf *b, const NodeTable *nt, const char *name, int recv, int argc, TyKind rt) {
  /* an empty array literal as a receiver: its node type is unknown (element
     type is usage-folded, but a bare literal has no usage). Handle the common
     methods directly against an empty (poly) array. */
  if (recv >= 0 && rt == TY_UNKNOWN) {
    const char *rty = nt_type(nt, recv);
    int empty_recv = 0;
    if (rty && sp_streq(rty, "ArrayNode")) {
      int en = 0; nt_arr(nt, recv, "elements", &en);
      empty_recv = (en == 0);
    }
    /* an argument-less `Array.new` is the same empty array, and carries the
       same unknown element type (#3613) */
    else if (rty && sp_streq(rty, "CallNode") && nt_str(nt, recv, "name") &&
             sp_streq(nt_str(nt, recv, "name"), "new") && nt_ref(nt, recv, "block") < 0) {
      int arn = nt_ref(nt, recv, "receiver");
      int aa = nt_ref(nt, recv, "arguments");
      int aac = 0; if (aa >= 0) nt_arr(nt, aa, "arguments", &aac);
      if (aac == 0 && arn >= 0 && nt_type(nt, arn) &&
          sp_streq(nt_type(nt, arn), "ConstantReadNode") &&
          nt_str(nt, arn, "name") && sp_streq(nt_str(nt, arn, "name"), "Array"))
        empty_recv = 1;
    }
    {
      if (empty_recv) {
        if (is_count_alias(name) && argc == 0) { buf_puts(b, "0"); return 1; }
        if (sp_streq(name, "empty?") && argc == 0) { buf_puts(b, "1"); return 1; }
        if (sp_streq(name, "frozen?") && argc == 0) { buf_puts(b, "0"); return 1; }
        if (sp_streq(name, "class") && argc == 0) { buf_puts(b, "((sp_Class){(sp_int)-1, SPL(\"Array\")})"); return 1; }
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
static int arysub_bind(Compiler *c, int n) {
  const char *at = arysub_array_ctype(c, ty_object_class(comp_ntype(c, n)));
  NodeKind nk = nt_kind(c->nt, n);
  Buf rb; memset(&rb, 0, sizeof rb);
  emit_expr(c, n, &rb);
  char cast[96];
  snprintf(cast, sizeof cast, "((%s *)(%s))", at, rb.p ? rb.p : "NULL");
  int slot;
  if ((nk == NK_LocalVariableReadNode || nk == NK_SelfNode) && strlen(cast) < sizeof g_argov_text[0])
    slot = view_bind(n, "%s", cast);
  else {
    int t = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "%s *_t%d = (%s *)(%s); SP_GC_ROOT(_t%d);\n", at, t, at, rb.p ? rb.p : "NULL", t);
    slot = view_bind(n, "_t%d", t);
  }
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
  if (comp_arysub_call(c, id, rt, &k) && (array_new_copies(k) || ty_is_hash(k))) {
    int hash = ty_is_hash(k);
    v->bound = arysub_bind(c, recv);
    v->vr = view_push(c, recv, k);
    v->vf = view_push_face(recv, k);
    /* a call that answers its receiver: the Array emitter's answer -- the
       Array itself, or boxed where a `!` method answers nil when it
       changed nothing (BOPF_SELF_OR_NIL) -- is turned back into the
       instance below */
    if (comp_arysub_self_result(c, id, hash)) {
      v->nat = comp_arysub_answer(c, id, hash) & BOPF_SELF_OR_NIL ? TY_POLY : k;
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
    else if (hash && (comp_arysub_answer(c, id, hash) & BOPF_COPY_CLASS)) v->copy_class = 1;
    rt = k;
  }
  int args = nt_ref(nt, id, "arguments"), an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  int want = 0;
  for (int i = 0; i < an; i++) want |= comp_ty_ary_root(c, comp_ntype(c, av[i])) >= 0;
  if (want && comp_arysub_args_viewed(c, id, rt)) {
    for (int i = 0; i < an && v->nv < 16; i++) {
      TyKind at = comp_ntype(c, av[i]);
      if (comp_ty_ary_root(c, at) < 0) continue;
      int slot = arysub_bind(c, av[i]);
      if (v->bound < 0) v->bound = slot;
      v->views[v->nv++] = view_push(c, av[i], comp_ary_kind(c, ty_object_class(at)));
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
  if (ka >= 0 && comp_ty_ary_root(c, comp_ntype(c, ka)) >= 0) { emit_expr(c, ka, b); return 1; }
  int recv = nt_ref(c->nt, id, "receiver");
  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  /* the view below retypes the call as the Array's own: a name that answers
     its oint there (last, pop, ...) is plain to the consumer outside the
     view, which sees the subclass receiver, so the answer is unwrapped */
  int outer_o = node_is_oint(c, id);
  ArysubView v;
  if (!arysub_view_open(c, id, &v)) return 0;
  int inner_o = node_is_oint(c, id) && oint_kind(comp_ntype(c, id)) && !outer_o;
  const char *cn = recv >= 0 && ty_is_object(rt) ? c->classes[ty_object_class(rt)].c_name : NULL;
  if (inner_o) buf_printf(b, "%s(", oint_arg(comp_ntype(c, id)));
  if (v.vi >= 0 && v.nat == TY_POLY) {
    int t = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = ", t);
    emit_call(c, id, b);
    buf_printf(b, "; _t%d.tag == SP_TAG_NIL ? NULL : (sp_%s *)_t%d.v.p; })", t, cn, t);
  }
  else if (v.copy) {
    buf_printf(b, "%s_dup(", arysub_array_ctype(c, ty_object_class(rt)));
    emit_call(c, id, b);
    buf_puts(b, ")");
  }
  else if (v.copy_class) {
    const char *at = arysub_array_ctype(c, ty_object_class(rt));
    int t = ++g_tmp, d = ++g_tmp;
    buf_printf(b, "({ %s *_t%d = ", at, t);
    emit_call(c, id, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_%s *_t%d = (sp_%s *)sp_%s__dup((void *)%s, 0); SP_GC_ROOT(_t%d); "
                  "%s_replace(&_t%d->ary, _t%d); _t%d; })",
               t, cn, d, cn, cn, g_argov_text[v.bound], d, at, d, t, d);
  }
  else {
    if (v.vi >= 0) buf_printf(b, "((sp_%s *)(", cn);
    emit_call(c, id, b);
    if (v.vi >= 0) buf_puts(b, "))");
  }
  if (inner_o) buf_puts(b, ")");
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
