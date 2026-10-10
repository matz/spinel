/* codegen_call_recv.c -- receiver-typed method-call emitters (array/hash/
   scalar/object/value/range/poly), split out of codegen_call.c. Pure code
   movement, no logic change. */

#include "codegen_internal.h"
#include "call_plan.h"
#include "repr.h"
#include "holder.h"
#include "builtin_ops.h"
#include "share.h"

/* ---- nil out of band: an emitter whose C result is an sp_oint / sp_ofloat
   leaves it bare when the node is one (node_is_oint: the dispatcher's
   consumer takes the oint) and otherwise reads it as the plain scalar through
   sp_oint_arg (TypeError for nil) -- the same wrap the dispatcher applies to an
   oint producer, applied here because this emitter decides the form. */
static void oint_open(Compiler *c, int id, TyKind t, Buf *b) { if (!node_is_oint(c, id)) buf_printf(b, "%s(", oint_arg(t)); }
static void oint_close(Compiler *c, int id, Buf *b) { if (!node_is_oint(c, id)) buf_puts(b, ")"); }
/* A plain answer where the call's consumer wants its oint form (the
   analysis marks the call nullable): wrapped as never nil. */
static void oint_lift_open(Compiler *c, int id, TyKind t, Buf *b) { if (node_is_oint(c, id)) buf_printf(b, "%s(", oint_of(t)); }
static void oint_lift_close(Compiler *c, int id, Buf *b) { if (node_is_oint(c, id)) buf_puts(b, ")"); }
/* The value a raising arm yields in the slot of kind t: nil beside the value
   where the call's consumer takes the oint, raise_tail_value_c's otherwise */
static const char *raise_tail_c(Compiler *c, int id, TyKind t) { return oint_kind(t) && node_is_oint(c, id) ? oint_nil(t) : raise_tail_value_c(c, t); }

/* The value of the block a `fetch` or `delete` runs when it finds nothing, as
   `({ bind; leading statements; setup; value; })`. `bind` sets the block's
   parameter to what the call was given, and has to run before the block's own
   expression: an array or hash literal in it builds its elements in `setup`,
   which lands in g_pre, ahead of the whole call, unless it is captured here
   -- there the parameter was still unset, so `{ |k| [k] }` held nil.
   `lead_always` 0 (`delete`) leaves the leading statements out of a block
   that has a `next`: spliced here, that `next` would be a C `continue`, which
   is not a loop's. */
static void emit_fallback_block_value(Compiler *c, const int *bb, int bn, const char *bind,
                                      int boxed, const char *empty, int lead_always, Buf *b) {
  buf_puts(b, "({ ");
  if (bind) buf_puts(b, bind);
  int lead = 1;
  if (!lead_always)
    for (int k = 0; k < bn; k++) if (subtree_has_own_next(c->nt, bb[k])) lead = 0;
  if (lead)
    for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], b, 0);
  Buf pre, val;
  memset(&pre, 0, sizeof pre); memset(&val, 0, sizeof val);
  if (bn > 0) emit_split_pre(c, bb[bn - 1], boxed ? emit_boxed : emit_expr, &pre, &val);
  buf_puts(b, pre.p ? pre.p : "");
  buf_puts(b, bn > 0 ? (val.p ? val.p : "0") : empty);
  buf_puts(b, "; })");
  free(pre.p); free(val.p);
}

/* Is the parameter `nm` of the spliced block `blk` a boxed slot, one that the
   value a call was given can be stored in? The block's own uses decide the
   slot's type: one that does `v << 1` makes it an Array, and storing a boxed
   value there is a C type error. */
static int block_param_is_boxed(Compiler *c, int blk, int site, const char *nm) {
  Scope *bs = comp_scope_of(c, blk);
  LocalVar *lv = bs ? scope_local(bs, nm) : NULL;
  if (!lv) { Scope *es = comp_scope_of(c, site); lv = es ? scope_local(es, nm) : NULL; }
  return lv && lv->type == TY_POLY;
}

/* Integer Array#delete(v) as an sp_oint (nil for nothing deleted). A
   boxed needle that is not an Integer deletes nothing -- it cannot equal an
   element -- where passing the sp_RbVal as the element did not compile
   (#4835). */
static void emit_int_array_delete(Compiler *c, const char *arr, int arg, int nil_elems, const char *held, Buf *b) {
  TyKind at = repr_of(c, arg).as_ty;
  if (held || at == TY_POLY || at == TY_NIL) {
    /* a nil needle deletes the nil elements (the answer is nil either way).
       `held` is the needle already boxed in a temporary of the caller's,
       which the block's parameter reads too. */
    int tv = ++g_tmp;
    char tvn[32]; snprintf(tvn, sizeof tvn, "_t%d", tv);
    const char *nd = held ? held : tvn;
    buf_puts(b, "({ ");
    if (!held) { buf_printf(b, "sp_RbVal %s = ", tvn); emit_boxed(c, arg, b); buf_puts(b, "; "); }
    buf_printf(b, "%s.tag == SP_TAG_INT ? sp_IntArray_delete_o(%s, %s.v.i) : ", nd, arr, nd);
    /* a Float that is a whole number equals the Integer element (2.0 == 2).
       The bounds are sp_int's, which is 32 bits on the -m32 and wasm32
       builds, so the cast is never out of range. */
    if (held)
      buf_printf(b, "(%s.tag == SP_TAG_FLT && %s.v.f >= (sp_float)INTPTR_MIN && %s.v.f < -(sp_float)INTPTR_MIN && %s.v.f == (sp_float)(sp_int)%s.v.f) ? sp_IntArray_delete_o(%s, (sp_int)%s.v.f) : ",
                 nd, nd, nd, nd, nd, arr, nd);
    if (nil_elems) buf_printf(b, "%s.tag == SP_TAG_NIL ? (sp_IntArray_delete_nil(%s), sp_oint_nil()) : ", nd, arr);
    buf_puts(b, "sp_oint_nil(); })");
    return;
  }
  /* a needle that may itself be nil deletes the nils then */
  buf_printf(b, "sp_IntArray_delete_on(%s, ", arr);
  emit_oint_expr(c, arg, TY_INT, b);
  buf_puts(b, ")");
}
#include "analyze.h"
/* codegen_fold.c: a step tail in its oint form, when it can be nil */
int emit_iter_step_tail_o(Compiler *c, const IterStep *st, TyKind t, Buf *vb);
/* codegen_fold.c: the call whose oint answer an arm keeps (bsearch) */
extern int g_face_oint_id, g_face_oint_got;   /* got: the kept oint's kind */

static void emit_str_encode_call(Compiler *c, const char *recv_txt, const int *argv, int argc, Buf *b);

/* Object's identity protocol, text form (defined with its node form at the end of this file). */
static void emit_native_object_protocol_text(Compiler *c, const char *name, TyKind rt, const char *r, TyKind at, const char *a, Buf *b);

/* Receiver type with the empty-container-literal coercion the inference
   layer applies (`[].m` -> poly array, `{}.m` -> str-keyed poly hash, the
   same C type the emitters build for the bare literals): comp_ntype answers
   UNKNOWN for them, which stranded direct calls like `{}.size`. */
/* The node whose poly receiver is being re-dispatched as a poly array. */
static int g_poly_redispatch_id = -1;

TyKind comp_recv_type(Compiler *c, int recv) {
  TyKind t = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  /* The emitters pick their arm from this, and a handle demand is about how
     the value is HANDED OVER, not about which surface answers the call: a
     String under one still answers String's methods. Without this, marking
     `obj.reader` for `equal?` took the call off the String surface and it
     compiled to "unsupported call" (#4363). */
  if (recv >= 0 && t == TY_STRBUF) {
    Repr rp = repr_of(c, recv);
    if (!rp.handle && rp.demand) t = TY_STRING;
  }
  if (t != TY_UNKNOWN || recv < 0) return t;
  const char *ty = nt_type(c->nt, recv);
  int en = 0;
  if (ty && sp_streq(ty, "ArrayNode")) {
    nt_arr(c->nt, recv, "elements", &en);
    if (en == 0) return TY_POLY_ARRAY;
  }
  else if (ty && (sp_streq(ty, "HashNode") || sp_streq(ty, "KeywordHashNode"))) {
    nt_arr(c->nt, recv, "elements", &en);
    if (en == 0) return TY_STR_POLY_HASH;
  }
  return t;
}

/* Boxing function that lifts an array of kind `kk` ("Int"/"Str"/"Float"/"Poly")
   into a poly sp_RbVal. */
static const char *array_box_fn(const char *kk) {
  if (sp_streq(kk, "Int"))   return "sp_box_int_array";
  if (sp_streq(kk, "Str"))   return "sp_box_str_array";
  if (sp_streq(kk, "Float")) return "sp_box_float_array";
  return "sp_box_poly_array";
}

/* Emit the return value of an in-place filter mutator (select!/filter!/reject!/
   keep_if/delete_if) into `b`, after the compaction loop has run. Shared by the
   typed-array, poly-array, and hash bang handlers.
   Temps available:
     _t<trecv> : the (now-mutated) receiver (typed array/hash or poly array)
     _t<torig> : element count BEFORE compaction (sp_int)
     _t<twp>   : element count AFTER compaction (sp_int) == survivor count
   `boxed_self` is the receiver boxed into a poly sp_RbVal (e.g.
   "sp_box_int_array(_t4)" or "sp_box_obj(_t7, SP_BUILTIN_SYM_INT_HASH)").

   CRuby contract:
     - reject! / select! / filter!  ->  nil when nothing was removed, else self.
       These infer TY_POLY, so self must be boxed (a typed array/hash can't hold
       nil), hence the boxed ternary.
     - keep_if / delete_if          ->  always self, returned bare as the
       receiver type. */
static void emit_filter_bang_result(const char *name, int trecv, int torig,
                                    int twp, const char *boxed_self, Buf *b) {
  if (is_select_reject_bang(name))
    buf_printf(b, "(_t%d != _t%d ? %s : sp_box_nil())", torig, twp, boxed_self);
  else
    buf_printf(b, "_t%d", trecv);  /* keep_if / delete_if: always self */
}

/* String#<< and String#concat take an Integer as a CODEPOINT, not a string:
   `s << 100` appends "d". Sent through the string slot, the integer reached
   sp_str_concat as a char pointer and the program died (#3544). */
/* `rtext` is the receiver's C string, already evaluated (a temp or a plain
   local), or NULL: a binary receiver takes the Integer as one byte (#5538). */
void emit_str_append_arg(Compiler *c, int arg, const char *rtext, Buf *b) {
  if (comp_ntype(c, arg) == TY_INT) {
    if (rtext) buf_printf(b, "sp_int_codepoint_to_str_in(%s, ", rtext);
    else buf_puts(b, "sp_int_codepoint_to_str(");
    emit_expr(c, arg, b); buf_puts(b, ")");
    return;
  }
  /* A BOXED integer is the same Integer: `s << 112` appends "p" whether or not
     the operand's type survived to this point. Falling through to the string
     conversion below made it append the decimal digits instead, silently, and
     a single poly-typed call site widened the operand for every caller of the
     method -- so a program printed "112113" where CRuby prints "pq" and nothing
     in it had changed (#4425). Decided at run time on the tag, because that is
     where the answer is: the typed arm above is the same rule with the tag
     known at compile time. */
  if (comp_ntype(c, arg) == TY_POLY) {
    int ta = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = ", ta); emit_boxed(c, arg, b);
    if (rtext)
      buf_printf(b, "; _t%d.tag == SP_TAG_INT ? sp_int_codepoint_to_str_in(%s, _t%d.v.i)"
                    " : sp_poly_to_s(_t%d); })", ta, rtext, ta, ta);
    else
      buf_printf(b, "; _t%d.tag == SP_TAG_INT ? sp_int_codepoint_to_str(_t%d.v.i)"
                    " : sp_poly_to_s(_t%d); })", ta, ta, ta);
    return;
  }
  emit_str_expr(c, arg, b);
}

/* A Float index is cut to the Integer it converts to, as CRuby's does (an
   error then names that offset); NaN and a Float outside the C int range,
   where CRuby raises RangeError, stay as they are. `tk` is the key temp, `tk0`
   the copy kept for the messages. */
static void emit_struct_float_offset(Buf *b, int tk, int tk0) {
  buf_printf(b, " if (_t%d.tag == SP_TAG_FLT && _t%d.v.f > -2147483649.0 && _t%d.v.f < 2147483648.0)"
                " _t%d = _t%d = sp_box_int((sp_int)_t%d.v.f);", tk, tk, tk, tk, tk0, tk);
}

/* One member read by an arbitrary key: an index (negative counts from the
   end), a Symbol or a String naming a member. CRuby raises for a key no member
   matches, so the miss is IndexError / NameError rather than nil. `rtxt` names
   a temp already holding the receiver. */
/* A Struct / Data member's field text boxed for a poly consumer: an Integer
   or Float member with a nil bit boxes as nil where the bit is set
   (emit_member_boxed_text, which takes the receiver prefix: the field text
   up to its last `iv_`). */
static void emit_member_box(Compiler *c, ClassInfo *sc, int i, const char *fld, Buf *b) {
  const char *iv = NULL;
  for (const char *q = strstr(fld, "iv_"); q; q = strstr(q + 1, "iv_")) iv = q;
  if (!iv) { emit_member_box(c, sc, i, fld, b); return; }
  char pfx[400]; size_t n = (size_t)(iv - fld); if (n >= sizeof pfx) n = sizeof pfx - 1;
  memcpy(pfx, fld, n); pfx[n] = 0;
  emit_member_boxed_text(c, sc, i, pfx, fld, b);
}

static void emit_struct_member_by_key(Compiler *c, ClassInfo *sc, const char *rtxt,
                                      int key, int int_only, int nil_on_miss, Buf *b) {
  int tk = ++g_tmp, tk0 = ++g_tmp, tr = ++g_tmp;
  buf_printf(b, "({ sp_RbVal _t%d = ", tk);
  emit_boxed(c, key, b);
  buf_printf(b, "; sp_RbVal _t%d = _t%d;", tk0, tk);
  /* Struct#values_at takes offsets only, unlike #[] / #dig; a Float inside the
     Integer range is truncated to one (the error paths below then name the
     offset it became) */
  if (int_only)
    buf_printf(b, " if (_t%d.tag == SP_TAG_FLT && _t%d.v.f >= (sp_float)INTPTR_MIN && _t%d.v.f < -(sp_float)INTPTR_MIN)"
                  " _t%d = _t%d = sp_box_int((sp_int)_t%d.v.f);"
                  " if (_t%d.tag != SP_TAG_INT) sp_raise_cls(\"TypeError\","
                  " sp_sprintf(\"no implicit conversion of %%s into Integer\","
                  " sp_poly_class_name(_t%d)));", tk, tk, tk, tk, tk0, tk, tk, tk);
  else
    emit_struct_float_offset(b, tk, tk0);
  buf_printf(b, " if (_t%d.tag == SP_TAG_INT && _t%d.v.i < 0) _t%d = sp_box_int(_t%d.v.i + %d);",
             tk, tk, tk, tk, sc->nmembers);
  buf_printf(b, " sp_RbVal _t%d = sp_box_nil();", tr);
  for (int i = 0; i < sc->nmembers; i++) {
    buf_printf(b, " if(sp_rbval_eql_key(_t%d,sp_box_sym((sp_sym)%d))||sp_rbval_eql_key(_t%d,sp_box_int(%lldLL))"
                  "||sp_rbval_eql_key(_t%d,sp_box_str(\"%s\"))){ _t%d = ",
               tk, comp_sym_intern(c, sc->ivars[i] + 1), tk, (long long)i,
               tk, sc->ivars[i] + 1, tr);
    char fld[300]; snprintf(fld, sizeof fld, "%s->iv_%s", rtxt, iv_c(sc->ivars[i] + 1));
    emit_member_box(c, sc, i, fld, b);
    buf_puts(b, ";}\nelse");
  }
  /* #dig answers nil for a key no member matches, where #[] raises (#3892) */
  if (nil_on_miss) buf_printf(b, " { (void)_t%d; } _t%d; })", tk0, tr);
  else
    buf_printf(b, " { if (_t%d.tag == SP_TAG_INT)"
                  " sp_raise_cls(\"IndexError\", sp_sprintf(\"offset %%lld too %%s for struct(size:%d)\","
                  " (long long)_t%d.v.i, _t%d.v.i < 0 ? \"small\" : \"large\"));"
                  " sp_raise_cls(\"NameError\", sp_sprintf(\"no member '%%s' in struct\", sp_poly_to_s(_t%d)));"
                  " } _t%d; })",
               tk0, sc->nmembers, tk0, tk0, tk0, tr);
}

/* 1 when the node is a user object whose class compares -- defines ==, ===
   or <=> (a Comparable includer) -- so a typed container's lookup would have
   to call it, and the typed slot has no room for the object. The arms refuse
   these at compile time, naming the case, rather than hand the C compiler the
   pointer. */
static int value_obj_compares(Compiler *c, int node) {
  TyKind t = comp_ntype(c, node);
  if (!ty_is_object(t)) return 0;
  int cid = ty_object_class(t);
  return cid >= 0 && (comp_method_in_chain(c, cid, "==", NULL) >= 0 ||
                      comp_method_in_chain(c, cid, "===", NULL) >= 0 ||
                      comp_method_in_chain(c, cid, "<=>", NULL) >= 0);
}

/* 1 when a value of the node's static kind can never be == to an element
   of kind `ek`: a String searched for in an Integer Array, a Symbol in a
   String Array, nil in either. CRuby compares and finds nothing -- Array#delete
   answers nil, count 0, all? false -- where the raw value in the typed slot
   stopped the C build. Integer and Float compare equal across kinds, so they
   are never a static miss of each other; a user object converts nothing
   here, and its class may define == (or ===, which the predicates use), so
   it stays on the comparing path when it does. Also the value slots that
   compare the same way: Hash#value?, Range#include? and #eql?. */

/* Bind a String iterator's block parameter to element `_t<ti>` of the
   StrArray `_t<ta>`. The parameter is the block's own String, unless the
   scope holds it boxed -- the same block is another receiver's arm too (an
   object whose each_line yields it), so the body reads the box: then the
   line is stored into that boxed local rather than shadowing it. */
static void emit_str_elem_param(Compiler *c, int blk, const char *bp, const char *bpn,
                                int ta, int ti, Buf *b) {
  Scope *bs = bp ? comp_scope_of(c, blk) : NULL;
  LocalVar *blv = bs ? scope_local(bs, bp) : NULL;
  if (bp && file_block_param_poly(c, blk, bp))
    buf_printf(b, " lv_%s = sp_box_str(sp_StrArray_get(_t%d, _t%d));", bpn, ta, ti);
  /* a parameter the scope holds as the shared handle: each element is a
     fresh String, which the handle wraps (as the typed each_char binds it) */
  else if (elem_param_is_handle(blv, TY_STRING))
    buf_printf(b, " lv_%s = sp_String_new_shared(sp_StrArray_get(_t%d, _t%d));", bpn, ta, ti);
  else
    buf_printf(b, " const char *lv_%s = sp_StrArray_get(_t%d, _t%d);", bpn, ta, ti);
}
static int value_kind_misses(Compiler *c, int node, TyKind ek) {
  TyKind t = comp_ntype(c, node);
  if (t == ek || t == TY_POLY || t == TY_UNKNOWN) return 0;
  int num = ek == TY_INT || ek == TY_FLOAT || ek == TY_BIGINT;
  if (num && (t == TY_INT || t == TY_FLOAT || (t == TY_BIGINT && ek != TY_INT))) return 0;
  if (ek == TY_STRING && (t == TY_STRING || t == TY_STRBUF)) return 0;
  if (!num && ek != TY_STRING) return 0;
  if (ty_is_object(t)) {
    /* a class defining <=> is a Comparable includer, whose == the module
       supplies (the same reading as respond_to?'s) */
    int cid = ty_object_class(t);
    return cid >= 0 && comp_method_in_chain(c, cid, "==", NULL) < 0 &&
           comp_method_in_chain(c, cid, "===", NULL) < 0 &&
           comp_method_in_chain(c, cid, "<=>", NULL) < 0;
  }
  if (ek == TY_INT && t == TY_BIGINT) return 1;  /* no sp_int equals a Bignum */
  return t == TY_NIL || t == TY_BOOL || t == TY_INT || t == TY_BIGINT || t == TY_FLOAT ||
         t == TY_SYMBOL || t == TY_STRING || t == TY_STRBUF || t == TY_RANGE ||
         t == TY_FLOAT_RANGE || t == TY_STR_RANGE || t == TY_TIME || t == TY_REGEX ||
         ty_is_array(t) || ty_is_hash(t);
}

/* An Integer or Float array whose elements can be nil: the array keeps a
   bit per element (nilbits, sp_types.h). There nil is an element like any
   other -- found, counted, deleted, compared -- so a nil needle is not the
   static miss value_kind_misses reads it as, and an element boxed for a poly
   compare boxes as nil where its bit is set. Any such array can hold one:
   analyze marks one built from, or written with, a nilable scalar (#3505),
   and a write past the end through a computed index fills its gap at run
   time, where no mark can see it. So every one takes the nil-aware arms; an
   array that never held a nil pays one pointer test (SP_MAY_NIL). */
int elem_nil_sentinel(Compiler *c, int recv, TyKind rt) {
  (void)c;
  return recv >= 0 && (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY);
}
/* ... and whether analyze marked it (a store of a nullable scalar) */
int elem_nil_marked(Compiler *c, int recv, TyKind rt) {
  return elem_nil_sentinel(c, recv, rt) && nullable_int_elem_array(c, recv);
}

/* An array literal of plain elements (no splat): what CRuby's VM answers
   min and max of without building the array (opt_newarray_send). A literal
   whose elements are all static -- numbers, nil, true, false, a Symbol, a
   Regexp or a plain String (frozen here) -- is a prebuilt array in CRuby
   instead, whose min and max are Array#min and #max. */
static int static_literal_elem(const NodeTable *nt, int n) {
  switch (nt_kind(nt, n)) {
    case NK_IntegerNode: case NK_FloatNode: case NK_RationalNode: case NK_ImaginaryNode:
    case NK_NilNode: case NK_TrueNode: case NK_FalseNode: case NK_SymbolNode:
    case NK_RegularExpressionNode: case NK_StringNode: return 1;
    default: return 0;
  }
}
static int array_literal_plain(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  if (recv < 0 || nt_kind(nt, recv) != NK_ArrayNode) return 0;
  int en = 0; const int *el = nt_arr(nt, recv, "elements", &en);
  int all_static = 1;
  for (int e = 0; el && e < en; e++) {
    if (nt_kind(nt, el[e]) == NK_SplatNode) return 0;
    if (!static_literal_elem(nt, el[e])) all_static = 0;
  }
  return en > 0 && !all_static;
}

/* value_kind_misses for a needle searched for in the array `recv`: nil is no
   miss where the array can hold nil. */
static int needle_misses(Compiler *c, int recv, TyKind rt, int node) {
  if (comp_ntype(c, node) == TY_NIL && (rt == TY_STR_ARRAY || elem_nil_sentinel(c, recv, rt))) return 0;
  return value_kind_misses(c, node, ty_array_elem(rt));
}

/* A boxed needle can hold a legacy shared handle in either build. A
   statically plain needle keeps its existing default emission. */
static int needle_needs_deref(Compiler *c, int node) {
  Repr r = repr_of(c, node);
  return repr_share_rule(c) || r.kind == RK_BOXED || r.kind == RK_STRBUF;
}

/* A needle in the Integer or Float array's own slot type, with its nil
   beside the value: a nil literal and a nilable scalar's nil pass
   unchecked, and the runtime search matches that nil against the array's
   nil elements, which finds it where the array holds one and misses where
   it cannot, CRuby's answer either way. The strict slot's
   TypeError was wrong here -- `[1, 2].include?(b)` is false for a nil b. */
static void emit_elem_needle(Compiler *c, TyKind rt, int node, Buf *b) {
  /* the needle as the element kind's oint: a nil (literal or run-time)
     is the nil needle, which the _o searches read as the nil elements */
  emit_oint_expr(c, node, rt == TY_INT_ARRAY ? TY_INT : TY_FLOAT, b);
}

/* Element `idx` (C text) of a typed array held in the temp `arr`, boxed for
   a poly compare: an Integer or Float element boxes as nil where its bit is
   set, so it meets a nil as nil. */
static void emit_elem_boxed_text(Compiler *c, int recv, TyKind rt, const char *arr, const char *idx, Buf *b) {
  if (elem_nil_sentinel(c, recv, rt))
    buf_printf(b, "sp_%sArray_box_elem(%s, %s)", rt == TY_INT_ARRAY ? "Int" : "Float", arr, idx);
  else {
    char el[96]; snprintf(el, sizeof el, "sp_%sArray_get(%s, %s)", array_kind(rt), arr, idx);
    emit_boxed_text(c, ty_array_elem(rt), el, b);
  }
}

/* The receiver of a compare (min, max, minmax, sort; `ck` "cmp") or of a
   blockless sum (`ck` "sum", `float_seed` when the seed is a Float), wrapped
   in the runtime's nil check where the array can hold nil: CRuby raises
   there. Checked through its bitmap pointer (_if_flagged), which scans only
   where a nil was stored: every nil an Integer / Float array holds is in
   its bitmap, so a marked array needs no unconditional scan. */
void emit_nil_ck_recv(Compiler *c, int recv, TyKind rt, const char *ck, int float_seed, Buf *b) {
  if (!elem_nil_sentinel(c, recv, rt)) { emit_expr(c, recv, b); return; }
  buf_printf(b, "sp_%sArray_nil_%s_%s(", rt == TY_INT_ARRAY ? "Int" : "Float", ck,
             "if_flagged");
  emit_expr(c, recv, b);
  if (sp_streq(ck, "sum")) buf_printf(b, ", %d", float_seed);
  buf_puts(b, ")");
}
/* The sum wrap around a receiver already held in C text (`out` holds the
   wrapped text; the caller frees it). */
static const char *nil_sum_ck_text(Compiler *c, int recv, TyKind rt, int float_seed, const char *txt, Buf *out) {
  if (!elem_nil_sentinel(c, recv, rt)) return txt;
  buf_printf(out, "sp_%sArray_nil_sum_%s(%s, %d)", rt == TY_INT_ARRAY ? "Int" : "Float",
             "if_flagged", txt, float_seed);
  return out->p;
}

/* (vg: the element read, "oget" where the receiver's element carries its nil) */
static void emit_values_at_boxed(const char *an, const char *vs, const char *vg, int to, int tr, const char *val, Buf *b) {
  int k = ++g_tmp;
  buf_printf(b, "{ sp_RbVal _e%d = %s; if (_e%d.tag == SP_TAG_OBJ && _e%d.cls_id == SP_BUILTIN_RANGE) {"
                " sp_Range _r%d = *(sp_Range *)_e%d.v.p; sp_int _n%d = sp_%sArray_length(_t%d);"
                " sp_int _f%d = _r%d.first == INTPTR_MIN ? 0 : (_r%d.first < 0 ? _r%d.first + _n%d : _r%d.first);",
             k, val, k, k, k, k, k, an, tr, k, k, k, k, k, k);
  buf_printf(b, " if (_f%d < 0) sp_raise_cls(\"RangeError\", sp_sprintf(\"%%s out of range\", sp_poly_inspect(_e%d)));"
                " sp_int _l%d = _r%d.last == INTPTR_MAX ? _n%d - 1 : ((_r%d.last < 0 ? _r%d.last + _n%d : _r%d.last) - (_r%d.excl ? 1 : 0));",
             k, k, k, k, k, k, k, k, k, k);
  buf_printf(b, " for (sp_int _k%d = _f%d; _k%d <= _l%d; _k%d++) sp_%sArray_push%s(_t%d, sp_%sArray_%s(_t%d, _k%d)); }"
                " else sp_%sArray_push%s(_t%d, sp_%sArray_%s(_t%d, sp_poly_arg_int_chk(_e%d))); } ",
             k, k, k, k, k, an, vs, to, an, vg, tr, k, an, vs, to, an, vg, tr, k);
}

/* The direct call of the container conversion obj_container_conv found:
   the compiled #to_ary / #to_hash of the defining class on the operand. */
static void emit_obj_conv_call(Compiler *c, int def, const char *conv, int by_value, const char *recv, Buf *b) {
  int mi = comp_method_in_chain(c, def, conv, NULL);
  if (mi >= 0 && !sp_streq(c->scopes[mi].name, conv)) { emit_method_cname(c, &c->scopes[mi], b); buf_puts(b, "("); }
  else buf_printf(b, "sp_%s_%s(", c->classes[def].c_name, mc(conv));
  if (!by_value) buf_printf(b, "(sp_%s *)", c->classes[def].c_name);
  buf_printf(b, "(%s)", recv);
  if (mi >= 0) emit_conv_dflt_args(c, mi, b);
  buf_puts(b, ")");
}

static void emit_obj_container_conv(Compiler *c, int node, int def, const char *conv, Buf *b) {
  Buf r = {0, 0, 0};
  emit_expr(c, node, &r);
  int byv = repr_of(c, node).kind == RK_VOBJ;
  if (!byv && !expr_is_held_ref(c, node)) {
    int tr = ++g_tmp;
    char rv[32];
    snprintf(rv, sizeof rv, "_t%d", tr);
    buf_printf(b, "({ sp_%s *_t%d = (sp_%s *)(%s); SP_GC_ROOT(_t%d); ", c->classes[def].c_name, tr,
               c->classes[def].c_name, r.p ? r.p : "NULL", tr);
    emit_obj_conv_call(c, def, conv, 0, rv, b);
    buf_puts(b, "; })");
  }
  else emit_obj_conv_call(c, def, conv, byv, r.p ? r.p : "NULL", b);
  free(r.p);
}

/* Array#product's operand as an Array: an array passes; an object converts
   through #to_ary; any other class is CRuby's TypeError. Answers the Array
   kind the text is typed as. */
static TyKind emit_product_operand(Compiler *c, int node, TyKind at, Buf *b) {
  int def = -1;
  TyKind k = obj_container_conv(c, at, "to_ary", &def);
  if (k != TY_UNKNOWN) { emit_obj_container_conv(c, node, def, "to_ary", b); return k; }
  /* a boxed operand is an Array only at run time: taken as one, or the
     implicit-conversion TypeError CRuby raises for anything else (a String,
     nil). Declared as the poly array it holds, the box did not build. */
  if (at == TY_POLY) {
    buf_puts(b, "sp_poly_set_operand("); emit_boxed(c, node, b); buf_puts(b, ")");
    return TY_POLY_ARRAY;
  }
  /* ... and so is the general Array's nil (NULL), which read as empty; a
     literal is never nil */
  if (at == TY_POLY_ARRAY && nt_kind(c->nt, node) != NK_ArrayNode) {
    int tn = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = ", tn); emit_expr(c, node, b);
    buf_printf(b, "; if (!_t%d) sp_raise_cls(\"TypeError\", \"no implicit conversion of nil into Array\"); _t%d; })",
               tn, tn);
    return at;
  }
  const char *cn = conv_cls_name_of(c, at);
  if (cn && !ty_is_array(at) && at != TY_POLY_ARRAY) {
    buf_puts(b, "({ (void)("); emit_expr(c, node, b);
    buf_printf(b, "); sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into Array\"); (sp_PolyArray *)0; })", cn);
    return TY_POLY_ARRAY;
  }
  emit_expr(c, node, b);
  return at;
}

/* The class a builtin type answers to #class, for a "no implicit conversion of
   X into Array" message. Only the kinds conv_to_ary_impossible admits. */
const char *conv_builtin_class_name(TyKind t) {
  if (t == TY_STRING || t == TY_STRBUF) return "String";
  if (t == TY_INT || t == TY_BIGINT) return "Integer";
  if (t == TY_FLOAT) return "Float";
  if (t == TY_SYMBOL) return "Symbol";
  if (t == TY_RANGE || t == TY_FLOAT_RANGE || t == TY_STR_RANGE) return "Range";
  if (t == TY_PROC) return "Proc";
  if (t == TY_TIME) return "Time";
  if (t == TY_COMPLEX) return "Complex";
  if (t == TY_RATIONAL) return "Rational";
  if (ty_is_hash(t)) return "Hash";
  return "Object";
}
/* True for a builtin type that certainly has no #to_ary, so an Array method
   taking "something Array-like" can say so at compile time rather than reach
   an arm that cannot serve it. Deliberately excludes TY_POLY / TY_UNKNOWN (may
   be an array at run time) and every OBJECT type (a user class may define
   #to_ary, which CRuby honours). */
int conv_to_ary_impossible(TyKind t) {
  return t == TY_STRING || t == TY_STRBUF || t == TY_INT || t == TY_BIGINT ||
         t == TY_FLOAT || t == TY_SYMBOL || t == TY_PROC || t == TY_TIME ||
         t == TY_RANGE || t == TY_FLOAT_RANGE || t == TY_STR_RANGE ||
         t == TY_COMPLEX || t == TY_RATIONAL || ty_is_hash(t);
}

/* The shared-mutable shim over an IVAR receiver, in expression position. The
   local form renames the slot so the value arm's reads and its write-back both
   land on a plain shadow; an ivar has no name to rename, so the shadow is
   published to the ivar emitter instead and the same re-run works unchanged.
   `rerun` is the emitter whose arms are being borrowed (#4363). */
static int sb_iv_expr_shim(Compiler *c, int id, int recvS, Buf *b,
                           int (*rerun)(Compiler *, int, Buf *)) {
  const NodeTable *nt = c->nt;
  if (strbuf_local_name(c, recvS)) return 0;
  if (nt_kind(nt, recvS) != NK_InstanceVariableReadNode || g_sb_iv_name) return 0;
  char srefI[1024];
  int icid = strbuf_ivar_owner(c, recvS);
  const char *ivn = nt_str(nt, recvS, "name");
  if (!ivn || icid < 0 || !strbuf_slot_ref(c, recvS, srefI, sizeof srefI)) return 0;
  Buf pre; memset(&pre, 0, sizeof pre);
  int mark = sb_shim_args_first(c, id, &pre, g_indent);
  int tH = ++g_tmp;
  Buf armb; memset(&armb, 0, sizeof armb);
  snprintf(g_sb_iv_repl, sizeof g_sb_iv_repl, "lv__sb%d", tH);
  g_sb_iv_name = ivn; g_sb_iv_cid = icid;
  int handled = rerun(c, id, &armb);
  g_sb_iv_name = NULL; g_sb_iv_cid = -1;
  view_unbind(mark);
  if (!handled) { free(armb.p); free(pre.p); return 0; }
  TyKind resty = repr_of(c, id).as_ty;
  buf_printf(b, "({ sp_String *_t%d = %s;%s"
                " if (sp_String_is_frozen(_t%d)) sp_raise_frozen_str(_t%d->data);"
                " const char *lv__sb%d = sp_str_concat(sp_String_cstr(_t%d), (&(\"\\xff\")[1]));"
                " SP_GC_ROOT(lv__sb%d); ",
             tH, srefI, pre.p ? pre.p : "", tH, tH, tH, tH, tH);
  free(pre.p);
  emit_ctype(c, resty == TY_UNKNOWN || resty == TY_VOID ? TY_STRING : resty, b);
  buf_printf(b, " _res%d = %s;", tH, armb.p ? armb.p : "0");
  free(armb.p);
  buf_printf(b, " sp_String_set_bin(_t%d, lv__sb%d); _res%d; })", tH, tH, tH);
  return 1;
}

/* The expression-position shim over a READER call that hands out the handle
   (`x = obj.name.insert(0, "-")`), as sb_iv_expr_shim is over an ivar. */
static int sb_reader_expr_shim(Compiler *c, int id, int recvS, Buf *b,
                               int (*rerun)(Compiler *, int, Buf *)) {
  char srefR[1024];
  SbReaderSave svR;
  int tH = sb_reader_shim_open(c, recvS, srefR, sizeof srefR, &svR);
  if (!tH) return 0;
  Buf pre; memset(&pre, 0, sizeof pre);
  int mark = sb_shim_args_first(c, id, &pre, g_indent);
  Buf armb; memset(&armb, 0, sizeof armb);
  int handled = rerun(c, id, &armb);
  view_unbind(mark);
  sb_reader_shim_close(c, recvS, &svR);
  if (!handled) { free(armb.p); free(pre.p); return 0; }
  TyKind resty = repr_of(c, id).as_ty;
  buf_printf(b, "({ sp_String *_t%d = %s;%s"
                " if (sp_String_is_frozen(_t%d)) sp_raise_frozen_str(_t%d->data);"
                " const char *lv__sb%d = sp_str_concat(sp_String_cstr(_t%d), (&(\"\\xff\")[1]));"
                " SP_GC_ROOT(lv__sb%d); ",
             tH, srefR, pre.p ? pre.p : "", tH, tH, tH, tH, tH);
  free(pre.p);
  emit_ctype(c, resty == TY_UNKNOWN || resty == TY_VOID ? TY_STRING : resty, b);
  buf_printf(b, " _res%d = %s;", tH, armb.p ? armb.p : "0");
  free(armb.p);
  buf_printf(b, " sp_String_set_bin(_t%d, lv__sb%d); _res%d; })", tH, tH, tH);
  return 1;
}

/* find_index { |x| cond } / index { |x| cond } / rindex { |x| cond } on an
   array of kind `k` ("Int", ..., "Poly") - the index or nil (rindex scans
   from the end). Returns 0 when the block has no body. */
static int emit_array_block_index(Compiler *c, int id, int recv, TyKind rt, const char *k,
                                  const char *name, int block, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn >= 1) {
    int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
    Buf rfi = expr_buf(c, recv);
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = %s; ", trecv, rfi.p ? rfi.p : "NULL"); free(rfi.p);
    /* rooted, as the poly-array find_index above already roots its own
       hoist. rindex takes its bound once and then counts down, so a
       collection mid-walk shows up in the elements rather than in the
       turn count. */
    emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_oint _t%d = sp_oint_nil();\n", tres);
    emit_indent(g_pre, g_indent);
    if (sp_streq(name, "rindex"))
      buf_printf(g_pre, "for (sp_int _t%d = sp_%sArray_length(_t%d) - 1; _t%d >= 0; _t%d--) {\n",
                 ti, k, trecv, ti, ti);
    else
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                 ti, ti, k, trecv, ti);
    if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
    /* the block's value by Ruby's truthiness: an Integer 0 is a hit, and
       a boxed value is no C scalar (read raw, 0 missed and a boxed value
       did not build) */
    Buf cb; memset(&cb, 0, sizeof cb);
    { IterStep st; emit_iter_step_open(c, block, 1, g_indent + 1, &st);
      int sv = g_indent; g_indent++;
      emit_iter_step_cond(c, &st, 0, &cb); g_indent = sv; }
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "if (%s) { _t%d = sp_oint_of(_t%d); break; }\n", cb.p ? cb.p : "0", tres, ti);
    free(cb.p);
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
    /* an Integer slot takes the oint; any other answers boxed */
    if (repr_of(c, id).as_ty == TY_INT) { oint_open(c, id, TY_INT, b); buf_printf(b, "_t%d", tres); oint_close(c, id, b); }
    else buf_printf(b, "sp_box_oint(_t%d)", tres);
    return 1;
  }
  return 0;
}

/* The box an Integer or Float element takes into a poly row: the element
   with its nil bit (sp_IntArray_box_elem, which answers nil past the end
   too -- the nil CRuby pads a zip row with), or NULL for any other kind.
   `*tnf` is 0: the bitmap rides in the array, no flag is read ahead. */
static const char *typed_elem_box_or_null(Compiler *c, int node, TyKind ty, int t, int *tnf, Buf *b) {
  (void)c; (void)node; (void)t; (void)b;
  *tnf = 0;
  return ty == TY_INT_ARRAY ? "sp_IntArray_box_elem" : ty == TY_FLOAT_ARRAY ? "sp_FloatArray_box_elem" : NULL;
}

/* A zip argument's box: a row past the argument's end reads nil */
static const char *zip_pad_box(TyKind ty) {
  return ty == TY_INT_ARRAY ? "sp_IntArray_box_elem" : ty == TY_FLOAT_ARRAY ? "sp_FloatArray_box_elem" : NULL;
}

/* `box` boxes an Integer or Float element from the array and its index
   (NULL: the plain box of the element read) */
static void emit_poly_push_elem(Buf *b, int tpair, TyKind ty, int t, int ti, const char *box, int tnf) {
  (void)tnf;
  if (ty == TY_INT_ARRAY) {
    if (box) buf_printf(b, " sp_PolyArray_push(_t%d, %s(_t%d, _t%d));", tpair, box, t, ti);
    else buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_IntArray_get(_t%d, _t%d)));", tpair, t, ti);
  }
  else if (ty == TY_STR_ARRAY)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_StrArray_get(_t%d, _t%d)));", tpair, t, ti);
  else if (ty == TY_FLOAT_ARRAY) {
    if (box) buf_printf(b, " sp_PolyArray_push(_t%d, %s(_t%d, _t%d));", tpair, box, t, ti);
    else buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_float(sp_FloatArray_get(_t%d, _t%d)));", tpair, t, ti);
  }
  else
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, t, ti);
}


/* Materialize each zip argument into a rooted array temp _t<tb[j]>, and
   rewrite at[j] to the array type the slot ended up holding. */
/* `rlen`: the receiver's length, as C; a Range argument materializes only
   that many elements */
static void emit_zip_args(Compiler *c, const int *argv, int nargs, const int *tb, TyKind *at,
                          const char *rlen, Buf *b) {
  for (int j = 0; j < nargs; j++) {
    /* a Range argument materializes to its int array, as far as the
       receiver reaches: `[1, 2].zip(10.upto(Float::INFINITY))` */
    if (at[j] == TY_RANGE) {
      int trj = ++g_tmp;
      buf_printf(b, " sp_IntArray *_t%d = ({ sp_Range _t%d = ", tb[j], trj);
      emit_expr(c, argv[j], b);
      buf_printf(b, "; sp_range_to_ia_first(_t%d, %s); }); SP_GC_ROOT(_t%d);", trj, rlen, tb[j]);
      at[j] = TY_INT_ARRAY;
      continue;
    }
    /* A scalar argument responds to no :each at all, which is CRuby's
       TypeError naming its class. Read as a container regardless, a nil
       became a column of nils, silently, and an Integer or a String
       stopped the C build. */
    if (ty_is_object(at[j])) {
      /* an object answering #to_ary zips as that Array; one answering
         #each enumerates; any other is the scalar's TypeError */
      int zdef = -1;
      TyKind zk = obj_container_conv(c, at[j], "to_ary", &zdef);
      if (zk != TY_UNKNOWN) {
        const char *kz = zk == TY_POLY_ARRAY ? "Poly" : array_kind(zk);
        buf_printf(b, " sp_%sArray *_t%d = ", kz ? kz : "Poly", tb[j]);
        emit_obj_container_conv(c, argv[j], zdef, "to_ary", b);
        /* rooted: the answer is the conversion's own allocation, read
           across every row the loop below allocates */
        buf_printf(b, "; SP_GC_ROOT(_t%d);", tb[j]);
        at[j] = kz ? zk : TY_POLY_ARRAY;
        continue;
      }
      int zcid = ty_object_class(at[j]);
      if (zcid >= 0 && comp_method_in_chain(c, zcid, "each", NULL) < 0) {
        /* sp_zip_arg would send :each and answer NoMethodError; the
           class is settled, so name CRuby's TypeError here */
        buf_printf(b, " sp_PolyArray *_t%d = ({ (void)(", tb[j]); emit_expr(c, argv[j], b);
        buf_printf(b, "); sp_raise_cls(\"TypeError\", \"wrong argument type %s (must respond to :each)\"); (sp_PolyArray *)0; });",
                   class_ruby_name(c, zcid));
        at[j] = TY_POLY_ARRAY;
        continue;
      }
    }
    /* A Complex, a Rational or a Time responds to no :each either, and
       its class is settled: CRuby's TypeError, named as the object arm
       above names it. Read as an array, its struct stopped the C build;
       boxed into sp_zip_arg, the send of :each answered NoMethodError. */
    if (at[j] == TY_COMPLEX || at[j] == TY_RATIONAL || at[j] == TY_TIME) {
      buf_printf(b, " sp_PolyArray *_t%d = ({ (void)(", tb[j]); emit_expr(c, argv[j], b);
      buf_printf(b, "); sp_raise_cls(\"TypeError\", \"wrong argument type %s (must respond to :each)\"); (sp_PolyArray *)0; });",
                 conv_builtin_class_name(at[j]));
      at[j] = TY_POLY_ARRAY;
      continue;
    }
    if (at[j] == TY_NIL || at[j] == TY_BOOL || at[j] == TY_INT ||
        at[j] == TY_FLOAT || at[j] == TY_STRING || at[j] == TY_STRBUF ||
        at[j] == TY_SYMBOL || at[j] == TY_VOID || ty_is_object(at[j]) ||
        /* a String Range enumerates as Range#each */
        at[j] == TY_STR_RANGE ||
        /* a Hash or an Enumerator DOES respond to :each; the same helper
           materializes it, where the typed line below spelled the slot
           sp_PolyArray* and assigned an sp_SymPolyHash* to it */
        ty_is_hash(at[j]) || at[j] == TY_ENUMERATOR) {
      buf_printf(b, " sp_PolyArray *_t%d = sp_zip_arg(", tb[j]);
      emit_boxed(c, argv[j], b);
      buf_puts(b, ");");
      buf_printf(b, " SP_GC_ROOT(_t%d);", tb[j]);
      at[j] = TY_POLY_ARRAY;
      continue;
    }
    /* a boxed (poly) argument -- e.g. an outer block param that holds an
       array at runtime -- must be unboxed to a poly array, not assigned
       raw into an sp_PolyArray* slot (#3190). */
    if (at[j] == TY_POLY) {
      buf_printf(b, " sp_PolyArray *_t%d = sp_poly_to_poly_array(", tb[j]);
      emit_expr(c, argv[j], b);
      buf_puts(b, ");");
      buf_printf(b, " SP_GC_ROOT(_t%d);", tb[j]);
      at[j] = TY_POLY_ARRAY;
      continue;
    }
    const char *kj = (at[j] == TY_POLY_ARRAY) ? "Poly" : (array_kind(at[j]) ? array_kind(at[j]) : "Poly");
    buf_printf(b, " sp_%sArray *_t%d = ", kj, tb[j]); emit_expr(c, argv[j], b); buf_puts(b, ";");
    buf_printf(b, " SP_GC_ROOT(_t%d);", tb[j]);
  }
}

static int emit_dig_splat(Compiler *c, int recv, int arg, Buf *b) {
  Buf rb; int ch = hold_recv_open(c, recv, 1, "sp_RbVal", "SP_GC_ROOT_RBVAL", b, &rb);
  buf_printf(b, "sp_poly_dig_list(%s, sp_poly_to_poly_array(", rb.p); free(rb.p);
  emit_boxed(c, arg, b); buf_puts(b, "))");
  if (ch) buf_puts(b, "; })");
  return 1;
}

/* --share-strings: the key reaches this parameter through the fetch row's
   existing route. A String slot reads its bytes; a handle slot takes the
   handle out of the boxed key, so both still denote the requested String. */
static int fetch_blk_param_routes_key(Compiler *c, int id, int blk, LocalVar *lv, const char *p0) {
  if (!repr_share_rule(c) || !lv) return 0;
  Repr r = repr_of_slot(c, lv);
  if (r.ty != TY_STRING && r.kind != RK_STRBUF) return 0;
  int ac = 0; const int *av = call_args(c->nt, id, &ac);
  if (ac < 1) return 0;
  Repr kr = repr_of(c, av[0]);
  if (kr.ty != TY_STRING && kr.kind != RK_STRBUF) return 0;
  ShareRoute q = share_route(id, av[0], 0);
  q.to = blk;
  q.to_name = p0;
  /* A fresh key needs its own handle. A variable's handle survives the
     argument temp; a String-valued call's temp can hold only its bytes. */
  char ref[1024];
  if (!share_node_fresh(c, av[0]))
    q.carry = strbuf_var_handle(c, av[0], ref, sizeof ref) ? av[0] : SHARE_CARRY_COPY;
  return share_route_defer(c, &q, "");
}

/* Is a fetch block of the program left with a slot of another type than its
   key? The types have settled and the answer is the whole program's, so it
   is asked once and kept (and asked again if the node table has changed):
   a walk of every call for each block that unboxes its key is quadratic. */
static int fetch_blk_left_behind(Compiler *c) {
  static const NodeTable *knt;
  static unsigned kver;
  static int kcnt, kept;
  const NodeTable *nt = c->nt;
  if (knt == nt && kver == nt->version && kcnt == nt->count) return kept;
  knt = nt; kver = nt->version; kcnt = nt->count; kept = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, f) {
    const char *fn = nt_str(nt, f, "name");
    int fb = nt_ref(nt, f, "block"), fac = 0;
    const int *fav = call_args(nt, f, &fac);
    const char *op = fn && sp_streq(fn, "fetch") && nt_kind(nt, fb) == NK_BlockNode ? block_param_name(c, fb, 0) : NULL;
    LocalVar *ov = op && fac > 0 && comp_ntype(c, nt_ref(nt, f, "receiver")) == TY_POLY ? scope_local(comp_scope_of(c, fb), op) : NULL;
    if (ov && ov->type != TY_POLY && ov->type != comp_ntype(c, fav[0]) &&
        !fetch_blk_param_routes_key(c, f, fb, ov, op)) { kept = 1; break; }
  }
  return kept;
}

/* Is the slot `flv` of `|k|`, the one parameter of the block `blk` given to
   the fetch `id`, typed as the key itself, so that a table holding its keys
   boxed can unbox the key into it? Only where this list shows it: the key
   is a Symbol or a String literal (frozen, so the unboxed value is no second
   name for a String someone changes in place) and the slot has that type
   once inference has settled, and is no shared handle (--share-strings);
   nothing assigns k; every read of k is a call's receiver, an interpolation
   or the block's value; the block answers a plain value, so that nothing
   built from the typed k is hidden behind fetch's boxed answer; and no
   fetch block of the program is left with a slot of another type than its
   key, whose failure a program that now builds would reach. */
static int fetch_blk_param_is_key(Compiler *c, int id, int blk, LocalVar *flv, const char *fp0) {
  /* The flag's route also carries a String whose slot became a handle. */
  int routed = fetch_blk_param_routes_key(c, id, blk, flv, fp0);
  const NodeTable *nt = c->nt;
  int ac = 0; const int *av = call_args(nt, id, &ac);
  if (!routed && (ac < 1 || flv->type != comp_ntype(c, av[0]) || repr_of_slot(c, flv).kind == RK_STRBUF ||
      !(flv->type == TY_SYMBOL || (flv->type == TY_STRING && nt_kind(nt, av[0]) == NK_StringNode)))) return 0;
  int pn = nt_ref(nt, nt_ref(nt, blk, "parameters"), "parameters");
  int rn = 0, on = 0, sn = 0, kn = 0;
  nt_arr(nt, pn, "requireds", &rn); nt_arr(nt, pn, "optionals", &on);
  nt_arr(nt, pn, "posts", &sn); nt_arr(nt, pn, "keywords", &kn);
  if (rn != 1 || on || sn || kn || nt_ref(nt, pn, "rest") >= 0 || nt_ref(nt, pn, "keyword_rest") >= 0 ||
      nt_ref(nt, pn, "block") >= 0) return 0;
  int body = nt_ref(nt, blk, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  int last = bn > 0 ? bb[bn - 1] : -1;
  TyKind vt = last >= 0 ? comp_ntype(c, last) : TY_NIL;
  if (!(vt == TY_INT || vt == TY_FLOAT || vt == TY_BOOL || vt == TY_NIL || vt == TY_SYMBOL || vt == TY_STRING ||
        (routed && vt == TY_STRBUF)) ||
      block_next_value_ty(c, body) != TY_UNKNOWN || !block_param_used_up(c, blk, fp0, last)) return 0;
  return !fetch_blk_left_behind(c);
}

/* fetch's block is spliced inline, so its parameter's slot is the enclosing
   scope's local: bind the missed key (or index), of type `kt` in temp `tk`,
   to it -- boxed on the way in when the slot is poly, as it is when the body
   reassigns it or another receiver kind yields a key of another class, and
   unboxed where a table of boxed keys meets a slot typed as the key
   (fetch_blk_param_is_key): `x.fetch(:k) { |k| k }` on a receiver that may
   be a Hash or an Array put the boxed key in a Symbol's slot and the C did
   not build. */
/* The fallback block of a fetch: its literal, or -- for a `&blk` that hands
   on the block an inlined body was given (a `super` into Array#fetch or
   Hash#fetch from a subclass's override passes the method's own) -- the
   caller's literal it stands for. A real proc stays the BlockArgumentNode. */
static int fetch_fallback_block(Compiler *c, int id) {
  int blk0 = nt_ref(c->nt, id, "block");
  int r = resolve_forwarded_block(c, blk0);
  if (blk0 >= 0 && r >= 0 && r != blk0 && nt_kind(c->nt, r) == NK_BlockNode) return r;
  return blk0;
}
static void emit_fetch_blk_param(Compiler *c, int id, int blk, TyKind kt, int tk, Buf *b) {
  const char *fp0 = block_param_name(c, blk, 0);
  if (!fp0) return;
  Scope *fbs = comp_scope_of(c, blk);
  LocalVar *flv = fbs ? scope_local(fbs, fp0) : NULL, *own = flv;
  if (!flv) { Scope *fes = comp_scope_of(c, id); flv = fes ? scope_local(fes, fp0) : NULL; }
  int fac = 0; const int *fav = call_args(c->nt, id, &fac);
  char kref[1024];
  if (flv && flv->type == TY_POLY && kt != TY_POLY) {
    char ktn[32]; snprintf(ktn, sizeof ktn, "_t%d", tk);
    buf_printf(b, "lv_%s = ", rename_local(fp0));
    /* --share-strings: the String asked for is the key the block is handed
       (`fetch(s) { |k| k << x }` changes s): its handle, boxed */
    if (kt == TY_STRING && fac >= 1 && strbuf_var_handle(c, fav[0], kref, sizeof kref))
      buf_printf(b, "sp_box_nullable_obj(%s, SP_BUILTIN_STRBUF)", kref);
    else emit_boxed_text(c, kt, ktn, b);
    buf_puts(b, "; ");
  }
  /* a parameter that is the shared handle (--share-strings) takes the key
     argument's handle, the String it was asked for, or a handle of its own
     around a key that is a String of its own */
  else if (flv && repr_of_slot(c, flv).kind == RK_STRBUF && kt == TY_STRING) {
    if (fac >= 1 && strbuf_slot_ref(c, fav[0], kref, sizeof kref))
      buf_printf(b, "lv_%s = %s; ", rename_local(fp0), kref);
    else buf_printf(b, "lv_%s = sp_String_new_shared(_t%d); ", rename_local(fp0), tk);
  }
  else if (own && kt == TY_POLY && fetch_blk_param_is_key(c, id, blk, own, fp0)) {
    char ktn[32]; snprintf(ktn, sizeof ktn, "_t%d", tk);
    buf_printf(b, "lv_%s = ", rename_local(fp0)); emit_unbox_text(c, repr_of_slot(c, own).as_ty, ktn, b); buf_puts(b, "; ");
  }
  else buf_printf(b, "lv_%s = _t%d; ", rename_local(fp0), tk);
}

/* One key of `slice` on an Integer-keyed table that the _okey lookup takes
   (hash_okey_form): a key no entry matches slices nothing (evaluated for
   its effects); a boxed key, or an Integer that can be nil, is copied only
   when it is an Integer the table holds. 0 for any other key. */
static int emit_slice_okey_key(Compiler *c, int key, TyKind kt, const char *hn, int th, int tr, int tk, int vn, Buf *b) {
  if (!hash_okey_form(c, key, kt)) return 0;
  if (hash_okey_miss(c, key, kt)) { buf_puts(b, " (void)("); emit_expr(c, key, b); buf_puts(b, ");"); return 1; }
  buf_printf(b, " { sp_oint _t%d = ", tk); emit_hash_key_o(c, key, kt, b);
  /* vn: the values may be nil, copied with it (DESIGN.md D3b-ii) */
  if (vn) buf_printf(b, "; if (sp_%sHash_has_key_okey(_t%d, _t%d)) sp_%sHash_oset(_t%d, _t%d.v, sp_%sHash_vget(_t%d, _t%d.v)); }",
                     hn, th, tk, hn, tr, tk, hn, th, tk);
  else buf_printf(b, "; if (sp_%sHash_has_key_okey(_t%d, _t%d)) sp_%sHash_set(_t%d, _t%d.v, sp_%sHash_get_okey(_t%d, _t%d)); }",
                  hn, th, tk, hn, tr, tk, hn, th, tk);
  return 1;
}

/* fetch's key on an Integer-keyed table, as the _okey lookup takes it
   (hash_okey_form): a boxed key, or an Integer that can be nil, becomes an
   sp_oint `_t<tk>` that is nil where it matches no entry. A boxed key is
   also held as it was given, in the temp answered (else -1), for the block's
   parameter. Any other key is the table's own kind. */
static int emit_fetch_blk_key(Compiler *c, int key, TyKind kt, int tk, Buf *b) {
  if (!hash_okey_form(c, key, kt)) {
    buf_printf(b, "; %s _t%d = ", c_type_name(kt), tk); emit_hash_key(c, key, kt, b);
    return -1;
  }
  if (repr_of(c, key).kind != RK_BOXED) {
    buf_printf(b, "; sp_oint _t%d = ", tk); emit_hash_key_o(c, key, kt, b);
    return -1;
  }
  int tb = ++g_tmp;
  buf_printf(b, "; sp_RbVal _t%d = ", tb); emit_boxed(c, key, b);
  buf_printf(b, "; sp_oint _t%d = _t%d.tag == SP_TAG_INT ? sp_oint_of(_t%d.v.i) : sp_oint_nil()", tk, tb, tb);
  return tb;
}
/* ... and the block's parameter, handed that key: the boxed key as given
   (tb), else the Integer-or-nil key, each in the parameter's form */
static void emit_fetch_blk_okey_param(Compiler *c, int blk, int tb, int tk, Buf *b) {
  const char *fp0 = block_param_name(c, blk, 0);
  if (!fp0) return;
  Scope *fbs = comp_scope_of(c, blk);
  LocalVar *flv = fbs ? scope_local(fbs, fp0) : NULL;
  int poly = flv && flv->type == TY_POLY, o = flv && slot_is_oint(flv);
  buf_printf(b, "lv_%s = ", rename_local(fp0));
  if (tb >= 0) {
    if (poly) buf_printf(b, "_t%d; ", tb);
    else if (o) buf_printf(b, "%s(_t%d); ", oint_unbox(TY_INT), tb);
    else buf_printf(b, "sp_poly_hkey_i(_t%d); ", tb);
  }
  else if (poly) buf_printf(b, "sp_box_oint(_t%d); ", tk);
  else if (o) buf_printf(b, "_t%d; ", tk);
  else buf_printf(b, "sp_oint_arg(_t%d); ", tk);
}

/* Bind a merge block's parameter `pn` (of block `blk`) to `text`, a value of
   type `vt`: boxed on the way in when the parameter's slot is boxed, as it
   is in a block handed through a method's `&blk` (emit_fetch_blk_param's
   rule). */
/* `otext`, when given, is the value with its nil (an sp_oint, read from a
   hash whose values may be nil, DESIGN.md D3b-ii): handed to an oint slot as
   is, boxed into a boxed one, else unwrapped. */
static void emit_merge_blk_param(Compiler *c, int blk, const char *pn, TyKind vt, const char *text, const char *otext, Buf *b) {
  if (!pn) return;
  Scope *bs = comp_scope_of(c, blk);
  LocalVar *lv = bs ? scope_local(bs, pn) : NULL;
  buf_printf(b, " lv_%s = ", rename_local(pn));
  if (otext) {
    if (lv && slot_is_oint(lv)) buf_puts(b, otext);
    else buf_printf(b, "%s(%s)", lv && lv->type == TY_POLY ? "sp_box_oint" : "sp_oint_arg", otext);
  }
  else if (lv && lv->type == TY_POLY && vt != TY_POLY) emit_boxed_text(c, vt, text, b);
  else buf_puts(b, text);
  buf_puts(b, ";");
}

static int emit_blk_value_via_next(Compiler *c, int blk, TyKind vt, Buf *b);
/* The runtime function that reads a String-keyed Hash of other values as
   the String-to-String replacement Hash sub/gsub take (each value's to_s),
   or NULL for a Hash kind that needs none or cannot be one */
static const char *repl_hash_to_s_fn(Repr hr) {
  return repr_hash_is(hr, TY_STRING, TY_POLY) ? "sp_StrPolyHash_to_s_values"
       : repr_hash_is(hr, TY_STRING, TY_INT) ? "sp_StrIntHash_to_s_values" : NULL;
}
static int str_arms_convert(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind a0, const char *r);
static void emit_blk_value_as(Compiler *c, int blk, TyKind vt, Buf *b);
static void emit_blk_value_as_o(Compiler *c, int blk, TyKind vt, Buf *b);

/* An operand whose evaluation cannot allocate: a local's read or a scalar
   literal. */
static int fetch_operand_is_inert(Compiler *c, int n) {
  switch (nt_kind(c->nt, n)) {
    case NK_LocalVariableReadNode: case NK_IntegerNode: case NK_FloatNode:
    case NK_SymbolNode: case NK_NilNode: case NK_TrueNode: case NK_FalseNode:
      return decide_node(c->nt, n, "fetch-inert", NULL);
    default:
      return 0;
  }
}

/* The text argument of String#insert. A splat there (`s.insert(i, *r)`, left
   whole because Array#insert takes any count) has to hold exactly the text. */
void emit_str_insert_text(Compiler *c, int arg, Buf *b) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, arg) != NK_SplatNode) { emit_str_expr(c, arg, b); return; }
  int inner = nt_ref(nt, arg, "expression"), ts = ++g_tmp;
  buf_printf(b, "({ sp_PolyArray *_t%d = ", ts);
  if (inner < 0) {
    if (!emit_anon_rest_ref(c, arg, b)) buf_puts(b, "sp_PolyArray_new()");
  }
  else {
    buf_puts(b, "sp_poly_to_poly_array(sp_splat_to_array(");
    emit_boxed(c, inner, b);
    buf_puts(b, "))");
  }
  buf_printf(b, "; if (_t%d->len != 1) sp_raise_cls(\"ArgumentError\","
                " sp_sprintf(\"wrong number of arguments (given %%lld, expected 2)\","
                " (long long)(_t%d->len + 1))); sp_poly_arg_str_chk(_t%d->data[0]); })",
             ts, ts, ts);
}

/* push / append / unshift / prepend / insert / concat with a `*splat` among
   the arguments: the splat spreads across the argument list, so its length is
   only known at run time. The per-name arms read each argument as one element,
   which stored the splatted array itself (`a.push(*r)` answered
   [1, 2, [3, 4]]) and dropped an anonymous `*` altogether. Gather the
   arguments into one poly array -- a splat contributes each of its elements --
   and apply the operation element by element. Used by the value and the
   statement form alike. A poly receiver is taken for insert alone, which may
   be a String's (one text) as well as an Array's. */
int emit_array_splat_mutator(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name || recv < 0 || nt_ref(nt, id, "block") >= 0) return 0;
  int is_push = sp_streq(name, "push") || sp_streq(name, "append");
  int is_unshift = is_prepend_alias(name);
  int is_insert = sp_streq(name, "insert");
  int is_concat = sp_streq(name, "concat");
  if (!is_push && !is_unshift && !is_insert && !is_concat) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int has_splat = 0;
  for (int a = 0; a < argc; a++)
    if (nt_kind(nt, argv[a]) == NK_SplatNode) has_splat = 1;
  if (!has_splat) return 0;
  if (is_insert && nt_kind(nt, argv[0]) == NK_SplatNode) return 0;
  Repr rr = repr_of(c, recv), ir = repr_of(c, id);
  TyKind rt = rr.as_ty;
  int poly = rr.kind == RK_BOXED && is_insert;
  if (poly)
    for (int k2 = 0; k2 < c->nclasses; k2++)
      if (comp_poly_arm_defines(c, k2, name)) return 0;
  /* a typed literal receiver given elements of another kind is rebuilt as
     the poly array the call's value was typed as */
  int lift = !poly && rr.elem != TY_POLY && ir.elem == TY_POLY &&
             nt_kind(nt, recv) == NK_ArrayNode;
  if (lift) rt = TY_POLY_ARRAY;
  else if (!poly && ir.elem != TY_UNKNOWN && ir.elem != rr.elem) return 0;
  const char *k = rt == TY_POLY_ARRAY ? "Poly" : array_kind(rt);
  if (!k && !poly) return 0;
  if (is_insert && rt == TY_FLOAT_ARRAY) return 0;
  for (int a = 0; a < argc; a++) {
    if (nt_kind(nt, argv[a]) != NK_SplatNode || nt_ref(nt, argv[a], "expression") >= 0) continue;
    Buf ab; memset(&ab, 0, sizeof ab);
    int ok = emit_anon_rest_ref(c, argv[a], &ab);
    free(ab.p);
    if (!ok) return 0;
  }
  TyKind et = ty_array_elem(rt);
  const char *conv = poly ? "" : et == TY_INT ? "sp_poly_elem_i" : et == TY_FLOAT ? "sp_poly_elem_f" :
                     et == TY_STRING ? "sp_poly_elem_s" : "";
  int tr = ++g_tmp, ta = ++g_tmp, ti = -1;
  if (poly) {
    buf_puts(b, "({ "); tr = hold_operand(c, recv, TY_POLY, 0, tr, 1, " ", b);
  }
  else {
    buf_printf(b, "({ sp_%sArray *_t%d = ", k, tr);
    if (lift) {
      buf_puts(b, "sp_poly_to_poly_array(");
      emit_boxed(c, recv, b);
      buf_printf(b, "); SP_GC_ROOT(_t%d); ", tr);
    }
    else emit_recv_rooted(c, recv, tr, "SP_GC_ROOT", b);
  }
  if (is_insert) {
    ti = ++g_tmp;
    buf_printf(b, "sp_int _t%d = ", ti); emit_int_expr(c, argv[0], b); buf_puts(b, "; ");
  }
  buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
  for (int a = is_insert ? 1 : 0; a < argc; a++) {
    if (nt_kind(nt, argv[a]) == NK_SplatNode) {
      int inner = nt_ref(nt, argv[a], "expression");
      buf_printf(b, " sp_PolyArray_append_all(_t%d, ", ta);
      TyKind at = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
      if (inner < 0) emit_anon_rest_ref(c, argv[a], b);
      else if (at == TY_RANGE || at == TY_STR_RANGE || ty_is_hash(at)) {
        buf_puts(b, "sp_enum_items_from(");
        emit_boxed(c, inner, b);
        buf_puts(b, ")");
      }
      else {
        buf_puts(b, "sp_poly_to_poly_array(sp_splat_to_array(");
        emit_boxed(c, inner, b);
        buf_puts(b, "))");
      }
      buf_puts(b, ");");
    }
    else {
      buf_printf(b, " sp_PolyArray_push(_t%d, ", ta);
      emit_boxed(c, argv[a], b);
      buf_puts(b, ");");
    }
  }
  if (is_concat) {
    int tf = ++g_tmp, tc = ++g_tmp;
    buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                  " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                  " sp_PolyArray_append_all(_t%d, sp_poly_set_operand(_t%d->data[_t%d]));",
               tf, tf, tc, tc, ta, tc, tf, ta, tc);
    ta = tf;
  }
  int tj = ++g_tmp;
  /* the values come boxed, and a nil converts to an Integer or Float
     array's nil element */
  const char *nsfx = nil_store_sfx(c, k, NIL_STORE_BOXED);
  /* a store that takes no nil (no `_nilable`, an insert) takes the plain
     value: nil there is the operand TypeError */
  const char *pconv = et == TY_INT ? "sp_poly_elem_i_v" : et == TY_FLOAT ? "sp_poly_elem_f_v" : conv;
  if (!nsfx[0]) conv = pconv;
  if (is_push || is_concat)
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                  " sp_%sArray_push%s(_t%d, %s(_t%d->data[_t%d]));",
               tj, tj, ta, tj, k, nsfx, tr, conv, ta, tj);
  else if (is_unshift && (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY))
    buf_printf(b, " for (sp_int _t%d = _t%d->len - 1; _t%d >= 0; _t%d--)"
                  " sp_%sArray_unshift%s(_t%d, %s(_t%d->data[_t%d]));",
               tj, ta, tj, tj, k, nsfx, tr, conv, ta, tj);
  else if (is_unshift)
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                  " sp_%sArray_insert(_t%d, _t%d, %s(_t%d->data[_t%d]));",
               tj, tj, ta, tj, k, tr, tj, pconv, ta, tj);
  else {
    /* a negative index is taken afresh against the grown array, which puts
       each element after the one before it, as counting up does for a
       non-negative one */
    if (poly) {
      buf_printf(b, " if ((_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)) && _t%d->len != 1)"
                    " sp_raise_cls(\"ArgumentError\", sp_sprintf(\"wrong number of arguments"
                    " (given %%lld, expected 2)\", (long long)(_t%d->len + 1)));",
                 tr, tr, ta, ta);
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                    " _t%d = sp_poly_insert(_t%d, _t%d < 0 ? _t%d : _t%d + _t%d, _t%d->data[_t%d]);",
                 tj, tj, ta, tj, tr, tr, ti, ti, ti, tj, ta, tj);
      /* a plain String box cannot hold the spliced contents: an lvalue
         receiver takes the result back */
      if (nt_kind(nt, recv) == NK_LocalVariableReadNode ||
          nt_kind(nt, recv) == NK_InstanceVariableReadNode) {
        buf_puts(b, " ");
        emit_expr(c, recv, b);
        buf_printf(b, " = _t%d;", tr);
      }
    }
    else
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                    " sp_%sArray_insert%s(_t%d, _t%d < 0 ? _t%d : _t%d + _t%d, %s(_t%d->data[_t%d]));",
                 tj, tj, ta, tj, k, rt == TY_INT_ARRAY ? nsfx : "", tr, ti, ti, ti, tj, conv, ta, tj);
  }
  buf_printf(b, " _t%d; })", tr);
  return 1;
}

static void emit_find_loop_head(Compiler *c, int id, const char *k, int ti, int trecv) {
  emit_indent(g_pre, g_indent);
  if (nt_int(c->nt, id, "rfind", 0))
    buf_printf(g_pre, "for (sp_int _t%d = sp_%sArray_length(_t%d); _t%d-- > 0;"
                      " _t%d = _t%d < sp_%sArray_length(_t%d) ? _t%d : sp_%sArray_length(_t%d)) {\n",
               ti, k, trecv, ti, ti, ti, k, trecv, ti, k, trecv);
  else
    buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, trecv, ti);
}

static Buf block_cond_buf(Compiler *c, int block, const int *bb, int bn) {
  Buf cb; memset(&cb, 0, sizeof cb);
  (void)bb; (void)bn;
  IterStep st; emit_iter_step_open(c, block, 1, g_indent + 1, &st);
  int sv = g_indent; g_indent++;
  emit_iter_step_cond(c, &st, 0, &cb); g_indent = sv;
  return cb;
}

/* to_h on an Array of Integers, Floats or Strings: no element is a pair, so
   the call raises CRuby's TypeError, or answers {} when the Array is empty.
   A program with a def, alias or define_method of to_h in a class or module
   keeps the path it had. */
static int emit_scalar_array_conversion(Compiler *c, int id, int recv, TyKind rt,
                                  const char *name, int argc, Buf *b) {
  if (emit_scalar_array_transpose(c, id, recv, rt, name, argc, b)) return 1;
  if (!(rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY || rt == TY_STR_ARRAY) ||
      !sp_streq(name, "to_h") || argc != 0 || nt_ref(c->nt, id, "block") >= 0 ||
      an_user_recv_defines_method(c, "to_h"))
    return 0;
  buf_puts(b, "((sp_PolyPolyHash *)sp_poly_to_h_val(");
  emit_boxed(c, recv, b); buf_puts(b, ").v.p)");
  return 1;
}

/* A poly (mixed-element) Array receiver: its elements are boxed sp_RbVal (emit_typed_array_call's arms, in their order) */
static int emit_poly_array_call(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind a0, TyKind res, int *out) {
  /* poly (mixed-element) array methods: elements are boxed sp_RbVal */
  if (!(rt == TY_POLY_ARRAY)) return 0;
  if (sp_streq(name, "[]") && argc == 1 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode")) {
    /* arr[a..b] / arr[a...b] -> subarray */
    int rn = argv[0];
    int excl = (int)(nt_int(nt, rn, "flags", 0) & 4) ? 1 : 0;
    int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
    Buf rb; int ch = hold_recv_open(c, recv, 0, "sp_PolyArray *", "SP_GC_ROOT", b, &rb);
    buf_printf(b, "sp_PolyArray_slice_range(%s, ", rb.p);
    char none_hi[64]; snprintf(none_hi, sizeof none_hi, "sp_PolyArray_length(%s)", rb.p);
    free(rb.p);
    if (lo >= 0) emit_int_expr_bound(c, lo, "0", b); else buf_puts(b, "0");
    buf_puts(b, ", ");
    /* a nil end reaches the length, as above */
    if (hi >= 0) emit_int_expr_bound(c, hi, none_hi, b); else buf_puts(b, "-1");
    buf_printf(b, ", %d)", hi >= 0 ? excl : 0);
    if (ch) buf_puts(b, "; })");
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "[]") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
    /* arr[range] with a range VALUE (a variable, a parenthesised
       expression): the typed arrays resolve the endpoints against the
       length and slice; the boxed array had only the literal arm and
       handed the sp_Range to the element read */
    int ta = ++g_tmp, tr = ++g_tmp, tf = ++g_tmp, tl = ++g_tmp, tn = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_recv_rooted(c, recv, ta, "SP_GC_ROOT", b);
    buf_printf(b, "sp_Range _t%d = sp_range_ix(", tr); emit_expr(c, argv[0], b); buf_puts(b, ")");
    buf_printf(b, "; sp_int _t%d = sp_PolyArray_length(_t%d);", tn, ta);
    buf_printf(b, " sp_int _t%d = _t%d.nobeg ? 0 :"
                  " (_t%d.first < 0 ? _t%d.first + _t%d : _t%d.first);",
               tf, tr, tr, tr, tn, tr);
    buf_printf(b, " sp_int _t%d = _t%d.noend ? _t%d - _t%d :"
                  " ((_t%d.last < 0 ? _t%d.last + _t%d : _t%d.last) - _t%d + (_t%d.excl ? 0 : 1));",
               tl, tr, tn, tf, tr, tr, tn, tr, tf, tr);
    buf_printf(b, " (_t%d < 0 || _t%d > _t%d) ? (sp_PolyArray *)0 : sp_PolyArray_slice(_t%d, _t%d, _t%d); })",
               tf, tf, tn, ta, tf, tl);
    { *out = 1; return 1; }
  }
  /* a boxed index is whatever it holds at run time: an Integer reads an
     element, a Range a sub-array (sp_poly_index_poly). Read as an Integer,
     a Range took element 0. */
  if (sp_streq(name, "[]") && argc == 1 && a0 == TY_POLY && nt_kind(nt, argv[0]) != NK_SplatNode) {
    buf_puts(b, "sp_poly_index_poly(sp_box_poly_array("); emit_expr(c, recv, b); buf_puts(b, "), ");
    emit_expr(c, argv[0], b); buf_puts(b, ")");
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "[]") && argc == 1) {
    buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", ");
    if (a0 == TY_POLY) { buf_puts(b, "sp_poly_arg_int_chk("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (a0 == TY_BIGINT) emit_int_expr(c, argv[0], b);   /* a widened counter used as an index */
    else emit_expr(c, argv[0], b);
    buf_puts(b, ")");
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "sample") && argc == 1 && nt_type(nt, argv[0]) &&
      sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) {
    /* sample(random: rng): one element, as sample's row (#2970) */
    buf_puts(b, "sp_PolyArray_sample("); emit_expr(c, recv, b); buf_puts(b, ")");
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "sample") && argc == 1) {
    int t = ++g_tmp, tn = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_shuffle(", t); emit_expr(c, recv, b);
    buf_printf(b, "); SP_GC_ROOT(_t%d); sp_int _t%d = ", t, tn); emit_int_expr(c, argv[0], b);
    buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative sample number\");"
                  " sp_PolyArray_slice(_t%d, 0, _t%d); })", tn, t, tn);
    { *out = 1; return 1; }
  }
  if (is_quantifier_or_count(name) &&
      argc == 1 && nt_ref(nt, id, "block") < 0) {
    /* poly_array.all?(pat)/one?/any?/none?/count(pat) -- Enumerable's
       pattern form is `pat === element` (Range cover, Regexp match, Class
       is_a, else ==); sp_poly_case_eq folds all of these and NIL (#2366,
       #2960). The receiver and the argument are rooted while the loop
       runs: a user == or === may allocate, and either temporary may be
       the only reference to what it holds. */
    int ta = ++g_tmp, tv = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
    Buf ra = expr_buf(c, recv);
    buf_printf(b, "({ sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);", ta, ra.p ? ra.p : "NULL", ta); free(ra.p);
    buf_printf(b, " sp_RbVal _t%d = ", tv); emit_boxed(c, argv[0], b); buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", tv);
    buf_printf(b, " sp_int _t%d = 0;", tc);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)", ti, ti, ta, ti);
    /* #count is the exception: it counts elements EQUAL to its argument,
       where the predicates match a PATTERN with === (#3817) */
    if (sp_streq(name, "count"))
      buf_printf(b, " if (sp_poly_rb_equal(sp_PolyArray_get(_t%d, _t%d), _t%d)) _t%d++;", ta, ti, tv, tc);
    else
      buf_printf(b, " if (sp_poly_case_eq(_t%d, sp_PolyArray_get(_t%d, _t%d))) _t%d++;", tv, ta, ti, tc);
    if (sp_streq(name, "all?"))        buf_printf(b, " _t%d == sp_PolyArray_length(_t%d); })", tc, ta);
    else if (sp_streq(name, "any?"))   buf_printf(b, " _t%d > 0; })", tc);
    else if (sp_streq(name, "none?"))  buf_printf(b, " _t%d == 0; })", tc);
    else if (sp_streq(name, "one?"))   buf_printf(b, " _t%d == 1; })", tc);
    else                              buf_printf(b, " _t%d; })", tc);
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "count") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
    /* count { |x| cond } on PolyArray */
    int blk = nt_ref(nt, id, "block");
    const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
    int body2 = nt_ref(nt, blk, "body");
    int bn2 = 0; const int *bb2 = body2 >= 0 ? nt_arr(nt, body2, "body", &bn2) : NULL;
    if (bn2 > 0) {
      int trecv = ++g_tmp, tcnt = ++g_tmp, ti = ++g_tmp;
      Buf rb2 = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = %s; ", trecv, rb2.p ? rb2.p : ""); free(rb2.p);
      /* rooted, as the poly find/detect and find_index above already are:
         the length is the loop bound and the block allocates */
      emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
      emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = 0;\n", tcnt);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n",
                 ti, ti, trecv, ti);
      char es_ct[64]; snprintf(es_ct, sizeof es_ct, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
      if (!emit_iter_autosplat(c, blk, rt, es_ct, g_indent + 1) && bp) {
        emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti);
      }
      /* The block value is a condition: route through emit_cond so a poly /
         nil / scalar predicate becomes a valid C truthiness test. */
      Buf vb2 = block_cond_buf(c, blk, bb2, bn2);
      emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (%s) _t%d++;\n", vb2.p ? vb2.p : "0", tcnt);
      free(vb2.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      buf_printf(b, "_t%d", tcnt);
      { *out = 1; return 1; }
    }
  }
  if (sp_streq(name, "fetch") && (argc == 1 || argc == 2)) {
    int blk = fetch_fallback_block(c, id);
    int ta = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp, tnorm = ++g_tmp;
    Buf ra = expr_buf(c, recv);
    /* rooted across the index argument, as the typed-array arm is */
    buf_printf(b, "({ sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);", ta, ra.p ? ra.p : "NULL", ta); free(ra.p);
    buf_printf(b, " sp_int _t%d = ", ti); emit_int_expr(c, argv[0], b); buf_puts(b, ";");
    buf_printf(b, " sp_int _t%d = sp_PolyArray_length(_t%d);", tn, ta);
    buf_printf(b, " sp_int _t%d = _t%d < 0 ? _t%d + _t%d : _t%d;", tnorm, ti, ti, tn, ti);
    buf_printf(b, " (_t%d >= 0 && _t%d < _t%d) ? sp_PolyArray_get(_t%d, _t%d) :", tnorm, tnorm, tn, ta, tnorm);
    if (argc == 2) {
      buf_puts(b, " "); emit_boxed(c, argv[1], b); buf_puts(b, "; })");
    }
    else if (blk >= 0) {
      buf_puts(b, " ({ ");
      emit_fetch_blk_param(c, id, blk, TY_INT, ti, b);
      /* The value with its setup, after the parameter is bound: left in
         g_pre the setup ran ahead of the whole call, on the nil the
         parameter starts as. */
      emit_blk_value_as(c, blk, TY_POLY, b);
      buf_puts(b, "; }); })");
    }
    else {
      /* CRuby's wording, the one the typed arrays and the boxed fetch use */
      buf_printf(b, " (sp_raise_cls(\"IndexError\", sp_sprintf(\"index %%lld outside of array bounds: %%lld...%%lld\", (long long)_t%d, (long long)-_t%d, (long long)_t%d)), sp_box_nil()); })", ti, tn, tn);
    }
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "zip") && argc >= 1 && argc <= 16 && nt_ref(nt, id, "block") < 0) {
    int ta = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tpair = ++g_tmp;
    int tb[16]; TyKind at[16]; int nargs = argc;
    for (int j = 0; j < nargs; j++) {
      tb[j] = ++g_tmp; at[j] = comp_ntype(c, argv[j]);
    }
    Buf ra = expr_buf(c, recv);
    buf_printf(b, "({ sp_PolyArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
    buf_printf(b, " SP_GC_ROOT(_t%d);", ta);   /* see the typed arm; the loop re-reads this length every turn and allocates inside it */
    char rlen[64]; snprintf(rlen, sizeof rlen, "sp_PolyArray_length(_t%d)", ta);
    emit_zip_args(c, argv, nargs, tb, at, rlen, b);
    buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, ta, ti);
    buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new();", tpair);
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, ta, ti);
    for (int j = 0; j < nargs; j++)
      emit_poly_push_elem(b, tpair, at[j], tb[j], ti, zip_pad_box(at[j]), 0);
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));", tr, tpair);
    buf_printf(b, " } _t%d; })", tr);
    { *out = 1; return 1; }
  }
  if ((is_membership_alias(name)) && argc == 1) {
    /* member? is a pure alias of include? for arrays. An empty [] literal
       receiver contains nothing; folding avoids the kind mismatch when the
       literal narrowed to a typed array elsewhere */
    int iel = 0;
    if (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
      int ien = 0; nt_arr(nt, recv, "elements", &ien);
      iel = ien == 0;
    }
    if (iel) {
      buf_puts(b, "((void)("); emit_boxed(c, argv[0], b); buf_puts(b, "), 0)");
      { *out = 1; return 1; }
    }
    Buf rb; int ch = hold_recv_open(c, recv, 0, "sp_PolyArray *", "SP_GC_ROOT", b, &rb);
    buf_printf(b, "sp_PolyArray_include(%s, ", rb.p); free(rb.p);
    emit_boxed(c, argv[0], b); buf_puts(b, ")");
    if (ch) buf_puts(b, "; })");
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "to_s") && argc == 0) {
    emit_null_guarded_call(c, recv, rt, "sp_PolyArray_inspect", "sp_str_empty", b);
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "product") && argc == 1 && a0 == TY_POLY_ARRAY) {
    int ta = ++g_tmp, tb = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp, tpair = ++g_tmp;
    Buf ra; memset(&ra, 0, sizeof ra); Buf rb2; memset(&rb2, 0, sizeof rb2);
    emit_expr(c, recv, &ra); emit_expr(c, argv[0], &rb2);
    buf_printf(b, "({ sp_PolyArray *_t%d = %s; sp_PolyArray *_t%d = %s;",
               ta, ra.p ? ra.p : "NULL", tb, rb2.p ? rb2.p : "NULL");
    free(ra.p); free(rb2.p);
    buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
    buf_printf(b, " sp_PolyArray *_t%d = NULL;", tpair);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, ta, ti);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", tj, tj, tb, tj);
    buf_printf(b, " _t%d = sp_PolyArray_new();", tpair);
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, ta, ti);
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, tb, tj);
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));", tr, tpair);
    buf_printf(b, " } } _t%d; })", tr);
    { *out = 1; return 1; }
  }
  if ((is_map_bang_alias(name)) && nt_ref(nt, id, "block") >= 0) {
    int blk = nt_ref(nt, id, "block");
    const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
    int body = nt_ref(nt, blk, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    if (bn >= 1) {
      int trecv = ++g_tmp, ti = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = %s; ", trecv, rb.p ? rb.p : ""); free(rb.p);
      /* rooted, as the TY_POLY map!/collect! near the top of this file
         already roots its own hoist: the loop stores into the receiver on
         every turn, so an unrooted hoist is a write into freed memory */
      emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", ti, ti, trecv, ti);
      if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
      IterStep st; emit_iter_step_open(c, blk, 1, g_indent + 1, &st);
      int sv = g_indent; g_indent++;
      /* The slot takes a boxed value: a block whose tail is statically
         typed (`[].map! { 0 }`, where the empty receiver leaves nothing to
         widen the tail against) would otherwise put a raw scalar in it. */
      Buf vb; memset(&vb, 0, sizeof vb); emit_iter_step_tail(c, &st, &vb); g_indent = sv;
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "sp_PolyArray_set(_t%d, _t%d, %s);\n", trecv, ti, vb.p ? vb.p : "sp_box_nil()");
      free(vb.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      buf_printf(b, "_t%d", trecv); { *out = 1; return 1; }
    }
  }
  /* select! / filter! / keep_if / reject! / delete_if { |x| cond }: the
     in-place filter (emit_array_filter_loop) */
  if (is_select_bang(name) && nt_ref(nt, id, "block") >= 0) {
    int trecv, torig, twp;
    if (emit_array_filter_loop(c, recv, nt_ref(nt, id, "block"), rt, name, g_pre, g_indent, &trecv, &torig, &twp)) {
      char box[64]; snprintf(box, sizeof box, "sp_box_poly_array(_t%d)", trecv);
      emit_filter_bang_result(name, trecv, torig, twp, box, b);
      { *out = 1; return 1; }
    }
  }
  if (sp_streq(name, "to_h") && argc == 0 && nt_ref(nt, id, "block") < 0) {
    Repr hr = repr_of(c, id);
    const char *hn = ty_hash_cname(hr.as_ty);
    if (!hn) hn = "SymPoly";
    TyKind kty = hr.key, vty = hr.val;
    int tr = ++g_tmp, th = ++g_tmp, ti = ++g_tmp, tp = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = ", tr); emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);", tr, hn, th, hn, th);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, tr, ti);
    /* Each pair is a boxed array whose own kind varies (IntArray for [1,2],
       StrArray for ["a","b"], PolyArray for mixed); sp_poly_arr_get boxes an
       element from any of them, so key/value extraction works regardless. */
    buf_printf(b, " sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);", tp, tr, ti);
    /* every element must be a two-element array; a longer or shorter one
       is an ArgumentError and a non-array a TypeError, where the extra
       elements were simply dropped (#3616). Hash[...] desugars to this
       same emitter but is laxer, so it opts out. */
    if (!nt_int(nt, id, "hash_brackets", 0))
    buf_printf(b, " if (_t%d.tag != SP_TAG_OBJ || !sp_poly_is_array_kind(_t%d.cls_id))"
                  " sp_raise_cls(\"TypeError\", sp_sprintf(\"wrong element type %%s at %%lld (expected array)\","
                  " sp_poly_class_name(_t%d), (long long)_t%d));"
                  " { sp_int _n%d = sp_poly_arr_len(_t%d);"
                  " if (_n%d != 2) sp_raise_cls(\"ArgumentError\","
                  " sp_sprintf(\"wrong array length at %%lld (expected 2, was %%lld)\","
                  " (long long)_t%d, (long long)_n%d)); }",
               tp, tp, tp, ti, tp, tp, tp, ti, tp);
    buf_printf(b, " sp_%sHash_set(_t%d, ", hn, th);
    /* an Integer key or an Integer or Float value is read with the typed
       hash's own check: a typed key or value has no nil (a hash that
       stores one is widened), so another kind -- nil too -- raises */
    char kexpr[128];
    if (kty == TY_SYMBOL)      snprintf(kexpr, sizeof kexpr, "(sp_sym)sp_poly_arr_get(_t%d, 0).v.i", tp);
    else if (kty == TY_STRING) snprintf(kexpr, sizeof kexpr, "sp_poly_arr_get(_t%d, 0).v.s", tp);
    else if (kty == TY_POLY)   snprintf(kexpr, sizeof kexpr, "sp_poly_arr_get(_t%d, 0)", tp);
    else if (kty == TY_INT)    snprintf(kexpr, sizeof kexpr, "sp_poly_hkey_i(sp_poly_arr_get(_t%d, 0))", tp);
    else                       snprintf(kexpr, sizeof kexpr, "sp_poly_arr_get(_t%d, 0).v.i", tp);
    buf_puts(b, kexpr); buf_puts(b, ", ");
    /* value extraction */
    if (vty == TY_POLY)        buf_printf(b, "sp_poly_arr_get(_t%d, 1)", tp);
    else if (vty == TY_INT)    buf_printf(b, "sp_poly_hval_i(sp_poly_arr_get(_t%d, 1))", tp);
    else if (vty == TY_STRING) buf_printf(b, "sp_poly_arr_get(_t%d, 1).v.s", tp);
    else if (vty == TY_FLOAT)  buf_printf(b, "sp_poly_hval_f(sp_poly_arr_get(_t%d, 1))", tp);
    else                       buf_printf(b, "sp_poly_arr_get(_t%d, 1)", tp);
    buf_printf(b, "); } _t%d; })", th);
    { *out = 1; return 1; }
  }
  /* to_h { |x| [k, v] } on a poly array -> a hash keyed by the block's
     literal [k, v] tail pair (the only shape analyze_infer types):
     string/symbol keys get their own hash kind, anything else a fully
     boxed hash (doom: flats.to_h { |f| [f.name, f] }). */
  if (sp_streq(name, "to_h") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
    int blk = nt_ref(nt, id, "block");
    const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
    int body = nt_ref(nt, blk, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    int tail = bn > 0 ? bb[bn - 1] : -1;
    const char *tty = tail >= 0 ? nt_type(nt, tail) : NULL;
    int pairn = 0;
    const int *pair = (tty && sp_streq(tty, "ArrayNode")) ? nt_arr(nt, tail, "elements", &pairn) : NULL;
    const char *hn = pair && pairn == 2 ? ty_hash_cname(res) : NULL;
    if (hn) {
      TyKind kt = comp_ntype(c, pair[0]);
      int trecv = ++g_tmp, ti = ++g_tmp, th = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);\n", trecv, rb.p ? rb.p : "NULL", trecv); free(rb.p);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);\n", hn, th, hn, th);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n",
                 ti, ti, trecv, ti);
      if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
      for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
      int sv = g_indent; g_indent++;
      Buf kb; memset(&kb, 0, sizeof kb);
      if (kt == TY_STRING) {
        /* A TY_STRING slot can carry nil (NULL); a Str-keyed hash can't
           store it (NULL marks an empty bucket and sp_str_hash reads
           k[-1]), so raise instead of segfaulting on a nil key. */
        int tk = ++g_tmp;
        buf_printf(&kb, "({ const char *_t%d = sp_str_dup(", tk);
        emit_expr(c, pair[0], &kb);
        buf_printf(&kb, "); if (!_t%d) sp_raise_cls(\"TypeError\", \"nil key in a string-keyed Hash\"); _t%d; })", tk, tk);
      }
      else if (kt == TY_SYMBOL) emit_expr(c, pair[0], &kb);
      else emit_boxed(c, pair[0], &kb);
      Buf vb; memset(&vb, 0, sizeof vb); emit_boxed(c, pair[1], &vb);
      g_indent = sv;
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "sp_%sHash_set(_t%d, %s, %s);\n", hn, th, kb.p ? kb.p : "", vb.p ? vb.p : "");
      free(kb.p); free(vb.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      buf_printf(b, "_t%d", th);
      { *out = 1; return 1; }
    }
  }
  return 0;
}

/* A kind-named typed Array's block iterators and folds: an index query with a block, find and detect, map!, the in-place filters, the quantifiers, count, sum (emit_kind_array_call's arms, in their order) */
static int emit_kind_array_iter_call(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, const char *k, int block, int *out) {
  if (is_index_query(name) && block >= 0 &&
      emit_array_block_index(c, id, recv, rt, k, name, block, b))
    { *out = 1; return 1; }
  /* find(ifnone) { |x| cond } on a typed array: the element (boxed) or
     the ifnone proc's value on no-match; the result rides poly since the
     proc can return anything. A non-proc ifnone stays a loud reject. */
  if ((is_find_alias(name)) && block >= 0 &&
      argc == 1 && comp_ntype(c, argv[0]) == TY_PROC) {
    const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
    int body = nt_ref(nt, block, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    if (bn >= 1) {
      TyKind et = ty_array_elem(rt);
      int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp, tfn = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
      /* root the receiver: the block body and the ifnone proc run arbitrary
         Ruby that can trigger GC while this array is live (as the poly-array
         find(ifnone) path already does) */
      buf_printf(g_pre, " _t%d = %s; SP_GC_ROOT(_t%d);\n", trecv, rb.p ? rb.p : "", trecv); free(rb.p);
      { Buf nb = expr_buf(c, argv[0]);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_Proc *_t%d = %s; SP_GC_ROOT(_t%d); int _tf%d = 0;\n",
                   tfn, nb.p ? nb.p : "NULL", tfn, tfn); free(nb.p); }
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tres, tres);
      emit_find_loop_head(c, id, k, ti, trecv);
      if (bp) { emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
      for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
      int sv = g_indent; g_indent++;
      Buf cb = expr_buf(c, bb[bn - 1]); g_indent = sv;
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "if (%s) { _t%d = ", cb.p ? cb.p : "0", tres);
      { char eltxt[128];
        if (bp) snprintf(eltxt, sizeof eltxt, "lv_%s", bp);
        else snprintf(eltxt, sizeof eltxt, "sp_%sArray_get(_t%d, _t%d)", k, trecv, ti);
        emit_boxed_text(c, et, eltxt, g_pre); }
      buf_printf(g_pre, "; _tf%d = 1; break; }\n", tfn);
      free(cb.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "if (!_tf%d) _t%d = ((void)sp_proc_call(_t%d, 0, (sp_int[16]){0}), _sp_proc_poly_ret);\n",
                 tfn, tres, tfn);
      buf_printf(b, "_t%d", tres);
      { *out = 1; return 1; }
    }
  }
  /* find / detect { |x| cond } - returns element or nil */
  if ((is_find_alias(name)) && block >= 0 && argc == 0) {
    const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
    int body = nt_ref(nt, block, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    if (bn >= 1) {
      TyKind et = ty_array_elem(rt);
      int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
      buf_printf(g_pre, " _t%d = %s; ", trecv, rb.p ? rb.p : ""); free(rb.p);
      /* rooted, as the find(ifnone) arm above and the poly-array find
         already are: same loop, same per-turn reads, same allocating block */
      emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
      emit_indent(g_pre, g_indent);
      /* an Integer / Float answer is nil when nothing matches: an oint */
      if (oint_kind(et)) buf_printf(g_pre, "%s _t%d = %s;\n", oint_ctype(et), tres, oint_nil(et));
      else {
        emit_ctype(c, et, g_pre);
        if (et == TY_STRING) buf_printf(g_pre, " _t%d = NULL;\n", tres);
        else buf_printf(g_pre, " _t%d = 0;\n", tres);
      }
      emit_find_loop_head(c, id, k, ti, trecv);
      /* Declare the block param in the loop body (not a bare assignment) so
         the find is self-contained: when this call is a parameter default
         hoisted to the call site, the enclosing function has no top-level
         declaration for the block local. Shadows the method-scope slot in
         the ordinary in-body case, which is harmless. */
      if (bp) { emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
      Buf cb; memset(&cb, 0, sizeof cb);
      { IterStep st; emit_iter_step_open(c, block, 1, g_indent + 1, &st);
        int sv = g_indent; g_indent++;
        emit_iter_step_cond(c, &st, 1, &cb); g_indent = sv; }
      emit_indent(g_pre, g_indent + 1);
      if (bp && oint_kind(et)) buf_printf(g_pre, "if (%s) { _t%d = %s(lv_%s); break; }\n", cb.p ? cb.p : "0", tres, oint_of(et), bp);
      else if (bp) buf_printf(g_pre, "if (%s) { _t%d = lv_%s; break; }\n", cb.p ? cb.p : "0", tres, bp);
      else if (oint_kind(et)) buf_printf(g_pre, "if (%s) { _t%d = sp_%sArray_oget(_t%d, _t%d); break; }\n",
                      cb.p ? cb.p : "0", tres, k, trecv, ti);
      else buf_printf(g_pre, "if (%s) { _t%d = sp_%sArray_get(_t%d, _t%d); break; }\n",
                      cb.p ? cb.p : "0", tres, k, trecv, ti);
      free(cb.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      if (oint_kind(et)) { oint_open(c, id, et, b); buf_printf(b, "_t%d", tres); oint_close(c, id, b); }
      else buf_printf(b, "_t%d", tres);
      { *out = 1; return 1; }
    }
  }
  /* map! / collect! { |x| body } - in-place transform, returns receiver */
  if ((is_map_bang_alias(name)) && block >= 0) {
    const char *bp0 = block_param_name(c, block, 0);
    const char *bp = bp0 ? rename_local(bp0) : NULL;
    int body = nt_ref(nt, block, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    if (bn >= 1) {
      TyKind et = ty_array_elem(rt);
      Scope *ms = comp_scope_of(c, block);
      LocalVar *mlv = (ms && bp0) ? scope_local(ms, bp0) : NULL;
      TyKind msaved = mlv ? mlv->type : TY_UNKNOWN;
      if (mlv) { mlv->type = et; for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]); }
      int trecv = ++g_tmp, ti = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
      buf_printf(g_pre, " _t%d = %s; ", trecv, rb.p ? rb.p : ""); free(rb.p);
      /* rooted, as the TY_POLY map!/collect! near the top of this file
         already roots its own hoist: this loop WRITES the block value back
         into the receiver on every turn, so an unrooted hoist is a store
         into freed memory and not only a short walk */
      emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                 ti, ti, k, trecv, ti);
      if (bp) {
        emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre);
        buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti);
      }
      IterStep st; emit_iter_step_open(c, block, 0, g_indent + 1, &st);
      int sv = g_indent; g_indent++;
      Buf vb; memset(&vb, 0, sizeof vb); emit_iter_step_tail(c, &st, &vb); g_indent = sv;
      { const char *msfx = nil_store_sfx(c, k, bb[bn - 1]);
        /* the value first: what it hoists lands in g_pre ahead of the store */
        Buf sv2; memset(&sv2, 0, sizeof sv2);
        /* the _nilable store takes the block value with its nil */
        if (msfx[0]) emit_oint_expr(c, bb[bn - 1], et, &sv2);
        else emit_typed_sink_text(c, bb[bn - 1], et, vb.p ? vb.p : "0", &sv2);
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "sp_%sArray_set%s(_t%d, _t%d, %s);\n", k, msfx, trecv, ti, sv2.p ? sv2.p : "0");
        free(sv2.p); }
      free(vb.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      if (mlv) mlv->type = msaved;
      buf_printf(b, "_t%d", trecv); { *out = 1; return 1; }
    }
  }
  /* select! / filter! / keep_if / reject! / delete_if { |x| cond }: the
     in-place filter, on a typed or a poly array (emit_array_filter_loop) */
  if (is_select_bang(name) && block >= 0) {
    const char *kk = (rt == TY_POLY_ARRAY) ? "Poly" : k;
    int trecv, torig, twp;
    /* --share-strings: over a fresh sharing String Array, the PolyArray of handles */
    if (iter_filter_src_as_handles(c, id, recv) &&
        emit_array_filter_loop_handles(c, recv, block, name, g_pre, g_indent, &trecv, &torig, &twp)) {
      char box[64]; snprintf(box, sizeof box, "sp_box_poly_array(_t%d)", trecv);
      emit_filter_bang_result(name, trecv, torig, twp, box, b);
      { *out = 1; return 1; }
    }
    if (kk && emit_array_filter_loop(c, recv, block, rt, name, g_pre, g_indent, &trecv, &torig, &twp)) {
      char box[64]; snprintf(box, sizeof box, "%s(_t%d)", array_box_fn(kk), trecv);
      emit_filter_bang_result(name, trecv, torig, twp, box, b);
      { *out = 1; return 1; }
    }
  }
  /* array.none?(a..b) / any?/all?/one? with a Range pattern -- membership
     test (===) over an integer array. */
  if (is_quantifier(name) &&
      argc == 1 && nt_ref(nt, id, "block") < 0 &&
      rt == TY_INT_ARRAY && comp_ntype(c, argv[0]) == TY_RANGE) {
    int ta = ++g_tmp, tv = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
    Buf ra = expr_buf(c, recv);
    buf_printf(b, "({ sp_IntArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
    buf_printf(b, " sp_Range _t%d = ", tv); emit_expr(c, argv[0], b); buf_puts(b, ";");
    buf_printf(b, " sp_int _t%d = 0;", tc);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++)", ti, ti, ta, ti);
    buf_printf(b, " if (sp_range_include(&_t%d, sp_IntArray_get(_t%d, _t%d))) _t%d++;", tv, ta, ti, tc);
    if (sp_streq(name, "all?"))       buf_printf(b, " _t%d == sp_IntArray_length(_t%d); })", tc, ta);
    else if (sp_streq(name, "any?"))  buf_printf(b, " _t%d > 0; })", tc);
    else if (sp_streq(name, "none?")) buf_printf(b, " _t%d == 0; })", tc);
    else                              buf_printf(b, " _t%d == 1; })", tc);
    { *out = 1; return 1; }
  }
  /* array.none?(/re/) / any?/all?/one? with a Regexp pattern over strings. */
  if (is_quantifier(name) &&
      argc == 1 && nt_ref(nt, id, "block") < 0 &&
      rt == TY_STR_ARRAY && re_lit_index(c, argv[0]) >= 0) {
    int rei = re_lit_index(c, argv[0]);
    int ta = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
    Buf ra = expr_buf(c, recv);
    buf_printf(b, "({ sp_StrArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
    buf_printf(b, " sp_int _t%d = 0;", tc);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++)", ti, ti, ta, ti);
    buf_printf(b, " if (sp_re_match(sp_re_pat_%d, sp_StrArray_get(_t%d, _t%d)) >= 0) _t%d++;", rei, ta, ti, tc);
    if (sp_streq(name, "all?"))       buf_printf(b, " _t%d == sp_StrArray_length(_t%d); })", tc, ta);
    else if (sp_streq(name, "any?"))  buf_printf(b, " _t%d > 0; })", tc);
    else if (sp_streq(name, "none?")) buf_printf(b, " _t%d == 0; })", tc);
    else                              buf_printf(b, " _t%d == 1; })", tc);
    { *out = 1; return 1; }
  }
  if (is_quantifier_or_count(name) &&
      argc == 1 && nt_ref(nt, id, "block") < 0 &&
      comp_ntype(c, argv[0]) == TY_CLASS) {
    /* A class argument on a TYPED array: the predicates ask `Class === e`,
       which for an int/float/string array is decided by the element type
       alone, and #count asks `e == Class`, which no element of one can
       satisfy. Emitting the class into the element slot did not even
       compile (#3817). */
    int cls_all = 0;
    { const char *cn2 = isa_const_name(nt, argv[0]);
      const char *want = rt == TY_INT_ARRAY ? "Integer" : rt == TY_FLOAT_ARRAY ? "Float"
                       : rt == TY_STR_ARRAY ? "String" : NULL;
      cls_all = (cn2 && want && (sp_streq(cn2, want) || sp_streq(cn2, "Object") ||
                                 sp_streq(cn2, "Comparable") ||
                                 (sp_streq(cn2, "Numeric") &&
                                  (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY)))); }
    int tlen = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = sp_%sArray_length(", tlen, k);
    emit_expr(c, recv, b); buf_puts(b, ");");
    if (sp_streq(name, "count")) buf_printf(b, " (void)_t%d; (sp_int)0; })", tlen);
    else if (sp_streq(name, "all?"))
      buf_printf(b, cls_all ? " (void)_t%d; TRUE; })" : " _t%d == 0; })", tlen);
    else if (sp_streq(name, "any?"))
      buf_printf(b, cls_all ? " _t%d > 0; })" : " (void)_t%d; FALSE; })", tlen);
    else if (sp_streq(name, "none?"))
      buf_printf(b, cls_all ? " _t%d == 0; })" : " (void)_t%d; TRUE; })", tlen);
    else
      buf_printf(b, cls_all ? " _t%d == 1; })" : " (void)_t%d; FALSE; })", tlen);
    { *out = 1; return 1; }
  }
  if (is_quantifier_or_count(name) &&
      argc == 1 && nt_ref(nt, id, "block") < 0) {
    /* array.all?(v)/any?(v)/none?(v)/one?(v)/count(v) -- compare by == */
    int ta = ++g_tmp, tv = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
    /* the hoisted receiver is rooted across the value, which may allocate */
    buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta);
    emit_recv_rooted(c, recv, ta, "SP_GC_ROOT", b);
    emit_indent(g_pre, 0);
    if (value_obj_compares(c, argv[0])) {
      unsupported_feature(c, id, "a user object defining == compared against a typed Array's elements");
      { *out = 0; return 1; }
    }
    if (needle_misses(c, recv, rt, argv[0])) {
      /* a value no element can equal: only an empty array is all? of it */
      buf_puts(b, " (void)("); emit_expr(c, argv[0], b); buf_puts(b, ");");
      if (sp_streq(name, "all?"))       buf_printf(b, " sp_%sArray_length(_t%d) == 0; })", k, ta);
      else if (sp_streq(name, "none?")) buf_puts(b, " 1; })");
      else                              buf_puts(b, " 0; })");
      { *out = 1; return 1; }
    }
    /* A boxed value is compared as Ruby's ==, element boxed: unboxing it
       to the element type raised for a value of another kind, and put an
       sp_RbVal into an sp_int for an Integer array (#4835). */
    TyKind cat = repr_of(c, argv[0]).as_ty;
    int cboxed = (cat == TY_POLY || cat == TY_NIL);
    /* an Integer or Float needle is read as its oint: a nil one counts the
       nil elements */
    int cscalar = !cboxed && elem_nil_sentinel(c, recv, rt);
    if (cboxed) { buf_printf(b, " sp_RbVal _t%d = ", tv); emit_boxed(c, argv[0], b); buf_puts(b, ";"); }
    else if (cscalar) { buf_printf(b, " %s _t%d = ", oint_ctype(ty_array_elem(rt)), tv); emit_elem_needle(c, rt, argv[0], b); buf_puts(b, ";"); }
    else { emit_ctype(c, ty_array_elem(rt), b);
           buf_printf(b, " _t%d = ", tv); emit_expr(c, argv[0], b); buf_puts(b, ";"); }
    buf_printf(b, " sp_int _t%d = 0;", tc);
    if (cboxed) {
      char idx[24], arr[24];
      snprintf(idx, sizeof idx, "_t%d", ti);
      snprintf(arr, sizeof arr, "_t%d", ta);
      /* a literal nil is only there where the array ever held one */
      if (cat == TY_NIL && elem_nil_sentinel(c, recv, rt))
        buf_printf(b, " if (sp_%sArray_may_nil(_t%d))", k, ta);
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)", ti, ti, k, ta, ti);
      buf_puts(b, " if (sp_poly_rb_equal(");
      emit_elem_boxed_text(c, recv, rt, arr, idx, b);
      buf_printf(b, ", _t%d)) _t%d++;", tv, tc);
    }
    else if (cscalar)
      buf_printf(b, " _t%d = sp_%sArray_count_o(_t%d, _t%d);", tc, k, ta, tv);
    else if (rt == TY_STR_ARRAY)
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)"
                    " if (sp_str_eq(sp_%sArray_get(_t%d, _t%d), _t%d)) _t%d++;",
                 ti, ti, k, ta, ti, k, ta, ti, tv, tc);
    else
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)"
                    " if (sp_%sArray_get(_t%d, _t%d) == _t%d) _t%d++;", ti, ti, k, ta, ti, k, ta, ti, tv, tc);
    if (sp_streq(name, "all?"))        buf_printf(b, " _t%d == sp_%sArray_length(_t%d); })", tc, k, ta);
    else if (sp_streq(name, "any?"))   buf_printf(b, " _t%d > 0; })", tc);
    else if (sp_streq(name, "none?"))  buf_printf(b, " _t%d == 0; })", tc);
    else if (sp_streq(name, "one?"))   buf_printf(b, " _t%d == 1; })", tc);
    else                              buf_printf(b, " _t%d; })", tc);
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "count") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
    /* count { |x| cond } -- loop and count truthy block results */
    int blk = nt_ref(nt, id, "block");
    const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
    int body2 = nt_ref(nt, blk, "body");
    int bn2 = 0; const int *bb2 = body2 >= 0 ? nt_arr(nt, body2, "body", &bn2) : NULL;
    if (bn2 > 0) {
      int trecv = ++g_tmp, tcnt = ++g_tmp, ti = ++g_tmp;
      Buf rb2 = expr_buf(c, recv);
      emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
      buf_printf(g_pre, " _t%d = %s; ", trecv, rb2.p ? rb2.p : ""); free(rb2.p);
      /* rooted, as the find(ifnone) arm above already is: the same walk
         over the same hoist, differing only in what it does with the
         predicate */
      emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
      emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = 0;\n", tcnt);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                 ti, ti, k, trecv, ti);
      if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
      /* The block value is a condition: route through emit_cond so a poly /
         nil / scalar predicate becomes a valid C truthiness test (e.g.
         `count(&:alive)` where the element method is poly-dispatched would
         otherwise emit `if (sp_RbVal)` -- a struct in scalar position). */
      Buf vb2 = block_cond_buf(c, blk, bb2, bn2);
      emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (%s) _t%d++;\n", vb2.p ? vb2.p : "0", tcnt);
      free(vb2.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      buf_printf(b, "_t%d", tcnt);
      { *out = 1; return 1; }
    }
  }
  if (sp_streq(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0) {
    TyKind init_t = fold_seed_ntype(c, argv[0]);
    /* promote typed the sum poly: the total can leave the word */
    if (rt == TY_INT_ARRAY && init_t == TY_INT && repr_of(c, id).kind == RK_BOXED) {
      int tf = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", tf); emit_boxed(c, argv[0], b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_IntArray_fold_v(", tf); emit_expr(c, recv, b);
      buf_printf(b, ", _t%d, 0); })", tf);
      { *out = 1; return 1; }
    }
    /* a String initial value concatenates (["a","b"].sum("") == "ab") */
    if (rt == TY_STR_ARRAY && init_t == TY_STRING) {
      emit_poly_sum_seed(c, recv, argv[0], b);
      { *out = 1; return 1; }
    }
    /* a float initial value promotes an integer-array sum to Float: add the
       float init to the integer total in floating point (sp_IntArray_sum
       returns sp_int, so accumulating the init through it would truncate). */
    if (rt == TY_INT_ARRAY && init_t == TY_FLOAT) {
      int tr = ++g_tmp, ts = ++g_tmp;
      buf_puts(b, "({ ");
      tr = hold_operand(c, recv, TY_INT_ARRAY, 0, tr, 1, " ", b);
      ts = hold_operand(c, argv[0], TY_FLOAT, 0, ts, 1, " ", b);
      char rn[32]; snprintf(rn, sizeof rn, "_t%d", tr);
      Buf ck; memset(&ck, 0, sizeof ck);
      const char *ar = nil_sum_ck_text(c, recv, rt, 1, rn, &ck);
      /* The empty sum keeps signed zero. An exact prefix may need a Bignum
         before conversion to Float, just as an Integer-seeded sum does. */
      buf_printf(b, "sp_IntArray_length(_t%d) == 0 ? _t%d : _t%d + ", tr, ts, ts);
      if (g_promote_mode)
        buf_printf(b, "sp_poly_to_f(sp_IntArray_sum_promote(%s, 0))", ar);
      else buf_printf(b, "(sp_float)sp_IntArray_sum(%s, 0)", ar);
      free(ck.p);
      buf_puts(b, "; })");
      { *out = 1; return 1; }
    }
    /* Any other seed keeps its OWN class for the whole fold: CRuby's
       accumulator is the seed object and every step is Ruby's `+` on it. A
       Rational seed therefore answers a Rational total, a Bignum one stops
       wrapping into an sp_int, and nil / a String / an Array reach the raise
       that operator itself produces -- worded for the ELEMENT's class, which
       the hard-coded String-seed raise that used to stand here could only
       get right over Integers. sp_poly_sum_seed runs CRuby's own phases. */
    if (rt == TY_STR_ARRAY || fold_sum_type(init_t, ty_array_elem(rt), g_promote_mode) == TY_POLY) {
      emit_poly_sum_seed(c, recv, argv[0], b);
      { *out = 1; return 1; }
    }
    /* A FLOAT seed over Floats compensates from the first element, as Ruby
       4.0.7 does: `[0.1, 0.2, 0.3].sum(0.0)` is 0.6, like `.sum(0)`. The
       boxed fold draws the same line; the two paths must not disagree. */
    if (rt == TY_FLOAT_ARRAY && init_t == TY_FLOAT) {
      Buf rb; int ch = hold_recv_open(c, recv, 0, "sp_FloatArray *", "SP_GC_ROOT", b, &rb);
      Buf ckb; memset(&ckb, 0, sizeof ckb);
      buf_printf(b, "sp_FloatArray_sum(%s, ", nil_sum_ck_text(c, recv, rt, 1, rb.p, &ckb)); free(rb.p); free(ckb.p);
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      if (ch) buf_puts(b, "; })");
      { *out = 1; return 1; }
    }
    /* the seed may allocate (a method call); the receiver is held across it */
    Buf rsm; char tym[32];
    snprintf(tym, sizeof tym, "sp_%sArray *", k);
    int csm = hold_recv_open(c, recv, 0, tym, "SP_GC_ROOT", b, &rsm);
    Buf ckm; memset(&ckm, 0, sizeof ckm);
    buf_printf(b, "sp_%sArray_sum(%s, ", k, nil_sum_ck_text(c, recv, rt, 0, rsm.p, &ckm)); free(ckm.p);
    if (rt == TY_FLOAT_ARRAY && init_t == TY_INT) {
      buf_puts(b, "(sp_float)("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else {
      emit_expr(c, argv[0], b);
    }
    buf_puts(b, ")");
    free(rsm.p);
    if (csm) buf_puts(b, "; })");
    { *out = 1; return 1; }
  }
  return 0;
}

/* A typed Array receiver with a kind of its own (Int, Float, String, a pointer array): array_kind names it (emit_typed_array_call's arms, in their order) */
static int emit_kind_array_call(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind a0, const char *k, int *out) {
  if (!(k)) return 0;
  if (sp_streq(name, "[]") && argc == 1 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode")) {
    /* arr[a..b] / arr[a...b] -> subarray */
    int rn = argv[0];
    int excl = (int)(nt_int(nt, rn, "flags", 0) & 4) ? 1 : 0;
    int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
    Buf rb; char tys[32]; snprintf(tys, sizeof tys, "sp_%sArray *", k);
    int ch = hold_recv_open(c, recv, 0, tys, "SP_GC_ROOT", b, &rb);
    buf_printf(b, "sp_%sArray_slice_range(%s, ", k, rb.p);
    /* a nil end is the endless Range: the length, so an exclusive `...`
       keeps the last element too (-1 would stop one short) */
    char none_hi[64]; snprintf(none_hi, sizeof none_hi, "sp_%sArray_length(%s)", k, rb.p);
    free(rb.p);
    /* a poly bound (a destructured tuple element, #2923) unboxes here */
    if (lo >= 0) emit_int_expr_bound(c, lo, "0", b); else buf_puts(b, "0");
    buf_puts(b, ", ");
    if (hi >= 0) emit_int_expr_bound(c, hi, none_hi, b); else buf_puts(b, "-1");
    buf_printf(b, ", %d)", hi >= 0 ? excl : 0);
    if (ch) buf_puts(b, "; })");
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "[]") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
    /* arr[range] where the range is a variable/param (a literal RangeNode is
       folded above). Resolve beginless (INTPTR_MIN), endless (INTPTR_MAX),
       and negative endpoints against the length, then slice -- a start
       outside [-len, len] is nil, matching Array#[]. */
    int ta = ++g_tmp, tr = ++g_tmp, tf = ++g_tmp, tl = ++g_tmp, tn = ++g_tmp;
    /* rooted across the range, whose bounds may allocate */
    buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_recv_rooted(c, recv, ta, "SP_GC_ROOT", b);
    buf_printf(b, "sp_Range _t%d = sp_range_ix(", tr); emit_expr(c, argv[0], b); buf_puts(b, ")");
    buf_printf(b, "; sp_int _t%d = sp_%sArray_length(_t%d);", tn, k, ta);
    buf_printf(b, " sp_int _t%d = _t%d.nobeg ? 0 :"
                  " (_t%d.first < 0 ? _t%d.first + _t%d : _t%d.first);",
               tf, tr, tr, tr, tn, tr);
    buf_printf(b, " sp_int _t%d = _t%d.noend ? _t%d - _t%d :"
                  " ((_t%d.last < 0 ? _t%d.last + _t%d : _t%d.last) - _t%d + (_t%d.excl ? 0 : 1));",
               tl, tr, tn, tf, tr, tr, tn, tr, tf, tr);
    /* a start before the array (`first < -len`, so the resolved `_tf` is
       still negative) or past its end (`_tf > len`) is nil in Ruby, not a
       clamped slice; `_tf == len` is the empty slice, which slice() yields. */
    buf_printf(b, " (_t%d < 0 || _t%d > _t%d) ? (sp_%sArray *)0 : sp_%sArray_slice(_t%d, _t%d, _t%d); })",
               tf, tf, tn, k, k, ta, tf, tl);
    { *out = 1; return 1; }
  }
  if ((is_element_at_alias(name)) && argc == 1) {
    /* an array whose header the loop being emitted holds (hc_array):
       in range, read the element there; anything else, the get */
    char hd[48], hl[48], hw[48];
    if ((rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY) && (comp_ntype(c, argv[0]) == TY_INT || hc_promoted_int(c, argv[0])) &&
        hc_array(c, recv, rt == TY_FLOAT_ARRAY, hd, hl, hw, sizeof hd)) {
      /* a read that can miss or meet a nil element is an oint (_oget); one
         the analysis proved in range and non-nil reads the plain element */
      TyKind ek = rt == TY_INT_ARRAY ? TY_INT : TY_FLOAT;
      int oread = node_is_oint(c, id);
      /* in range by the loop's own test: the cached element itself -- lifted
         where the node answers an oint, which an array whose elements are
         not marked nil-capable never holds (the analysis did not see the
         loop's bound that the header cache proves) */
      if (hc_index_in_range(c, recv, argv[0]) && (!oread || !repr_of(c, recv).elem_nil_marked)) {
        if (oread) buf_printf(b, "%s(", oint_of(ek));
        buf_printf(b, "%s[", hd); emit_int_expr(c, argv[0], b); buf_puts(b, "]");
        if (oread) buf_puts(b, ")");
        { *out = 1; return 1; }
      }
      if (repr_of(c, recv).nil_cold) {
        /* the receiver may be nil: tested where a nil one goes, ahead of
           the index's own nil test, which a nil index also reaches */
        int tk = ++g_tmp;
        /* a nil-capable index stays an oint until the receiver is tested:
           a nil receiver's NoMethodError comes before the nil index's
           TypeError, as CRuby calls [] on the receiver first */
        int io = node_has_oint_form(c, argv[0]);
        if (io) {
          buf_printf(b, "({ sp_oint _o%d = ", tk); emit_oint_expr(c, argv[0], TY_INT, b);
          buf_printf(b, "; sp_int _t%d = _o%d.v", tk, tk);
        }
        else {
          buf_printf(b, "({ sp_int _t%d = ", tk);
          (void)emit_int_index_raw(c, argv[0], b);
        }
        const char *inil = "";
        char inb[32]; if (io) { snprintf(inb, sizeof inb, "!_o%d.nil && ", tk); inil = inb; }
        /* an oint read below the header's nil-free length (_hcr) is the
           element, with no test of its own; the rest take the oget */
        char hr[48]; hc_read_len(hd, hr, sizeof hr);
        if (oread) buf_printf(b, "; SP_LIKELY(%s(unsigned long long)_t%d < (unsigned long long)%s) ? %s(%s[_t%d]) : ({ ", inil, tk, hr, oint_of(ek), hd, tk);
        else buf_printf(b, "; SP_LIKELY(%s(unsigned long long)_t%d < (unsigned long long)%s) ? %s[_t%d] : ({ ", inil, tk, hl, hd, tk);
        emit_nil_cold_test(c, id, recv, b);
        buf_puts(b, " ");
        buf_printf(b, "sp_%sArray_%s(", k, oread ? "oget" : "get");
        emit_expr(c, recv, b);
        if (io) buf_printf(b, ", sp_oint_arg(_o%d)); }); })", tk);
        else buf_printf(b, ", _t%d); }); })", tk);
        { *out = 1; return 1; }
      }
      int tk = ++g_tmp;
      buf_printf(b, "({ sp_int _t%d = ", tk);
      (void)emit_int_index_raw(c, argv[0], b);
      /* unwrapped at once by its consumer (g_ck_node): the plain cached
         element, else the checked read raising the unwrap's error */
      if (oread && g_ck_node == id) {
        char hr[48]; hc_read_len(hd, hr, sizeof hr);
        buf_printf(b, "; SP_LIKELY((unsigned long long)_t%d < (unsigned long long)%s) ? %s[_t%d] : sp_%sArray_get_%s(",
                   tk, hr, hd, tk, k, g_ck_op == g_ck_opnd ? "opnd" : g_ck_op ? "ck" : "arg");
        emit_expr(c, recv, b);
        if (g_ck_op && g_ck_op != g_ck_opnd) buf_printf(b, ", _t%d, \"%s\"); })", tk, g_ck_op);
        else buf_printf(b, ", _t%d); })", tk);
        g_ck_done = 1;
        { *out = 1; return 1; }
      }
      if (oread) {
        char hr[48]; hc_read_len(hd, hr, sizeof hr);
        buf_printf(b, "; SP_LIKELY((unsigned long long)_t%d < (unsigned long long)%s) ? %s(%s[_t%d]) : ", tk, hr, oint_of(ek), hd, tk);
      }
      else buf_printf(b, "; SP_LIKELY((unsigned long long)_t%d < (unsigned long long)%s) ? %s[_t%d] : ", tk, hl, hd, tk);
      buf_printf(b, "sp_%sArray_%s(", k, oread ? "oget" : "get");
      emit_expr(c, recv, b);
      buf_printf(b, ", _t%d); })", tk);
      { *out = 1; return 1; }
    }
    int uo = (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY) && node_is_oint(c, id);
    /* an oint unwrapped at once by its consumer (g_ck_node): the checked read */
    int uck = uo && g_ck_node == id;
    const char *uop = g_ck_op == g_ck_opnd ? NULL : g_ck_op;
    buf_printf(b, "sp_%sArray_%s(", k, uck ? (g_ck_op == g_ck_opnd ? "get_opnd" : uop ? "get_ck" : "get_arg") : uo ? "oget" : "get");
    if (uck) g_ck_done = 1;
    emit_expr(c, recv, b); buf_puts(b, ", ");
    /* a splat is its one element (emit_int_expr_ex), not a boxed index */
    if (repr_of(c, argv[0]).kind == RK_BOXED && nt_kind(nt, argv[0]) != NK_SplatNode) {
      /* a checked conversion, not a raw `.v.i`: the union read assumed
         the box held an Integer, so a boxed user object indexed by its
         pointer bits and the read answered a wrong element in silence;
         and not sp_poly_to_i either, which read a String or a Symbol as
         0 where CRuby raises TypeError. */
      buf_puts(b, "sp_poly_arg_int_chk(");
      emit_expr(c, argv[0], b);
      buf_puts(b, ")");
    }
else {
      /* emit_int_expr, not raw: an unresolved-constant index lowers to a
         NameError raise whose C value is an sp_Class, which the int slot
         rejects at C compile time. */
      emit_int_expr(c, argv[0], b);
    }
    if (uck && uop) buf_printf(b, ", \"%s\"", uop);
    buf_puts(b, ")");
    { *out = 1; return 1; }
  }
  /* concat(*arrays) as a value: append in place, evaluate to the
     receiver (the mutating statement form lives in emit_array_mutate_stmt).
     Same-kind arguments only; a differently-typed argument has already
     widened the receiver to poly in inference. */
  if (sp_streq(name, "concat") && argc >= 1) {
    int same = 1, all_poly = 1;
    for (int j = 0; j < argc; j++) {
      /* Ask the SLOT, not the node: a block parameter's node type can
         read as the element kind while its declaration is boxed, and
         emitting the boxed local into a typed pointer does not compile
         (#3850). */
      Repr ar = repr_of(c, argv[j]);
      if (nt_type(nt, argv[j]) && sp_streq(nt_type(nt, argv[j]), "LocalVariableReadNode")) {
        Scope *asc = comp_scope_of(c, argv[j]);
        LocalVar *alv = asc ? scope_local(asc, nt_str(nt, argv[j], "name")) : NULL;
        if (alv && alv->type != TY_UNKNOWN) ar = repr_of_slot(c, alv);
      }
      if (ar.elem != ty_array_elem(rt)) same = 0;
      if (ar.kind != RK_BOXED && ar.elem != TY_POLY && ar.elem != ty_array_elem(rt)) all_poly = 0;
    }
    /* A boxed argument -- an element read out of a poly array, which is
       what `g.each_with_object([]) { |r, acc| acc.concat(r) }` hands it --
       is an array at run time; reading it as this array's C type did not
       compile (#3850). Append its elements through the boxed surface. A
       poly array argument takes the same path: `[2].concat([1, nil])`
       as a value had no arm and raised NoMethodError. An Integer or
       Float element goes through sp_poly_elem_i / _f, so a nil lands as
       the slot's nil bit rather than 0. */
    if (!same && all_poly && k) {
      int ta = ++g_tmp;
      Buf ra = expr_buf(c, recv);
      buf_printf(b, "({ sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);", k, ta, ra.p ? ra.p : "NULL", ta);
      free(ra.p);
      int vbase = g_tmp + 1; g_tmp += 2 * argc;   /* each value, then its length */
      for (int j = 0; j < argc; j++) {
        int tv = vbase + 2 * j, tn = tv + 1;
        buf_printf(b, " sp_RbVal _t%d = ", tv); emit_boxed(c, argv[j], b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", tv);
        buf_printf(b, " sp_int _t%d = sp_poly_length(_t%d);", tn, tv);
      }
      for (int j = 0; j < argc; j++) {
        int tv = vbase + 2 * j, tn = tv + 1, ti = ++g_tmp;
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)", ti, ti, tn, ti);
        buf_printf(b, " sp_%sArray_push%s(_t%d, ", k, nil_store_sfx(c, k, NIL_STORE_BOXED), ta);
        { char el[64]; snprintf(el, sizeof el, "sp_poly_each_elem(_t%d, _t%d)", tv, ti);
          TyKind et = ty_array_elem(rt);
          if (et == TY_INT) buf_printf(b, "sp_poly_elem_i(%s)", el);
          else if (et == TY_FLOAT) buf_printf(b, "sp_poly_elem_f(%s)", el);
          else emit_unbox_text(c, et, el, b); }
        buf_puts(b, ");");
      }
      buf_printf(b, " _t%d; })", ta);
      { *out = 1; return 1; }
    }
    if (same) {
      int ta = ++g_tmp;
      Buf ra = expr_buf(c, recv);
      buf_printf(b, "({ sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);", k, ta, ra.p ? ra.p : "NULL", ta);
      free(ra.p);
      /* every argument and its length first: one aliasing the receiver is
         appended as it was, not as an earlier append grew it (CRuby) */
      int base = g_tmp + 1; g_tmp += argc;
      int lbase = g_tmp + 1; g_tmp += argc;
      for (int j = 0; j < argc; j++) {
        buf_printf(b, " sp_%sArray *_t%d = ", k, base + j);
        emit_expr(c, argv[j], b);
        buf_printf(b, "; SP_GC_ROOT(_t%d);", base + j);
      }
      for (int j = 0; j < argc; j++)
        buf_printf(b, " sp_int _t%d = sp_%sArray_length(_t%d);", lbase + j, k, base + j);
      for (int j = 0; j < argc; j++) {
        int ii = ++g_tmp, sn = lbase + j;
        /* the appended elements carry their array's nils */
        int onum = rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY;
        buf_printf(b, " { for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)"
                      " sp_%sArray_push%s(_t%d, sp_%sArray_%s(_t%d, _t%d)); }",
                   ii, ii, sn, ii, k, onum ? "_o" : "", ta, k, onum ? "oget" : "get", base + j, ii);
      }
      buf_printf(b, " _t%d; })", ta);
      { *out = 1; return 1; }
    }
  }
  if (sp_streq(name, "fetch") && (argc == 1 || argc == 2)) {
    int blk = fetch_fallback_block(c, id);
    TyKind et = ty_array_elem(rt);
    /* the whole expression's inferred type: poly when the default (or
       block value) type differs from the element type -- box both arms */
    TyKind ft = repr_of(c, id).as_ty;
    int boxed = ft != et;
    int ta = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp, tnorm = ++g_tmp;
    Buf ra = expr_buf(c, recv);
    /* The receiver is rooted across the index argument: a method's
       return or a chain is held by nothing else, and an index that
       allocates let a collection hand the array's slot on before the
       length and the element were read. */
    buf_printf(b, "({ sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);", k, ta, ra.p ? ra.p : "NULL", ta); free(ra.p);
    buf_printf(b, " sp_int _t%d = ", ti); emit_int_expr(c, argv[0], b); buf_puts(b, ";");
    buf_printf(b, " sp_int _t%d = sp_%sArray_length(_t%d);", tn, k, ta);
    buf_printf(b, " sp_int _t%d = _t%d < 0 ? _t%d + _t%d : _t%d;", tnorm, ti, ti, tn, ti);
    buf_printf(b, " (_t%d >= 0 && _t%d < _t%d) ? ", tnorm, tnorm, tn);
    /* an Integer or Float answer whose slot holds its nil (a default that
       can be nil: `fetch(*idx)`) is the oint: the element lifted, the
       default in its own oint form */
    int fo = !boxed && oint_kind(et) && node_is_oint(c, id);
    if (boxed) {
      char getexpr[96];
      snprintf(getexpr, sizeof getexpr, "sp_%sArray_get(_t%d, _t%d)", k, ta, tnorm);
      emit_boxed_text(c, et, getexpr, b);
    }
    else if (fo) buf_printf(b, "%s(sp_%sArray_get(_t%d, _t%d))", oint_of(et), k, ta, tnorm);
    else buf_printf(b, "sp_%sArray_get(_t%d, _t%d)", k, ta, tnorm);
    buf_puts(b, " :");
    if (argc == 2) {
      buf_puts(b, " ");
      if (boxed && repr_of(c, argv[1]).kind != RK_BOXED) emit_boxed(c, argv[1], b);
      else if (fo) emit_oint_expr(c, argv[1], et, b);
      else emit_expr(c, argv[1], b);
      buf_puts(b, "; })");
    }
    else if (blk >= 0) {
      /* fetch(i) { |i| default }: an out-of-bounds index yields the
         (original) index to the block; its value is the result */
      buf_puts(b, " ({ ");
      emit_fetch_blk_param(c, id, blk, TY_INT, ti, b);
      /* the value with its setup after the parameter is bound, as above */
      if (fo) emit_blk_value_as_o(c, blk, et, b);
      else emit_blk_value_as(c, blk, boxed ? TY_POLY : et, b);
      buf_puts(b, "; }); })");
    }
    else {
      /* CRuby's message names the index and the valid bounds */
      buf_printf(b, " (sp_raise_cls(\"IndexError\","
                    " sp_sprintf(\"index %%lld outside of array bounds: -%%lld...%%lld\","
                    " (long long)_t%d, (long long)_t%d, (long long)_t%d)), %s); })",
                 ti, tn, tn, boxed ? "sp_box_nil()" : fo ? oint_nil(et) : default_value_from_compiler(c, et));
    }
    { *out = 1; return 1; }
  }
  if ((is_prepend_alias(name)) && argc >= 1) {
    int t = ++g_tmp;
    /* the hoisted receiver is rooted across its arguments unless a slot
       read with slot-keeping arguments already holds it (push_recv_in_slot) */
    int held = push_recv_in_slot(c, recv, argc, argv, rt);
    if (rt == TY_INT_ARRAY) {
      buf_printf(b, "({ sp_IntArray *_t%d = ", t);
      if (held) { emit_expr(c, recv, b); buf_puts(b, ";"); }
      else emit_recv_rooted(c, recv, t, "SP_GC_ROOT", b);
      /* the arguments left to right into temporaries, then prepended in
         reverse, as the Float branch below does */
      /* a value that can be nil travels as its oint into the _nilable store */
      for (int a = 0; a < argc; a++) {
        if (nil_store_sfx(c, "Int", argv[a])[0]) { buf_printf(b, " sp_oint _u%d_%d = ", t, a); emit_elem_store_value(c, "Int", argv[a], b); }
        else { buf_printf(b, " sp_int _u%d_%d = ", t, a); emit_typed_elem_value(c, argv[a], TY_INT, b); }
        buf_puts(b, ";");
      }
      for (int a = argc - 1; a >= 0; a--) {
        buf_printf(b, " sp_IntArray_unshift%s(_t%d, _u%d_%d);", nil_store_sfx(c, "Int", argv[a]), t, t, a);
      }
    }
    else if (rt == TY_STR_ARRAY) {
      buf_printf(b, "({ sp_StrArray *_t%d = ", t);
      if (held) { emit_expr(c, recv, b); buf_puts(b, ";"); }
      else emit_recv_rooted(c, recv, t, "SP_GC_ROOT", b);
      /* every argument before the first insert, each rooted across the
         later ones, then inserted in order */
      for (int a = 0; a < argc; a++) {
        buf_printf(b, " const char *_u%d_%d = ", t, a); emit_typed_elem_value(c, argv[a], TY_STRING, b);
        buf_printf(b, "; SP_GC_ROOT_STR(_u%d_%d);", t, a);
      }
      for (int a = 0; a < argc; a++) {
        buf_printf(b, " sp_StrArray_insert(_t%d, %d, _u%d_%d);", t, a, t, a);
      }
    }
    else {
      /* FloatArray (the only other element kind that reaches this typed
         dispatch; poly arrays route elsewhere). Evaluate the arguments
         left to right into temporaries (Ruby's argument-evaluation order),
         then prepend them in reverse so a multi-arg unshift keeps order. */
      buf_printf(b, "({ sp_FloatArray *_t%d = ", t);
      if (held) { emit_expr(c, recv, b); buf_puts(b, ";"); }
      else emit_recv_rooted(c, recv, t, "SP_GC_ROOT", b);
      for (int a = 0; a < argc; a++) {
        if (nil_store_sfx(c, "Float", argv[a])[0]) { buf_printf(b, " sp_ofloat _u%d_%d = ", t, a); emit_elem_store_value(c, "Float", argv[a], b); }
        else { buf_printf(b, " sp_float _u%d_%d = ", t, a); emit_typed_elem_value(c, argv[a], TY_FLOAT, b); }
        buf_puts(b, ";");
      }
      for (int a = argc - 1; a >= 0; a--) {
        buf_printf(b, " sp_FloatArray_unshift%s(_t%d, _u%d_%d);", nil_store_sfx(c, "Float", argv[a]), t, t, a);
      }
    }
    buf_printf(b, " _t%d; })", t);
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "clone") && argc == 1 && nt_type(nt, argv[0]) &&
      sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) {
    /* a real copy: arrays are mutable, so clone must not alias. The
       freeze: keyword forces the frozen flag, or (nil) carries the
       receiver's over; the plain dup and clone are builtin-op rows. */
    int fz = -1;  /* -1: not a recognized keyword; -2: copy receiver's flag; 0/1: forced */
    int fv = kwh_lookup(nt, argv[0], "freeze");
    const char *fvt = fv >= 0 ? nt_type(nt, fv) : NULL;
    if (fvt && sp_streq(fvt, "FalseNode")) fz = 0;
    else if (fvt && sp_streq(fvt, "TrueNode")) fz = 1;
    else if (fvt && sp_streq(fvt, "NilNode")) fz = -2;
    if (fz != -1) {
      int ts = ++g_tmp, td = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, ts); emit_expr(c, recv, b);
      buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_dup(_t%d); ", k, td, k, ts);
      if (fz == -2) buf_printf(b, "_t%d->frozen = _t%d ? _t%d->frozen : 0; ", td, ts, ts);
      else buf_printf(b, "_t%d->frozen = %d; ", td, fz);
      buf_printf(b, "_t%d; })", td);
      { *out = 1; return 1; }
    }
  }
  if (sp_streq(name, "zip") && argc >= 1 && argc <= 16 && nt_ref(nt, id, "block") < 0) {
    /* recv.zip(b, c...) → [[recv[0],b[0],c[0],...], ...] as PolyArray of PolyArrays */
    int ta = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tpair = ++g_tmp;
    int tb[16]; TyKind at[16]; int nargs = argc;
    for (int j = 0; j < nargs; j++) {
      tb[j] = ++g_tmp; at[j] = comp_ntype(c, argv[j]);
    }
    const char *ka = (rt == TY_POLY_ARRAY) ? "Poly" : k;
    buf_printf(b, "({ sp_%sArray *_t%d = ", ka, ta); emit_expr(c, recv, b); buf_puts(b, ";");
    /* The loop below re-reads this length on every turn and allocates a
       pair inside it, so a receiver nothing else holds -- the array a
       method answered -- could be collected mid-walk, and so could each
       argument, materialized here and read on every row. Same rule the
       builtin loops follow (#4367, #4369, #4370). */
    buf_printf(b, " SP_GC_ROOT(_t%d);", ta);
    char rlen[64]; snprintf(rlen, sizeof rlen, "sp_%sArray_length(_t%d)", ka, ta);
    emit_zip_args(c, argv, nargs, tb, at, rlen, b);
    int tnf; const char *rbox = typed_elem_box_or_null(c, recv, rt, ta, &tnf, b);
    buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {",
               ti, ti, ka, ta, ti);
    buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new();", tpair);
    emit_poly_push_elem(b, tpair, rt, ta, ti, rbox, tnf);
    for (int j = 0; j < nargs; j++)
      emit_poly_push_elem(b, tpair, at[j], tb[j], ti, zip_pad_box(at[j]), 0);
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));", tr, tpair);
    buf_printf(b, " } _t%d; })", tr);
    { *out = 1; return 1; }
  }
  /* product(other) { |pair| }: yield each pair to the block, evaluate to
     the receiver (CRuby returns self) */
  if (sp_streq(name, "product") && argc == 1 && nt_ref(nt, id, "block") >= 0) {
    int blk = nt_ref(nt, id, "block");
    /* an empty `[]` argument has no element type of its own and would emit
       as the int-array default, which this arm then reads as the poly array
       it dispatches on (#3975 sweep): a view, for this arm's emission */
    int pv = -1;
    if (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ArrayNode")) {
      int aen = 0; nt_arr(nt, argv[0], "elements", &aen);
      if (aen == 0 && (repr_of(c, argv[0]).untyped ||
                       ty_is_array(comp_ntype(c, argv[0]))))
        pv = view_push(c, argv[0], TY_POLY_ARRAY);
    }
    TyKind at = comp_ntype(c, argv[0]);
    Buf ra; memset(&ra, 0, sizeof ra);
    emit_expr(c, recv, &ra);  /* the receiver's prelude first, as Ruby evaluates */
    Buf rb2; memset(&rb2, 0, sizeof rb2);
    at = emit_product_operand(c, argv[0], at, &rb2);
    const char *kb = (at == TY_POLY_ARRAY) ? "Poly" : (array_kind(at) ? array_kind(at) : "Poly");
    int bbody = nt_ref(nt, blk, "body");
    int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
    const char *fp0 = block_param_name(c, blk, 0);
    int ta = ++g_tmp, tb = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp, tpair = ++g_tmp;
    buf_printf(b, "({ sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d); sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);",
               k, ta, ra.p ? ra.p : "NULL", ta, kb, tb, rb2.p ? rb2.p : "NULL", tb);
    free(ra.p); free(rb2.p);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", ti, ti, k, ta, ti);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", tj, tj, kb, tb, tj);
    buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tpair, tpair);
    char e1[96], e2[96];
    snprintf(e1, sizeof e1, "sp_%sArray_get(_t%d, _t%d)", k, ta, ti);
    snprintf(e2, sizeof e2, "sp_%sArray_get(_t%d, _t%d)", kb, tb, tj);
    buf_printf(b, " sp_PolyArray_push(_t%d, ", tpair);
    emit_boxed_text(c, ty_array_elem(rt), e1, b);
    buf_printf(b, "); sp_PolyArray_push(_t%d, ", tpair);
    emit_boxed_text(c, ty_array_elem(at), e2, b);
    buf_puts(b, ");");
    char tsrc[48]; snprintf(tsrc, sizeof tsrc, "sp_box_poly_array(_t%d)", tpair);
    if (emit_tuple_block_params(c, id, blk, tsrc, b)) { }
    else if (fp0) buf_printf(b, " lv_%s = %s;", rename_local(fp0), tsrc);
    buf_puts(b, " {");
    emit_iter_step_body(c, blk, b, 0);
    buf_printf(b, " } } } _t%d; })", ta);
    if (pv >= 0) view_pop(c, pv);
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "product") && argc == 1) {
    TyKind at = comp_ntype(c, argv[0]);
    Buf ra; memset(&ra, 0, sizeof ra);
    emit_expr(c, recv, &ra);  /* the receiver's prelude first, as Ruby evaluates */
    Buf rb2; memset(&rb2, 0, sizeof rb2);
    at = emit_product_operand(c, argv[0], at, &rb2);
    const char *kb = (at == TY_POLY_ARRAY) ? "Poly" : (array_kind(at) ? array_kind(at) : "Poly");
    int ta = ++g_tmp, tb = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp, tpair = ++g_tmp;
    buf_printf(b, "({ sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d); sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);",
               k, ta, ra.p ? ra.p : "NULL", ta, kb, tb, rb2.p ? rb2.p : "NULL", tb);
    free(ra.p); free(rb2.p);
    int tnfa, tnfb;
    const char *boxa = typed_elem_box_or_null(c, recv, rt, ta, &tnfa, b), *boxb = typed_elem_box_or_null(c, argv[0], at, tb, &tnfb, b);
    buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
    buf_printf(b, " sp_PolyArray *_t%d = NULL;", tpair);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", ti, ti, k, ta, ti);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", tj, tj, kb, tb, tj);
    buf_printf(b, " _t%d = sp_PolyArray_new();", tpair);
    emit_poly_push_elem(b, tpair, rt, ta, ti, boxa, tnfa);
    emit_poly_push_elem(b, tpair, at, tb, tj, boxb, tnfb);
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));", tr, tpair);
    buf_printf(b, " } } _t%d; })", tr);
    { *out = 1; return 1; }
  }
  /* concat in VALUE position with a source of another kind (a general
     Array read at run time, another typed kind): the statement emitter
     owns the per-kind element loop, so run it inside a compound whose
     value is the receiver. A same-kind source keeps its own arm below.
     Reached since a typed parameter no longer widens under such a
     concat (#4481); before, the receiver was a general Array here. */
  if (sp_streq(name, "concat") && argc >= 1 &&
      nt_kind(nt, recv) == NK_LocalVariableReadNode) {
    int other = 0;
    for (int ai = 0; ai < argc; ai++) {
      TyKind at = comp_ntype(c, argv[ai]);
      if (at == TY_POLY_ARRAY || at == TY_POLY || (ty_is_array(at) && at != rt)) other = 1;
    }
    if (other) {
      buf_puts(b, "({ ");
      emit_array_mutate_stmt(c, id, b, 0);
      buf_puts(b, " "); emit_expr(c, recv, b); buf_puts(b, "; })");
      { *out = 1; return 1; }
    }
  }
  if (sp_streq(name, "insert") && argc >= 2 && (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY || rt == TY_STR_ARRAY)) {
    /* insert(i, v1, v2, ...): normalize a negative index ONCE against the
       pre-insert length (per-element normalization would drift as the
       array grows), then insert consecutively. */
    int t = ++g_tmp, ti2 = ++g_tmp, to2 = ++g_tmp;
    buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
    /* rooted across the index, as the poly arm and take/drop are */
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_int _t%d = ", t, ti2); emit_int_expr(c, argv[0], b);
    /* normalize ONCE, keeping the too-negative IndexError the runtime
       helper would have raised (it must not see a pre-added index) */
    buf_printf(b, "; sp_int _t%d = _t%d; if (_t%d < 0) { _t%d += (_t%d ? _t%d->len : 0) + 1;"
                  " if (_t%d < 0) sp_raise_cls(\"IndexError\","
                  " sp_sprintf(\"index %%lld too small for array; minimum: %%lld\","
                  " (long long)_t%d, (long long)(-((_t%d ? _t%d->len : 0) + 1)))); }",
               to2, ti2, ti2, ti2, t, t, ti2, to2, t, t);
    for (int a2 = 1; a2 < argc; a2++) {
      const char *isfx = nil_store_sfx(c, k, argv[a2]);
      buf_printf(b, " sp_%sArray_insert%s(_t%d, _t%d + %d, ", k, isfx, t, ti2, a2 - 1);
      /* the _nilable store takes the value with its nil */
      if (isfx[0]) emit_elem_store_value(c, k, argv[a2], b);
      else emit_typed_elem_value(c, argv[a2], ty_array_elem(rt), b);
      buf_puts(b, ");");
    }
    buf_printf(b, " _t%d; })", t);
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "delete") && argc == 1 &&
      (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY || rt == TY_FLOAT_ARRAY)) {
    /* A Float receiver classifies its needle here (the delete arm's form
       of elem_mismatch): only an Integer or Float compares. A Rational or
       a bare {} needle broke the C build inside emit_float_expr, and a
       boxed needle rides the tag-guarded helper instead of a lenient
       to-f coercion. Bignum, Rational and Complex miss where CRuby's ==
       can match -- the Int arm fails the C build on those same shapes. */
    int df_boxed = rt == TY_FLOAT_ARRAY && (a0 == TY_POLY || a0 == TY_UNKNOWN);
    int dnil = elem_nil_sentinel(c, recv, rt);
    int df_never = rt == TY_FLOAT_ARRAY && !df_boxed && a0 != TY_INT && a0 != TY_FLOAT &&
                   !(a0 == TY_NIL && dnil);
    int dblk = nt_ref(nt, id, "block");
    if (dblk >= 0 && nt_type(nt, dblk) && sp_streq(nt_type(nt, dblk), "BlockNode")) {
      int dbody = nt_ref(nt, dblk, "body");
      int dbn = 0; const int *dbb = dbody >= 0 ? nt_arr(nt, dbody, "body", &dbn) : NULL;
      if (value_obj_compares(c, argv[0])) {
        unsupported_feature(c, id, "Array#delete of a user object defining == from a typed Array");
        { *out = 0; return 1; }
      }
      if (dbn >= 1 && (df_never || needle_misses(c, recv, rt, argv[0]))) {
        /* nothing to delete: the block, handed the value, supplies the answer */
        const char *dp0 = block_param_name(c, dblk, 0);
        buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); ");
        Buf nbind; memset(&nbind, 0, sizeof nbind);
        if (dp0 && block_param_is_boxed(c, dblk, id, dp0)) { buf_printf(&nbind, "lv_%s = ", rename_local(dp0)); emit_boxed(c, argv[0], &nbind); buf_puts(&nbind, "; "); }
        else { buf_puts(b, "(void)("); emit_expr(c, argv[0], b); buf_puts(b, "); "); }
        emit_fallback_block_value(c, dbb, dbn, nbind.p, 1, "sp_box_nil()", 0, b);
        free(nbind.p);
        buf_puts(b, "; })");
        { *out = 1; return 1; }
      }
      if (dbn >= 1) {
        /* the block form holds the receiver across the value as the
           plain form below does */
        int tdr = ++g_tmp;
        /* the parameter is the value the call was given; on a miss it is
           read again, so a value that runs code has nowhere to be kept --
           which only matters to a block that reads the parameter */
        const char *dpb = block_param_name(c, dblk, 0);
        Buf dpbind; memset(&dpbind, 0, sizeof dpbind);
        char held[32] = "";   /* the value boxed once, when it runs code */
        if (dpb && subtree_reads_local(nt, dbody, dpb) && block_param_is_boxed(c, dblk, id, dpb)) {
          if (subtree_has_side_effect(c, argv[0])) snprintf(held, sizeof held, "_t%d", ++g_tmp);
          buf_printf(&dpbind, "lv_%s = ", rename_local(dpb));
          if (held[0]) buf_puts(&dpbind, held); else emit_boxed(c, argv[0], &dpbind);
          buf_puts(&dpbind, "; ");
        }
        Buf rdb; char tyb[32];
        snprintf(tyb, sizeof tyb, "sp_%sArray *", k);
        int cdb = hold_recv_open(c, recv, 0, tyb, "SP_GC_ROOT", b, &rdb);
        if (held[0]) {
          buf_printf(b, "({ sp_RbVal %s = ", held); emit_boxed(c, argv[0], b);
          buf_printf(b, "; SP_GC_ROOT_RBVAL(%s); ", held);
        }
        if (rt == TY_INT_ARRAY) {
          buf_printf(b, "({ sp_oint _t%d = ", tdr);
          emit_int_array_delete(c, rdb.p, argv[0], dnil, held[0] ? held : NULL, b);
          buf_printf(b, "; !_t%d.nil ? sp_box_int(_t%d.v) : ", tdr, tdr);
        }
        else if (rt == TY_FLOAT_ARRAY) {
          buf_printf(b, "({ sp_ofloat _t%d = sp_FloatArray_delete%s(%s, ", tdr, (df_boxed || held[0]) ? "_key" : "_on", rdb.p);
          if (held[0]) buf_puts(b, held);
          else if (df_boxed) emit_boxed(c, argv[0], b);
          else emit_elem_needle(c, rt, argv[0], b);
          buf_printf(b, "); !_t%d.nil ? sp_box_float(_t%d.v) : ", tdr, tdr);
        }
        else if (a0 == TY_POLY || held[0]) {
          /* a boxed needle: a String compares, anything else is not
             there, as include? and index read it (#4458) */
          int tv = ++g_tmp;
          char tvn[32]; snprintf(tvn, sizeof tvn, "_t%d", tv);
          const char *nd = held[0] && !needle_needs_deref(c, argv[0]) ? held : tvn;
          buf_puts(b, "({ ");
          if (!held[0] || needle_needs_deref(c, argv[0])) { buf_printf(b, "sp_RbVal %s = %s", tvn, needle_needs_deref(c, argv[0]) ? "sp_poly_strbuf_deref(" : "");
            if (held[0]) buf_puts(b, held); else emit_boxed(c, argv[0], b); buf_puts(b, needle_needs_deref(c, argv[0]) ? "); " : "; "); }
          buf_printf(b, "const char *_t%d = %s.tag == SP_TAG_STR ? sp_StrArray_delete(%s, %s.v.s)"
                        " : (const char *)0; _t%d ? sp_box_str(_t%d) : ", tdr, nd, rdb.p, nd, tdr, tdr);
        }
        else {
          buf_printf(b, "({ const char *_t%d = sp_StrArray_delete(%s, ", tdr, rdb.p);
          emit_expr(c, argv[0], b);
          buf_printf(b, "); _t%d ? sp_box_str(_t%d) : ", tdr, tdr);
        }
        emit_fallback_block_value(c, dbb, dbn, dpbind.p, 1, "sp_box_nil()", 0, b);
        free(dpbind.p);
        buf_puts(b, "; })");
        if (held[0]) buf_puts(b, "; })");
        free(rdb.p);
        if (cdb) buf_puts(b, "; })");
        { *out = 1; return 1; }
      }
    }
    if (value_obj_compares(c, argv[0])) {
      unsupported_feature(c, id, "Array#delete of a user object defining == from a typed Array");
      { *out = 0; return 1; }
    }
    if (df_never || needle_misses(c, recv, rt, argv[0])) {
      buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)("); emit_expr(c, argv[0], b);
      buf_printf(b, "); %s; })", rt == TY_INT_ARRAY ? "sp_oint_nil()" : rt == TY_STR_ARRAY ? "(const char *)0" : "sp_ofloat_nil()");
      { *out = 1; return 1; }
    }
    /* held across the value, as the poly arm holds it */
    Buf rdl; char tyl[32];
    snprintf(tyl, sizeof tyl, "sp_%sArray *", k);
    int cdl = hold_recv_open(c, recv, 0, tyl, "SP_GC_ROOT", b, &rdl);
    if (rt == TY_INT_ARRAY) emit_int_array_delete(c, rdl.p, argv[0], dnil, NULL, b);
    else if (rt == TY_STR_ARRAY && a0 == TY_POLY) {
      /* a boxed needle, read as the block form above reads it */
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = %s", tv, needle_needs_deref(c, argv[0]) ? "sp_poly_strbuf_deref(" : ""); emit_boxed(c, argv[0], b); if (needle_needs_deref(c, argv[0])) buf_puts(b, ")");
      buf_printf(b, "; _t%d.tag == SP_TAG_STR ? sp_StrArray_delete(%s, _t%d.v.s)"
                    " : _t%d.tag == SP_TAG_NIL ? sp_StrArray_delete(%s, NULL) : (const char *)0; })",
                 tv, rdl.p, tv, tv, rdl.p);
    }
    else {
      buf_printf(b, "sp_%sArray_delete%s(%s, ", k, df_boxed ? "_key" : rt == TY_FLOAT_ARRAY ? "_on" : "", rdl.p);
      if (df_boxed) emit_boxed(c, argv[0], b);
      else if (rt == TY_FLOAT_ARRAY) emit_elem_needle(c, rt, argv[0], b);
      else emit_expr(c, argv[0], b);
      buf_puts(b, ")");
    }
    free(rdl.p);
    if (cdl) buf_puts(b, "; })");
    { *out = 1; return 1; }
  }
  int block = nt_ref(nt, id, "block");
  { int r; if (emit_kind_array_iter_call(c, id, b, nt, name, recv, argc, argv, rt, k, block, &r)) { *out = r; return 1; } }
  if (sp_streq(name, "to_s") && argc == 0) {
    char fn[64]; snprintf(fn, sizeof fn, "sp_%sArray_inspect", k);
    /* the array's nil (NULL) answers nil.to_s, the empty string */
    emit_null_guarded_call(c, recv, rt, fn, "sp_str_empty", b);
    { *out = 1; return 1; }
  }
  /* `[a, b, c].min` on a literal of Integers that cannot be nil: the
     extreme of the values, compared in place, with no array built. The
     array was a fresh allocation per call, and in a hot loop (an edit
     distance's `[ins, del, sub].min`) its garbage grew the heap to twice
     what the program kept. Each element runs once, in order. */
  if ((is_minmax_query(name)) && argc == 0 &&
      rt == TY_INT_ARRAY && nt_kind(nt, recv) == NK_ArrayNode &&
      nt_ref(nt, id, "block") < 0) {
    int en = 0; const int *el = nt_arr(nt, recv, "elements", &en);
    int plain = en > 0 && en <= 8;
    for (int e = 0; plain && e < en; e++)
      if (comp_ntype(c, el[e]) != TY_INT || nullable_int_value(c, el[e])) plain = 0;
    if (plain) {
      int base = g_tmp + 1; g_tmp += en + 1;
      buf_puts(b, "({");
      for (int e = 0; e < en; e++) {
        buf_printf(b, " sp_int _t%d = ", base + e); emit_int_expr(c, el[e], b); buf_puts(b, ";");
      }
      int m = base + en;
      buf_printf(b, " sp_int _t%d = _t%d;", m, base);
      for (int e = 1; e < en; e++)
        buf_printf(b, " if (_t%d %s _t%d) _t%d = _t%d;", base + e, sp_streq(name, "min") ? "<" : ">", m, m, base + e);
      /* the extreme of values that are never nil is still answered through
         the oint form where the call's consumer asks for one (min can
         answer nil on other receivers) */
      if (node_is_oint(c, id)) buf_printf(b, " sp_oint_of(_t%d); })", m);
      else buf_printf(b, " _t%d; })", m);
      { *out = 1; return 1; }
    }
  }
  /* A literal's min and max compare each new element with the extreme
     so far, as CRuby's VM does for them (sp_PolyArray_minmax_lit): the
     nil check and a boxed literal's comparisons name the pair that
     way round, where Array#max's on an array value is the other. */
  if ((is_minmax_query(name)) && argc == 0 && block < 0 &&
      array_literal_plain(c, recv)) {
    int want_max = sp_streq(name, "max");
    if (rt == TY_POLY_ARRAY) {
      buf_puts(b, "sp_PolyArray_minmax_lit("); emit_expr(c, recv, b); buf_printf(b, ", %d)", want_max);
      { *out = 1; return 1; }
    }
    /* a literal analyze marked checks for nil in that order always; any
       other checks only where a nil was ever stored, in the same order */
    if ((rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY) && elem_nil_sentinel(c, recv, rt)) {
      oint_open(c, id, ty_array_elem(rt), b);
      buf_printf(b, "sp_%sArray_%s_o(sp_%sArray_nil_lit_%s(", k, name, k,
                 "if_flagged");
      emit_expr(c, recv, b); buf_printf(b, ", %d))", want_max);
      oint_close(c, id, b);
      { *out = 1; return 1; }
    }
  }
  if ((is_minmax_query(name)) && argc == 0) {
    /* an Integer or Float extreme is nil when the array is empty: an oint */
    int onum = rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY;
    if (onum) oint_open(c, id, ty_array_elem(rt), b);
    buf_printf(b, "sp_%sArray_%s%s(", k, name, onum ? "_o" : ""); emit_nil_ck_recv(c, recv, rt, "cmp", 0, b); buf_puts(b, ")");
    if (onum) oint_close(c, id, b);
    { *out = 1; return 1; }
  }
  /* a typed array never holds an element of another kind: include? is
     false and index is nil, with both operands still evaluated */
  int elem_mismatch = 0;
  /* ... except nil, in an array that can hold it */
  int nil_needle = argc == 1 && a0 == TY_NIL && elem_nil_sentinel(c, recv, rt);
  if (argc == 1 && rt == TY_STR_ARRAY && a0 != TY_STRING && a0 != TY_NIL && a0 != TY_UNKNOWN && a0 != TY_POLY) elem_mismatch = 1;
  if (argc == 1 && (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY) && !nil_needle &&
      a0 != TY_INT && a0 != TY_FLOAT && a0 != TY_UNKNOWN && a0 != TY_POLY) elem_mismatch = 1;
  if (is_index_query(name) && argc == 1 &&
      (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY || rt == TY_FLOAT_ARRAY)) {
    if (elem_mismatch) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), (void)("); emit_expr(c, argv[0], b); buf_puts(b, "), sp_box_nil())");
      { *out = 1; return 1; }
    }
    /* nil-on-miss -> poly */
    const char *fn = sp_streq(name, "rindex") ? "rindex_poly" : "index_poly";
    if (value_obj_compares(c, argv[0])) {
      unsupported_feature(c, id, "Array#index of a user object defining == in a typed Array");
      { *out = 0; return 1; }
    }
    if (needle_misses(c, recv, rt, argv[0])) {
      buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)("); emit_expr(c, argv[0], b);
      buf_puts(b, "); sp_box_nil(); })");
      { *out = 1; return 1; }
    }
    if (rt == TY_FLOAT_ARRAY && (a0 == TY_POLY || a0 == TY_UNKNOWN)) {
      /* boxed needle: the tag-guarded helper compares Float and Integer
         tags and misses every other, where a to-f coercion made false hits */
      buf_printf(b, "sp_FloatArray_%s(", sp_streq(name, "rindex") ? "rindex_key" : "index_key");
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      { *out = 1; return 1; }
    }
    if (rt == TY_INT_ARRAY && (a0 == TY_POLY || (a0 == TY_NIL && !nil_needle))) {
      /* the Integer twin of the String arm below: only an Integer can be
         there, where unboxing the needle raised TypeError (#4835) */
      int ta = ++g_tmp, tv = ++g_tmp;
      buf_printf(b, "({ sp_IntArray *_t%d = ", ta); emit_recv_rooted(c, recv, ta, "SP_GC_ROOT", b);
      buf_printf(b, "sp_RbVal _t%d = ", tv); emit_boxed(c, argv[0], b);
      buf_printf(b, "; _t%d.tag == SP_TAG_INT ? sp_IntArray_%s(_t%d, _t%d.v.i) : ", tv, fn, ta, tv);
      /* a boxed nil finds the array's nil elements */
      if (elem_nil_sentinel(c, recv, rt))
        buf_printf(b, "_t%d.tag == SP_TAG_NIL ? sp_box_oint(sp_IntArray_index_nil(_t%d, %d)) : ", tv, ta, sp_streq(name, "rindex") ? 1 : 0);
      buf_puts(b, "sp_box_nil(); })");
      { *out = 1; return 1; }
    }
    if (rt == TY_STR_ARRAY && (a0 == TY_POLY || a0 == TY_NIL)) {
      /* a boxed needle into a String array: a String compares, anything
         else (nil first of all) is simply not there. The boxed value
         used to be passed as the const char* itself, which did not
         compile (#4458). */
      int ta = ++g_tmp, tv = ++g_tmp;
      buf_printf(b, "({ sp_StrArray *_t%d = ", ta); emit_recv_rooted(c, recv, ta, "SP_GC_ROOT", b);
      buf_printf(b, "sp_RbVal _t%d = %s", tv, needle_needs_deref(c, argv[0]) ? "sp_poly_strbuf_deref(" : ""); emit_boxed(c, argv[0], b); if (needle_needs_deref(c, argv[0])) buf_puts(b, ")");
      buf_printf(b, "; _t%d.tag == SP_TAG_STR ? sp_StrArray_%s(_t%d, _t%d.v.s)"
                    " : _t%d.tag == SP_TAG_NIL ? sp_StrArray_%s(_t%d, NULL) : sp_box_nil(); })", tv, fn, ta, tv, tv, fn, ta);
      { *out = 1; return 1; }
    }
    if (nil_needle) {
      /* a literal nil: the first (last) nil element */
      int ta = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
      buf_puts(b, "; (void)("); emit_expr(c, argv[0], b);
      buf_printf(b, "); sp_box_oint(sp_%sArray_index_nil(_t%d, %d)); })", k, ta, sp_streq(name, "rindex") ? 1 : 0);
      { *out = 1; return 1; }
    }
    /* held across the needle, which may allocate */
    Buf rix; char tyx[32];
    snprintf(tyx, sizeof tyx, "sp_%sArray *", k);
    int cix = hold_recv_open(c, recv, 0, tyx, "SP_GC_ROOT", b, &rix);
    if ((rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY) && node_has_oint_form(c, argv[0])) {
      /* a needle that can be nil searches with its nil (a nil needle finds
         the nil elements); the _o answer is boxed as the _poly one is */
      buf_printf(b, "sp_box_oint(sp_%sArray_%s_o(%s, ", k, sp_streq(name, "rindex") ? "rindex" : "index", rix.p);
      emit_elem_needle(c, rt, argv[0], b);
      buf_puts(b, "))");
    }
    else {
      buf_printf(b, "sp_%sArray_%s(%s, ", k, fn, rix.p);
      /* the _poly search takes the plain element: the needle's oint form,
         which is never nil here, unwrapped */
      if (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY) {
        buf_printf(b, "%s(", oint_arg(ty_array_elem(rt))); emit_elem_needle(c, rt, argv[0], b); buf_puts(b, ")");
      }
      else emit_expr(c, argv[0], b);
      buf_puts(b, ")");
    }
    free(rix.p);
    if (cix) buf_puts(b, "; })");
    { *out = 1; return 1; }
  }
  if ((is_membership_alias(name)) && argc == 1) {
    if (elem_mismatch) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), (void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
      { *out = 1; return 1; }
    }
  }
  if ((is_membership_alias(name)) && nil_needle) {
    /* a literal nil: is there a nil element */
    int ta = ++g_tmp;
    buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
    buf_puts(b, "; (void)("); emit_expr(c, argv[0], b);
    buf_printf(b, "); sp_%sArray_has_nil(_t%d); })", k, ta);
    { *out = 1; return 1; }
  }
  if ((is_membership_alias(name)) && argc == 1 && rt == TY_FLOAT_ARRAY &&
      (a0 == TY_POLY || a0 == TY_UNKNOWN) && elem_nil_sentinel(c, recv, rt)) {
    /* a boxed needle that may be nil: the tag-guarded index finds it,
       the receiver held across the needle as the arm below holds it */
    Buf rfk;
    int cfk = hold_recv_open(c, recv, 0, "sp_FloatArray *", "SP_GC_ROOT", b, &rfk);
    buf_printf(b, "(sp_FloatArray_index_key(%s, ", rfk.p);
    emit_boxed(c, argv[0], b); buf_puts(b, ").tag != SP_TAG_NIL)");
    free(rfk.p);
    if (cfk) buf_puts(b, "; })");
    { *out = 1; return 1; }
  }
  if ((is_membership_alias(name)) && argc == 1 && rt == TY_FLOAT_ARRAY) {
    /* held across the needle: a freed receiver here read a reused slot
       as a float array and crashed */
    Buf rfi;
    int cfi = hold_recv_open(c, recv, 0, "sp_FloatArray *", "SP_GC_ROOT", b, &rfi);
    buf_printf(b, "sp_FloatArray_include_o(%s, ", rfi.p);
    emit_elem_needle(c, rt, argv[0], b); buf_puts(b, ")");
    free(rfi.p);
    if (cfi) buf_puts(b, "; })");
    { *out = 1; return 1; }
  }
  if ((sp_streq(name, "include?") || sp_streq(name, "member?") || sp_streq(name, "index") || sp_streq(name, "find_index")) && argc == 1 && rt != TY_FLOAT_ARRAY) {
    const char *fn = (is_membership_alias(name)) ? "include" : "index";
    /* A boxed argument into a String array is an equality scan. A foreign
       kind misses rather than raising in unboxing (#4458); nil searches
       for a NULL element, which is distinct from the empty String. */
    TyKind sat = repr_of(c, argv[0]).as_ty;
    if (rt == TY_STR_ARRAY && (sat == TY_POLY || sat == TY_NIL)) {
      int ta = ++g_tmp, tv = ++g_tmp;
      buf_printf(b, "({ sp_StrArray *_t%d = ", ta); emit_recv_rooted(c, recv, ta, "SP_GC_ROOT", b);
      buf_printf(b, "sp_RbVal _t%d = %s", tv, needle_needs_deref(c, argv[0]) ? "sp_poly_strbuf_deref(" : ""); emit_boxed(c, argv[0], b); if (needle_needs_deref(c, argv[0])) buf_puts(b, ")");
      buf_printf(b, "; _t%d.tag == SP_TAG_STR ? sp_StrArray_%s(_t%d, _t%d.v.s)"
                    " : _t%d.tag == SP_TAG_NIL ? sp_StrArray_%s(_t%d, NULL) : FALSE; })",
                 tv, fn, ta, tv, tv, fn, ta);
      { *out = 1; return 1; }
    }
    /* The same for an Integer array: a search for a value of another kind
       is a well-defined "not there" (false / no index), where unboxing the
       needle raised the conversion TypeError (#4835). */
    if (rt == TY_INT_ARRAY && (sat == TY_POLY || (sat == TY_NIL && !nil_needle))) {
      int ta = ++g_tmp, tv = ++g_tmp;
      int isinc = sp_streq(fn, "include");
      if (!isinc) oint_open(c, id, TY_INT, b);
      buf_printf(b, "({ sp_IntArray *_t%d = ", ta); emit_recv_rooted(c, recv, ta, "SP_GC_ROOT", b);
      buf_printf(b, "sp_RbVal _t%d = ", tv); emit_boxed(c, argv[0], b);
      buf_printf(b, "; _t%d.tag == SP_TAG_INT ? sp_IntArray_%s_o(_t%d, sp_oint_of(_t%d.v.i)) : ", tv, fn, ta, tv);
      if (elem_nil_sentinel(c, recv, rt))   /* as the index arm above */
        buf_printf(b, "_t%d.tag == SP_TAG_NIL ? sp_IntArray_%s_o(_t%d, sp_oint_nil()) : ", tv, fn, ta);
      buf_printf(b, "%s; })", isinc ? "FALSE" : "sp_oint_nil()");
      if (!isinc) oint_close(c, id, b);
      { *out = 1; return 1; }
    }
    /* held across the needle, which may allocate */
    Buf rin; char tyn[32];
    snprintf(tyn, sizeof tyn, "sp_%sArray *", k);
    int cin = hold_recv_open(c, recv, 0, tyn, "SP_GC_ROOT", b, &rin);
    /* an Integer array's search takes the needle as an oint (a nil needle
       finds the nil elements); index answers an oint, the others a bool or
       a position */
    int isinc = sp_streq(fn, "include");
    int oix = rt == TY_INT_ARRAY && !isinc;
    if (oix) oint_open(c, id, TY_INT, b);
    buf_printf(b, "sp_%sArray_%s%s(%s, ", k, fn, rt == TY_INT_ARRAY ? "_o" : "", rin.p);
    /* a poly argument into a string array's const char* slot (`arr.include?(
       params[k])`) needs coercing; emit_str_expr passes a plain string
       through and sp_poly_to_s's a poly value. */
    if (rt == TY_INT_ARRAY) emit_elem_needle(c, rt, argv[0], b);
    else if (sp_streq(k, "Int")) emit_int_expr(c, argv[0], b);
    else if (rt == TY_STR_ARRAY) emit_str_expr(c, argv[0], b);
    else emit_expr(c, argv[0], b);
    buf_puts(b, ")");
    if (oix) oint_close(c, id, b);
    free(rin.p);
    if (cin) buf_puts(b, "; })");
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "sample") && argc == 1 && nt_type(nt, argv[0]) &&
      sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) {
    /* sample(random: rng): one element, as sample's row (the RNG kwarg uses
       the global generator here) (#2970) */
    int onum = rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY;
    if (onum) oint_open(c, id, ty_array_elem(rt), b);
    buf_printf(b, "sp_%sArray_sample%s(", k, onum ? "_o" : ""); emit_expr(c, recv, b); buf_puts(b, ")");
    if (onum) oint_close(c, id, b);
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "sample") && argc == 1) {
    int t = ++g_tmp, tn = ++g_tmp;
    buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_shuffle(", k, t, k); emit_expr(c, recv, b);
    buf_printf(b, "); SP_GC_ROOT(_t%d); sp_int _t%d = ", t, tn); emit_int_expr(c, argv[0], b);
    buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative sample number\");"
                  " sp_%sArray_slice(_t%d, 0, _t%d); })", tn, k, t, tn);
    { *out = 1; return 1; }
  }
  return 0;
}

/* A nil / true / false OPERAND to the Array-expecting family (concat,
   replace, product, union, difference, intersection), or one typed as a
   kind no Array conversion takes, is CRuby's TypeError ("no implicit
   conversion of nil into Array") -- concat fell to NoMethodError, product
   answered [] -- with the receiver and every argument evaluated first, in
   order, as a real call would. Each argument is a statement of its own after
   its own prelude: as one expression, an argument built in place (an Array
   literal) ran ahead of the ones before it. CRuby converts the arguments in
   turn, so a boxed one ahead of the misfit that holds no Array raises
   first, naming its own class. */
static int emit_array_operand_misfit(Compiler *c, int id, const char *name, int recv, int argc,
                                     const int *argv, Buf *b) {
  if (!(sp_streq(name, "concat") || sp_streq(name, "replace") || sp_streq(name, "product") ||
        sp_streq(name, "union") || sp_streq(name, "difference") || sp_streq(name, "intersection")) ||
      argc < 1) return 0;
  int bad = -1;
  for (int ai = 0; ai < argc; ai++) {
    TyKind at = comp_ntype(c, argv[ai]);
    if (at == TY_NIL || at == TY_BOOL || conv_to_ary_impossible(at)) { bad = ai; break; }
  }
  if (bad < 0) return 0;
  TyKind arty = repr_of(c, id).as_ty;
  int tb = ++g_tmp, t0 = g_tmp + 1;
  g_tmp += argc;
  Buf *sv_pre = g_pre;
  buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); ");
  for (int ai = 0; ai < argc; ai++) {
    Buf ap = {0, 0, 0}, av = {0, 0, 0};
    TyKind at = comp_ntype(c, argv[ai]);
    g_pre = &ap;
    if (ai == bad && at == TY_BOOL) emit_expr(c, argv[ai], &av);
    else if (ai < bad && at == TY_POLY) emit_boxed(c, argv[ai], &av);
    else emit_expr(c, argv[ai], &av);
    g_pre = sv_pre;
    if (ap.p) buf_puts(b, ap.p);
    if (ai == bad && at == TY_BOOL) buf_printf(b, "int _t%d = (%s); ", tb, av.p ? av.p : "0");
    else if (ai < bad && at == TY_POLY)
      buf_printf(b, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d); ", t0 + ai, av.p ? av.p : "sp_box_nil()", t0 + ai);
    else buf_printf(b, "(void)(%s); ", av.p ? av.p : "0");
    free(ap.p); free(av.p);
  }
  for (int ai = 0; ai < bad; ai++)
    if (comp_ntype(c, argv[ai]) == TY_POLY)
      buf_printf(b, "if (_t%d.tag != SP_TAG_OBJ || !sp_poly_is_array_kind(_t%d.cls_id))"
                    " sp_raise_cls(\"TypeError\", sp_sprintf(\"no implicit conversion of %%s into Array\", sp_convert_src_name(_t%d))); ",
                 t0 + ai, t0 + ai, t0 + ai);
  if (comp_ntype(c, argv[bad]) == TY_NIL)
    buf_puts(b, "sp_raise_cls(\"TypeError\", \"no implicit conversion of nil into Array\");");
  else if (comp_ntype(c, argv[bad]) == TY_BOOL)
    buf_printf(b, "sp_raise_cls(\"TypeError\", _t%d"
                  " ? \"no implicit conversion of true into Array\""
                  " : \"no implicit conversion of false into Array\");", tb);
  else
    buf_printf(b, "sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into Array\");",
               conv_builtin_class_name(comp_ntype(c, argv[bad])));
  buf_printf(b, " %s; })", raise_tail_value(arty));
  return 1;
}

/* A typed Array receiver (emit_array_call's arms, in their order) */
static int emit_typed_array_call(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind a0, TyKind res, int *out) {
  if (!(recv >= 0 && ty_is_array(rt))) return 0;
  if (emit_scalar_array_conversion(c, id, recv, rt, name, argc, b)) { *out = 1; return 1; }
  /* `product(*xs)` spreads xs across the ARGUMENT LIST, one operand array
     per element. The arms below read a splat as a single operand instead,
     so `[1,2].product(*[])` answered [] where CRuby answers [[1],[2]], and
     `product(*[[3]])` nested the operand. Spreading a runtime-length list
     needs a variadic helper these arms do not have; refuse rather than
     answer wrongly (#4298). A splat of a literal empty array is the one
     case with an answer here: no operands at all. */
  if (sp_streq(name, "product") && argc >= 1) {
    int spl = -1;
    for (int ai = 0; ai < argc; ai++)
      if (nt_kind(nt, argv[ai]) == NK_SplatNode) { spl = ai; break; }
    if (spl >= 0) {
      int se = nt_ref(nt, argv[spl], "expression");
      int sen = 0;
      int empty_lit = se >= 0 && nt_kind(nt, se) == NK_ArrayNode &&
                      (nt_arr(nt, se, "elements", &sen), sen == 0);
      /* ...and a local whose every write is an empty literal is the same
         empty list, which is the shape the report is about. */
      if (!empty_lit && se >= 0 && nt_kind(nt, se) == NK_LocalVariableReadNode) {
        const char *sn = nt_str(nt, se, "name");
        Scope *ssc = sn ? comp_scope_of(c, se) : NULL;
        if (sn && ssc && local_all_writes_empty_array(c, ssc, sn)) empty_lit = 1;
      }
      if (!(argc == 1 && empty_lit)) {
        unsupported_feature(c, id, "Array#product with a splatted argument list");
        { *out = 1; return 1; }
      }
      /* an empty splat is no operands at all: `[1,2].product` -- each
         element wrapped in a one-element array */
      {
        int tp = ++g_tmp, ti2 = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tp, tp);
        buf_printf(b, " sp_RbVal _t%d = ", ti2); emit_boxed(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", ti2);
        int tk = ++g_tmp;
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_poly_length(_t%d); _t%d++) {"
                      " sp_PolyArray *_t%d_e = sp_PolyArray_new();"
                      " sp_PolyArray_push(_t%d_e, sp_poly_arr_get(_t%d, _t%d));"
                      " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d_e)); }",
                   tk, tk, ti2, tk, tk, tk, ti2, tk, tp, tk);
        buf_printf(b, " _t%d; })", tp);
        { *out = 1; return 1; }
      }
    }
  }
  if (emit_array_operand_misfit(c, id, name, recv, argc, argv, b)) { *out = 1; return 1; }
  /* product(b, c, ...) with two or more array arguments: the n-way Cartesian
     product. The single-argument form is specialized below (per element-type
     boxing); for 2+ arguments box the receiver and every argument into rooted
     locals -- the GC is precise, so they must stay reachable across the
     helper's allocations -- and hand them to sp_poly_product as one vector. */
  /* product(b, c, ...) WITH a block: CRuby runs the block for each tuple and
     answers the receiver. Emitting the product alone answered the tuple array
     instead, so `arr.product(x, y) { }.equal?(arr)` was false (and the
     inference, which already said self, disagreed with the emission). */
  if (sp_streq(name, "product") && (argc >= 2 || (argc == 1 && rt == TY_POLY_ARRAY)) &&
      nt_ref(nt, id, "block") >= 0) {
    int blk = nt_ref(nt, id, "block");
    int bbody = nt_ref(nt, blk, "body");
    int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
    const char *fp0 = block_param_name(c, blk, 0);
    int nn = argc + 1;
    int *ids = (int *)malloc(sizeof(int) * nn);
    if (!ids) { perror("malloc"); exit(1); }
    int trecv = ++g_tmp, tprod = ++g_tmp, ti = ++g_tmp;
    buf_puts(b, "({ ");
    trecv = hold_operand(c, recv, TY_POLY, 1, trecv, 1, " ", b);
    ids[0] = trecv;
    for (int i = 0; i < argc; i++) {
      ids[i + 1] = ++g_tmp;
      buf_printf(b, "sp_RbVal _t%d = ", ids[i + 1]); emit_boxed(c, argv[i], b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", ids[i + 1]);
    }
    buf_printf(b, "sp_RbVal _tp%d[%d] = { _t%d", tprod, nn, ids[0]);
    for (int i = 1; i < nn; i++) buf_printf(b, ", _t%d", ids[i]);
    buf_printf(b, " }; sp_PolyArray *_t%d = sp_poly_product(_tp%d, %d); SP_GC_ROOT(_t%d);",
               tprod, tprod, nn, tprod);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {",
               ti, ti, tprod, ti);
    char tsrc[64]; snprintf(tsrc, sizeof tsrc, "sp_PolyArray_get(_t%d, _t%d)", tprod, ti);
    if (emit_tuple_block_params(c, id, blk, tsrc, b)) { }
    else if (fp0) buf_printf(b, " lv_%s = %s;", rename_local(fp0), tsrc);
    buf_puts(b, " {");
    emit_iter_step_body(c, blk, b, 0);
    buf_puts(b, " } } ");
    /* the receiver, in the C type this call is inferred to have */
    TyKind pres = repr_of(c, id).as_ty;
    if (pres == TY_POLY || pres == TY_UNKNOWN) buf_printf(b, "_t%d; })", trecv);
    else { emit_unbox_text(c, pres, ({ static char rb9[32]; snprintf(rb9, sizeof rb9, "_t%d", trecv); rb9; }), b); buf_puts(b, "; })"); }
    free(ids);
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "product") && argc >= 2) {
    int nn = argc + 1;
    int *ids = (int *)malloc(sizeof(int) * nn);
    if (!ids) { perror("malloc"); exit(1); }
    buf_puts(b, "({ ");
    ids[0] = ++g_tmp;
    buf_printf(b, "sp_RbVal _t%d = ", ids[0]); emit_boxed(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", ids[0]);
    for (int i = 0; i < argc; i++) {
      ids[i + 1] = ++g_tmp;
      buf_printf(b, "sp_RbVal _t%d = ", ids[i + 1]); emit_boxed(c, argv[i], b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", ids[i + 1]);
    }
    buf_printf(b, "sp_RbVal _tp[%d] = { _t%d", nn, ids[0]);
    for (int i = 1; i < nn; i++) buf_printf(b, ", _t%d", ids[i]);
    buf_printf(b, " }; sp_poly_product(_tp, %d); })", nn);
    free(ids);
    { *out = 1; return 1; }
  }
  /* values_at(i, j, ...) -> fresh same-kind array of the picked elements
     (works for typed and poly arrays alike, and range args) */
  if (sp_streq(name, "values_at") && argc >= 1) {
    const char *an = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (an) {
      /* an index past the end reads nil, the receiver's own nils come
         along: an Integer or Float element is read as its oint */
      int onum = rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY;
      const char *vs = onum ? "_o" : "";
      const char *vg = onum ? "oget" : "get";
      int tr = ++g_tmp, to = ++g_tmp;
      /* the receiver and the result are rooted across the indices, as
         fetch_values roots its result */
      buf_printf(b, "({ sp_%sArray *_t%d = ", an, tr); emit_recv_rooted(c, recv, tr, "SP_GC_ROOT", b);
      buf_printf(b, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d); ", an, to, an, to);
      for (int a = 0; a < argc; a++) {
        TyKind at = comp_ntype(c, argv[a]);
        if (nt_type(nt, argv[a]) && sp_streq(nt_type(nt, argv[a]), "SplatNode")) {
          /* values_at(*idx): each element of the splatted array is a
             separate index (#3277). */
          int ts = ++g_tmp, tk = ++g_tmp;
          char ev[64]; snprintf(ev, sizeof ev, "sp_PolyArray_get(_t%d, _t%d)", ts, tk);
          buf_printf(b, "{ sp_PolyArray *_t%d = ", ts); emit_expr(c, argv[a], b);
          buf_printf(b, "; for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) ", tk, tk, ts, tk);
          emit_values_at_boxed(an, vs, vg, to, tr, ev, b);
          buf_puts(b, "} ");
        }
        else if (at == TY_POLY) {
          Buf bv = {0, 0, 0}; emit_boxed(c, argv[a], &bv);
          emit_values_at_boxed(an, vs, vg, to, tr, bv.p ? bv.p : "sp_box_nil()", b);
          free(bv.p);
        }
        else if (at == TY_RANGE) {
          /* an open or negative endpoint resolves against the length, the
             way Array#[] resolves it: read raw, an endless range ran to
             INTPTR_MAX and pushed until the process died (#3847) */
          int trng = ++g_tmp, ti = ++g_tmp, tlen = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp;
          buf_printf(b, "{ sp_Range _t%d = sp_range_ix(", trng); emit_expr(c, argv[a], b); buf_puts(b, ")");
          buf_printf(b, "; sp_int _t%d = sp_%sArray_length(_t%d);", tlen, an, tr);
          buf_printf(b, " sp_int _t%d = _t%d.nobeg ? 0"
                        " : (_t%d.first < 0 ? _t%d.first + _t%d : _t%d.first);",
                     tlo, trng, trng, trng, tlen, trng);
          buf_printf(b, " sp_int _t%d = _t%d.noend ? _t%d - 1"
                        " : ((_t%d.last < 0 ? _t%d.last + _t%d : _t%d.last) - (_t%d.excl ? 1 : 0));",
                     thi, trng, tlen, trng, trng, tlen, trng, trng);
          buf_printf(b, " for (sp_int _t%d = _t%d; _t%d <= _t%d; _t%d++)"
                        " sp_%sArray_push%s(_t%d, sp_%sArray_%s(_t%d, _t%d)); } ",
                     ti, tlo, ti, thi, ti, an, vs, to, an, vg, tr, ti);
        }
        else {
          /* the index is an index: a Float one converts here, as it does for
             every other index-taking method. Emitted raw, a literal Float
             reached `sp_XArray_get`'s sp_int parameter and the C compiler
             truncated it with a warning of its own (#3936). */
          buf_printf(b, "sp_%sArray_push%s(_t%d, sp_%sArray_%s(_t%d, ", an, vs, to, an, vg, tr);
          emit_int_expr(c, argv[a], b); buf_puts(b, ")); ");
        }
      }
      buf_printf(b, "_t%d; })", to);
      { *out = 1; return 1; }
    }
  }
  /* fetch_values(i, ...): like values_at but raises IndexError on an
     out-of-range index (#2321) */
  /* fetch_values(i, ...) { |i| fallback }: an out-of-range index takes the
     block's value instead of raising; the mixed result is a poly array. */
  if (sp_streq(name, "fetch_values") && argc >= 1 && nt_ref(nt, id, "block") >= 0 &&
      nt_type(nt, nt_ref(nt, id, "block")) &&
      sp_streq(nt_type(nt, nt_ref(nt, id, "block")), "BlockNode")) {
    const char *an = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (an) {
      int fblk = nt_ref(nt, id, "block");
      const char *fp0 = block_param_name(c, fblk, 0);
      const char *fp0r = fp0 ? rename_local(fp0) : NULL;
      int fbody = nt_ref(nt, fblk, "body");
      int fbn = 0; const int *fbb = fbody >= 0 ? nt_arr(nt, fbody, "body", &fbn) : NULL;
      int tr = ++g_tmp, to = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", an, tr); emit_expr(c, recv, b);
      buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", to, to);
      for (int a = 0; a < argc; a++) {
        int ti = ++g_tmp;
        buf_printf(b, "{ sp_int _t%d = ", ti); emit_int_expr(c, argv[a], b);
        buf_printf(b, "; sp_int _len = sp_%sArray_length(_t%d);"
                      " sp_int _ix = _t%d < 0 ? _t%d + _len : _t%d;"
                      " if (_ix < 0 || _ix >= _len) { ",
                   an, tr, ti, ti, ti);
        Buf bind; memset(&bind, 0, sizeof bind);
        if (fp0r) buf_printf(&bind, "lv_%s = _t%d; ", fp0r, ti);
        buf_printf(b, "sp_PolyArray_push(_t%d, ", to);
        /* Build the fallback value after binding the missing index; its
           literal setup must stay inside this out-of-range branch. */
        emit_fallback_block_value(c, fbb, fbn, bind.p, 1, "sp_box_nil()", 1, b);
        free(bind.p);
        buf_puts(b, "); }\nelse { ");
        { char getx[96]; snprintf(getx, sizeof getx, "sp_%sArray_get(_t%d, _ix)", an, tr);
          buf_printf(b, "sp_PolyArray_push(_t%d, ", to);
          if (rt == TY_POLY_ARRAY) buf_puts(b, getx);
          else emit_boxed_text(c, ty_array_elem(rt), getx, b);
          buf_puts(b, "); } } "); }
      }
      buf_printf(b, "_t%d; })", to);
      { *out = 1; return 1; }
    }
  }
  if (sp_streq(name, "fetch_values") && argc >= 1) {
    const char *an = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (an) {
      int tr = ++g_tmp, to = ++g_tmp;
      /* the receiver is rooted across the indices, as the result already is */
      buf_printf(b, "({ sp_%sArray *_t%d = ", an, tr); emit_recv_rooted(c, recv, tr, "SP_GC_ROOT", b);
      buf_printf(b, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d); ", an, to, an, to);
      /* a `_nilable` store takes the element with its nil (oget) */
      const char *fsfx = nil_store_sfx(c, an, NIL_STORE_BOXED);
      for (int a = 0; a < argc; a++) {
        int ti = ++g_tmp;
        buf_printf(b, "{ sp_int _t%d = ", ti); emit_int_expr(c, argv[a], b);
        buf_printf(b, "; sp_int _len = sp_%sArray_length(_t%d);"
                      " sp_int _ix = _t%d < 0 ? _t%d + _len : _t%d;"
                      " if (_ix < 0 || _ix >= _len) sp_raise_cls(\"IndexError\","
                      " sp_sprintf(\"index %%lld outside of array bounds: %%lld...%%lld\","
                      " (long long)_t%d, (long long)-_len, (long long)_len));"
                      " sp_%sArray_push%s(_t%d, sp_%sArray_%s(_t%d, _ix)); } ",
                   an, tr, ti, ti, ti, ti, an, fsfx, to, an, fsfx[0] ? "oget" : "get", tr);
      }
      buf_printf(b, "_t%d; })", to);
      { *out = 1; return 1; }
    }
  }
  const char *k = array_kind(rt);
  /* fetch_values with no keys reads nothing, of a boxed-element array too
     (an empty literal is one) */
  if (sp_streq(name, "fetch_values") && argc == 0 && rt == TY_POLY_ARRAY &&
      !an_zero_arg_builtin_shadowed(c, name, argc)) {
    int tr = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = ", tr); emit_expr(c, recv, b);
    buf_printf(b, "; if (!_t%d) sp_nil_recv(\"fetch_values\"); sp_PolyArray_new(); })", tr);
    { *out = 1; return 1; }
  }
  /* poly-array max/min: boxed elements compared at runtime (numerics,
     strings, int-array tuples lexicographically). */
  if ((is_minmax_query(name)) && argc == 0 &&
      rt == TY_POLY_ARRAY && nt_ref(nt, id, "block") < 0) {
    /* a literal's in CRuby's VM order (sp_PolyArray_minmax_lit) */
    if (array_literal_plain(c, recv)) {
      buf_puts(b, "sp_PolyArray_minmax_lit("); emit_expr(c, recv, b);
      buf_printf(b, ", %d)", sp_streq(name, "max"));
      { *out = 1; return 1; }
    }
    buf_printf(b, "sp_PolyArray_%s(", name); emit_expr(c, recv, b); buf_puts(b, ")");
    { *out = 1; return 1; }
  }
  /* fill(val[, start[, len]]): fill a range with val, evaluate to self. */
  /* fill([start[, length]]) { |i| ... } / fill(range) { |i| ... }: the block
     form takes NO value argument -- the positional args are the index span and
     the value at each index comes from the block. (The no-block forms, where
     the first argument IS the value, are handled below.) */
  if (sp_streq(name, "fill") && argc <= 2 && nt_ref(nt, id, "block") >= 0) {
    /* a block value the element type cannot hold rebuilds through a poly
       array, as the value form below does (inference typed the call poly);
       a local receiver was widened at the write site instead */
    int fill_conflict = rt != TY_POLY_ARRAY && repr_of(c, id).elem == TY_POLY;
    const char *fk = (rt == TY_POLY_ARRAY || fill_conflict) ? "Poly" : k;
    TyKind frt = fill_conflict ? TY_POLY_ARRAY : rt;
    int fblk = nt_ref(nt, id, "block");
    int fbody = nt_ref(nt, fblk, "body");
    int fbn = 0; const int *fbb = fbody >= 0 ? nt_arr(nt, fbody, "body", &fbn) : NULL;
    if (fk && fbn > 0) {
      TyKind et = ty_array_elem(frt);
      int trecv = ++g_tmp, tn = ++g_tmp, ts = ++g_tmp, te = ++g_tmp, ti = ++g_tmp;
      const char *ip = block_param_name(c, fblk, 0); if (ip) ip = rename_local(ip);
      Buf rb; memset(&rb, 0, sizeof rb);
      if (fill_conflict) { buf_puts(&rb, "sp_poly_to_poly_array("); emit_boxed(c, recv, &rb); buf_puts(&rb, ")"); }
      else rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent); emit_ctype(c, frt, g_pre);
      buf_printf(g_pre, " _t%d = %s; ", trecv, rb.p ? rb.p : ""); free(rb.p);
      /* rooted, as the TY_POLY map!/collect! near the top of this file
         already roots its own hoist: fill stores into the receiver on every
         turn, and the block never mentions the receiver, so this temporary is
         the only thing holding it while the block allocates */
      emit_gc_root_tmp(c, frt, trecv, g_pre); buf_puts(g_pre, "\n");
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_int _t%d = sp_%sArray_length(_t%d);\n", tn, fk, trecv);
      /* resolve the [start, end) span from the arguments */
      int is_range = (argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE);
      if (is_range) {
        int tr = ++g_tmp;
        emit_indent(g_pre, g_indent);
        /* rendered first: emit_expr may want g_pre lines of its own (#4065) */
        { Buf rgb2; memset(&rgb2, 0, sizeof rgb2); emit_expr(c, argv[0], &rgb2);
          buf_printf(g_pre, "sp_Range _t%d = sp_range_ix(%s);\n", tr, rgb2.p ? rgb2.p : "(sp_Range){0}");
          free(rgb2.p); }
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_int _t%d = _t%d.first; if (_t%d < 0) _t%d += _t%d; if (_t%d < 0) _t%d = 0;\n",
                   ts, tr, ts, ts, tn, ts, ts);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_int _t%d = (_t%d.last < 0 ? _t%d.last + _t%d : _t%d.last) + (_t%d.excl ? 0 : 1);\n",
                   te, tr, tr, tn, tr, tr);
      }
      else {
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_int _t%d = 0;", ts);
        if (argc >= 1) { buf_printf(g_pre, " _t%d = ", ts); emit_int_expr(c, argv[0], g_pre);
                         buf_printf(g_pre, "; if (_t%d < 0) _t%d += _t%d; if (_t%d < 0) _t%d = 0;", ts, ts, tn, ts, ts); }
        buf_puts(g_pre, "\n");
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_int _t%d = _t%d;", te, tn);
        if (argc == 2) { buf_printf(g_pre, " { sp_int _tl = "); emit_int_expr(c, argv[1], g_pre);
                         buf_printf(g_pre, "; if (_tl < 0) _tl = 0; _t%d = _t%d + _tl; }", te, ts); }
        buf_puts(g_pre, "\n");
      }
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (sp_int _t%d = _t%d; _t%d < _t%d; _t%d++) {\n", ti, ts, ti, te, ti);
      if (ip) {
        Scope *fic = comp_scope_of(c, fblk);
        LocalVar *filv = fic ? scope_local(fic, ip) : NULL;
        TyKind fit = filv ? filv->type : TY_INT;
        emit_indent(g_pre, g_indent + 1);
        if (fit == TY_POLY) buf_printf(g_pre, "lv_%s = sp_box_int(_t%d);\n", ip, ti);
        else if (slot_is_oint(filv)) buf_printf(g_pre, "lv_%s = sp_oint_of(_t%d);\n", ip, ti);   /* an index is never nil */
        else buf_printf(g_pre, "lv_%s = _t%d;\n", ip, ti);
      }
      /* A poly value is boxed by the step, once: boxed again here, with
         the store begun, its setup lines landed inside the call. */
      Repr vr = repr_of(c, fbb[fbn - 1]);
      TyKind vt = vr.as_ty;
      IterStep st; emit_iter_step_open(c, fblk, sp_streq(fk, "Poly") && (vt == TY_POLY || vr.untyped ||
        (repr_share_rule(c) && (vr.strbuf_src != RS_NONE || strbuf_value_carries(c, fbb[fbn - 1])))), g_indent + 1, &st);
      Buf vb; memset(&vb, 0, sizeof vb); vt = emit_iter_step_tail(c, &st, &vb);
      emit_indent(g_pre, g_indent + 1);
      if (sp_streq(fk, "Poly")) {
        buf_printf(g_pre, "sp_PolyArray_set(_t%d, _t%d, ", trecv, ti);
        if (vt != TY_POLY && vt != TY_UNKNOWN) emit_boxed_text(c, vt, vb.p ? vb.p : "sp_box_nil()", g_pre);
        else buf_puts(g_pre, vb.p ? vb.p : "sp_box_nil()");
        buf_puts(g_pre, ");\n");
      }
      else {
        const char *fsfx = nil_store_sfx(c, fk, fbb[fbn - 1]);
        buf_printf(g_pre, "sp_%sArray_set%s(_t%d, _t%d, ", fk, fsfx, trecv, ti);
        /* the _nilable store takes the value with its nil (a block
           parameter that can be nil, a nullable read); the plain store the
           sink's plain value */
        if (fsfx[0]) emit_oint_expr(c, fbb[fbn - 1], sp_streq(fk, "Float") ? TY_FLOAT : TY_INT, g_pre);
        else emit_typed_sink_text(c, fbb[fbn - 1], sp_streq(fk, "Int") ? TY_INT : sp_streq(fk, "Float") ? TY_FLOAT : TY_UNKNOWN, vb.p ? vb.p : "", g_pre);
        buf_puts(g_pre, ");\n");
      }
      free(vb.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      buf_printf(b, "_t%d", trecv);
      { *out = 1; return 1; }
    }
  }
  if (sp_streq(name, "fill") && argc >= 1 && argc <= 3) {
    /* fill(value, start[, length]): start and length are offsets, and a
       value with no integer conversion is CRuby's TypeError. They went into
       the offset slot as-is, so a String start read as a pointer and the
       fill quietly did nothing (#3611). */
    for (int fa = 1; fa < argc; fa++) {
      TyKind ft = comp_ntype(c, argv[fa]);
      /* a Range is a start only in fill(value, range); with a length
         after it, CRuby converts it to an Integer and raises, once every
         operand has run in order */
      if (fa == 1 && argc == 3 &&
          (ft == TY_RANGE || ft == TY_FLOAT_RANGE || ft == TY_STR_RANGE)) {
        buf_printf(b, "({ (void)("); emit_expr(c, recv, b);
        for (int fo = 0; fo < argc; fo++) { buf_puts(b, "); (void)("); emit_expr(c, argv[fo], b); }
        buf_puts(b, "); sp_raise_cls(\"TypeError\", \"no implicit conversion of Range into Integer\"); ");
        buf_printf(b, "(sp_%sArray *)0; })", (rt == TY_POLY_ARRAY) ? "Poly" : k);
        { *out = 1; return 1; }
      }
      /* nil is allowed: it means "from the start" / "to the end" */
      const char *fcn = ft == TY_STRING ? "String" : ft == TY_SYMBOL ? "Symbol"
                      : ty_is_array(ft) ? "Array"
                      : ty_is_hash(ft) ? "Hash" : NULL;
      if (!fcn) continue;
      int tf = ++g_tmp;
      buf_printf(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)(");
      emit_expr(c, argv[fa], b);
      buf_printf(b, "); sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into Integer\");"
                    " (sp_%sArray *)0; })", fcn, (rt == TY_POLY_ARRAY) ? "Poly" : k);
      (void)tf;
      { *out = 1; return 1; }
    }
    /* a fill VALUE incompatible with the element type rebuilds through a
       poly array (inference typed the result poly to match); only literal
       and temp receivers reach this -- a conflicting fill on a LOCAL
       already widened the local itself at the write site */
    int fill_conflict = rt != TY_POLY_ARRAY && repr_of(c, id).elem == TY_POLY;
    const char *fk = (rt == TY_POLY_ARRAY || fill_conflict) ? "Poly" : k;
    TyKind fill_rt = fill_conflict ? TY_POLY_ARRAY : rt;
    if (fk) {
      int t = ++g_tmp, ti = ++g_tmp, tv = ++g_tmp, tn = ++g_tmp, ts = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", fk, t);
      if (fill_conflict) {
        buf_puts(b, "sp_poly_to_poly_array(");
        emit_boxed(c, recv, b);
        buf_puts(b, ")");
      }
      else emit_expr(c, recv, b);
      buf_puts(b, "; ");
      const char *fsfx = nil_store_sfx(c, fk, argv[0]);   /* a nil fill value takes the _nilable store */
      if (fsfx[0] && fill_rt != TY_POLY_ARRAY) buf_printf(b, "%s _t%d = ", oint_ctype(ty_array_elem(fill_rt)), tv);
      else { emit_ctype(c, ty_array_elem(fill_rt), b); buf_printf(b, " _t%d = ", tv); }
      if (fill_rt == TY_POLY_ARRAY) emit_boxed(c, argv[0], b);
      else if (fsfx[0]) emit_elem_store_value(c, fk, argv[0], b);
      else emit_typed_elem_value(c, argv[0], ty_array_elem(fill_rt), b);
      buf_printf(b, "; sp_int _t%d = sp_%sArray_length(_t%d);", tn, fk, t);
      if (argc >= 2 && comp_ntype(c, argv[1]) == TY_RANGE) {
        /* fill(val, range): use range as index span */
        int tr = ++g_tmp, te = ++g_tmp;
        buf_printf(b, " sp_Range _t%d = sp_range_ix(", tr); emit_expr(c, argv[1], b); buf_puts(b, ")");
        buf_printf(b, "; sp_int _t%d = _t%d.first; if (_t%d < 0) _t%d += _t%d; if (_t%d < 0) _t%d = 0;",
                   ts, tr, ts, ts, tn, ts, ts);
        /* a negative end counts from the end, an endless one runs to the
           last element -- unnormalized, `fill(v, 2..)` grew the array
           forever (#3605) and `fill(v, 1..-1)` filled nothing (#3606) */
        buf_printf(b, " sp_int _t%d = _t%d.last;"
                      " if (_t%d == INTPTR_MAX) _t%d = _t%d - 1;"
                      " else { if (_t%d < 0) _t%d += _t%d; _t%d -= _t%d.excl; }",
                   te, tr,
                   te, te, tn,
                   te, te, tn, te, tr);
        buf_printf(b, " for (sp_int _t%d = _t%d; _t%d <= _t%d; _t%d++)"
                      " sp_%sArray_set%s(_t%d, _t%d, _t%d); _t%d; })",
                   ti, ts, ti, te, ti, fk, fsfx, t, ti, tv, t);
      }
      else if (argc >= 2) {
        /* nil start / length are legal: "from the start" / "to the end" */
        buf_printf(b, " sp_int _t%d = ", ts);
        /* a boxed start may be a Range or no number at all; a nil one is 0 */
        if (repr_of(c, argv[1]).kind == RK_BOXED) {
          buf_puts(b, "({ sp_oint _o = sp_array_fill_offset_arg("); emit_expr(c, argv[1], b); buf_printf(b, ", %d); _o.nil ? 0 : _o.v; })", argc == 2);
        }
        else emit_int_expr_bound(c, argv[1], "0", b);
        buf_printf(b, "; if (_t%d < 0) _t%d += _t%d; if (_t%d < 0) _t%d = 0;", ts, ts, tn, ts, ts);
        if (argc == 3 && comp_ntype(c, argv[2]) == TY_NIL) {
          /* a nil LENGTH is "to the end": keep the array-length bound */
          buf_puts(b, " (void)("); emit_expr(c, argv[2], b); buf_puts(b, ");");
        }
        else if (argc == 3) {
          int tl = ++g_tmp;
          /* a length that is nil at run time -- boxed, or a nullable Integer
             slot -- is "to the end" as the literal one is */
          buf_printf(b, " sp_oint _t%d = ", tl);
          if (repr_of(c, argv[2]).kind == RK_BOXED) { buf_puts(b, "sp_array_fill_offset_arg("); emit_expr(c, argv[2], b); buf_puts(b, ", 0)"); }
          else emit_oint_expr(c, argv[2], TY_INT, b);
          /* end = start+len; negative len = no-op (empty range) */
          buf_printf(b, "; if (!_t%d.nil) { if (_t%d.v < 0) _t%d.v = 0; _t%d = _t%d + _t%d.v; }",
                     tl, tl, tl, tn, ts, tl);
        }
        buf_printf(b, " for (sp_int _t%d = _t%d; _t%d < _t%d; _t%d++)"
                      " sp_%sArray_set%s(_t%d, _t%d, _t%d); _t%d; })",
                   ti, ts, ti, tn, ti, fk, fsfx, t, ti, tv, t);
      }
      else {
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)"
                      " sp_%sArray_set%s(_t%d, _t%d, _t%d); _t%d; })",
                   ti, ti, tn, ti, fk, fsfx, t, ti, tv, t);
      }
      { *out = 1; return 1; }
    }
  }
  if (rt == TY_POLY_ARRAY && sp_streq(name, "cycle") && argc == 1 &&
      nt_ref(nt, id, "block") < 0 && comp_ntype(c, id) == TY_ENUMERATOR) {
    /* the call is typed as an Enumerator, so it has to BE one: materializing
       the repeated array here handed a poly array to sp_Enumerator_to_a,
       which read it as an Enumerator (#3617) */
    buf_puts(b, "sp_Enumerator_new_cycle("); emit_boxed(c, recv, b);
    buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
    { *out = 1; return 1; }
  }
  if (rt == TY_POLY_ARRAY && sp_streq(name, "dig") && argc >= 1) {
    /* dig(*keys): walk the runtime key list (see the hash arm) */
    /* the receiver is held across the keys, which may allocate */
    if (nt_kind(nt, argv[0]) == NK_SplatNode)
      { *out = emit_dig_splat(c, recv, argv[0], b); return 1; }
    if (argc == 1) {
      Buf rb; int ch = hold_recv_open(c, recv, 0, "sp_PolyArray *", "SP_GC_ROOT", b, &rb);
      buf_printf(b, "sp_PolyArray_get(%s, ", rb.p); free(rb.p);
      emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      if (ch) buf_puts(b, "; })");
    }
    else {
      /* each later step goes through the dig-specific helper so a scalar
         intermediate raises TypeError instead of bit/char-indexing (#2983) */
      /* the key goes boxed: a Struct member can be named, and a String or
         Symbol reached the integer offset slot as a pointer (#3575) */
      Buf rb; int ch = hold_recv_open(c, recv, 0, "sp_PolyArray *", "SP_GC_ROOT", b, &rb);
      for (int di = argc - 1; di >= 1; di--) buf_printf(b, "sp_poly_dig_step_key(");
      buf_printf(b, "sp_PolyArray_get(%s, ", rb.p); free(rb.p); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      for (int di = 1; di < argc; di++) { buf_puts(b, ", "); emit_boxed(c, argv[di], b); buf_puts(b, ")"); }
      if (ch) buf_puts(b, "; })");
    }
    { *out = 1; return 1; }
  }
  /* unshift/prepend(*elems): insert each element at the front (reverse order
     so the arg order is preserved), return the receiver. */
  if (rt == TY_POLY_ARRAY && (is_prepend_alias(name)) && argc >= 1) {
    int t = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = ", t);
    /* the hoisted receiver is rooted across its arguments unless a slot
       read with slot-keeping arguments already holds it (push_recv_in_slot) */
    if (push_recv_in_slot(c, recv, argc, argv, rt)) { emit_expr(c, recv, b); buf_puts(b, ";"); }
    else emit_recv_rooted(c, recv, t, "SP_GC_ROOT", b);
    /* evaluate (and root) every element left-to-right first, THEN insert them
       at the front in reverse so the arg order is preserved -- keeps Ruby's
       left-to-right evaluation independent of the receiver mutations. */
    int base = g_tmp + 1; g_tmp += argc;
    for (int ai = 0; ai < argc; ai++) {
      buf_printf(b, " sp_RbVal _t%d = ", base + ai); emit_boxed(c, argv[ai], b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", base + ai);
    }
    for (int ai = argc - 1; ai >= 0; ai--)
      buf_printf(b, " sp_PolyArray_insert(_t%d, 0, _t%d);", t, base + ai);
    buf_printf(b, " _t%d; })", t);
    { *out = 1; return 1; }
  }
  /* each_index { |i| ... } - iterate with index (works for all array kinds) */
  {
    int ei_blk = nt_ref(nt, id, "block");
    if (sp_streq(name, "each_index") && ei_blk >= 0) {
      const char *ek = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
      if (ek) {
        const char *ip0 = block_param_name(c, ei_blk, 0);
        const char *ip = ip0 ? rename_local(ip0) : NULL;
        int body = nt_ref(nt, ei_blk, "body");
        int trecv = ++g_tmp, ti = ++g_tmp;
        Buf rb = expr_buf(c, recv);
        emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
        buf_printf(g_pre, " _t%d = %s; ", trecv, rb.p ? rb.p : ""); free(rb.p);
        /* rooted, as the poly each_index above already roots its own hoist:
           the length is the loop bound and the block can allocate */
        emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                   ti, ti, ek, trecv, ti);
        if (ip) {
          Scope *eic = comp_scope_of(c, ei_blk);
          LocalVar *eilv = eic ? scope_local(eic, ip0) : NULL;
          TyKind eit = eilv ? eilv->type : TY_INT;
          emit_indent(g_pre, g_indent + 1);
          if (eit == TY_POLY)
            buf_printf(g_pre, "lv_%s = sp_box_int(_t%d);\n", ip, ti);
          /* a slot that holds its nil (a block-splat temp shared with nil) */
          else if (eilv && slot_is_oint(eilv))
            buf_printf(g_pre, "lv_%s = sp_oint_of(_t%d);\n", ip, ti);
          else
            buf_printf(g_pre, "lv_%s = _t%d;\n", ip, ti);
        }
        emit_iter_loop_stmts(c, body, g_pre, g_indent + 1);
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        buf_printf(b, "_t%d", trecv); { *out = 1; return 1; }
      }
    }
  }
  /* Array#delete(v) (value-based, not index-based) on TY_POLY_ARRAY --
     same array_kind()==NULL gating gap as delete_at above. The typed
     forms live inside the `if (k)` block below; poly arrays never get
     there. Needed for the array-backed Set package's #delete, whose
     @data widens to poly for mixed-element sets. */
  if (rt == TY_POLY_ARRAY && sp_streq(name, "delete") && argc == 1) {
    int dblk = nt_ref(nt, id, "block");
    if (dblk >= 0 && nt_type(nt, dblk) && sp_streq(nt_type(nt, dblk), "BlockNode")) {
      /* delete(v) { not-found value }: yield the block's value on a miss */
      int dbody = nt_ref(nt, dblk, "body");
      int dbn = 0; const int *dbb = dbody >= 0 ? nt_arr(nt, dbody, "body", &dbn) : NULL;
      if (dbn >= 1) {
        Buf rb; int ch = hold_recv_open(c, recv, 0, "sp_PolyArray *", "SP_GC_ROOT", b, &rb);
        int tdr = ++g_tmp, tdn = ++g_tmp;
        /* the value is bound once, for the delete and for the block's parameter */
        buf_puts(b, "({ "); tdn = hold_operand(c, argv[0], TY_POLY, 1, tdn, 1, " ", b);
        buf_printf(b, "sp_RbVal _t%d = sp_PolyArray_delete(%s, _t%d);", tdr, rb.p, tdn);
        free(rb.p);
        Buf pbind; memset(&pbind, 0, sizeof pbind);
        const char *pp0 = block_param_name(c, dblk, 0);
        if (pp0 && block_param_is_boxed(c, dblk, id, pp0)) buf_printf(&pbind, "lv_%s = _t%d; ", rename_local(pp0), tdn);
        buf_printf(b, " _t%d.tag != SP_TAG_NIL ? _t%d : ", tdr, tdr);
        emit_fallback_block_value(c, dbb, dbn, pbind.p, 1, "sp_box_nil()", 0, b);
        free(pbind.p);
        buf_puts(b, "; })");
        if (ch) buf_puts(b, "; })");
        { *out = 1; return 1; }
      }
    }
    Buf rb; int ch = hold_recv_open(c, recv, 0, "sp_PolyArray *", "SP_GC_ROOT", b, &rb);
    buf_printf(b, "sp_PolyArray_delete(%s, ", rb.p); free(rb.p);
    emit_boxed(c, argv[0], b); buf_puts(b, ")");
    if (ch) buf_puts(b, "; })");
    { *out = 1; return 1; }
  }
  /* find / detect { |x| cond } on a poly array -> the element or nil. The
     typed-array forms live inside the `if (k)` block below, but array_kind is
     NULL for a poly array, so handle it here with the boxed element type. */
  if (rt == TY_POLY_ARRAY && (is_find_alias(name))) {
    int fblock = nt_ref(nt, id, "block");
    /* find(ifnone) { }: the proc is called on no-match, so its value (any
       type) rides the boxed result. A non-proc ifnone stays a loud reject. */
    int f_ifnone = argc == 1 && comp_ntype(c, argv[0]) == TY_PROC;
    if (fblock >= 0 && (argc == 0 || f_ifnone)) {
      const char *bp = block_param_name(c, fblock, 0); if (bp) bp = rename_local(bp);
      int body = nt_ref(nt, fblock, "body");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      if (bn >= 1) {
        int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp, tfn = f_ifnone ? ++g_tmp : -1;
        Buf rb = expr_buf(c, recv);
        emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
        buf_printf(g_pre, " _t%d = %s; SP_GC_ROOT(_t%d);\n", trecv, rb.p ? rb.p : "", trecv); free(rb.p);
        if (f_ifnone) {
          /* bind the ifnone proc up front (CRuby evaluates args first) plus
             a found flag: a matched nil element must NOT call the proc */
          Buf nb = expr_buf(c, argv[0]);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_Proc *_t%d = %s; SP_GC_ROOT(_t%d); int _tf%d = 0;\n",
                     tfn, nb.p ? nb.p : "NULL", tfn, tfn); free(nb.p);
        }
        emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", tres, default_value_from_compiler(c, TY_POLY), tres);
        emit_find_loop_head(c, id, "Poly", ti, trecv);
        /* Declare the block param in the loop body so the form is self-contained
           when this find is a parameter default hoisted to the call site (whose
           function has no top-level declaration for the block local). */
        char es_fd[64]; snprintf(es_fd, sizeof es_fd, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
        int splat_fd = emit_iter_autosplat(c, fblock, rt, es_fd, g_indent + 1);
        if (!splat_fd && bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
        Buf cb = block_cond_buf(c, fblock, bb, bn);
        emit_indent(g_pre, g_indent + 1);
        {
          char fset[24] = "";
          if (f_ifnone) snprintf(fset, sizeof fset, " _tf%d = 1;", tfn);
          if (!splat_fd && bp) buf_printf(g_pre, "if (%s) { _t%d = lv_%s;%s break; }\n", cb.p ? cb.p : "0", tres, bp, fset);
          else buf_printf(g_pre, "if (%s) { _t%d = sp_PolyArray_get(_t%d, _t%d);%s break; }\n", cb.p ? cb.p : "0", tres, trecv, ti, fset);
        }
        free(cb.p);
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        if (f_ifnone) {
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "if (!_tf%d) _t%d = ((void)sp_proc_call(_t%d, 0, (sp_int[16]){0}), _sp_proc_poly_ret);\n",
                     tfn, tres, tfn);
        }
        buf_printf(b, "_t%d", tres); { *out = 1; return 1; }
      }
    }
  }
  /* index { |x| cond } on a poly array -> the index or nil. The typed-array
     form lives inside the `if (k)` block below; array_kind is NULL for a
     poly array, so handle it here. Unlike the int/str-array forms (which
     infer TY_POLY and box), index on a poly array infers as a
     nullable Integer -- an sp_oint, not boxed.
     find_index used to be here too: now a Ruby definition (builtins/
     enumerable.rb), it is always rewritten to the generic __enum_find_index__
     dispatch before codegen ever sees this name here (desugar_builtin_enum_calls'
     `ty_is_array(rt)` gate covers TY_POLY_ARRAY), so this arm is dead for
     it -- and a stale hand-written find_index arm sharing a guard with a
     still-live name (emit_find_index_poly_expr, elsewhere in this
     migration) is exactly what raced the new dispatch and corrupted an
     unrelated poly call the moment Set was merely required; drop it here
     too rather than leave a second copy of that trap. */
  if (rt == TY_POLY_ARRAY && sp_streq(name, "index") &&
      nt_ref(nt, id, "block") >= 0) {
    int fblock = nt_ref(nt, id, "block");
    const char *bp = block_param_name(c, fblock, 0); if (bp) bp = rename_local(bp);
    int body = nt_ref(nt, fblock, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    if (bn >= 1) {
      int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);\n", trecv, rb.p ? rb.p : "NULL", trecv); free(rb.p);
      emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_oint _t%d = sp_oint_nil();\n", tres);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n",
                 ti, ti, trecv, ti);
      /* Declare the block param in the loop body so the form is self-contained
         (same rationale as find/detect above); a |k, v| header destructures
         the pair element (#1876 family). */
      {
        char es_fi[64]; snprintf(es_fi, sizeof es_fi, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
        int splat_fi = emit_iter_autosplat(c, fblock, rt, es_fi, g_indent + 1);
        if (!splat_fi && bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
      }
      Buf cb = block_cond_buf(c, fblock, bb, bn);
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "if (%s) { _t%d = sp_oint_of(_t%d); break; }\n", cb.p ? cb.p : "0", tres, ti);
      free(cb.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      oint_open(c, id, TY_INT, b); buf_printf(b, "_t%d", tres); oint_close(c, id, b);
      { *out = 1; return 1; }
    }
  }
  /* rindex { |x| cond } on a poly array: the typed arrays' walk over the
     boxed elements (index / find_index with a block reach Enumerable's) */
  if (rt == TY_POLY_ARRAY && sp_streq(name, "rindex") && argc == 0 &&
      nt_ref(nt, id, "block") >= 0 &&
      emit_array_block_index(c, id, recv, rt, "Poly", name, nt_ref(nt, id, "block"), b))
    { *out = 1; return 1; }
  {
    int block = nt_ref(nt, id, "block");
    /* The blockless form is enum_for(:bsearch), not a search with an
       implicitly missing callback. Keep the original Array as its source. */
    if (sp_streq(name, "bsearch") && block < 0 && argc == 0 && ty_is_array(rt)) {
      buf_puts(b, "sp_poly_bsearch_enum(");
      emit_boxed(c, recv, b);
      buf_puts(b, ")");
      *out = 1;
      return 1;
    }
    /* bsearch { |x| cond } - find-minimum mode. Every array kind including
       the poly one, whose elements are already boxed (#2892). */
    if (sp_streq(name, "bsearch") && block >= 0) {
      const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
      int body = nt_ref(nt, block, "body");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      if (bn >= 1) {
        TyKind et = ty_array_elem(rt);
        const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
        if (!k) { *out = 0; return 1; }
        int trecv = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp, tres = ++g_tmp, tmid = ++g_tmp;
        Buf rbs = expr_buf(c, recv);
        emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
        buf_printf(g_pre, " _t%d = %s; ", trecv, rbs.p ? rbs.p : "NULL"); free(rbs.p);
        /* rooted, as the poly-array find_index above already roots its own
           hoist: a halving search still reads its element out of the receiver
           on every turn, and the block allocates between turns */
        emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_int _t%d = 0, _t%d = sp_%sArray_length(_t%d) - 1;\n", tlo, thi, k, trecv);
        emit_indent(g_pre, g_indent);
        /* the answer is an element read with its nil for a number array */
        const char *egn = oint_kind(et) ? "oget" : "get";
        if (oint_kind(et)) buf_printf(g_pre, "%s _t%d = %s;", oint_ctype(et), tres, oint_nil(et));
        else { emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = %s;", tres, et == TY_POLY ? "sp_box_nil()" : "NULL"); }
        /* The running answer is lifted OUT of the receiver and held while the
           search narrows, so rooting the receiver does not cover it: a turn
           that drops the captured element from the array leaves this
           temporary as its only holder. The element type picks the macro --
           an Integer or Float answer roots to nothing, which is why
           bsearch_index needs none. */
        if (needs_root(et)) { buf_puts(g_pre, " "); emit_gc_root_tmp(c, et, tres, g_pre); }
        buf_puts(g_pre, "\n");
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "while (_t%d <= _t%d) {\n", tlo, thi);
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "sp_int _t%d = _t%d + (_t%d - _t%d) / 2;\n", tmid, tlo, thi, tlo);
        if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, tmid); }
        IterStep st; emit_iter_step_open(c, block, 0, g_indent + 1, &st);
        int sv = g_indent; g_indent++;
        Buf cb; memset(&cb, 0, sizeof cb);
        /* an Integer answer that can be nil: nil searches right */
        int bv_o = emit_iter_step_tail_o(c, &st, TY_INT, &cb);
        TyKind bvt = bv_o ? TY_INT : emit_iter_step_tail(c, &st, &cb); g_indent = sv;
        /* An Integer-valued block selects find-ANY mode (CRuby dispatches on
           the block value's kind): 0 means found, negative searches left,
           positive right. A boolean block is find-minimum, as before. */
        if (bvt != TY_INT && bvt != TY_FLOAT && bvt != TY_BOOL && bvt != TY_NIL &&
            bvt != TY_POLY && bvt != TY_UNKNOWN) {
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "(void)(%s); sp_raise_cls(\"TypeError\", \"wrong argument type for Array#bsearch block\");\n",
                     cb.p ? cb.p : "sp_box_nil()");
        }
        else if (bvt == TY_INT && bv_o) {
          int tcmp = ++g_tmp;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_oint _t%d = %s;\n", tcmp, cb.p ? cb.p : "sp_oint_nil()");
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (_t%d.nil) { _t%d = _t%d + 1; }\n", tcmp, tlo, tmid);
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "else if (_t%d.v == 0) { _t%d = sp_%sArray_%s(_t%d, _t%d); break; }\n",
                     tcmp, tres, k, egn, trecv, tmid);
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "else if (_t%d.v < 0) { _t%d = _t%d - 1; }\n", tcmp, thi, tmid);
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
        }
        else if (bvt == TY_INT) {
          int tcmp = ++g_tmp;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_int _t%d = %s;\n", tcmp, cb.p ? cb.p : "0");
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (_t%d == 0) { _t%d = sp_%sArray_%s(_t%d, _t%d); break; }\n",
                     tcmp, tres, k, egn, trecv, tmid);
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "else if (_t%d < 0) { _t%d = _t%d - 1; }\n", tcmp, thi, tmid);
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
        }
        else if (bvt == TY_FLOAT) {
          int tcmp = ++g_tmp;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_float _t%d = %s;\n", tcmp, cb.p ? cb.p : "0.0");
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (_t%d == 0.0) { _t%d = sp_%sArray_%s(_t%d, _t%d); break; }\n", tcmp, tres, k, egn, trecv, tmid);
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "else if (_t%d < 0.0) { _t%d = _t%d - 1; }\n", tcmp, thi, tmid);
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
        }
        else if (bvt == TY_NIL) {
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "_t%d = _t%d + 1;\n", tlo, tmid);
        }
        else if (bvt == TY_POLY) {
          /* mixed block: Integer is find-any (0 found, positive right,
             negative left), other truthy is find-min, nil/false right */
          int tv = ++g_tmp;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", tv, cb.p ? cb.p : "sp_box_nil()");
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (_t%d.tag == SP_TAG_INT) {\n", tv);
          emit_indent(g_pre, g_indent + 2);
          buf_printf(g_pre, "if (_t%d.v.i == 0) { _t%d = sp_%sArray_%s(_t%d, _t%d); break; }\n",
                     tv, tres, k, egn, trecv, tmid);
          emit_indent(g_pre, g_indent + 2);
          buf_printf(g_pre, "else if (_t%d.v.i > 0) { _t%d = _t%d + 1; }\n", tv, tlo, tmid);
          emit_indent(g_pre, g_indent + 2);
          buf_printf(g_pre, "else { _t%d = _t%d - 1; }\n", thi, tmid);
          emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "else if (_t%d.tag == SP_TAG_FLT) {\n", tv);
          emit_indent(g_pre, g_indent + 2);
          buf_printf(g_pre, "if (_t%d.v.f == 0.0) { _t%d = sp_%sArray_%s(_t%d, _t%d); break; }\n", tv, tres, k, egn, trecv, tmid);
          emit_indent(g_pre, g_indent + 2);
          buf_printf(g_pre, "else if (_t%d.v.f < 0.0) { _t%d = _t%d - 1; }\n", tv, thi, tmid);
          emit_indent(g_pre, g_indent + 2); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
          emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "else if (_t%d.tag == SP_TAG_BOOL || _t%d.tag == SP_TAG_NIL) {\n", tv, tv);
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (sp_poly_truthy(_t%d)) { _t%d = sp_%sArray_%s(_t%d, _t%d); _t%d = _t%d - 1; } else { _t%d = _t%d + 1; }\n", tv, tres, k, egn, trecv, tmid, thi, tmid, tlo, tmid);
          emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "else { sp_raise_cls(\"TypeError\", \"wrong argument type for Array#bsearch block\"); }\n");
        }
        else {
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (%s) { _t%d = sp_%sArray_%s(_t%d, _t%d); _t%d = _t%d - 1; }\n",
                     cb.p ? cb.p : "0", tres, k, egn, trecv, tmid, thi, tmid);
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
        }
        free(cb.p);
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        if (oint_kind(et)) oint_open(c, id, et, b);
        buf_printf(b, "_t%d", tres);
        if (oint_kind(et)) oint_close(c, id, b);
        { *out = 1; return 1; }
      }
    }
    /* bsearch_index { |x| cond }: find-minimum binary search returning the
       index of the first element satisfying the predicate, or nil. Kept here,
       before the typed-kind `if (k)` gate, so a poly array (one widened by
       flowing through a method param) reaches it too -- like bsearch (#3165). */
    if (sp_streq(name, "bsearch_index") && block >= 0) {
      const char *bk = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
      if (!bk) { *out = 0; return 1; }
      const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
      int body = nt_ref(nt, block, "body");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      if (bn >= 1) {
        int trecv = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp, tres = ++g_tmp, tmid = ++g_tmp;
        Buf rbs = expr_buf(c, recv);
        emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
        buf_printf(g_pre, " _t%d = %s; ", trecv, rbs.p ? rbs.p : "NULL"); free(rbs.p);
        /* rooted, as the poly-array find_index above already roots its own
           hoist: the element the block judges comes out of the receiver on
           every turn, and the block allocates between turns. The answer here
           is an index rather than an element, so it needs no root of its own. */
        emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_int _t%d = 0, _t%d = sp_%sArray_length(_t%d) - 1; sp_oint _t%d = sp_oint_nil();\n", tlo, thi, bk, trecv, tres);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "while (_t%d <= _t%d) {\n", tlo, thi);
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "sp_int _t%d = _t%d + (_t%d - _t%d) / 2;\n", tmid, tlo, thi, tlo);
        if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, bk, trecv, tmid); }
        IterStep st; emit_iter_step_open(c, block, comp_ntype(c, bb[bn - 1]) != TY_INT, g_indent + 1, &st);
        int sv = g_indent; g_indent++;
        /* The block value is the search predicate: route through emit_cond so a
           poly / nullable-scalar result becomes a valid C truthiness test rather
           than `if (sp_RbVal)` or `if (sp_oint)`. */
        Buf cb; memset(&cb, 0, sizeof cb);
        /* Integer-valued block: find-ANY mode (0 found, <0 left, >0 right),
           yielding the index. Boolean block: find-minimum, as before. */
        if (comp_ntype(c, bb[bn - 1]) == TY_INT) {
          Buf ib; memset(&ib, 0, sizeof ib); emit_iter_step_tail(c, &st, &ib); g_indent = sv;
          int tcmp = ++g_tmp;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_int _t%d = %s;\n", tcmp, ib.p ? ib.p : "0");
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (_t%d == 0) { _t%d = sp_oint_of(_t%d); break; }\n", tcmp, tres, tmid);
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "else if (_t%d < 0) { _t%d = _t%d - 1; }\n", tcmp, thi, tmid);
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
          free(ib.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          oint_open(c, id, TY_INT, b); buf_printf(b, "_t%d", tres); oint_close(c, id, b); { *out = 1; return 1; }
        }
        emit_iter_step_cond(c, &st, 0, &cb); g_indent = sv;
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "if (%s) { _t%d = sp_oint_of(_t%d); _t%d = _t%d - 1; }\n", cb.p ? cb.p : "0", tres, tmid, thi, tmid);
        free(cb.p);
        emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        oint_open(c, id, TY_INT, b); buf_printf(b, "_t%d", tres); oint_close(c, id, b); { *out = 1; return 1; }
      }
    }
  }
  /* builtin-op rows (builtin_ops.c): the arms that read only the receiver's
     kind, the receiver and the arguments, for the typed arrays and the
     poly array alike. Every arm above that stays reads something a row
     cannot: the operand nodes, a block, the call's own type, or a kind the
     family does not share. */
  if (emit_builtin_op(c, id, recv, rt, name, b)) { *out = 1; return 1; }
  { int r; if (emit_kind_array_call(c, id, b, nt, name, recv, argc, argv, rt, a0, k, &r)) { *out = r; return 1; } }
  { int r; if (emit_poly_array_call(c, id, b, nt, name, recv, argc, argv, rt, a0, res, &r)) { *out = r; return 1; } }
  return 0;
}

/* The variable a chain of String value-form bangs starts from (`s` in
   `s.upcase!.downcase!`), or -1 when `recv` is no bang or the chain starts
   from no variable. Each link answers its receiver, or nil, which the next
   link raises on; so what a mutator computes from the chain's value is the
   variable's new value. */
static int str_self_mutator_name(const char *n) {
  return n && (sp_streq(n, "insert") || sp_streq(n, "prepend") || sp_streq(n, "concat") ||
               sp_streq(n, "replace") || sp_streq(n, "<<"));
}
static int str_bang_chain_var(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  int cur = unwrap_parens(c, recv), links = 0;
  /* insert / prepend / concat / replace / << answer their receiver always,
     so a bang on their result (`s.insert(1, "-").sub!("-", "+")`) mutates
     the variable too; the chain passes through them as through a bang */
  while (nt_kind(nt, cur) == NK_CallNode && nt_ref(nt, cur, "receiver") >= 0 &&
         (ty_str_typed_bang_flags(nt_str(nt, cur, "name")) ||
          str_self_mutator_name(nt_str(nt, cur, "name")))) {
    cur = unwrap_parens(c, nt_ref(nt, cur, "receiver"));
    links++;
  }
  TyKind bt = links ? comp_ntype(c, cur) : TY_UNKNOWN;
  /* a boxed variable takes the value back as emit_face_str_bang's links do */
  if (bt == TY_POLY)
    return nt_kind(nt, cur) == NK_LocalVariableReadNode || nt_kind(nt, cur) == NK_InstanceVariableReadNode ? cur : -1;
  return (bt == TY_STRING || bt == TY_STRBUF) && str_mut_var_recv(c, cur) ? cur : -1;
}
/* A value-form mutator's write-back of its result _t<tn>: to the receiver
   when it is a variable (lvw), else to the variable a bang chain receiver
   starts from, whose links the mutation reaches in CRuby (one object) */
static void emit_str_mut_writeback(Compiler *c, int recv, int lvw, int tn, Buf *b) {
  if (lvw) { emit_expr(c, recv, b); buf_printf(b, " = _t%d; ", tn); return; }
  int base = str_bang_chain_var(c, recv);
  if (base < 0) return;
  char sref[1024];
  if (strbuf_slot_ref(c, base, sref, sizeof sref))
    buf_printf(b, "sp_String_set_bin(%s, _t%d); ", sref, tn);
  else if (comp_ntype(c, base) == TY_STRING) { emit_expr(c, base, b); buf_printf(b, " = _t%d; ", tn); }
  else if (comp_ntype(c, base) == TY_POLY) {
    emit_expr(c, base, b); buf_puts(b, " = sp_poly_str_become(");
    emit_expr(c, base, b); buf_printf(b, ", _t%d); ", tn);
  }
}

/* sub! / gsub! ask the runtime's flag whether the call made a substitution.
   It is cleared ahead of the statement, so a sub or gsub that the receiver,
   an argument, a conversion of one or the block runs leaves its own answer
   there (`y.sub!("q", z.sub("a", "b"))`). A read runs none: what
   subtree_is_pure_read names, a global, a constant, and a reader call on
   one of those. */
static int sub_bang_reads(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  while (n >= 0) {
    NodeKind k = nt_kind(nt, n);
    int alloc = 0;
    if (k == NK_GlobalVariableReadNode || k == NK_ConstantReadNode) return 1;
    if (k == NK_ConstantPathNode) n = nt_ref(nt, n, "parent");
    else if (k == NK_CallNode && call_is_field_read(c, n, &alloc)) n = nt_ref(nt, n, "receiver");
    else return subtree_is_pure_read(c, n);
  }
  return 1;
}
/* Nor does an operand that is a String or Regexp literal, or a read of a
   String or a Regexp. `boxed_ok` takes a read of a boxed value too: the
   runtime tells a boxed pattern apart by its tag and never converts it, and
   a boxed replacement converts through #to_str only in a program that
   defines one. */
static int sub_bang_operand_runs(Compiler *c, int n, int boxed_ok) {
  if (n < 0) return 0;
  NodeKind k = nt_kind(c->nt, n);
  if (k == NK_StringNode || k == NK_RegularExpressionNode) return 0;
  TyKind t = comp_ntype(c, n);
  return !((t == TY_STRING || t == TY_REGEX || (boxed_ok && t == TY_POLY)) && sub_bang_reads(c, n));
}
/* Is the call `id` all that the statement `s` runs ahead of its branches or
   its store: `if x.sub!(a, b)`, `v = x.sub!(a, b)`, `p x.sub!(a, b)`? Beside
   another operand that runs (`"#{w.sub(a, b)} #{x.sub!(c, d)}"`) the flag
   holds that one's answer. */
static int sub_bang_alone(Compiler *c, int s, int id) {
  const NodeTable *nt = c->nt;
  while (s >= 0 && s != id) {
    switch (nt_kind(nt, s)) {
      case NK_IfNode: case NK_UnlessNode: case NK_WhileNode: case NK_UntilNode:
        s = nt_ref(nt, s, "predicate"); break;
      case NK_LocalVariableWriteNode: case NK_InstanceVariableWriteNode: case NK_GlobalVariableWriteNode:
        s = nt_ref(nt, s, "value"); break;
      case NK_CallNode: {
        int ac = 0; const int *av = call_args(nt, s, &ac);
        int r = nt_ref(nt, s, "receiver"), run = r >= 0 && !sub_bang_reads(c, r) ? r : -1;
        if (nt_ref(nt, s, "block") >= 0) return 0;
        for (int a = 0; a < ac; a++) {
          if (nt_kind(nt, av[a]) == NK_StringNode || sub_bang_reads(c, av[a])) continue;
          if (run >= 0) return 0;
          run = av[a];
        }
        s = run; break;
      }
      default: return 0;
    }
  }
  return s == id;
}
/* A call that is not alone, or one of whose operands may run a sub, is
   named in g_sub_bang_id while its plain form is emitted: the emitters of
   sub and gsub then enter the runtime through the wrapper that clears the
   flag after the operands have run (sp_str_sub_own and its kin, in
   spinel_rt.h), and a block form answers from a C local of its loop. Every
   other sub! / gsub! reads the flag as it did. */
static int sub_bang_reenters(Compiler *c, int id, int recv, int argc, const int *argv) {
  const NodeTable *nt = c->nt;
  if (!sub_bang_alone(c, g_stmt_cur, id) || !sub_bang_reads(c, recv)) return 1;
  for (int a = 0; a < argc; a++)
    if (sub_bang_operand_runs(c, argv[a], a == 0 || !prog_has_conv_method(c, "to_str", TY_STRING))) return 1;
  int blk = nt_ref(nt, id, "block");
  if (blk < 0) return 0;
  int body = nt_kind(nt, blk) == NK_BlockNode ? nt_ref(nt, blk, "body") : -1;
  if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) return 1;
  int sn = 0; const int *sb = nt_arr(nt, body, "body", &sn);
  for (int k = 0; k < sn; k++) if (sub_bang_operand_runs(c, sb[k], 0)) return 1;
  return 0;
}
/* A String mutator: the value-form bangs, the in-place mutators, append_as_bytes, bytesplice (emit_array_call's arms, in their order) */
static int emit_str_mutator_call(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, int *out) {
  /* String value-form mutators: the expression yields the post-mutation
     string -- or nil for the no-change bang contract -- and reassigns an
     lvalue receiver (value-semantics strings). The transform reuses the
     non-bang emitter through a temporary node rename. */
  /* TY_STRBUF as well: a reader whose ivar became the shared handle answers
     the handle type, and every arm below is a String operation that
     strbuf_slot_ref already knows how to reach through one. Without it the
     value form of `obj.buf << x` matched no arm at all and was refused,
     while the statement form -- which asks strbuf_slot_ref directly, not the
     node type -- compiled. */
  if ((rt == TY_STRING || rt == TY_STRBUF) && recv >= 0) {
    /* a String value-form bang (ty_str_typed_bang_flags), which answers nil
       when nothing changed unless it answers self (PF_STR_SELF). The names
       are copied: the node's name is rewritten to the plain form and back
       below. */
    unsigned sb_fl = ty_str_typed_bang_flags(name);
    int sbi = sb_fl ? 0 : -1;
    char sb_bang[64], sb_plain[64];
    if (sbi >= 0) {
      snprintf(sb_bang, sizeof sb_bang, "%s", name);
      str_bang_plain(name, sb_plain, sizeof sb_plain);
    }
    int sb_nil_nc = !(sb_fl & PF_STR_SELF);
    int sb_sub = sbi >= 0 && (sp_streq(sb_bang, "gsub!") || sp_streq(sb_bang, "sub!"));
    /* gsub!(pattern) with no replacement and no block answers an
       Enumerator, which its own arm builds: the value form below answers
       the String, and with a pattern the plain form refuses it went into
       the Enumerator slot */
    if (sbi >= 0 && comp_ntype(c, id) == TY_ENUMERATOR) sbi = -1;
    if (sbi >= 0) {
      int lvw = str_mut_var_recv(c, recv) || sb_shadowed_reader(recv);
      /* A shared-mutable (STRBUF) local mutates its buffer IN PLACE so every
         alias/container observes it: recompute via the non-bang transform of
         the current contents, then replace the buffer (#3227). */
      { char srefB[1024];
        int hrB = strbuf_recv_handle(c, id, recv, srefB, sizeof srefB);
        if (hrB) {
          /* a reader call answering the handle (`obj.name.strip!`, `name.strip!`
             inside the class) emits as the sp_String *, which the plain form
             below cannot take as its receiver: it reads the contents already
             bound in _tob instead, and the call runs once (#6436); so does a
             route over a handle (`(+s).upcase!`, --share-strings) */
          int rd_call = (nt_kind(nt, recv) == NK_CallNode || hrB == 2) && g_n_argov < MAX_ARG_OVERRIDE;
          int tsb = ++g_tmp, tob = ++g_tmp, tnb = ++g_tmp;
          buf_printf(b, "({ sp_String *_t%d = %s;", tsb, srefB);
          emit_route_recv_root(c, hrB, tsb, 0, NULL, TY_UNKNOWN, b);
          buf_printf(b, " const char *_t%d = sp_String_cstr(_t%d); (void)_t%d; ", tob, tsb, tob);
          /* gsub!/sub! answer nil when no SUBSTITUTION was made, which the
             text comparison below cannot tell from a match that wrote the same
             bytes (`"cats".sub!(/s$/, "s")`): the runtime's matched flag says.
             Cleared in the prelude, ahead of a block form's loop, which the
             emitter hoists there. */
          int subm = sb_sub;
          if (subm) buf_puts(g_pre ? g_pre : b, "sp_re_sub_matched = 0; ");
          nt_node_set_str((NodeTable *)nt, id, "name", sb_plain);
          Buf nbB; memset(&nbB, 0, sizeof nbB);
          int vC = -1, nC = 0;
          if (rd_call) {
            /* as sb_reader_shim_open: the call reads as a plain String, its
               marks lifted and its handle type dropped as views */
            int was_sb = repr_of(c, recv).ty == TY_STRBUF;
            vC = view_push_repr(c, recv, VR_STRBUF_BOX, 0);
            view_push_repr(c, recv, VR_HANDLE_DEMAND, 0);
            nC = 2;
            if (was_sb) { view_push(c, recv, TY_STRING); nC = 3; }
            view_bind(recv, "_t%d", tob);
          }
          /* a sibling, an operand or the block may run a sub of its own: the
             call enters the runtime through its wrapper, and a block form answers
             "a match was found" in a C local of its loop, which the block
             cannot touch (sub_bang_reenters, emit_gsub_block_expr) */
          int sv_sbB = g_sub_bang_id;
          g_sub_bang_id = subm && sub_bang_reenters(c, id, recv, argc, argv) ? id : -1; g_sub_bang_tm = 0;
          emit_expr(c, id, &nbB);
          int tsmB = g_sub_bang_tm; g_sub_bang_id = sv_sbB; g_sub_bang_tm = 0;
          char smB[40] = "";
          if (tsmB) snprintf(smB, sizeof smB, " || _t%d", tsmB);
          else if (subm) snprintf(smB, sizeof smB, " || sp_re_sub_matched");
          if (rd_call) {
            view_unbind(g_n_argov - 1);
            for (int k = nC - 1; k >= 0; k--) view_pop(c, vC + k);
          }
          nt_node_set_str((NodeTable *)nt, id, "name", sb_bang);
          /* The "did it change?" test has to run BEFORE the write: _tob is
             sp_String_cstr, a pointer INTO the buffer rather than a snapshot
             of it, so comparing after set_bin compared the new content with
             itself and every successful mutation answered nil (#4014). */
          int tchg = ++g_tmp;
          buf_printf(b, "const char *_t%d = %s; ", tnb, nbB.p ? nbB.p : "");
          if (sb_nil_nc)
            buf_printf(b, "int _t%d = !sp_str_eq(_t%d, _t%d)%s; ", tchg, tob, tnb, smB);
          buf_printf(b, "sp_String_set_bin(_t%d, _t%d); ", tsb, tnb);
          free(nbB.p);
          if (sb_nil_nc)
            buf_printf(b, "_t%d ? _t%d : NULL; })", tchg, tnb);
          else
            buf_printf(b, "_t%d; })", tnb);
          { *out = 1; return 1; }
        }
      }
      int to = ++g_tmp, tn2 = ++g_tmp;
      /* the plain form reads the receiver as well: a receiver that is a call
         (`s.upcase!.downcase!`) ran a second time there, and the second
         upcase! changed nothing and answered nil. It reads the receiver held
         once, in _to, or for gsub!/sub!'s block form, whose loop goes ahead
         of the statement (g_pre), in a temp held there. A receiver an
         enclosing emitter already holds (arg_ran_first: the case-mapping
         option check's computed receiver) reads its temp, which is declared
         in the statement, not ahead of it. */
      int held = arg_ran_first(recv, 0);
      int hbind = -1;
      if (!lvw && !held && sb_sub && g_pre && nt_ref(nt, id, "block") >= 0) {
        int th = ++g_tmp;
        Buf hb; memset(&hb, 0, sizeof hb); emit_expr(c, recv, &hb);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "const char *_t%d = %s; SP_GC_ROOT_STR(_t%d);\n", th, hb.p ? hb.p : "NULL", th);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "if (!_t%d) sp_nil_recv(\"%s\");\n", th, sb_bang);
        free(hb.p);
        hbind = view_bind(recv, "_t%d", th);
      }
      buf_printf(b, "({ const char *_t%d = ", to); emit_expr(c, recv, b); buf_puts(b, "; (void)_t"); buf_printf(b, "%d; ", to);
      /* a chained bang that changed nothing answers nil, on which this one
         is NoMethodError for its own name, not the FrozenError the
         mutability check reads a NULL as */
      if (!lvw) buf_printf(b, "if (!_t%d) sp_nil_recv(\"%s\"); ", to, sb_bang);
      /* an in-place mutator on a frozen string raises FrozenError (#3003) */
      buf_printf(b, "if (sp_str_is_frozen_val(_t%d)) sp_raise_frozen_str(_t%d); ", to, to);
      int subm2 = sb_sub;   /* gsub!/sub!: nil means no substitution, see above */
      if (subm2) buf_puts(g_pre ? g_pre : b, "sp_re_sub_matched = 0; ");
      nt_node_set_str((NodeTable *)nt, id, "name", sb_plain);
      Buf nb; memset(&nb, 0, sizeof nb);
      int nbind = lvw || held || hbind >= 0 ? -1 : view_bind(recv, "_t%d", to);
      int sv_sb = g_sub_bang_id;   /* the wrapper or a block form's local, as above */
      g_sub_bang_id = subm2 && sub_bang_reenters(c, id, recv, argc, argv) ? id : -1; g_sub_bang_tm = 0;
      emit_expr(c, id, &nb);
      int tsm = g_sub_bang_tm; g_sub_bang_id = sv_sb; g_sub_bang_tm = 0;
      char sm[40] = "";
      if (tsm) snprintf(sm, sizeof sm, " && !_t%d", tsm);
      else if (subm2) snprintf(sm, sizeof sm, " && !sp_re_sub_matched");
      if (nbind >= 0) view_unbind(nbind);
      if (hbind >= 0) view_unbind(hbind);
      nt_node_set_str((NodeTable *)nt, id, "name", sb_bang);
      buf_printf(b, "const char *_t%d = %s; ", tn2, nb.p ? nb.p : "");
      free(nb.p);
      emit_str_mut_writeback(c, recv, lvw, tn2, b);
      if (sb_nil_nc)
        buf_printf(b, "(sp_str_eq(_t%d, _t%d)%s) ? NULL : _t%d; })", to, tn2, sm, tn2);
      else
        buf_printf(b, "_t%d; })", tn2);
      { *out = 1; return 1; }
    }
    if (emit_string_handle_append(c, id, b, name, recv, argc, argv)) { *out = 1; return 1; }
    /* chained append in value position (`t = s << a << b`): the generic form
       below writes back only when the receiver is a direct lvalue read, so a
       chain's outer links never reach the base -- `s` kept just the first
       append. Unroll the chain onto the base, one write-back per link, and
       yield the base (each `<<` returns its receiver). */
    if (sp_streq(name, "<<") && argc == 1) {
      int chain[64]; int cur;
      int nchain = str_append_chain(c, recv, chain, &cur);
      const char *bty = nt_type(nt, cur);
      LocalVar *blv = (bty && sp_streq(bty, "LocalVariableReadNode"))
                      ? scope_local(comp_scope_of(c, cur), nt_str(nt, cur, "name")) : NULL;
      /* STRBUF base: the buffer appends in place (its cstr read is not an
         lvalue, so the concat-and-write-back form below can't serve it) */
      /* a global or a constant holding the handle (--share-strings) too */
      char sref9[256];
      int static9 = nchain > 0 && holder_static_handle_text(c, cur, sref9, sizeof sref9);
      if (nchain > 0 && (static9 || (repr_of_slot(c, blv).kind == RK_STRBUF &&
          bty && sp_streq(bty, "LocalVariableReadNode")))) {
        int tb9 = ++g_tmp;
        /* through emit_local_ref: a block made a real proc reads the
           base through its capture (`*_cap->c_s`), a captured local
           through its cell -- `lv_s` exists in neither */
        buf_printf(b, "({ sp_String *_t%d = ", tb9);
        if (static9) buf_puts(b, sref9);
        else emit_local_ref(c, cur, nt_str(nt, cur, "name"), b);
        buf_puts(b, ";");
        for (int j = nchain; j >= 0; j--) {  /* innermost link first */
          int arg = j > 0 ? chain[j - 1] : argv[0];
          buf_printf(b, " sp_String_append(_t%d, ", tb9);
          { char rt[48]; snprintf(rt, sizeof rt, "sp_String_cstr(_t%d)", tb9);
            emit_str_append_arg(c, arg, rt, b); }
          buf_puts(b, ");");
        }
        buf_printf(b, " sp_String_cstr(_t%d); })", tb9);
        { *out = 1; return 1; }
      }
      if (nchain > 0 && repr_of_slot(c, blv).kind != RK_STRBUF && str_mut_var_recv(c, cur)) {
        buf_puts(b, "({ ");
        for (int j = nchain; j >= 0; j--) {  /* innermost link first, outer arg last */
          int arg = j > 0 ? chain[j - 1] : argv[0];
          buf_puts(b, "sp_str_check_mutable("); emit_expr(c, cur, b); buf_puts(b, "); ");
          emit_expr(c, cur, b); buf_puts(b, " = sp_str_concat(");
          emit_expr(c, cur, b); buf_puts(b, ", ");
          { Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, cur, &rb);
            emit_str_append_arg(c, arg, rb.p, b); free(rb.p); }
          buf_puts(b, "); ");
        }
        emit_expr(c, cur, b);
        buf_puts(b, "; })");
        { *out = 1; return 1; }
      }
    }
    if ((sp_streq(name, "concat") || sp_streq(name, "<<") ||
         sp_streq(name, "prepend")) && argc >= 1) {
      int lvw = str_mut_var_recv(c, recv) || sb_shadowed_reader(recv);
      int tn2 = ++g_tmp, trc = ++g_tmp;
      /* Evaluate the receiver once into a temp: it feeds both the frozen-mutability
         check and the concatenation, and a chained `s << a << b` receiver has a
         side effect that must not run twice. */
      /* rooted across the arguments, which may allocate */
      buf_printf(b, "({ const char *_t%d = ", trc); emit_recv_rooted(c, recv, trc, "SP_GC_ROOT_STR", b);
      buf_printf(b, "const char *_t%d = ", tn2);
      if (sp_streq(name, "prepend")) {
        /* args first (in order), then the receiver */
        for (int j = 0; j < argc; j++) buf_puts(b, "sp_str_concat(");
        emit_str_expr(c, argv[0], b);
        for (int j = 1; j < argc; j++) { buf_puts(b, ", "); emit_str_expr(c, argv[j], b); buf_puts(b, ")"); }
        buf_printf(b, ", _t%d)", trc);
      }
      else {
        for (int j = 0; j < argc; j++) buf_puts(b, "sp_str_concat(");
        buf_printf(b, "_t%d", trc);
        { char rt[24]; snprintf(rt, sizeof rt, "_t%d", trc);
          for (int j = 0; j < argc; j++) { buf_puts(b, ", "); emit_str_append_arg(c, argv[j], rt, b); buf_puts(b, ")"); } }
      }
      buf_puts(b, "; ");
      /* Ruby evaluates the argument(s) before invoking the mutator, so the
         frozen check must fire AFTER the concatenation builds (which is what
         evaluates the args). sp_str_concat allocates a fresh string and never
         mutates the receiver, so a frozen receiver is still untouched here. */
      buf_printf(b, "sp_str_check_mutable(_t%d); ", trc);
      emit_str_mut_writeback(c, recv, lvw, tn2, b);
      buf_printf(b, "_t%d; })", tn2);
      { *out = 1; return 1; }
    }
    if (sp_streq(name, "insert") && argc == 2) {
      int lvw = str_mut_var_recv(c, recv) || sb_shadowed_reader(recv);
      int to = ++g_tmp, ti2 = ++g_tmp, tn2 = ++g_tmp;
      /* CRuby evaluates the receiver, the index and the text, and only then
         reads the String: a variable receiver is read after them, so a text
         that changes it (`s.insert(1, (s << "x"; "y"))`) is seen; any other
         receiver is taken first and rooted across them, which may allocate.
         The text is converted, and rooted, before the frozen check, where
         CRuby raises its TypeError. */
      buf_puts(b, "({ ");
      if (!lvw) { buf_printf(b, "const char *_t%d = ", to); emit_recv_rooted(c, recv, to, "SP_GC_ROOT_STR", b); }
      buf_printf(b, "sp_int _t%d = ", ti2); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; const char *_v%d = ", tn2); emit_str_insert_text(c, argv[1], b);
      buf_printf(b, "; SP_GC_ROOT_STR(_v%d); ", tn2);
      if (lvw) { buf_printf(b, "const char *_t%d = ", to); emit_recv_rooted(c, recv, to, "SP_GC_ROOT_STR", b); }
      buf_printf(b, "sp_str_check_mutable(_t%d);", to);   /* frozen -> FrozenError (#3003) */
      /* -1 appends; an index past the ends raises IndexError, as CRuby */
      buf_printf(b, " const char *_t%d = sp_str_splice_at(_t%d, _t%d == -1 ? (sp_int)sp_str_length(_t%d) : _t%d < 0 ? _t%d + 1 : _t%d, 0, _v%d, 0); ",
                 tn2, to, ti2, to, ti2, ti2, ti2, tn2);
      emit_str_mut_writeback(c, recv, lvw, tn2, b);
      buf_printf(b, "_t%d; })", tn2);
      { *out = 1; return 1; }
    }
    if (sp_streq(name, "replace") && argc == 1) {
      /* shared-mutable local: swap the buffer contents in place (#3227) */
      { char srefR[1024];
        int hrR = strbuf_recv_handle(c, id, recv, srefR, sizeof srefR);
        if (hrR) {
          int tbR = ++g_tmp;
          buf_printf(b, "({ sp_String *_t%d = %s;", tbR, srefR);
          emit_route_recv_root(c, hrR, tbR, argc, argv, TY_STRING, b);
          buf_printf(b, " sp_String_set_bin(_t%d, ", tbR);
          emit_str_expr(c, argv[0], b);
          /* marked to hand out the handle (`r = obj.buf.replace(x)`): the
             receiver itself, as for the appends */
          if (repr_of(c, id).handle) buf_printf(b, "); _t%d; })", tbR);
          else buf_printf(b, "); sp_String_cstr(_t%d); })", tbR);
          { *out = 1; return 1; }
        }
      }
      int lvw = str_mut_var_recv(c, recv) || sb_shadowed_reader(recv);
      int tn2 = ++g_tmp;
      buf_printf(b, "({ sp_str_check_mutable(");   /* frozen -> FrozenError (#3003) */
      emit_expr(c, recv, b);
      /* the receiver takes a copy of the argument's bytes: handed the
         argument's own pointer it became that object (`t.replace(s)` then
         `t.equal?(s)`, and `s.freeze` froze t) */
      buf_printf(b, "); const char *_t%d = sp_str_dup(", tn2); emit_str_expr(c, argv[0], b); buf_puts(b, "); ");
      emit_str_mut_writeback(c, recv, lvw, tn2, b);
      buf_printf(b, "_t%d; })", tn2);
      { *out = 1; return 1; }
    }
  }
  if (rt == TY_STRING && emit_builtin_op_stage(c, id, recv, rt, name, 6, b)) { *out = 1; return 1; }
  /* String#bytesplice(start, len, str): byte-range replace returning self
     (value-semantics strings: the helper builds the new value and an lvalue
     receiver is rebound to it). */
  /* bytesplice(range, str): lower the Range index to (start, len) (#2396) */
  if (rt == TY_STRING && sp_streq(name, "bytesplice") && argc == 2 && recv >= 0 &&
      comp_ntype(c, argv[0]) == TY_RANGE) {
    { char srefBR[1024];
      int hrBR = strbuf_recv_handle(c, id, recv, srefBR, sizeof srefBR);
      if (hrBR) {
        int tm2 = ++g_tmp, tr3 = ++g_tmp, tn3 = ++g_tmp;
        buf_printf(b, "({ sp_String *_t%d = %s;", tm2, srefBR);
        emit_route_recv_root(c, hrBR, tm2, 0, NULL, TY_UNKNOWN, b);
        buf_printf(b, " sp_Range _t%d = sp_range_ix(", tr3);
        emit_expr(c, argv[0], b); buf_puts(b, ")");
        buf_printf(b, "; const char *_t%d = sp_str_bytesplice(sp_String_cstr(_t%d),"
                      " _t%d.first, _t%d.last - _t%d.first + (_t%d.excl ? 0 : 1), ",
                   tn3, tm2, tr3, tr3, tr3, tr3);
        emit_str_expr(c, argv[1], b);
        buf_printf(b, "); sp_String_set_bin(_t%d, _t%d); _t%d; })", tm2, tn3, tn3);
        { *out = 1; return 1; }
      } }
    int lvw9 = str_mut_var_recv(c, recv);
    int tr9 = ++g_tmp, tn9 = ++g_tmp;
    buf_puts(b, "({ sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, "); ");
    buf_printf(b, "sp_Range _t%d = sp_range_ix(", tr9); emit_expr(c, argv[0], b); buf_puts(b, ")");
    buf_printf(b, "; const char *_t%d = sp_str_bytesplice(", tn9);
    emit_expr(c, recv, b);
    buf_printf(b, ", _t%d.first, _t%d.last - _t%d.first + (_t%d.excl ? 0 : 1), ", tr9, tr9, tr9, tr9);
    emit_str_expr(c, argv[1], b); buf_puts(b, ")");
    if (lvw9) { buf_puts(b, "; "); emit_expr(c, recv, b); buf_printf(b, " = _t%d", tn9); }
    buf_printf(b, "; _t%d; })", tn9);
    { *out = 1; return 1; }
  }
  /* append_as_bytes copies bytes without negotiating the receiver's encoding. */
  if (rt == TY_STRING && sp_streq(name, "append_as_bytes") && argc >= 1 && recv >= 0) {
    { char srefAB[1024];
      int hrAB = strbuf_recv_handle(c, id, recv, srefAB, sizeof srefAB);
      if (hrAB) {
        int tm2 = ++g_tmp;
        buf_printf(b, "({ sp_String *_t%d = %s;", tm2, srefAB);
        emit_route_recv_root(c, hrAB, tm2, 0, NULL, TY_UNKNOWN, b);
        for (int a9 = 0; a9 < argc; a9++) {
          buf_printf(b, " sp_String_append_bytes(_t%d, ", tm2);
          if (comp_ntype(c, argv[a9]) == TY_INT) { buf_puts(b, "sp_int_chr("); emit_int_expr(c, argv[a9], b); buf_puts(b, ")"); }
          else emit_str_expr(c, argv[a9], b);
          buf_puts(b, ");");
        }
        buf_printf(b, " sp_String_cstr(_t%d); })", tm2);
        { *out = 1; return 1; }
      } }
    int lvw9 = str_mut_var_recv(c, recv);
    int tn9 = ++g_tmp;
    /* append_as_bytes accepts String AND Integer arguments; an Integer is the
       raw byte value (100 -> "d"), materialized via sp_int_chr (#2463). A
       frozen receiver raises first, like every other in-place append (#3333). */
    buf_puts(b, "({ sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, "); ");
    buf_printf(b, "const char *_t%d = sp_str_append_bytes(", tn9);
    emit_expr(c, recv, b); buf_puts(b, ", ");
    if (comp_ntype(c, argv[0]) == TY_INT) { buf_puts(b, "sp_int_chr("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
    else emit_str_expr(c, argv[0], b);
    buf_puts(b, ")");
    for (int a9 = 1; a9 < argc; a9++) {
      buf_printf(b, "; _t%d = sp_str_append_bytes(_t%d, ", tn9, tn9);
      if (comp_ntype(c, argv[a9]) == TY_INT) { buf_puts(b, "sp_int_chr("); emit_int_expr(c, argv[a9], b); buf_puts(b, ")"); }
      else emit_str_expr(c, argv[a9], b);
      buf_puts(b, ")");
    }
    if (lvw9) { buf_puts(b, "; "); emit_expr(c, recv, b); buf_printf(b, " = _t%d", tn9); }
    buf_printf(b, "; _t%d; })", tn9);
    { *out = 1; return 1; }
  }
  if (rt == TY_STRING && sp_streq(name, "bytesplice") && argc == 3 && recv >= 0) {
    /* shared handle receiver: swap the buffer in place (#3227) */
    { char srefBS[1024];
      int hrBS = strbuf_recv_handle(c, id, recv, srefBS, sizeof srefBS);
      if (hrBS) {
        int tm2 = ++g_tmp, tn3 = ++g_tmp;
        buf_printf(b, "({ sp_String *_t%d = %s;", tm2, srefBS);
        emit_route_recv_root(c, hrBS, tm2, 0, NULL, TY_UNKNOWN, b);
        buf_printf(b, " const char *_t%d = sp_str_bytesplice(sp_String_cstr(_t%d), ", tn3, tm2);
        emit_int_expr(c, argv[0], b);
        buf_puts(b, ", "); emit_int_expr(c, argv[1], b);
        buf_puts(b, ", "); emit_str_expr(c, argv[2], b);
        buf_printf(b, "); sp_String_set_bin(_t%d, _t%d); _t%d; })", tm2, tn3, tn3);
        { *out = 1; return 1; }
      } }
    int lvw = str_mut_var_recv(c, recv) || sb_shadowed_reader(recv);
    int tn2 = ++g_tmp;
    /* in-place mutator: a frozen receiver raises before the splice (#3333) */
    buf_puts(b, "({ sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, "); ");
    buf_printf(b, "const char *_t%d = sp_str_bytesplice(", tn2);
    emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_int_expr(c, argv[0], b);
    buf_puts(b, ", "); emit_int_expr(c, argv[1], b);
    buf_puts(b, ", "); emit_str_expr(c, argv[2], b); buf_puts(b, ")");
    if (lvw) { buf_puts(b, "; "); emit_expr(c, recv, b); buf_printf(b, " = _t%d", tn2); }
    buf_printf(b, "; _t%d; })", tn2);
    { *out = 1; return 1; }
  }
  return 0;
}

/* emit_array_call's views of an empty `[]` receiver as the poly array its
   dispatch builds, into pv (at most three); answers how many it pushed. They
   hold for the call's emission alone and emit_array_call pops them. */
static int array_call_empty_views(Compiler *c, int id, int *pv) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  TyKind rt = comp_recv_type(c, recv);
  int n = 0;
  /* An empty [] literal receiver has no element type of its own, so emit_expr
     would default it to sp_IntArray_new() -- but comp_recv_type coerces it to
     TY_POLY_ARRAY for dispatch, and the poly-array arms below build/consume it
     as a PolyArray. View the node as one so the receiver emits as a
     PolyArray too, keeping the generated C well-typed (#3223). */
  if (recv >= 0 && rt == TY_POLY_ARRAY &&
      (repr_of(c, recv).untyped || ty_is_array(comp_ntype(c, recv))) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
    int en = 0; nt_arr(nt, recv, "elements", &en);
    /* A cached typed-array kind is no better than UNKNOWN here: the literal is
       empty, so the kind is a default rather than an element type, and emitting
       (say) sp_IntArray_new() into a poly-array slot reads back the wrong
       struct (#3608). */
    if (en == 0) pv[n++] = view_push(c, recv, TY_POLY_ARRAY);
  }
  /* The same literal one pass-through call down (`[].freeze.rotate`): the
     freeze/dup/clone arm builds the literal at ITS type, which for an empty one
     is the int-array default, and the poly-array arm below then handed an
     sp_IntArray * to sp_PolyArray_dup. View the literal, and the call that
     carries it, to the poly array this dispatch is about to build. */
  if (recv >= 0 && rt == TY_POLY_ARRAY && nt_kind(nt, recv) == NK_CallNode) {
    const char *pnm = nt_str(nt, recv, "name");
    int pin_recv = nt_ref(nt, recv, "receiver");
    if (pnm && pin_recv >= 0 && nt_ref(nt, recv, "block") < 0 &&
        is_self_copy(pnm) &&
        nt_type(nt, pin_recv) && sp_streq(nt_type(nt, pin_recv), "ArrayNode")) {
      int pen = 0; nt_arr(nt, pin_recv, "elements", &pen);
      if (pen == 0 &&
          (repr_of(c, pin_recv).untyped || ty_is_array(comp_ntype(c, pin_recv)))) {
        pv[n++] = view_push(c, pin_recv, TY_POLY_ARRAY);
        pv[n++] = view_push(c, recv, TY_POLY_ARRAY);
      }
    }
  }
  return n;
}

static int emit_array_call_arms(Compiler *c, int id, Buf *b);

int emit_array_call(Compiler *c, int id, Buf *b) {
  if (emit_array_splat_mutator(c, id, b)) return 1;
  /* An array indexed by a String or a Symbol is CRuby's TypeError. A
     parameter can be typed that way by a call that never runs it -- one arm
     of a dispatch over several classes' [] (#5076) -- and the typed read
     below would pass the String as an index, which the C build refused.
     Answer the raise, with the call's zero for the value it never makes. */
  {
    const NodeTable *ntI = c->nt;
    const char *nmI = nt_str(ntI, id, "name");
    int recvI = nt_ref(ntI, id, "receiver");
    int aI = nt_ref(ntI, id, "arguments"); int acI = 0;
    const int *avI = aI >= 0 ? nt_arr(ntI, aI, "arguments", &acI) : NULL;
    if (nmI && recvI >= 0 && acI == 1 && ty_is_array(comp_ntype(c, recvI)) &&
        (sp_streq(nmI, "[]") || sp_streq(nmI, "at") || sp_streq(nmI, "slice")) &&
        nt_kind(ntI, avI[0]) != NK_SplatNode) {
      TyKind itI = comp_ntype(c, avI[0]);
      const char *knI = itI == TY_STRING ? "String" : itI == TY_SYMBOL ? "Symbol" : NULL;
      if (knI) {
        TyKind rtI = repr_of(c, id).as_ty;
        buf_puts(b, "((void)("); emit_expr(c, recvI, b);
        buf_puts(b, "), (void)("); emit_expr(c, avI[0], b);
        buf_printf(b, "), sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into Integer\"), ", knI);
        if (rtI == TY_UNKNOWN || rtI == TY_VOID || rtI == TY_NIL) buf_puts(b, "sp_box_nil()");
        else if (oint_kind(rtI) && node_is_oint(c, id)) buf_puts(b, oint_nil(rtI));   /* the element read's oint */
        else buf_puts(b, default_value_from_compiler(c, rtI));
        buf_puts(b, ")");
        return 1;
      }
    }
  }
  /* The variadic Array mutators accept zero elements and return the receiver
     unchanged; every arm below is written for argc >= 1, so a no-argument call
     fell through to the unsupported-call refusal (#3340). */
  {
    const NodeTable *ntZ = c->nt;
    const char *nmZ = nt_str(ntZ, id, "name");
    int recvZ = nt_ref(ntZ, id, "receiver");
    int aZ = nt_ref(ntZ, id, "arguments"); int acZ = 0;
    if (aZ >= 0) nt_arr(ntZ, aZ, "arguments", &acZ);
    if (nmZ && recvZ >= 0 && acZ == 0 && nt_ref(ntZ, id, "block") < 0 &&
        ty_is_array(comp_ntype(c, recvZ)) &&
        (sp_streq(nmZ, "push") || sp_streq(nmZ, "append") ||
         sp_streq(nmZ, "concat") || sp_streq(nmZ, "unshift") ||
         sp_streq(nmZ, "prepend"))) {
      emit_expr(c, recvZ, b);
      return 1;
    }
    /* `zip(*xs)` / `product(*xs)`: the splat spreads across the ARGUMENT LIST,
       one operand per element, and its length is only known at run time. Every
       arm below reads a splat as a SINGLE operand, so zip handed an array where
       a value was expected and stopped the C build, and product refused the
       shape outright rather than answer wrongly (#4322, #4323). Build the
       operand list here -- a splat contributes each of its elements, anything
       else contributes itself -- and hand it to the variadic runtime. */
    if (nmZ && recvZ >= 0 && acZ >= 1 && nt_ref(ntZ, id, "block") < 0 &&
        (sp_streq(nmZ, "zip") || sp_streq(nmZ, "product")) &&
        (ty_is_array(comp_ntype(c, recvZ)) || comp_ntype(c, recvZ) == TY_POLY)) {
      const int *avZ = nt_arr(ntZ, aZ, "arguments", &acZ);
      int splZ = 0;
      for (int ai = 0; ai < acZ; ai++)
        if (nt_kind(ntZ, avZ[ai]) == NK_SplatNode) { splZ = 1; break; }
      /* ...and so does a zip past the 16 operands the typed arms below hold:
         they stopped at the 16th and dropped the rest without a word */
      if (splZ || (sp_streq(nmZ, "zip") && acZ > 16)) {
        int tops = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tops, tops);
        for (int ai = 0; ai < acZ; ai++) {
          if (nt_kind(ntZ, avZ[ai]) == NK_SplatNode) {
            int sx = nt_ref(ntZ, avZ[ai], "expression");
            int tsp = ++g_tmp, tsi = ++g_tmp;
            buf_printf(b, " { sp_PolyArray *_t%d = sp_enum_items_from(", tsp);
            if (sx >= 0) emit_boxed(c, sx, b); else buf_puts(b, "sp_box_nil()");
            buf_printf(b, "); SP_GC_ROOT(_t%d);"
                          " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                          " sp_PolyArray_push(_t%d, _t%d->data[_t%d]); }",
                       tsp, tsi, tsi, tsp, tsi, tops, tsp, tsi);
          }
          else {
            buf_printf(b, " sp_PolyArray_push(_t%d, ", tops);
            emit_boxed(c, avZ[ai], b);
            buf_puts(b, ");");
          }
        }
        buf_printf(b, " sp_poly_%s_n(", sp_streq(nmZ, "zip") ? "zip" : "product");
        emit_boxed(c, recvZ, b);
        buf_printf(b, ", _t%d); })", tops);
        return 1;
      }
    }
    /* zip with nothing to zip against: each element alone in a one-element
       array. Every zip arm below is written for argc >= 1 (#3612). */
    if (nmZ && recvZ >= 0 && acZ == 0 && nt_ref(ntZ, id, "block") < 0 &&
        sp_streq(nmZ, "zip") && ty_is_array(comp_ntype(c, recvZ))) {
      buf_puts(b, "sp_poly_zip_none(");
      emit_boxed(c, recvZ, b);
      buf_puts(b, ")");
      return 1;
    }
  }

  /* Shared-mutable shim, value position (#3227): same shadow-copy re-entry
     as emit_array_mutate_stmt's -- the existing arm computes the value and
     reassigns the shadow, then the handle's buffer swaps in place. */
  {
    const NodeTable *ntS = c->nt;
    const char *nmS = nt_str(ntS, id, "name");
    int recvS = nt_ref(ntS, id, "receiver");
    if (nmS && recvS >= 0 && repr_of(c, recvS).as_ty == TY_STRBUF &&
        (is_string_position_mutator(nmS)) &&
        sb_reader_expr_shim(c, id, recvS, b, emit_array_call)) return 1;
    if (nmS && recvS >= 0 && comp_ntype(c, recvS) == TY_STRING &&
        (is_string_position_mutator(nmS))) {
      if (sb_iv_expr_shim(c, id, recvS, b, emit_array_call)) return 1;
      /* a global holding the handle (--share-strings) */
      if (repr_static_read_kind(nt_kind(ntS, recvS)) &&
          sb_reader_expr_shim(c, id, recvS, b, emit_array_call)) return 1;
      const char *sbn = strbuf_local_name(c, recvS);
      /* the handle's slot through the holder (a captured local's cell) */
      char srefL[1024];
      if (sbn && g_nren < MAX_RENAME && strbuf_slot_ref(c, recvS, srefL, sizeof srefL)) {
        Scope *shs = comp_scope_of(c, recvS);
        LocalVar *shlv = scope_local(shs, sbn);
        Buf pre; memset(&pre, 0, sizeof pre);
        int mark = sb_shim_args_first(c, id, &pre, g_indent);
        int tH = ++g_tmp;
        Buf armb; memset(&armb, 0, sizeof armb);
        snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", sbn);
        snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_sb%d", tH);
        g_nren++;
        SbShimSave sv_sh = sb_shim_enter(shlv);
        int handled = emit_array_call(c, id, &armb);
        sb_shim_leave(shlv, sv_sh);
        g_nren--;
        view_unbind(mark);
        if (!handled) { free(armb.p); free(pre.p); }
        else {
          TyKind resty = repr_of(c, id).as_ty;
          buf_printf(b, "({ sp_String *_t%d = %s;%s"
                        " if (sp_String_is_frozen(_t%d)) sp_raise_frozen_str(_t%d->data);"
                        " const char *lv__sb%d = sp_str_concat(sp_String_cstr(_t%d), (&(\"\\xff\")[1]));"
                        " SP_GC_ROOT(lv__sb%d); ",
                     tH, srefL, pre.p ? pre.p : "", tH, tH, tH, tH, tH);
          free(pre.p);
          emit_ctype(c, resty == TY_UNKNOWN || resty == TY_VOID ? TY_STRING : resty, b);
          buf_printf(b, " _res%d = %s;", tH, armb.p ? armb.p : "0");
          free(armb.p);
          buf_printf(b, " sp_String_set_bin(_t%d, lv__sb%d); _res%d; })", tH, tH, tH);
          return 1;
        }
      }
    }
  }
  /* String#clear in VALUE position on an unnamed mutable receiver
     ((+"abc").clear): the temp's mutation is unobservable, so evaluate the
     receiver (a frozen value still raises, as CRuby) and yield a fresh
     unfrozen empty string. Named receivers keep the assignable arms. */
  {
    const NodeTable *ntC = c->nt;
    const char *nmC = nt_str(ntC, id, "name");
    int recvC = nt_ref(ntC, id, "receiver");
    int rcore = recvC;
    while (rcore >= 0 && nt_type(ntC, rcore) &&
           sp_streq(nt_type(ntC, rcore), "ParenthesesNode")) {
      int pb0 = nt_ref(ntC, rcore, "body"); int pbn0 = 0;
      const int *pbb0 = pb0 >= 0 ? nt_arr(ntC, pb0, "body", &pbn0) : NULL;
      rcore = pbn0 == 1 ? pbb0[0] : -1;
    }
    if (nmC && sp_streq(nmC, "clear") && recvC >= 0 && rcore >= 0 &&
        comp_ntype(c, recvC) == TY_STRING &&
        nt_type(ntC, rcore) &&
        (sp_streq(nt_type(ntC, rcore), "CallNode") ||
         sp_streq(nt_type(ntC, rcore), "StringNode") ||
         sp_streq(nt_type(ntC, rcore), "InterpolatedStringNode")) &&
        !strbuf_local_name(c, rcore)) {
      int aC = nt_ref(ntC, id, "arguments"); int anC = 0;
      if (aC >= 0) nt_arr(ntC, aC, "arguments", &anC);
      if (anC == 0 && nt_ref(ntC, id, "block") < 0) {
        int tC = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = ", tC);
        emit_expr(c, recvC, b);
        buf_printf(b, "; sp_str_check_mutable(_t%d); (void)_t%d; sp_str_from_bytes(\"\", 0); })", tC, tC);
        return 1;
      }
    }
  }
  /* Array#slice(i) / #slice(range) are exactly #[](...) -- reuse that arm
     through a rename re-entry (the two-argument slice already works). */
  {
    const NodeTable *nt0 = c->nt;
    const char *nm0 = nt_str(nt0, id, "name");
    if (nm0 && sp_streq(nm0, "slice")) {
      int recv0 = nt_ref(nt0, id, "receiver");
      int args0 = nt_ref(nt0, id, "arguments");
      int an0 = 0;
      if (args0 >= 0) nt_arr(nt0, args0, "arguments", &an0);
      if (recv0 >= 0 && an0 == 1 && ty_is_array(comp_ntype(c, recv0)) &&
          nt_ref(nt0, id, "block") < 0) {
        nt_node_set_str((NodeTable *)nt0, id, "name", "[]");
        int h = emit_array_call(c, id, b);
        nt_node_set_str((NodeTable *)nt0, id, "name", "slice");
        if (h) return 1;
      }
    }
    /* combination-family, slice/cons and zip block forms in VALUE
       position: run the statement emitter against a hoisted receiver, then
       evaluate to the receiver (combination family returns self) or nil
       (zip; a valued break routes through the brk wrapper instead). */
    if (nm0 && nt_ref(nt0, id, "block") >= 0 && g_n_argov < MAX_ARG_OVERRIDE &&
        (sp_streq(nm0, "combination") || sp_streq(nm0, "permutation") ||
         sp_streq(nm0, "repeated_combination") || sp_streq(nm0, "repeated_permutation") ||
         sp_streq(nm0, "each_slice") || sp_streq(nm0, "each_cons") ||
         sp_streq(nm0, "zip"))) {
      int recv0 = nt_ref(nt0, id, "receiver");
      TyKind rt0 = recv0 >= 0 ? comp_ntype(c, recv0) : TY_UNKNOWN;
      if (recv0 >= 0 && ty_is_array(rt0)) {
        int ta0 = ++g_tmp;
        Buf ra0 = expr_buf(c, recv0);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, rt0, g_pre);
        buf_printf(g_pre, " _t%d = %s; SP_GC_ROOT(_t%d);\n", ta0, ra0.p ? ra0.p : "NULL", ta0);
        free(ra0.p);
        view_bind(recv0, "_t%d", ta0);
        buf_puts(b, "({ ");
        emit_stmt(c, id, b, 0);
        view_unbind(g_n_argov - 1);
        if (sp_streq(nm0, "zip"))
          buf_puts(b, " sp_box_nil(); })");   /* zip { } returns nil */
        else
          buf_printf(b, " _t%d; })", ta0);  /* the others return self (Ruby >= 3.1) */
        return 1;
      }
    }
    /* Array#equal? -- object identity is pointer identity. A boxed argument
       can hold this very Array (`b = [a, 1][0]`), so its payload is
       compared; any other argument can never be the same object, but it
       and the receiver still run. */
    if (nm0 && sp_streq(nm0, "equal?")) {
      int recv0 = nt_ref(nt0, id, "receiver");
      int args0 = nt_ref(nt0, id, "arguments");
      int an0 = 0;
      const int *av0 = args0 >= 0 ? nt_arr(nt0, args0, "arguments", &an0) : NULL;
      if (recv0 >= 0 && an0 == 1 && ty_is_array(comp_ntype(c, recv0))) {
        TyKind at0 = comp_ntype(c, av0[0]);
        if (ty_is_array(at0) || ty_is_hash(at0)) {
          Buf rb = expr_buf(c, recv0), ab = expr_buf(c, av0[0]);
          buf_printf(b, "((void *)(%s) == (void *)(%s))",
                     rb.p ? rb.p : "0", ab.p ? ab.p : "0");
          free(rb.p); free(ab.p);
        }
        else if (at0 == TY_POLY) {
          int teq = ++g_tmp, trq = ++g_tmp;
          buf_printf(b, "({ void *_t%d = (void *)(", trq);
          emit_expr(c, recv0, b);
          buf_printf(b, "); sp_RbVal _t%d = ", teq);
          emit_boxed(c, av0[0], b);
          buf_printf(b, "; (sp_bool)(_t%d.tag == SP_TAG_OBJ && _t%d.v.p == _t%d); })", teq, teq, trq);
        }
        else {
          buf_puts(b, "((void)("); emit_expr(c, recv0, b);
          buf_puts(b, "), (void)("); emit_expr(c, av0[0], b);
          buf_puts(b, "), (sp_bool)0)");
        }
        return 1;
      }
    }
  }
  int pv[3], npv = array_call_empty_views(c, id, pv);
  int r = emit_array_call_arms(c, id, b);
  while (npv > 0) view_pop(c, pv[--npv]);
  return r;
}

/* emit_array_call past its prefix arms, under the empty-receiver views */
static int emit_array_call_arms(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = comp_recv_type(c, recv);
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  TyKind res = repr_of(c, id).as_ty;   /* a Hash result's C table (to_h) */
  /* [].first / [].last on an empty literal: there is no element type to read;
     the value is nil (boxed -- the call types poly). */
  if (recv >= 0 && argc == 0 && (is_endpoint_query(name)) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
    int fe_n = 0; nt_arr(nt, recv, "elements", &fe_n);
    if (fe_n == 0) { buf_puts(b, "sp_box_nil()"); return 1; }
  }
  /* Homogeneous object array (sp_PtrArray of unboxed sp_X*), produced by the
     post-fixpoint narrow_object_arrays pass. Indexing yields a typed `sp_X *`
     directly -- no sp_RbVal box, no cls-id dispatch. Only the op set the pass
     admits reaches here; the pass and this block stay in lockstep. */
  if (recv >= 0 && ty_is_ptr_array(rt)) {
    const char *nested = ty_ptr_array_elem_ctype(rt);
    int ecls = nested ? -1 : ty_obj_array_class(rt);
    /* element C type: the indexed pointer type. For a nested scalar-array kind
       the element is an sp_IntArray or sp_FloatArray pointer; for an object
       array it is the class's own struct, which for a native class is the name
       its `native_struct` declared rather than one derived from the Ruby name.
       Copied out of class_ctype's rotating buffer, since it is held across emit
       calls. */
    char ecbuf[192];
    snprintf(ecbuf, sizeof ecbuf, "%s", nested ? nested : class_ctype(c, ecls));
    const char *ecn = ecbuf;
    if ((is_element_at_alias(name)) && argc == 1) {
      buf_printf(b, "((%s *)sp_PtrArray_get(", ecn);
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, "))");
      return 1;
    }
    if ((is_endpoint_query(name)) && argc == 0) {
      buf_printf(b, "((%s *)sp_PtrArray_get(", ecn);
      emit_expr(c, recv, b);
      buf_puts(b, sp_streq(name, "first") ? ", 0))" : ", -1))");
      return 1;
    }
    if (sp_streq(name, "[]=") && argc == 2) {
      int tv = ++g_tmp;
      buf_printf(b, "({ %s *_t%d = ", ecn, tv);
      /* A poly value carries its pointer under a tag, and the slot takes the
         pointer, not the sp_RbVal -- the same unboxing the push arm below does
         for the same reason (#4293). Without it `t[i] = f(x)`, where f's return
         widened to poly, initialized a typed element pointer from an sp_RbVal
         and the C did not compile. */
      if (repr_of(c, argv[1]).kind == RK_BOXED) {
        buf_printf(b, "(%s *)sp_poly_obj_ptr(", ecn);
        emit_expr(c, argv[1], b);
        buf_puts(b, ")");
      }
      else emit_expr(c, argv[1], b);
      /* Ruby's index rules: a store past the end grows the array (nil-filled),
         a negative index counts from the end, below -len is IndexError, and a
         frozen array refuses. The fixed-shape setter dropped a store past the
         end on the floor, so `banks = []; banks[0] = Rom.new` kept an empty
         array and the read of banks[0] crashed (#4512). */
      buf_puts(b, "; sp_PtrArray_set_grow("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_int_expr(c, argv[0], b); buf_printf(b, ", _t%d); _t%d; })", tv, tv);
      return 1;
    }
    if (is_push_alias(name) && argc >= 1) {
      int tr = ++g_tmp;
      buf_printf(b, "({ sp_PtrArray *_t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, ";");
      for (int a = 0; a < argc; a++) {
        buf_printf(b, " sp_PtrArray_push(_t%d, ", tr);
        /* A poly value carries its pointer under a tag: the slot takes the
           pointer, not the sp_RbVal. A method of the array's own class whose
           return widened to poly -- one that answers its argument, reached
           once with a boxed one -- pushed the whole struct and the C did not
           compile (#4293). */
        emit_poly_unboxed(c, argv[a], repr_of(c, argv[a]).as_ty, "sp_poly_obj_ptr(", b);
        buf_puts(b, ");");
      }
      buf_printf(b, " _t%d; })", tr);
      return 1;
    }
    if (is_len_alias(name) && argc == 0) {
      buf_puts(b, "sp_PtrArray_length("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "empty?") && argc == 0) {
      buf_puts(b, "sp_PtrArray_empty("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    /* no-block comparisons via the boxed comparator (user `<=>` through the
       cmp hook); the narrowing pass admits these only when the element class
       has `<=>` and (for sort) the result lands in a modeled consumer. */
    if (!nested && sp_streq(name, "sort") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      buf_puts(b, "sp_PtrArray_sort_obj("); emit_expr(c, recv, b);
      buf_printf(b, ", %d)", ecls);
      return 1;
    }
    if (!nested && sp_streq(name, "sort!") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      int tr = ++g_tmp;
      buf_printf(b, "({ sp_PtrArray *_t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; sp_PtrArray_sort_obj_bang(_t%d, %d); _t%d; })", tr, ecls, tr);
      return 1;
    }
    if (!nested && (is_minmax_query(name)) && argc == 0 &&
        nt_ref(nt, id, "block") < 0) {
      buf_printf(b, "((%s *)sp_PtrArray_minmax_obj(", ecn);
      emit_expr(c, recv, b);
      buf_printf(b, ", %d, %d))", ecls, sp_streq(name, "max") ? 1 : 0);
      return 1;
    }
    return 0;  /* unsupported obj-array op: pass should have prevented this. */
  }
  { int r; if (emit_str_mutator_call(c, id, b, nt, name, recv, argc, argv, rt, &r)) return r; }

  /* find/detect over a bare poly value that is only known to be an array at
     runtime (an inner array read out of a poly container:
     `[[1,2],[3,4]].map { |row| row.find { } }`): coerce to a poly array and
     scan, mirroring the poly-array form inside the ty_is_array block below,
     which this receiver type does not enter (#2904). */
  if (recv >= 0 && rt == TY_POLY && (is_find_alias(name)) &&
      nt_ref(nt, id, "block") >= 0 && argc == 0) {
    int fblock = nt_ref(nt, id, "block");
    const char *bp = block_param_name(c, fblock, 0); if (bp) bp = rename_local(bp);
    int fbody = nt_ref(nt, fblock, "body");
    int fbn = 0; const int *fbb = fbody >= 0 ? nt_arr(nt, fbody, "body", &fbn) : NULL;
    if (fbn >= 1) {
      int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_arr_recv(%s, \"%s\"); SP_GC_ROOT(_t%d);\n",
                 trecv, rb.p ? rb.p : "sp_box_nil()", name, trecv);
      free(rb.p);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tres, tres);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
      char es[64]; snprintf(es, sizeof es, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
      int splat = emit_iter_autosplat(c, fblock, TY_POLY_ARRAY, es, g_indent + 1);
      if (!splat && bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = %s;\n", bp, es); }
      Buf cb = block_cond_buf(c, fblock, fbb, fbn);
      emit_indent(g_pre, g_indent + 1);
      if (!splat && bp) buf_printf(g_pre, "if (%s) { _t%d = lv_%s; break; }\n", cb.p ? cb.p : "0", tres, bp);
      else buf_printf(g_pre, "if (%s) { _t%d = %s; break; }\n", cb.p ? cb.p : "0", tres, es);
      free(cb.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      buf_printf(b, "_t%d", tres); return 1;
    }
  }

  /* sort / reject / each_index over a bare poly value that is an array at
     runtime (an inner array read out of a poly container), following #2904.
     Coerce to a poly array and run the operation. (#2928) */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "sort") && argc == 0 &&
      nt_ref(nt, id, "block") < 0) {
    buf_puts(b, "sp_poly_sort("); emit_expr(c, recv, b); buf_puts(b, ")");
    return 1;
  }
  /* uniq on a bare poly value that is an array at runtime (an ivar assigned a
     caller-splat rest array widens to poly): the distinct elements (#3341). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "uniq") && argc == 0 &&
      nt_ref(nt, id, "block") < 0 && !recv_user_defines(c, name)) {
    buf_puts(b, "sp_poly_uniq("); emit_expr(c, recv, b); buf_puts(b, ")");
    return 1;
  }
  /* compact / flatten on the same shape: an Array read out of a container.
     uniq had an arm and these did not, so they raised NoMethodError naming
     Array -- which is what the receiver was (#3423). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "compact") || sp_streq(name, "flatten")) &&
      !recv_user_defines(c, name)) {
    /* compact keeps the receiver's kind (a Hash drops its nil VALUES and stays
       a Hash), so it answers boxed; flatten is an Array either way. */
    buf_printf(b, "sp_poly_%s(", sp_streq(name, "compact") ? "compact_val" : "flatten");
    emit_expr(c, recv, b); buf_puts(b, ")");
    return 1;
  }
  /* flatten(depth): the depth unwraps that many levels (sp_poly_flatten_d) */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      sp_streq(name, "flatten") && comp_ntype(c, argv[0]) == TY_INT &&
      !recv_user_defines(c, name)) {
    buf_puts(b, "sp_poly_flatten_d("); emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ", 1)");
    return 1;
  }
  /* a boxed depth (an Integer local in promote mode): nil is no depth, as
     Array#flatten(nil) is the full flatten */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      sp_streq(name, "flatten") && comp_ntype(c, argv[0]) == TY_POLY &&
      !recv_user_defines(c, name)) {
    int tr = ++g_tmp, td = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = ", tr); emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_RbVal _t%d = ", tr, td); emit_expr(c, argv[0], b);
    buf_printf(b, "; sp_poly_flatten_d(_t%d, _t%d.tag == SP_TAG_NIL ? 0 : sp_poly_arg_int_chk(_t%d), _t%d.tag != SP_TAG_NIL); })",
               tr, td, td, td);
    return 1;
  }
  /* `enum.drop(n)` / `enum.reject|select|filter { }` on an each_with_index-style
     Enumerator: materialize its pairs to a poly array and re-dispatch as the
     array form (drop returns a slice; the block forms run the block over each
     pair). (#2878, #2943) */
  if (recv >= 0 && rt == TY_ENUMERATOR && g_n_argov < MAX_ARG_OVERRIDE &&
      ((sp_streq(name, "drop") && argc == 1 && nt_ref(nt, id, "block") < 0) ||
       ((sp_streq(name, "reject") || sp_streq(name, "select") || sp_streq(name, "filter") ||
         sp_streq(name, "sort_by") ||
         sp_streq(name, "map") || sp_streq(name, "collect") ||
         sp_streq(name, "sum")) &&
        argc == 0 && nt_ref(nt, id, "block") >= 0))) {
    int ta = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_Enumerator_to_a(%s); SP_GC_ROOT(_t%d);\n",
               ta, rb.p ? rb.p : "", ta);
    free(rb.p);
    view_bind(recv, "_t%d", ta);
    int v = view_push(c, recv, TY_POLY_ARRAY);
    emit_expr(c, id, b);
    view_pop(c, v);
    view_unbind(g_n_argov - 1);
    return 1;
  }
  /* `poly.reduce/inject { }` (with or without a seed) where poly is an
     array read out of a container (a transpose row, a nested element):
     coerce to a poly array and re-enter the array fold emitter with the
     receiver overridden (#3312). */
  /* The operator-SYMBOL form takes the same route: `reduce(:+)` and
     `reduce(init, :+)` carry no block, and without this they fell through to
     the Hash/Enumerable face, which converts the receiver to a hash and refuses
     an Array at run time -- so a partitioned array answered NoMethodError
     (#4079). */
  int red_sym = (nt_ref(nt, id, "block") < 0 && argc >= 1 && argc <= 2 &&
                 nt_type(nt, argv[argc - 1]) &&
                 sp_streq(nt_type(nt, argv[argc - 1]), "SymbolNode"));
  if (recv >= 0 && rt == TY_POLY &&
      (nt_ref(nt, id, "block") >= 0 || red_sym) &&
      (is_reduce_alias(name)) &&
      (argc <= 1 || red_sym) && g_n_argov < MAX_ARG_OVERRIDE) {
    int ta = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_arr_recv(%s, \"%s\"); SP_GC_ROOT(_t%d);\n",
               ta, rb.p ? rb.p : "sp_box_nil()", name, ta);
    free(rb.p);
    view_bind(recv, "_t%d", ta);
    int v = view_push(c, recv, TY_POLY_ARRAY);
    emit_expr(c, id, b);
    view_pop(c, v);
    view_unbind(g_n_argov - 1);
    return 1;
  }
  /* `poly.reject/select/filter { |x| ... }` where poly is an array read out of
     a container: coerce to a poly array and filter it into a fresh poly array,
     mirroring the find arm above (this TY_POLY receiver would otherwise skip to
     the loud NoMethodError). (#2930) */
  /* find_all rides the same loop and answers the plain Array of elements
     whatever the receiver is (CRuby's Hash#find_all gives the [k, v] pairs,
     unlike Hash#select), so it skips the sp_poly_kept_result hop that hands
     a Hash receiver a Hash back. */
  int pf_rej = sp_streq(name, "reject");
  int pf_sel = is_select_alias(name);
  int pf_fa  = sp_streq(name, "find_all");
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") >= 0 && argc == 0 &&
      (pf_rej || pf_sel || pf_fa)) {
    int fblock = nt_ref(nt, id, "block");
    const char *bp = block_param_name(c, fblock, 0); if (bp) bp = rename_local(bp);
    int fbody = nt_ref(nt, fblock, "body");
    int fbn = 0; const int *fbb = fbody >= 0 ? nt_arr(nt, fbody, "body", &fbn) : NULL;
    if (fbn >= 1) {
      int keep_truthy = !pf_rej;  /* select/filter/find_all keep truthy */
      int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp, tbox = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      /* Keep the receiver boxed as well as coerced: Hash#select answers a
         Hash, Array#select an Array, and only the runtime value says which
         (#3449). sp_poly_arr_recv renders a hash as its [key, value] pairs, so
         the loop below is the same either way. */
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
                 tbox, rb.p ? rb.p : "sp_box_nil()", tbox);
      free(rb.p);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_arr_recv(_t%d, \"%s\"); SP_GC_ROOT(_t%d);\n",
                 trecv, tbox, name, trecv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
      /* Hash#select and Hash#reject yield the key and the value as two
         values (find_all yields the pair whole, as Hash#each does): a lone
         `|k|` takes the key, and a block of any other shape than plain
         requireds binds the step's values by the proc distribution. */
      int gather = block_binds_gathered(c, fblock), thash = 0;
      int lone = !gather && bp && !block_param_name(c, fblock, 1) && !block_rest_marker(c, fblock) &&
                 !block_param_is_multi(c, fblock, 0);
      if ((gather || lone) && !pf_fa) {
        thash = ++g_tmp;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "int _t%d = _t%d.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(_t%d.cls_id);\n",
                   thash, tbox, tbox);
      }
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
      char es[64]; snprintf(es, sizeof es, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
      int splat = 0;
      if (gather) {
        char vals[128];
        if (thash) snprintf(vals, sizeof vals, "sp_yielded_args(_t%d, %s)", thash, es);
        else snprintf(vals, sizeof vals, "sp_yielded_args(0, %s)", es);
        emit_boxed_step_binds(c, fblock, vals, g_pre, g_indent + 1, 0);
        splat = 1;
      }
      else splat = emit_iter_autosplat(c, fblock, TY_POLY_ARRAY, es, g_indent + 1);
      if (!splat && bp && thash) {
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "sp_RbVal lv_%s = sp_yielded_first(_t%d, %s);\n", bp, thash, es);
      }
      else if (!splat && bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = %s;\n", bp, es); }
      Buf cb = block_cond_buf(c, fblock, fbb, fbn);
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "if (%s(%s)) sp_PolyArray_push(_t%d, %s);\n",
                 keep_truthy ? "" : "!", cb.p ? cb.p : "0", tres, es);
      free(cb.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      if (pf_sel || pf_rej) buf_printf(b, "sp_poly_kept_result(_t%d, _t%d)", tbox, tres);
      else buf_printf(b, "_t%d", tres);
      return 1;
    }
  }
  /* `poly.split(sep[, limit])` where poly holds a string (a String param
     widened to poly by a poly call site): dispatch String#split at runtime.
     Without this the whole `str.split.map` chain stayed UNKNOWN and a following
     multiple assignment was rejected (#3186 / #3164). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "split") &&
      (argc == 0 || argc == 1 || argc == 2) && nt_ref(nt, id, "block") < 0) {
    int tv = ++g_tmp;
    /* A shared-string handle (`s = +""; s << "a;b"`) is a String, and every
       arm below reads `.v.s`, so the handle is dereferenced into the
       immediate form first -- the same retry sp_poly_add makes. Without it
       the guard was single-armed and a heap String raised NoMethodError
       naming String, for a method String has (#4279). */
    buf_printf(b, "({ sp_RbVal _t%d = sp_poly_strbuf_deref(", tv); emit_expr(c, recv, b);
    buf_puts(b, ")");
    buf_printf(b, "; _t%d.tag == SP_TAG_STR ? ", tv);
    if (argc == 0) buf_printf(b, "sp_str_split_ws(_t%d.v.s)", tv);
    else if (argc == 1) {
      /* a regex separator splits with sp_re_split, not the string-separator path
         (which would coerce the pattern to a bogus literal separator) (#3212). */
      if (comp_ntype(c, argv[0]) == TY_REGEX) {
        buf_puts(b, "sp_re_split("); emit_expr(c, argv[0], b); buf_printf(b, ", _t%d.v.s)", tv);
      }
      else {
        /* the split separator slot, shared with the String-receiver path:
           handles the nil whitespace mode, statically and at run time, and
           raises split's own TypeError wording for a wrong class (#4223) */
        buf_printf(b, "sp_str_split_drop_trailing(_t%d.v.s, ", tv);
        emit_str_pattern_expr(c, argv[0], b); buf_puts(b, ")");
      }
    }
    else if (comp_ntype(c, argv[0]) == TY_REGEX) {
      /* a regex separator with a limit: sp_re_split_limit, as the typed
         String path; the string-separator slot below refused the pattern */
      buf_puts(b, "sp_re_split_limit("); emit_expr(c, argv[0], b);
      buf_printf(b, ", _t%d.v.s, ", tv); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
    }
    else {
      /* The separator of String#split can be nil ("split on whitespace" in
         CRuby); sp_str_split_limit treats a NULL separator as that mode.
         emit_str_pattern_expr is the split-separator slot shared with the
         String-receiver path: it evaluates a statically nil separator for
         its side effects and passes NULL, keeps the same answer for a
         separator that is nil only at run time, and raises split's own
         TypeError wording for a wrong class (#4223). */
      buf_printf(b, "sp_str_split_limit(_t%d.v.s, ", tv);
      emit_str_pattern_expr(c, argv[0], b);
      buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
    }
    buf_printf(b, " : (sp_StrArray *)(sp_raise_nomethod(sp_nomethod_msg(\"split\", _t%d)), (void *)0); })", tv);
    return 1;
  }
  /* poly.lines(sep) / lines(chomp: ...) / lines(sep, chomp: ...): the typed
     String path's lines over the receiver read as a String, which raises
     NoMethodError for anything else, as the argumentless poly.lines does */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "lines") && nt_ref(nt, id, "block") < 0 &&
      poly_lines_args(c, argc, argv) && !user_defines_or_reads(c, "lines")) {
    int tl = ++g_tmp;
    char rl[32]; snprintf(rl, sizeof rl, "_t%d", tl);
    buf_printf(b, "({ const char *_t%d = sp_poly_recv_s(", tl); emit_expr(c, recv, b);
    buf_printf(b, ", \"lines\"); SP_GC_ROOT(_t%d); ", tl);
    if (argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
      buf_printf(b, "sp_str_lines_sep(%s, ", rl); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else str_arms_convert(c, id, b, nt, name, recv, argc, argv, TY_UNKNOWN, rl);
    buf_puts(b, "; })");
    return 1;
  }
  /* poly.each_line(sep / chomp: ...): the lines the same arguments give
     lines; blockless, that array (as the argumentless poly.each_line), and
     with a block each one yielded and the receiver answered, as
     poly.each_line { } does */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "each_line") &&
      poly_lines_args(c, argc, argv) &&
      !user_defines_or_reads(c, "each_line") && !user_defines_or_reads(c, "lines")) {
    int tl = ++g_tmp, ta = ++g_tmp;
    char rl[32]; snprintf(rl, sizeof rl, "_t%d", tl);
    buf_printf(b, "({ const char *_t%d = sp_poly_recv_s(", tl); emit_expr(c, recv, b);
    buf_printf(b, ", \"each_line\"); SP_GC_ROOT(_t%d); sp_StrArray *_t%d = ", tl, ta);
    if (argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
      buf_printf(b, "sp_str_lines_sep(%s, ", rl); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else str_arms_convert(c, id, b, nt, "lines", recv, argc, argv, TY_UNKNOWN, rl);
    buf_printf(b, "; SP_GC_ROOT(_t%d);", ta);
    int eblk = nt_ref(nt, id, "block");
    if (eblk < 0) { buf_printf(b, " _t%d; })", ta); return 1; }
    const char *ebp = block_param_name(c, eblk, 0);
    const char *ebpn = ebp ? rename_local(ebp) : NULL;
    int ebody = nt_ref(nt, eblk, "body");
    int ebn = 0; const int *ebb = ebody >= 0 ? nt_arr(nt, ebody, "body", &ebn) : NULL;
    int ti = ++g_tmp;
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++) {", ti, ti, ta, ti);
    if (ebpn) emit_str_elem_param(c, eblk, ebp, ebpn, ta, ti, b);
    for (int k2 = 0; k2 < ebn; k2++) emit_stmt(c, ebb[k2], b, 0);
    buf_printf(b, " } _t%d; })", tl);
    return 1;
  }
  /* `poly.map! { |x| ... }` / `collect!` where poly is an array read out of a
     container: coerce to a poly array and rewrite each element in place with
     the block result, returning the (mutated) array (#3162). */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") >= 0 && argc == 0 &&
      (is_map_bang_alias(name))) {
    int mblock = nt_ref(nt, id, "block");
    const char *bp = block_param_name(c, mblock, 0); if (bp) bp = rename_local(bp);
    int mbody = nt_ref(nt, mblock, "body");
    int mbn = 0; const int *mbb = mbody >= 0 ? nt_arr(nt, mbody, "body", &mbn) : NULL;
    if (mbn >= 1) {
      int trecv = ++g_tmp, ti = ++g_tmp, torig = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
                 torig, rb.p ? rb.p : "sp_box_nil()", torig);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_arr_recv(_t%d, \"map!\"); SP_GC_ROOT(_t%d);\n",
                 trecv, torig, trecv);
      free(rb.p);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
      char es[64]; snprintf(es, sizeof es, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
      int splat = emit_iter_autosplat(c, mblock, TY_POLY_ARRAY, es, g_indent + 1);
      /* the element is boxed; a parameter the analysis typed (the receiver
         read back from a box it filled with typed values) takes it unboxed,
         where a boxed copy shadowed the typed slot the body reads */
      Scope *mbs = comp_scope_of(c, mblock);
      LocalVar *mlv = bp && mbs ? scope_local(mbs, block_param_name(c, mblock, 0)) : NULL;
      TyKind mpt = mlv ? mlv->type : TY_UNKNOWN;
      if (!splat && bp && mpt != TY_POLY && mpt != TY_UNKNOWN) {
        emit_indent(g_pre, g_indent + 1);
        emit_block_param_from_boxed(c, bp, mpt, es, g_pre);
      }
      else if (!splat && bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = %s;\n", bp, es); }
      IterStep st; emit_iter_step_open(c, mblock, 1, g_indent + 1, &st);
      int sv = g_indent; g_indent++;
      Buf vb; memset(&vb, 0, sizeof vb); emit_iter_step_tail(c, &st, &vb); g_indent = sv;
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "sp_PolyArray_set(_t%d, _t%d, %s);\n", trecv, ti, vb.p ? vb.p : "sp_box_nil()");
      free(vb.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_poly_arr_writeback(_t%d, _t%d);\n", torig, trecv);
      buf_printf(b, "_t%d", trecv);
      return 1;
    }
  }
  /* `poly.zip(other...)` on a poly array read out of a container (e.g. a row
     that is a block param of an outer nested-array iterator): coerce the
     receiver to a poly array and re-dispatch as the array zip form, whose arg
     handling already unboxes a poly argument too (#3190). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "zip") && argc >= 1 &&
      nt_ref(nt, id, "block") < 0 && g_n_argov < MAX_ARG_OVERRIDE) {
    int ta = ++g_tmp;
    Buf rb = expr_buf(c, recv);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_arr_recv(%s, \"zip\"); SP_GC_ROOT(_t%d);\n",
               ta, rb.p ? rb.p : "sp_box_nil()", ta);
    free(rb.p);
    view_bind(recv, "_t%d", ta);
    int v = view_push(c, recv, TY_POLY_ARRAY);
    emit_expr(c, id, b);
    view_pop(c, v);
    view_unbind(g_n_argov - 1);
    return 1;
  }
  /* `poly.each_index { |i| ... }` on a poly array read out of a container:
     iterate the index range, yield each index, and return self. (#2930) */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "each_index") &&
      nt_ref(nt, id, "block") >= 0 && argc == 0) {
    int eb = nt_ref(nt, id, "block");
    const char *ip_orig = block_param_name(c, eb, 0);
    Scope *eic = comp_scope_of(c, eb);
    /* An unused index param is pruned by liveness (scope_local NULL, no lv_<name>
       declared): gate the binding so we never assign to an undeclared C name. */
    LocalVar *eilv = (ip_orig && eic) ? scope_local(eic, ip_orig) : NULL;
    int body = nt_ref(nt, eb, "body");
    int tself = ++g_tmp, trecv = ++g_tmp, ti = ++g_tmp;
    Buf rb = expr_buf(c, recv);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
               tself, rb.p ? rb.p : "sp_box_nil()", tself);
    free(rb.p);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_arr_recv(_t%d, \"each_index\"); SP_GC_ROOT(_t%d);\n",
               trecv, tself, trecv);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
    if (eilv) {
      const char *ip = rename_local(ip_orig);
      emit_indent(g_pre, g_indent + 1);
      if (eilv->type == TY_POLY) buf_printf(g_pre, "lv_%s = sp_box_int(_t%d);\n", ip, ti);
      else buf_printf(g_pre, "lv_%s = _t%d;\n", ip, ti);
    }
    emit_iter_loop_stmts(c, body, g_pre, g_indent + 1);
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
    buf_printf(b, "_t%d", tself);
    return 1;
  }
  /* Blockless each_slice(n) / each_cons(n) on a boxed receiver: a boxed
     generator or endless Enumerator regroups as it is pulled (materializing
     it never returned), anything else through its elements as below. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      (is_each_window(name)) &&
      comp_ntype(c, id) == TY_ENUMERATOR && !user_defines_or_reads(c, name)) {
    int te = ++g_tmp;
    Buf eb; memset(&eb, 0, sizeof eb); emit_expr(c, recv, &eb);
    Buf nb; memset(&nb, 0, sizeof nb); emit_int_expr(c, argv[0], &nb);
    buf_printf(b, "({ sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d); sp_poly_regroup(_t%d, %s, %d); })",
               te, eb.p ? eb.p : "sp_box_nil()", te, te, nb.p ? nb.p : "0", sp_streq(name, "each_cons"));
    free(eb.p); free(nb.p);
    return 1;
  }
  /* `poly.sort_by { |k, v| ... }` where poly is a hash/array read out of a
     container: materialize its elements (a hash yields [k, v] pairs) as a poly
     array and re-dispatch as an array sort_by -- the array path's 2-param
     autosplat destructures each pair, matching the typed Hash#sort_by. (#2935) */
  /* The blockless grouping enumerators take the same route: CRuby answers an
     Enumerator, spinel materializes it, so re-dispatching as the array form
     gives the groups a later .map / .to_a can walk. */
  int rd_kind = recv >= 0 && rt == TY_POLY && g_n_argov < MAX_ARG_OVERRIDE && g_poly_redispatch_id != id
                ? poly_redispatch_kind(c, id, name, argc) : 0;
  if (rd_kind) {
    int ta = ++g_tmp;
    int ret_recv = rd_kind == 2;
    if (g_plan_check) {   /* the re-entry, held against the plan before the view */
      int pa_frame = pa_begin(id);
      pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + (ret_recv ? PB_REDISPATCH_RECV : PB_REDISPATCH), -1, TY_UNKNOWN,
                 PC_SAME);
      pa_end(c, pa_frame, cplan_poly_redispatch(c, id));
    }
    int tbox = ret_recv ? ++g_tmp : 0;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    if (ret_recv) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
                 tbox, rb.p ? rb.p : "sp_box_nil()", tbox);
      free(rb.p); memset(&rb, 0, sizeof rb);
      buf_printf(&rb, "_t%d", tbox);
    }
    emit_indent(g_pre, g_indent);
    /* nil has to_a but not these: it raises, naming the method */
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_enum_recv_arr(%s, \"%s\"); SP_GC_ROOT(_t%d);\n",
               ta, rb.p ? rb.p : "sp_box_nil()", name, ta);
    free(rb.p);
    view_bind(recv, "_t%d", ta);
    int v = view_push(c, recv, TY_POLY_ARRAY);
    /* and pin it for the inference too, the way the hash face does: the cached
       type alone does not survive a safe-navigation guard, whose re-emission
       asks again and re-establishes the receiver as poly -- the array emitters
       then decline the very call this arm re-entered to have them serve. */
    int fv = view_push_face(recv, TY_POLY_ARRAY);
    /* The re-entry below is the SAME node, and neither the type nor the pin
       stops it reaching this arm again. Latch the node. */
    int sv_rd = g_poly_redispatch_id; g_poly_redispatch_id = id;
    if (ret_recv) {
      Buf vb; memset(&vb, 0, sizeof vb);
      emit_call(c, id, &vb);
      buf_printf(b, "({ (void)(%s); _t%d; })", vb.p ? vb.p : "0", tbox);
      free(vb.p);
    }
    else emit_call(c, id, b);
    g_poly_redispatch_id = sv_rd;
    view_pop(c, fv);
    view_pop(c, v);
    view_unbind(g_n_argov - 1);
    return 1;
  }
  { int r; if (emit_typed_array_call(c, id, b, nt, name, recv, argc, argv, rt, a0, res, &r)) return r; }
  return 0;
}

void emit_push_hash_key(TyKind kt, int dest, int th, int ti, Buf *b) {
  if (kt == TY_SYMBOL)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym(_t%d->order[_t%d]));", dest, th, ti);
  else if (kt == TY_STRING)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(_t%d->order[_t%d]));", dest, th, ti);
  else if (kt == TY_INT)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(_t%d->order[_t%d]));", dest, th, ti);
  else
    buf_printf(b, " sp_PolyArray_push(_t%d, _t%d->keys[_t%d->order[_t%d]]);", dest, th, th, ti);
}

/* The value at key text `k` of the typed hash text `h` (receiver node
   `recv`, of kind name hn and value kind vt), boxed: with its nil when the
   hash's values may be nil (DESIGN.md D3b-ii) */
static void emit_hash_val_boxed(Compiler *c, int recv, TyKind vt, const char *hn, const char *h, const char *k, Buf *b) {
  if (oint_kind(vt) && hash_vals_nullable(c, recv)) { buf_printf(b, "sp_box_oint(sp_%sHash_vget(%s, %s))", hn, h, k); return; }
  char g[160]; snprintf(g, sizeof g, "sp_%sHash_get(%s, %s)", hn, h, k);
  emit_boxed_text(c, vt, g, b);
}

/* Emit a statement-expression materializing a hash's entries as a PolyArray of
   [key, value] poly pairs in insertion order. The source hash is GC-rooted
   because each pair allocates inside the walk. Shared by Hash#to_a/#entries and
   Hash#sort. */
void emit_hash_pairs_expr(Compiler *c, int recv, TyKind rt, const char *hn, Buf *b) {
  int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tp = ++g_tmp;
  TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
  buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th); emit_expr(c, recv, b);
  buf_printf(b, "; SP_GC_ROOT(_t%d);", th);
  buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
  buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
  buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tp, tp);
  emit_push_hash_key(kt, tp, th, ti, b);
  if (rt == TY_POLY_POLY_HASH)
    buf_printf(b, " sp_PolyArray_push(_t%d, _t%d->vals[_t%d->order[_t%d]]);", tp, th, th, ti);
  else if (vt == TY_POLY)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_%sHash_get(_t%d, _t%d->order[_t%d]));", tp, hn, th, th, ti);
  else if (vt == TY_INT) {
    char hs[24], ks[48]; snprintf(hs, sizeof hs, "_t%d", th); snprintf(ks, sizeof ks, "_t%d->order[_t%d]", th, ti);
    buf_printf(b, " sp_PolyArray_push(_t%d, ", tp); emit_hash_val_boxed(c, recv, vt, hn, hs, ks, b); buf_puts(b, ");");
  }
  else
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", tp, hn, th, th, ti);
  buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));", tr, tp);
  buf_printf(b, " } _t%d; })", tr);
}

/* An empty hash literal: no variant to infer, nothing to fold into a merge. */
static int hash_lit_empty(const NodeTable *nt, int n) {
  const char *ty = nt_type(nt, n);
  if (!ty || !(sp_streq(ty, "HashNode") || sp_streq(ty, "KeywordHashNode"))) return 0;
  int en = 0; nt_arr(nt, n, "elements", &en);
  return en == 0;
}

/* A block body with a `next` of its own answers through a slot: the next
   assigns it and leaves the do{}while(0) emit_block_value_into wraps the
   body in, where a bare `continue` would skip the caller's use of the value.
   Emits `({ T _tN = nil; ...; _tN; })` and answers 1; 0 for a body with no
   next, which the caller reads as leading statements and a tail. */
static int emit_blk_value_via_next(Compiler *c, int blk, TyKind vt, Buf *b) {
  if (!fold_body_has_next(c, nt_ref(c->nt, blk, "body"))) return 0;
  int t = ++g_tmp;
  char dest[32]; snprintf(dest, sizeof dest, "_t%d", t);
  buf_puts(b, "({ "); emit_ctype(c, vt, b);
  buf_printf(b, " %s = %s;\n", dest, vt == TY_POLY ? "sp_box_nil()" : default_value_from_compiler(c, vt));
  Buf *saved_pre = g_pre; int saved_ind = g_indent;
  g_pre = b;
  if (vt != TY_POLY) g_bv_dest_ty = vt;
  emit_block_value_into(c, blk, dest, vt == TY_POLY, 0);
  g_pre = saved_pre; g_indent = saved_ind;
  buf_printf(b, " %s; })", dest);
  return 1;
}

static void emit_blk_value_as(Compiler *c, int blk, TyKind vt, Buf *b) {
  const NodeTable *nt = c->nt;
  if (emit_blk_value_via_next(c, blk, vt, b)) return;
  int bbody = nt_ref(nt, blk, "body");
  int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
  int bval = bn > 0 ? bb[bn - 1] : -1;
  buf_puts(b, "({ ");
  /* The value's setup lands here, after the block parameters are bound for
     this pair: hoisted to the enclosing statement it ran once, before the
     merge loop, on the parameters' initial nil. */
  Buf *saved_pre = g_pre; g_pre = b;
  for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], b, 0);
  Buf tv; memset(&tv, 0, sizeof tv);
  if (bval >= 0) {
    int bval_boxed = repr_of(c, bval).kind == RK_BOXED;
    if (vt == TY_POLY && !bval_boxed) emit_boxed(c, bval, &tv);
    /* and a boxed value into a typed slot (a block handed through a
       method's `&blk` sees boxed parameters, so `o + n` is boxed) */
    else if (bval_boxed && vt != TY_POLY && vt != TY_UNKNOWN && vt != TY_VOID) {
      Buf ev; memset(&ev, 0, sizeof ev);
      emit_expr(c, bval, &ev);
      emit_unbox_text(c, vt, ev.p ? ev.p : "sp_box_nil()", &tv);
      free(ev.p);
    }
    else emit_expr(c, bval, &tv);
  }
  g_pre = saved_pre;
  if (tv.p) buf_puts(b, tv.p);
  else if (bval < 0) buf_puts(b, vt == TY_POLY ? "sp_box_nil()" : default_value_from_compiler(c, vt));
  free(tv.p);
  buf_puts(b, "; })");
}

/* emit_blk_value_as in the oint form of an Integer or Float kind vt: a
   block value that can be nil (`fetch(i) { nil }`) answers it rather than
   raising at the unwrap. A body with a `next` keeps the plain form, lifted. */
static void emit_blk_value_as_o(Compiler *c, int blk, TyKind vt, Buf *b) {
  const NodeTable *nt = c->nt;
  int bbody = nt_ref(nt, blk, "body");
  if (fold_body_has_next(c, bbody)) {
    buf_printf(b, "%s(", oint_of(vt)); emit_blk_value_as(c, blk, vt, b); buf_puts(b, ")");
    return;
  }
  int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
  int bval = bn > 0 ? bb[bn - 1] : -1;
  buf_puts(b, "({ ");
  Buf *saved_pre = g_pre; g_pre = b;
  for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], b, 0);
  Buf tv; memset(&tv, 0, sizeof tv);
  if (bval >= 0) emit_oint_expr(c, bval, vt, &tv);
  g_pre = saved_pre;
  buf_puts(b, tv.p ? tv.p : oint_nil(vt));
  free(tv.p);
  buf_puts(b, "; })");
}

/* Hash#merge(other) { |key, old, new| } built as the general boxed hash:
   walk the other hash's pairs into a boxed copy of the receiver, consulting
   the block on a collision. Answers 0 for an empty block. */
static int emit_merge_block_boxed(Compiler *c, int id, int recv, int arg, int mblk, Buf *b) {
  const NodeTable *nt = c->nt;
  int mbody = nt_ref(nt, mblk, "body");
  int mbn = 0; const int *mbb = mbody >= 0 ? nt_arr(nt, mbody, "body", &mbn) : NULL;
  if (mbn > 0) {
    const char *mp[3];
    for (int i = 0; i < 3; i++) {
      const char *pn = block_param_name(c, mblk, i);
      mp[i] = pn ? rename_local(pn) : NULL;
    }
    /* the block sees three boxed values */
    Scope *ms = comp_scope_of(c, mblk);
    LocalVar *mlv[3]; TyKind msave[3];
    for (int i = 0; i < 3; i++) {
      const char *pn = block_param_name(c, mblk, i);
      mlv[i] = (ms && pn) ? scope_local(ms, pn) : NULL;
      msave[i] = mlv[i] ? mlv[i]->type : TY_UNKNOWN;
      if (mlv[i]) mlv[i]->type = TY_POLY;
    }
    for (int j = 0; j < mbn; j++) infer_subtree(c, mbb[j]);
    int ta = ++g_tmp, tb = ++g_tmp, tr = ++g_tmp, tp = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp;
    buf_puts(b, "({ "); ta = hold_operand(c, recv, TY_POLY, 1, ta, 1, " ", b);
    buf_printf(b, "sp_RbVal _t%d = ", tb); emit_boxed(c, arg, b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", tb);
    buf_printf(b, " sp_PolyPolyHash *_t%d = sp_poly_hash_merge(_t%d, sp_box_nil()); SP_GC_ROOT(_t%d);",
               tr, ta, tr);
    buf_printf(b, " sp_PolyArray *_t%d = sp_poly_to_a_arr(_t%d); SP_GC_ROOT(_t%d);", tp, tb, tp);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d && _t%d < sp_PolyArray_length(_t%d); _t%d++) {",
               ti, tp, ti, tp, ti);
    buf_printf(b, " sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);", tk, tp, ti);
    buf_printf(b, " sp_RbVal _tk%d = sp_poly_arr_get(_t%d, 0), _tv%d = sp_poly_arr_get(_t%d, 1);",
               tk, tk, tk, tk);
    buf_printf(b, " if (sp_PolyPolyHash_has_key(_t%d, _tk%d)) {", tr, tk);
    if (mp[0]) buf_printf(b, " sp_RbVal lv_%s = _tk%d;", mp[0], tk);
    if (mp[1]) buf_printf(b, " sp_RbVal lv_%s = sp_PolyPolyHash_get(_t%d, _tk%d);", mp[1], tr, tk);
    if (mp[2]) buf_printf(b, " sp_RbVal lv_%s = _tv%d;", mp[2], tk);
    Buf nv; memset(&nv, 0, sizeof nv);
    if (emit_blk_value_via_next(c, mblk, TY_POLY, &nv)) {
      buf_printf(b, " sp_PolyPolyHash_set(_t%d, _tk%d, %s); }", tr, tk, nv.p);
      free(nv.p);
    }
    else { Buf *saved_pre = g_pre; g_pre = b;
      for (int j = 0; j < mbn - 1; j++) { emit_stmt(c, mbb[j], b, 0); buf_puts(b, " "); }
      /* the tail's own prelude (an interpolation temp) has to land BEFORE
         the set call, so render the value into its own buffer while g_pre
         still points at the statement stream */
      Buf tailv; memset(&tailv, 0, sizeof tailv);
      emit_boxed(c, mbb[mbn - 1], &tailv);
      g_pre = saved_pre;
      buf_printf(b, " sp_PolyPolyHash_set(_t%d, _tk%d, %s); }",
                 tr, tk, tailv.p ? tailv.p : "sp_box_nil()");
      free(tailv.p); }
    /* newline before `else`: the arm above ends with `}`, and the two would
       otherwise concatenate into the `} else` form the C style forbids */
    buf_printf(b, "\nelse sp_PolyPolyHash_set(_t%d, _tk%d, _tv%d); }", tr, tk, tk);
    buf_printf(b, " _t%d; })", tr);
    for (int i = 0; i < 3; i++) if (mlv[i]) mlv[i]->type = msave[i];
    return 1;
  }
  return 0;
}

/* Hash#merge(other) with any block on a boxed or cross-layout receiver: a
   literal block, a method's own block passed on (`&block`, the caller's
   block where the method is spliced in), a proc at run time (a caller's
   `&pr`, a `&pr` at the call), or none, which merges plainly. Answers 0 when
   it emits nothing. */
static int emit_merge_any_block_boxed(Compiler *c, int id, int recv, int arg, Buf *b) {
  const NodeTable *nt = c->nt;
  int mblk = nt_ref(nt, id, "block");
  if (nt_kind(nt, mblk) == NK_BlockArgumentNode) {
    int rb = resolve_forwarded_block(c, mblk);
    if (rb >= 0 && rb != mblk && nt_kind(nt, rb) == NK_BlockNode) mblk = rb;
    else if (rb < 0 || rb == mblk) {
      /* no literal block to splice: rb < 0 is the method's own block
         forwarded in a splice, which runs under the caller's `&pr` when it
         has one; rb == mblk is a `&x` value at the call */
      Buf pb; memset(&pb, 0, sizeof pb);
      if (rb >= 0) emit_forwarded_proc_arg(c, mblk, &pb);
      else if (g_yield_proc_ref) buf_puts(&pb, g_yield_proc_ref);
      if (!pb.p || sp_streq(pb.p, "NULL")) {
        buf_puts(b, "sp_poly_hash_merge("); emit_boxed(c, recv, b);
        buf_puts(b, ", "); emit_boxed(c, arg, b); buf_puts(b, ")");
        free(pb.p);
        return 1;
      }
      /* a copy of the receiver, merged through the proc */
      int th = ++g_tmp, to = ++g_tmp, tp = ++g_tmp;
      buf_printf(b, "({ sp_PolyPolyHash *_t%d = sp_poly_hash_merge(", th); emit_boxed(c, recv, b);
      buf_printf(b, ", sp_box_nil()); SP_GC_ROOT(_t%d); sp_RbVal _t%d = ", th, to); emit_boxed(c, arg, b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_Proc *_t%d = %s; SP_GC_ROOT(_t%d); ", to, tp, pb.p, tp);
      buf_printf(b, "sp_poly_hash_merge_blk(sp_box_nullable_obj((void *)_t%d, SP_BUILTIN_POLY_POLY_HASH), _t%d, _t%d, \"merge\"); _t%d; })",
                 th, to, tp, th);
      free(pb.p);
      return 1;
    }
  }
  return nt_kind(nt, mblk) == NK_BlockNode && emit_merge_block_boxed(c, id, recv, arg, mblk, b);
}

/* merge!/update on a typed Hash given an argument typed as no Hash, or
   one known only at run time (boxed): the receiver and every argument run,
   in order, then the nil and frozen checks; then each argument in turn
   merges in, as CRuby's does, until the first that converts to no Hash
   raises its TypeError -- a boxed one at run time, the typed misfit there.
   The arms that merge took no such argument, and the call fell to
   NoMethodError. A block form keeps those arms when a Hash comes first:
   the block resolves its conflicts (a boxed argument with a block is left
   to them too). Each argument is a statement of its own after its own
   prelude, so one built in place does not run ahead of the ones before
   it. */
static int merge_arg_dyn_to_hash(Compiler *c, TyKind at) {
  int mi = ty_is_object(at) ? comp_method_in_chain(c, ty_object_class(at), "to_hash", NULL) : -1;
  return mi >= 0 && c->scopes[mi].nrequired == 0 && c->scopes[mi].rest_idx < 0 && !ty_is_hash(c->scopes[mi].ret);
}

static int emit_hash_merge_misfit(Compiler *c, int id, TyKind rt, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  if (!is_hash_merge_bang(name) || argc < 1 || !nt_call_args_plain(nt, id)) return 0;
  /* the receiver is a builtin Hash: only a reopening of Hash owns the name
     over it, not a program class of its own with a merge! */
  int hc_ci = comp_class_index(c, "Hash");
  if (hc_ci >= 0 && comp_method_in_chain(c, hc_ci, name, NULL) >= 0) return 0;
  int bad = -1, boxed = 0;
  for (int i = 0; i < argc && bad < 0; i++) {
    if (nt_kind(nt, argv[i]) == NK_HashNode) continue;
    TyKind at = comp_ntype(c, argv[i]);
    int def = -1;
    TyKind mk = ty_is_object(at) ? obj_container_conv(c, at, "to_hash", &def) : TY_UNKNOWN;
    int fits = mk != TY_UNKNOWN && (ty_hash_key(rt) == TY_POLY || ty_hash_key(rt) == ty_hash_key(mk)) &&
               (ty_hash_val(rt) == TY_POLY || ty_hash_val(rt) == ty_hash_val(mk));
    if (mk != TY_UNKNOWN && !fits) { unsupported_feature(c, id, "Hash#merge! with a #to_hash of another layout than the receiver"); return 0; }
    if (fits || merge_arg_dyn_to_hash(c, at)) boxed = 1;
    else if (face_arg_misfit(c, PF_HASH, argv[i]) || ty_is_object(at)) bad = i;
    else if (at == TY_POLY || at == TY_UNKNOWN) boxed = 1;
  }
  if ((bad < 0 && !boxed) || (bad != 0 && nt_ref(nt, id, "block") >= 0)) return 0;
  int last = bad >= 0 ? bad : argc - 1;
  int tr = ++g_tmp, t0 = g_tmp + 1;
  g_tmp += argc;
  Buf *sv_pre = g_pre;
  buf_puts(b, "({ ");
  for (int i = -1; i < argc; i++) {
    Buf ap = {0, 0, 0}, av = {0, 0, 0};
    g_pre = &ap;
    if (i < 0) emit_expr(c, nt_ref(nt, id, "receiver"), &av);
    else emit_boxed(c, argv[i], &av);
    g_pre = sv_pre;
    if (ap.p) buf_puts(b, ap.p);
    if (i < 0) buf_printf(b, "%s _t%d = %s; SP_GC_ROOT(_t%d); ", c_type_name(rt), tr, av.p ? av.p : "NULL", tr);
    else buf_printf(b, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d); ", t0 + i, av.p ? av.p : "sp_box_nil()", t0 + i);
    free(ap.p); free(av.p);
  }
  buf_printf(b, "if (!_t%d) sp_nil_recv(\"%s\"); ", tr, name);
  buf_printf(b, "if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s); ", tr, tr, hash_box_cls(rt));
  char rtxt[32];
  snprintf(rtxt, sizeof rtxt, "_t%d", tr);
  for (int i = 0; i <= last; i++) {
    TyKind at = comp_ntype(c, argv[i]);
    int def = -1;
    int hmi = ty_is_object(at) ? comp_method_in_chain(c, ty_object_class(at), "to_hash", &def) : -1;
    if (i != bad && ty_is_object(at) && hmi >= 0 && c->scopes[hmi].nparams && conv_takes_no_args(c, hmi)) {
      int byv = comp_ty_value_obj(c, at);
      char rv[96];
      if (byv) snprintf(rv, sizeof rv, "*(sp_%s *)_t%d.v.p", c->classes[def].c_name, t0 + i);
      else snprintf(rv, sizeof rv, "_t%d.v.p", t0 + i);
      Buf cb = {0, 0, 0};
      emit_obj_conv_call(c, def, "to_hash", byv, rv, &cb);
      buf_printf(b, "_t%d = ", t0 + i);
      emit_boxed_text(c, c->scopes[hmi].ret, cb.p ? cb.p : "NULL", b);
      buf_puts(b, "; ");
      free(cb.p);
    }
    else if (i != bad && ty_is_object(at) && (obj_container_conv(c, at, "to_hash", &def) != TY_UNKNOWN || merge_arg_dyn_to_hash(c, at)))
      buf_printf(b, "_t%d = sp_kw_splat_conv(_t%d, 0); ", t0 + i, t0 + i);
    buf_printf(b, "if (_t%d.tag != SP_TAG_OBJ || !sp_poly_is_hash_kind(_t%d.cls_id))"
                  " sp_raise_cls(\"TypeError\", sp_sprintf(\"no implicit conversion of %%s into Hash\", sp_convert_src_name(_t%d))); ",
               t0 + i, t0 + i, t0 + i);
    if (i != bad) { buf_puts(b, "sp_poly_hash_merge_into("); emit_boxed_text(c, rt, rtxt, b); buf_printf(b, ", _t%d); ", t0 + i); }
  }
  buf_printf(b, "_t%d; })", tr);
  return 1;
}

/* A read of an Integer-keyed typed hash whose key can be nil (a nil
   literal, a nullable Integer) or of another class (a boxed key that is not
   an Integer): no key equals it, so the read misses --
   `[]` answers the default, key? false, fetch(k, d) its default, values_at
   a nil element -- where the plain key spelling unwrapped it and raised.
   The key is bound once; the ordinary emission of the call reads its value
   through a view binding. Answers 1 when it emitted. */
static int g_nilkey_busy = -1;
static int emit_hash_nilkey_read(Compiler *c, int id, const char *name, int recv, TyKind rt,
                                 int argc, const int *argv, Buf *b) {
  const NodeTable *nt = c->nt;
  if (g_nilkey_busy == id || argc < 1 || ty_hash_key(rt) != TY_INT) return 0;
  if (nt_ref(nt, id, "block") >= 0) return 0;
  const char *hn = ty_hash_cname(rt);
  if (!hn || !(sp_streq(hn, "IntInt") || sp_streq(hn, "IntStr"))) return 0;
  int key = argv[0];
  TyKind kt = comp_ntype(c, key);
  int kbox = repr_of(c, key).kind == RK_BOXED || kt == TY_POLY;   /* a boxed key: its nil tag */
  if (!(kt == TY_NIL || kbox || (kt == TY_INT && node_has_oint_form(c, key)))) return 0;
  int is_aref = sp_streq(name, "[]") && argc == 1;
  int is_q = is_key_query(name) && argc == 1;
  int is_fetch = sp_streq(name, "fetch") && argc == 2;
  int is_va = sp_streq(name, "values_at") && argc == 1;
  if (!(is_aref || is_q || is_fetch || is_va)) return 0;
  Repr cr = repr_of(c, id);
  TyKind ct = cr.as_ty;
  if (is_fetch && !(cr.kind == RK_BOXED || comp_ntype(c, argv[1]) == ct)) return 0;
  if (is_va && !(ct == TY_POLY_ARRAY || ct == TY_INT_ARRAY || ct == TY_STR_ARRAY)) return 0;
  if (g_n_argov + 2 > MAX_ARG_OVERRIDE) return 0;
  int tk = ++g_tmp, tr = ++g_tmp;
  if (kbox && kt != TY_NIL) {
    buf_printf(b, "({ sp_RbVal _t%d = ", tk); emit_boxed(c, key, b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_%sHash *_t%d = ", tk, hn, tr); emit_expr(c, recv, b);
    /* a boxed key of any class but Integer (nil, a Float, a String) is not
       in an Integer-keyed table: the miss; a store keeps its A5 refusal */
    buf_printf(b, "; SP_GC_ROOT(_t%d); _t%d.tag != SP_TAG_INT ? ", tr, tk);
  }
  else {
    buf_printf(b, "({ sp_oint _t%d = ", tk);
    if (kt == TY_NIL) { buf_puts(b, "((void)("); emit_expr(c, key, b); buf_puts(b, "), sp_oint_nil())"); }
    else emit_oint_expr(c, key, TY_INT, b);
    buf_printf(b, "; sp_%sHash *_t%d = ", hn, tr); emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); _t%d.nil ? ", tr, tk);
  }
  /* the miss, in the form the call's consumer takes */
  if (is_aref && sp_streq(hn, "IntInt")) {
    char d[160]; snprintf(d, sizeof d, "((_t%d && !_t%d->default_nil) ? sp_oint_of(_t%d->default_v) : sp_oint_nil())", tr, tr, tr);
    if (cr.kind == RK_BOXED) buf_printf(b, "sp_box_oint(%s)", d);
    else if (node_is_oint(c, id)) buf_puts(b, d);
    else buf_printf(b, "sp_oint_arg(%s)", d);
  }
  else if (is_aref) {
    if (cr.kind == RK_BOXED) buf_printf(b, "sp_box_str(_t%d ? _t%d->default_v : NULL)", tr, tr);
    else buf_printf(b, "(_t%d ? _t%d->default_v : (const char *)NULL)", tr, tr);
  }
  else if (is_q) buf_puts(b, cr.kind == RK_BOXED ? "sp_box_bool(0)" : "(sp_bool)0");
  else if (is_fetch) {
    /* a fetch whose hit answers an oint (a hash whose values may be nil,
       DESIGN.md D3b-ii) takes its default in that form too */
    if (cr.kind == RK_BOXED) emit_boxed(c, argv[1], b);
    else if (oint_kind(ct) && node_is_oint(c, id)) emit_oint_expr(c, argv[1], ct, b);
    else emit_expr(c, argv[1], b);
  }
  else if (ct == TY_POLY_ARRAY) buf_puts(b, "({ sp_PolyArray *_va = sp_PolyArray_new(); sp_PolyArray_push(_va, sp_box_nil()); _va; })");
  else if (ct == TY_INT_ARRAY) buf_puts(b, "({ sp_IntArray *_va = sp_IntArray_new(); sp_IntArray_push_nilable(_va, sp_oint_nil()); _va; })");
  else buf_puts(b, "({ sp_StrArray *_va = sp_StrArray_new(); sp_StrArray_push(_va, NULL); _va; })");
  buf_puts(b, " : ");
  /* the hit: the ordinary emission, the key and receiver read through their binds */
  int kb = (kbox && kt != TY_NIL) ? view_bind(key, "_t%d", tk) : view_bind(key, "_t%d.v", tk);
  int rb2 = view_bind(recv, "_t%d", tr);
  int sv = g_nilkey_busy; g_nilkey_busy = id;
  Buf hb; memset(&hb, 0, sizeof hb);
  emit_call(c, id, &hb);
  g_nilkey_busy = sv;
  view_unbind(rb2); view_unbind(kb);
  buf_puts(b, hb.p ? hb.p : "0"); free(hb.p);
  buf_puts(b, "; })");
  return 1;
}

/* merge(other) { |k, v1, v2| } typed as the receiver's own variant (an
   emit_hash_call arm, kept out of that function's length) */
static int emit_merge_block_same_variant(Compiler *c, int id, int recv, int arg, TyKind rt, const char *hn, Buf *b) {
  const NodeTable *nt = c->nt;
  /* merge(other) { |k, v1, v2| } -- conflict-resolution block. The
     result starts as a copy of the receiver, then each key of `other`
     is inserted; on a collision the block picks the value. */
  int blk = nt_ref(nt, id, "block");
  const char *bp0 = block_param_name(c, blk, 0);
  const char *bp1 = block_param_name(c, blk, 1);
  const char *bp2 = block_param_name(c, blk, 2);
  TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
  /* values that may be nil are read and stored with their nil (D3b-ii) */
  int mvn = oint_kind(vt) && hash_vals_nullable(c, recv);
  const char *mset = mvn ? "oset" : "set", *mget = mvn ? "vget" : "get";
  int tr = ++g_tmp, to = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp, tc = ++g_tmp, tj = ++g_tmp;
  buf_printf(b, "({ %s _t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);", c_type_name(rt), tr, hn, tr);
  /* copy the receiver into the fresh result */
  buf_printf(b, " %s _t%d = ", c_type_name(rt), tc); emit_expr(c, recv, b); buf_puts(b, ";");
  buf_printf(b, " _t%d->default_v = _t%d->default_v;", tr, tc);
  if (rt == TY_STR_INT_HASH || rt == TY_INT_INT_HASH)
    buf_printf(b, " _t%d->default_nil = _t%d->default_nil;", tr, tc);
  if (vt == TY_POLY)
    buf_printf(b, " _t%d->dproc = _t%d->dproc; _t%d->dproc_self = _t%d->dproc_self;", tr, tc, tr, tc);
  buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                " sp_%sHash_%s(_t%d, _t%d->order[_t%d], sp_%sHash_%s(_t%d, _t%d->order[_t%d]));",
             tj, tj, tc, tj, hn, mset, tr, tc, tj, hn, mget, tc, tc, tj);
  buf_printf(b, " %s _t%d = ", c_type_name(rt), to); emit_expr(c, arg, b); buf_puts(b, ";");
  buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, to, ti);
  buf_printf(b, " %s _t%d = _t%d->order[_t%d];", c_type_name(kt), tk, to, ti);
  buf_printf(b, " if (sp_%sHash_has_key(_t%d, _t%d)) {", hn, tr, tk);
  if (bp0) buf_printf(b, " lv_%s = _t%d;", rename_local(bp0), tk);
  if (mvn) {
    char v1[96], v2[96];
    snprintf(v1, sizeof v1, "sp_%sHash_vget(_t%d, _t%d)", hn, tr, tk);
    snprintf(v2, sizeof v2, "sp_%sHash_vget(_t%d, _t%d)", hn, to, tk);
    emit_merge_blk_param(c, blk, bp1, vt, v1, v1, b);
    emit_merge_blk_param(c, blk, bp2, vt, v2, v2, b);
  }
  else {
    if (bp1) buf_printf(b, " lv_%s = sp_%sHash_get(_t%d, _t%d);", rename_local(bp1), hn, tr, tk);
    if (bp2) buf_printf(b, " lv_%s = sp_%sHash_get(_t%d, _t%d);", rename_local(bp2), hn, to, tk);
  }
  buf_printf(b, " sp_%sHash_%s(_t%d, _t%d, ", hn, mset, tr, tk);
  if (mvn) emit_blk_value_as_o(c, blk, vt, b); else emit_blk_value_as(c, blk, vt, b);
  buf_printf(b, "); }\nelse { sp_%sHash_%s(_t%d, _t%d, sp_%sHash_%s(_t%d, _t%d)); } }", hn, mset, tr, tk, hn, mget, to, tk);
  buf_printf(b, " _t%d; })", tr);
  return 1;
}

/* `h.fetch(k, default)` past the key: the hit or the default, as the value
   kind wants it -- boxed for a poly slot, through one fetch_or probe for an
   inert Integer default, with its nil where the hash's values may be nil
   (DESIGN.md D3b-ii), else plain. `ks` is the key's _okey suffix. */
static void emit_hash_fetch_default_tail(Compiler *c, int id, int dflt, const char *hn, const char *ks,
                                         TyKind vt, TyKind dt, int needs_box, int th, int tk, Buf *b) {
  if (needs_box) {
    buf_printf(b, "; sp_%sHash_has_key%s(_t%d, _t%d) ? ", hn, ks, th, tk);
    Buf _bx; memset(&_bx, 0, sizeof _bx);
    buf_printf(&_bx, "sp_%sHash_get%s(_t%d, _t%d)", hn, ks, th, tk);
    emit_boxed_text(c, vt, _bx.p, b);
    free(_bx.p);
    buf_puts(b, " : "); emit_boxed(c, dflt, b);
  }
  else if (vt == TY_INT && (dt == TY_INT || dt == TY_NIL) && fetch_operand_is_inert(c, dflt) &&
           (sp_streq(hn, "IntInt") || sp_streq(hn, "StrInt"))) {
    /* an inert default costs nothing to evaluate up front: one probe;
       the default travels with its nil (fetch_or takes an sp_oint)
       and the answer is unwrapped where the consumer wants it plain */
    buf_puts(b, "; "); oint_open(c, id, TY_INT, b);
    buf_printf(b, "sp_%sHash_fetch_or%s(_t%d, _t%d, ", hn, ks, th, tk);
    emit_oint_expr(c, dflt, TY_INT, b);
    buf_puts(b, ")"); oint_close(c, id, b);
  }
  else if (oint_kind(vt) && node_is_oint(c, id)) {
    /* a hash whose values may be nil: the hit with its nil, the default
       in its oint form (DESIGN.md D3b-ii) */
    if (ks[0]) buf_printf(b, "; sp_%sHash_has_key%s(_t%d, _t%d) ? sp_oint_of(sp_%sHash_get%s(_t%d, _t%d)) : ", hn, ks, th, tk, hn, ks, th, tk);
    else buf_printf(b, "; sp_%sHash_has_key(_t%d, _t%d) ? sp_%sHash_vget(_t%d, _t%d) : ", hn, th, tk, hn, th, tk);
    emit_oint_expr(c, dflt, vt, b);
  }
  else {
    buf_printf(b, "; sp_%sHash_has_key%s(_t%d, _t%d) ? sp_%sHash_get%s(_t%d, _t%d) : ", hn, ks, th, tk, hn, ks, th, tk);
    if (vt == TY_POLY && dt != TY_POLY) emit_boxed(c, dflt, b);
    else emit_expr(c, dflt, b);
  }
}

int emit_hash_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = comp_recv_type(c, recv);
  if (recv >= 0 && ty_is_hash(rt)) {
    if (emit_hash_nilkey_read(c, id, name, recv, rt, argc, argv, b)) return 1;
    /* the arms that read only the receiver's variant, the receiver and the
       arguments: builtin-op rows (builtin_ops.c, codegen_call_hash.c). The
       arms below that stay read the argument nodes, the block or the
       program's own methods, and none of them can take a call a row takes. */
    if (emit_hash_merge_misfit(c, id, rt, b) || emit_builtin_op(c, id, recv, rt, name, b)) return 1;
    /* merge(other, &pr): a block that is not a literal one -- a proc at run
       time, or a method's own block passed on -- has no body for the arms
       below to read, which stored nil for every conflict. The any-block
       emitter calls it (its result is the general boxed hash, as the call
       is typed). */
    if (sp_streq(name, "merge") && argc == 1 && nt_kind(nt, nt_ref(nt, id, "block")) == NK_BlockArgumentNode &&
        repr_hash_is(repr_of(c, id), TY_POLY, TY_POLY) && emit_merge_any_block_boxed(c, id, recv, argv[0], b))
      return 1;
    if (sp_streq(name, "compare_by_identity"))  /* any arity: identity hashing is unsupported */
      unsupported(c, id, "Hash#compare_by_identity (identity-keyed hashing)");
    const char *hn = ty_hash_cname(rt);
    if (hn) {
      /* select! / filter! / reject! / keep_if / delete_if { |k, v| cond } in
         expression position (the statement form lives in emit_iteration_stmt;
         the loop is emit_hash_filter_loop's). Mutates in place; `!` forms
         yield nil when nothing was removed else self, keep_if/delete_if
         always yield self. */
      if (is_select_bang(name) &&
          nt_ref(nt, id, "block") >= 0) {
        int block = nt_ref(nt, id, "block");
        Buf rb = expr_buf(c, recv);
        int tr, torig, twp;
        int ok = emit_hash_filter_loop(c, recv, block, rt, name, rb.p ? rb.p : "NULL", g_pre, g_indent, &tr, &torig, &twp);
        free(rb.p);
        if (ok) {
          char box[96]; snprintf(box, sizeof box, "sp_box_obj(_t%d, %s)", tr, hash_box_cls(rt));
          emit_filter_bang_result(name, tr, torig, twp, box, b);
          return 1;
        }
      }
      if (sp_streq(name, "dig") && argc >= 1) {
        /* dig(*keys): the key list only exists at run time, so walk it there.
           Emitting the splat as a single key read the key array through the
           key's own type and the C did not compile. */
        /* the receiver is held across the keys, which may allocate */
        if (nt_kind(nt, argv[0]) == NK_SplatNode)
          return emit_dig_splat(c, recv, argv[0], b);
        TyKind vt = ty_hash_val(rt);
        TyKind kt = ty_hash_key(rt);
        /* Static key-type mismatch (string key on sym hash, a String on an
           Integer-keyed one, etc.) -> nil: a key of another class is never
           eql? to any key the hash holds (1.0 is not 1 either) */
        TyKind arg0t = comp_ntype(c, argv[0]);
        int ksc = kt == TY_INT || kt == TY_FLOAT || kt == TY_STRING || kt == TY_SYMBOL;
        int asc = arg0t == TY_INT || arg0t == TY_FLOAT || arg0t == TY_STRING || arg0t == TY_SYMBOL;
        if (ksc && asc && kt != arg0t) {
          buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)("); emit_expr(c, argv[0], b);
          if (oint_kind(vt)) { buf_puts(b, "); "); oint_open(c, id, vt, b); buf_puts(b, oint_nil(vt)); oint_close(c, id, b); buf_puts(b, "; })"); }
          else if (vt == TY_STRING) buf_puts(b, "); NULL; })");
          else buf_puts(b, "); sp_box_nil(); })");
          return 1;
        }
        const char *getter = vt == TY_INT ? "oget" : "get";
        if (argc == 1) {
          Buf rb; int ch = hold_recv_open(c, recv, 0, c_type_name(rt), "SP_GC_ROOT", b, &rb);
          if (vt == TY_INT) oint_open(c, id, TY_INT, b);
          buf_printf(b, "sp_%sHash_%s(%s, ", hn, getter, rb.p); free(rb.p);
          emit_hash_key(c, argv[0], kt, b); buf_puts(b, ")");
          if (vt == TY_INT) oint_close(c, id, b);
          if (ch) buf_puts(b, "; })");
        }
        else {
          /* multi-step dig: use a compound statement to guarantee
             left-to-right key-expression evaluation order. */
          int tr = ++g_tmp, th = ++g_tmp;
          buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th);
          emit_recv_rooted(c, recv, th, "SP_GC_ROOT", b);
          /* first key -> box to sp_RbVal so remaining steps are uniform */
          buf_printf(b, "sp_RbVal _t%d = ", tr);
          if (vt == TY_INT) {
            int tk0 = ++g_tmp;
            buf_printf(b, "sp_box_oint(sp_%sHash_%s(_t%d, ", hn, getter, th);
            emit_hash_key(c, argv[0], kt, b);
            buf_puts(b, "));"); (void)tk0;
          }
          else if (vt == TY_STRING) {
            int tk0 = ++g_tmp;
            buf_printf(b, "({ const char *_t%d = sp_%sHash_%s(_t%d, ", tk0, hn, getter, th);
            emit_hash_key(c, argv[0], kt, b);
            buf_printf(b, "); _t%d ? sp_box_str(_t%d) : sp_box_nil(); });", tk0, tk0);
          }
          else {
            /* TY_POLY: getter already returns sp_RbVal */
            buf_printf(b, "sp_%sHash_%s(_t%d, ", hn, getter, th);
            emit_hash_key(c, argv[0], kt, b);
            buf_puts(b, ");");
          }
          /* remaining keys via sp_poly_get_sym / sp_poly_get_str / sp_poly_arr_get */
          for (int di = 1; di < argc; di++) {
            int tk = ++g_tmp;
            /* Ruby's dig stops at nil and raises on anything else that has no
               #dig; only the first receiver is known to be a container, so
               every later step has to check what it landed on (#3567). */
            buf_printf(b, " sp_poly_dig_check(_t%d);", tr);
            /* the reads below raise NoMethodError for a nil receiver now
               (#4485); a nil met part way through a dig is the walk's end,
               so the key is still evaluated (CRuby takes every argument
               first) and the read is skipped */

            /* Past the first key the receiver `_tr` is whatever the previous
               step returned (a nested hash, an Array element, ...), whose key
               type is not the top hash's key type. So `{a:[10,20]}.dig(:a,1)`
               must index the Array with `1`, not look up symbol `1`. Infer the
               sub-key type from the argument node itself; sp_poly_arr_get_hash
               then dispatches on the runtime receiver (array/hash/etc.). */
            TyKind dkt = comp_ntype(c, argv[di]);
            if (dkt == TY_SYMBOL) {
              buf_printf(b, " sp_sym _t%d = ", tk);
              emit_expr(c, argv[di], b);
              buf_printf(b, "; if (_t%d.tag != SP_TAG_NIL) _t%d = sp_poly_get_sym(_t%d, _t%d);", tr, tr, tr, tk);
            }
            else if (dkt == TY_STRING) {
              buf_printf(b, " const char *_t%d = ", tk);
              emit_expr(c, argv[di], b);
              buf_printf(b, "; if (_t%d.tag != SP_TAG_NIL) _t%d = sp_poly_get_str(_t%d, _t%d);", tr, tr, tr, tk);
            }
            else if (dkt == TY_POLY) {
              /* A poly sub-key is stored as sp_RbVal, not sp_int; dispatch on
                 both the runtime receiver and key kind. */
              buf_printf(b, " sp_RbVal _t%d = ", tk);
              emit_expr(c, argv[di], b);
              buf_printf(b, "; if (_t%d.tag != SP_TAG_NIL) _t%d = sp_poly_index_poly(_t%d, _t%d);", tr, tr, tr, tk);
            }
            else {
              buf_printf(b, " sp_int _t%d = ", tk);
              emit_int_expr(c, argv[di], b);
              buf_printf(b, "; if (_t%d.tag != SP_TAG_NIL) _t%d = sp_poly_arr_get_hash(_t%d, _t%d);", tr, tr, tr, tk);
            }
          }
          buf_printf(b, " _t%d; })", tr);
        }
        return 1;
      }
      if (sp_streq(name, "fetch_values") && argc == 0 && !an_zero_arg_builtin_shadowed(c, name, argc)) {
        /* no key: an empty array, from a Hash that is not nil */
        int tr = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), tr); emit_expr(c, recv, b);
        buf_printf(b, "; if (!_t%d) sp_nil_recv(\"fetch_values\"); sp_PolyArray_new(); })", tr);
        return 1;
      }
      if ((sp_streq(name, "values_at") || sp_streq(name, "fetch_values")) && argc >= 1) {
        /* collect looked-up values into a poly array; values_at yields nil for
           a missing key, fetch_values raises KeyError */
        int is_fetch = sp_streq(name, "fetch_values");
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        int th = ++g_tmp, tr = ++g_tmp;
        /* the table is rooted across the keys, as the result already is */
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_recv_rooted(c, recv, th, "SP_GC_ROOT", b);
        buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        for (int a = 0; a < argc; a++) {
          int tk = ++g_tmp;
          int is_splat = nt_type(nt, argv[a]) && sp_streq(nt_type(nt, argv[a]), "SplatNode");
          int ts = 0, ti = 0;
          if (is_splat) {
            /* values_at(*keys): each element of the splatted array is a
               separate key (#3277). */
            ts = ++g_tmp; ti = ++g_tmp;
            buf_printf(b, " { sp_PolyArray *_t%d = ", ts); emit_expr(c, argv[a], b);
            buf_printf(b, "; for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, ts, ti);
            buf_printf(b, " %s _t%d = ", c_type_name(kt), tk);
            if (kt == TY_STRING) buf_printf(b, "%ssp_poly_to_s(sp_PolyArray_get(_t%d, _t%d))%s;", env_key_open(nt, argv[a]), ts, ti, env_key_close(nt, argv[a]));
            /* a poly-keyed table takes the element boxed, as it is (#7051) */
            else if (kt == TY_POLY) buf_printf(b, "sp_PolyArray_get(_t%d, _t%d);", ts, ti);
            else buf_printf(b, "(%s)sp_poly_to_i(sp_PolyArray_get(_t%d, _t%d));", c_type_name(kt), ts, ti);
          }
          else if (hash_key_misses(c, argv[a], kt) &&
                   !(hash_nil_key_stored(c, argv[a], kt) && !(is_fetch && nt_ref(nt, id, "block") >= 0))) {
            /* a key of a kind the table cannot hold: values_at answers nil,
               fetch_values raises naming the key, boxed once here */
            int fv_blk = nt_ref(nt, id, "block");
            if (is_fetch && fv_blk >= 0) {
              unsupported_feature(c, id, "Hash#fetch_values with a block and a key of another class than the hash's keys");
              return 0;
            }
            buf_printf(b, " sp_RbVal _t%d = ", tk); emit_boxed(c, argv[a], b); buf_puts(b, ";");
            if (is_fetch) {
              char htmp[32]; snprintf(htmp, sizeof htmp, "_t%d", th);
              buf_puts(b, " sp_exc_stage_recv(");
              emit_boxed_text(c, rt, htmp, b);
              buf_printf(b, "); sp_raise_key_not_found(_t%d);", tk);
            }
            else buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_nil());", tr);
            continue;
          }
          else {
            buf_printf(b, " %s _t%d = ", c_type_name(kt), tk); emit_hash_key(c, argv[a], kt, b); buf_puts(b, ";");
          }
          /* A boxed-value hash answers its default on a miss, and values_at
             wants that default; only a typed-value hash needs the has_key
             guard, whose zero would otherwise read as a real value. */
          int use_default = !is_fetch && vt == TY_POLY;
          if (!use_default) buf_printf(b, " if (sp_%sHash_has_key(_t%d, _t%d))", hn, th, tk);
          buf_printf(b, " sp_PolyArray_push(_t%d, ", tr);
          char getexpr[128]; snprintf(getexpr, sizeof getexpr, "sp_%sHash_get(_t%d, _t%d)", hn, th, tk);
          if (vt == TY_POLY) buf_puts(b, getexpr);
          else { char hs[24], ks[24]; snprintf(hs, sizeof hs, "_t%d", th); snprintf(ks, sizeof ks, "_t%d", tk); emit_hash_val_boxed(c, recv, vt, hn, hs, ks, b); }
          buf_puts(b, ");");
          int fv_blk = nt_ref(nt, id, "block");
          if (is_fetch && fv_blk >= 0 && nt_type(nt, fv_blk) &&
              sp_streq(nt_type(nt, fv_blk), "BlockNode")) {
            /* fetch_values(...) { |k| fallback }: the block supplies the
               value for each MISSING key instead of raising */
            const char *fp0 = block_param_name(c, fv_blk, 0);
            int fvb = nt_ref(nt, fv_blk, "body");
            int fvn = 0; const int *fvv = fvb >= 0 ? nt_arr(nt, fvb, "body", &fvn) : NULL;
            buf_puts(b, " else {");
            if (fp0) {
              char keytmp[32]; snprintf(keytmp, sizeof keytmp, "_t%d", tk);
              buf_printf(b, " lv_%s = ", rename_local(fp0));
              if (kt == TY_POLY) buf_puts(b, keytmp);
              else emit_boxed_text(c, kt, keytmp, b);
              buf_puts(b, ";");
            }
            if (fvn > 0) {
              /* the whole body, its leading statements too, inside this
                 key's arm (they ran nowhere) */
              Buf inner; memset(&inner, 0, sizeof inner);
              Buf vb; memset(&vb, 0, sizeof vb);
              Buf *sv_pre = g_pre; g_pre = &inner;
              int svlm = g_line_map; g_line_map = 0;
              IterStep st; emit_iter_step_open(c, fv_blk, 1, 0, &st);
              emit_iter_step_tail(c, &st, &vb);
              g_line_map = svlm; g_pre = sv_pre;
              if (inner.p) buf_printf(b, " %s", inner.p);
              buf_printf(b, " sp_PolyArray_push(_t%d, %s);", tr, vb.p ? vb.p : "sp_box_nil()");
              free(inner.p); free(vb.p);
            }
            buf_puts(b, " }");
          }
          else if (is_fetch) {
            char keytmp[32], htmp[32];
            snprintf(keytmp, sizeof keytmp, "_t%d", tk);
            snprintf(htmp, sizeof htmp, "_t%d", th);
            buf_puts(b, " else { sp_exc_stage_recv(");
            emit_boxed_text(c, rt, htmp, b);
            buf_puts(b, "); sp_raise_key_not_found(");
            emit_boxed_text(c, kt, keytmp, b);
            buf_puts(b, "); }");
          }
          else if (!use_default) buf_printf(b, " else sp_PolyArray_push(_t%d, sp_box_nil());", tr);
          if (is_splat) buf_puts(b, " } }");
        }
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      /* A block supersedes a positional default: CRuby warns and calls the
           block, where the default was being returned (#3566). Treating the
           two-argument-with-block form as the one-argument-with-block form is
           exactly that rule. fetch(key) without a block is a row. */
      if (sp_streq(name, "fetch") && (argc == 1 || argc == 2) && nt_ref(nt, id, "block") >= 0) {
        int blk = fetch_fallback_block(c, id);
        /* a proc handed in (`&b` holding one, or nil): called with the key
           at run time, after the nil check a missing block raises KeyError
           for; the call is typed boxed (infer: a block argument) */
        if (nt_kind(nt, blk) == NK_BlockArgumentNode) {
          Buf pp; memset(&pp, 0, sizeof pp);
          int pb = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
          const char *fwd = forwarded_real_proc(nt_ref(nt, id, "block"), pb);
          if (pb >= 0) emit_forwarded_proc_arg(c, pb, &pp);
          else buf_puts(&pp, fwd ? fwd : "NULL");
          TyKind vt = ty_hash_val(rt), kt = ty_hash_key(rt);
          int th = ++g_tmp, tk = ++g_tmp, tp = ++g_tmp, tb = ++g_tmp;
          buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
          buf_printf(b, "; SP_GC_ROOT(_t%d); %s _t%d = ", th, c_type_name(kt), tk);
          emit_hash_key(c, argv[0], kt, b);
          buf_printf(b, "; sp_Proc *_t%d = %s; SP_GC_ROOT(_t%d); sp_RbVal _t%d; ", tp, pp.p ? pp.p : "NULL", tp, tb);
          free(pp.p);
          char getexpr[128], keyexpr[32];
          snprintf(getexpr, sizeof getexpr, "sp_%sHash_get(_t%d, _t%d)", hn, th, tk);
          snprintf(keyexpr, sizeof keyexpr, "_t%d", tk);
          buf_printf(b, "if (sp_%sHash_has_key(_t%d, _t%d)) _t%d = ", hn, th, tk, tb);
          emit_boxed_text(c, vt, getexpr, b);
          buf_printf(b, "; else if (_t%d) { _sp_proc_poly_args[0] = ", tp);
          emit_boxed_text(c, kt, keyexpr, b);
          buf_printf(b, "; sp_int _sl[1] = { sp_poly_slot_i(_sp_proc_poly_args[0]) };"
                        " _t%d = sp_poly_callable_call(sp_box_obj(_t%d, SP_BUILTIN_PROC), 1, _sl); }"
                        " else { _t%d = sp_box_nil(); sp_raise_key_not_found(", tb, tp, tb);
          emit_boxed_text(c, kt, keyexpr, b);
          buf_printf(b, "); } _t%d; })", tb);
          return 1;
        }
        if (blk >= 0 && hash_key_misses(c, argv[0], ty_hash_key(rt))) {
          /* the block receives the missing key, and its parameter is typed
             as the table's key kind; a key of another kind has no slot */
          unsupported_feature(c, id, "Hash#fetch with a block and a key of another class than the hash's keys");
          return 0;
        }
        if (blk >= 0) {
          /* fetch(key) { default } -> has_key? ? get : block-default */
          TyKind vt = ty_hash_val(rt);
          int th = ++g_tmp, tk = ++g_tmp;
          buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
          buf_printf(b, "; SP_GC_ROOT(_t%d)", th);   /* rooted across the key, as the array arms are */
          int okf = hash_okey_form(c, argv[0], ty_hash_key(rt));
          const char *ks = okf ? "_okey" : "";
          int tb = emit_fetch_blk_key(c, argv[0], ty_hash_key(rt), tk, b);
          int bbody = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
          int bval = bn > 0 ? bb[bn - 1] : -1;
          TyKind bvt = bval >= 0 ? repr_of(c, bval).as_ty : vt;
          /* When the block's return type differs from the hash value type,
             box both arms so the ternary produces a consistent sp_RbVal --
             and so for the caller's literal an inlined `&blk` stands for,
             which the call's type (boxed) was given without reading */
          int mismatch = vt != TY_POLY && (bvt != vt || repr_of(c, id).as_ty == TY_POLY);
          /* a hash whose values may be nil answers an oint (the node's form) */
          int fvo = !mismatch && oint_kind(vt) && node_is_oint(c, id);
          if (mismatch) {
            buf_printf(b, "; sp_%sHash_has_key%s(_t%d, _t%d) ? ", hn, ks, th, tk);
            char getexpr[128]; snprintf(getexpr, sizeof getexpr, "sp_%sHash_get%s(_t%d, _t%d)", hn, ks, th, tk);
            emit_boxed_text(c, vt, getexpr, b);
            buf_puts(b, " : ");
          }
else if (fvo) {
            /* the hit with its nil (DESIGN.md D3b-ii); the block's value lifted */
            if (okf) buf_printf(b, "; sp_%sHash_has_key%s(_t%d, _t%d) ? sp_oint_of(sp_%sHash_get%s(_t%d, _t%d)) : sp_oint_of(",
                                hn, ks, th, tk, hn, ks, th, tk);
            else buf_printf(b, "; sp_%sHash_has_key%s(_t%d, _t%d) ? sp_%sHash_vget(_t%d, _t%d) : sp_oint_of(",
                            hn, ks, th, tk, hn, th, tk);
          }
else {
            buf_printf(b, "; sp_%sHash_has_key%s(_t%d, _t%d) ? sp_%sHash_get%s(_t%d, _t%d) : ",
                       hn, ks, th, tk, hn, ks, th, tk);
          }
          Buf fbind; memset(&fbind, 0, sizeof fbind);
          if (okf) emit_fetch_blk_okey_param(c, blk, tb, tk, &fbind);
          else emit_fetch_blk_param(c, id, blk, ty_hash_key(rt), tk, &fbind);  /* fetch yields the key */
          {
            /* a valued `next` in the block answers through a destination
               temporary, after the key is bound */
            Buf fv; memset(&fv, 0, sizeof fv);
            if (emit_blk_value_via_next(c, blk, (vt == TY_POLY || mismatch) ? TY_POLY : vt, &fv)) {
              buf_puts(b, "({ ");
              if (fbind.p) buf_puts(b, fbind.p);
              buf_puts(b, fv.p ? fv.p : "");
              buf_puts(b, "; })");
              free(fv.p); free(fbind.p);
              buf_puts(b, fvo ? "); })" : "; })");
              return 1;
            }
            free(fv.p);
          }
          emit_fallback_block_value(c, bb, bn, fbind.p,
                                    (vt == TY_POLY || mismatch) && bvt != TY_POLY,
                                    (vt == TY_POLY || mismatch) ? "sp_box_nil()" : default_value_from_compiler(c, vt), 1, b);
          free(fbind.p);
          buf_puts(b, fvo ? "); })" : "; })");
          return 1;
        }
      }
      if (sp_streq(name, "fetch") && argc == 2) {
        /* fetch(key, default) -> has_key? ? value : default */
        TyKind vt = ty_hash_val(rt);
        Repr dr = repr_of(c, argv[1]);
        TyKind dt = dr.as_ty;
        /* Empty `{}` default infers TY_UNKNOWN but is a hash -- incompatible with int/str etc. */
        if (dr.untyped) {
          const char *atn = nt_type(c->nt, argv[1]);
          if (atn && (sp_streq(atn, "HashNode") || sp_streq(atn, "KeywordHashNode")))
            dt = TY_POLY_POLY_HASH;
        }
        int needs_box = (vt != TY_POLY && ty_unify(vt, dt) == TY_POLY);
        int th = ++g_tmp, tk = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        /* rooted across the key and the default, as the array arms are; a
           local or literal key with a literal default allocates nothing, and
           the root was a push and a pop per lookup in a counting loop */
        if (!(fetch_operand_is_inert(c, argv[0]) && fetch_operand_is_inert(c, argv[1]) && !needs_box &&
              (ty_hash_key(rt) != TY_STRING || comp_ntype(c, argv[0]) == TY_STRING) &&
              !(vt == TY_POLY && dt != TY_POLY)))
          buf_printf(b, "; SP_GC_ROOT(_t%d)", th);
        /* a key no entry matches (hash_okey_miss) goes through the _okey ops */
        const char *ks = hash_okey_sfx(c, argv[0], ty_hash_key(rt));
        buf_printf(b, "; %s _t%d = ", hash_key_ctype(c, argv[0], ty_hash_key(rt)), tk); emit_hash_key_o(c, argv[0], ty_hash_key(rt), b);
        emit_hash_fetch_default_tail(c, id, argv[1], hn, ks, vt, dt, needs_box, th, tk, b);
        buf_puts(b, "; })");
        return 1;
      }
      /* an Array init concatenates the pairs onto it, flat (#3571) */
      if (sp_streq(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0 &&
          (ty_is_array(comp_ntype(c, argv[0])) ||
           (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ArrayNode")))) {
        buf_puts(b, "sp_poly_hash_sum_arr("); emit_boxed(c, recv, b); buf_puts(b, ", ");
        if (ty_is_array(comp_ntype(c, argv[0]))) {
          if (repr_of(c, argv[0]).elem == TY_POLY) emit_expr(c, argv[0], b);
          else { buf_puts(b, "sp_poly_to_poly_array("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
        }
        else { buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), sp_PolyArray_new())"); }
        buf_puts(b, ")");
        return 1;
      }
      /* A seed of any other class folds the pairs into IT: `nil + [k, v]` is
         nil's own missing `+` (NoMethodError), not the Integer coercion the
         int arm below reports -- and an empty hash adds nothing and answers
         the seed itself, so `{}.sum(nil)` is nil. */
      if (sp_streq(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0 &&
          !fold_seed_typed(fold_seed_ntype(c, argv[0]), TY_INT)) {
        emit_poly_sum_seed(c, recv, argv[0], b);
        return 1;
      }
      if (sp_streq(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        /* Hash#sum without a block folds each [k,v] PAIR into the init value;
           `init + [k,v]` is Integer#+ Array -> TypeError, so only an empty hash
           (which returns the init unchanged) is well-defined. Without an init
           value it is a row. */
        int t = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sHash_length(_t%d) == 0 ? (sp_int)(", hn, t);
        emit_int_expr(c, argv[0], b);
        buf_puts(b, ") : (sp_raise_cls(\"TypeError\", \"Array can't be coerced into Integer\"), (sp_int)0); })");
        return 1;
      }
      if ((sp_streq(name, "value?") || sp_streq(name, "has_value?")) && argc == 1) {
        int poly = (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH ||
                    rt == TY_POLY_POLY_HASH);  /* boxed-value variants (#2373) */
        if (!poly && value_obj_compares(c, argv[0])) {
          unsupported_feature(c, id, "Hash#value? of a user object defining == in a typed Hash");
          return 0;
        }
        /* a boxed argument against a typed hash: compared boxed, pair by
           pair, so a value of another kind is simply not found; passed raw,
           the sp_RbVal reached a const char * parameter (#4939) */
        if (!poly && repr_of(c, argv[0]).kind == RK_BOXED) {
          int th = ++g_tmp, ta = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
          buf_puts(b, "({ "); th = hold_operand(c, recv, TY_POLY, 1, th, 1, " ", b);
          ta = hold_operand(c, argv[0], TY_POLY, 0, ta, 1, " ", b);
          buf_printf(b, "sp_bool _t%d = 0;", tr);
          buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_poly_length(_t%d); _t%d++) {"
                        " sp_RbVal _k, _v; sp_poly_hash_pair(_t%d, _t%d, &_k, &_v);"
                        " if (sp_poly_rb_equal(_v, _t%d)) { _t%d = 1; break; } } _t%d; })",
                     ti, ti, th, ti, th, ti, ta, tr, tr);
          return 1;
        }
        if (!poly && value_kind_misses(c, argv[0], ty_hash_val(rt))) {
          buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)("); emit_expr(c, argv[0], b); buf_puts(b, "); 0; })");
          return 1;
        }
        buf_printf(b, "sp_%sHash_has_value(", hn);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (poly) emit_boxed(c, argv[0], b); else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      /* merge!/update with no Hash has nothing to fold in and answers the
         receiver, after the nil and frozen checks; a block has no conflict to
         resolve. A program that defines the name is left to the path it took before. */
      if ((is_hash_merge_bang(name)) && argc == 0 &&
          !an_zero_arg_builtin_shadowed(c, name, argc)) {
        int tr = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), tr); emit_expr(c, recv, b); buf_puts(b, ";");
        buf_printf(b, " if (!_t%d) sp_nil_recv(\"%s\");", tr, name);
        buf_printf(b, " if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", tr, tr, hash_box_cls(rt));
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      /* PolyPoly receiver: any hash-variant argument folds in through the
         boxed [k, v] pair walk (a heterogeneous hash routinely absorbs a
         typed one). Blockless. */
      /* merge!(*hashes): the sources are an array whose elements are only
         known at run time, so walk it and merge each element in (#3848). */
      if ((is_hash_merge_bang(name)) && argc == 1 &&
          nt_ref(nt, id, "block") < 0 && nt_kind(nt, argv[0]) == NK_SplatNode) {
        int inner = nt_ref(nt, argv[0], "expression");
        if (inner < 0) return 0;
        TyKind it = comp_ntype(c, inner);
        if (!ty_is_array(it) && it != TY_POLY_ARRAY && it != TY_POLY) return 0;
        int tr = ++g_tmp, ts = ++g_tmp, ti = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), tr); emit_expr(c, recv, b); buf_puts(b, ";");
        buf_printf(b, " if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", tr, tr, hash_box_cls(rt));
        buf_printf(b, " sp_RbVal _t%d = ", ts); emit_boxed(c, inner, b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", ts);
        buf_printf(b, " sp_int _t%d = sp_poly_arr_len_ex(_t%d);", ti, ts);
        char rtxt[32]; snprintf(rtxt, sizeof rtxt, "_t%d", tr);
        buf_printf(b, " for (sp_int _i8 = 0; _i8 < _t%d; _i8++)"
                      " sp_poly_hash_merge_into(", ti);
        emit_boxed_text(c, rt, rtxt, b);
        buf_printf(b, ", sp_poly_each_elem(_t%d, _i8));", ts);
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      /* ... and so with a block handed in as a proc (`update(other, &block)`,
         nil when the caller gave none), which resolves a key already there:
         the proc is called with the key, the old value and the new one */
      int mb_blk = nt_ref(nt, id, "block");
      int mb_proc = mb_blk >= 0 && nt_kind(nt, mb_blk) == NK_BlockArgumentNode
                    ? nt_ref(nt, mb_blk, "expression") : -1;
      /* the proc a forward of an inlined body's own block stands for: the
         caller's literal built into one, its `&expr`, the real proc the
         inline was handed, or none */
      int mb_res = mb_blk >= 0 ? resolve_forwarded_block(c, mb_blk) : -1;
      int mb_fwd = mb_blk >= 0 && mb_res != mb_blk;
      if ((is_hash_merge_bang(name)) && argc >= 1 &&
          (mb_blk < 0 || (mb_proc >= 0 && (mb_fwd || nt_kind(nt, mb_res) != NK_BlockNode))) &&
          rt == TY_POLY_POLY_HASH) {
        /* An argument that is a Hash at run time only is checked there, as
           CRuby's implicit conversion would, so a boxed hash merges into
           another; an empty literal has no variant to infer and nothing to
           fold in. */
        for (int ai = 0; ai < argc; ai++) {
          TyKind at = comp_ntype(c, argv[ai]);
          if (!ty_is_hash(at) && at != TY_POLY && !hash_lit_empty(nt, argv[ai])) return 0;
        }
        int tr = ++g_tmp, tb = -1;
        buf_printf(b, "({ sp_PolyPolyHash *_t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, ";");
        buf_printf(b, " if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", tr, tr, hash_box_cls(rt));   /* (#3001) */
        if (mb_proc >= 0) {
          Buf pp; memset(&pp, 0, sizeof pp);
          if (!mb_fwd) emit_boxed(c, mb_proc, &pp);
          else {
            Buf pr; memset(&pr, 0, sizeof pr);
            if (mb_res >= 0) { if (!emit_forwarded_proc_arg(c, mb_res, &pr)) emit_proc_literal(c, mb_res, &pr); }
            else { const char *fwd = forwarded_real_proc(mb_blk, mb_res); buf_puts(&pr, fwd ? fwd : "NULL"); }
            buf_printf(&pp, "({ sp_Proc *_pp = %s; _pp ? sp_box_obj(_pp, SP_BUILTIN_PROC) : sp_box_nil(); })",
                       pr.p ? pr.p : "NULL");
            free(pr.p);
          }
          tb = ++g_tmp;
          buf_printf(b, " sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);", tb, pp.p ? pp.p : "sp_box_nil()", tb);
          free(pp.p);
        }
        for (int ai = 0; ai < argc; ai++) {
          if (hash_lit_empty(nt, argv[ai])) continue;
          int to = ++g_tmp, ti = ++g_tmp, tp = ++g_tmp;
          buf_printf(b, " sp_RbVal _t%d = ", to); emit_boxed(c, argv[ai], b);
          buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", to);
          if (comp_ntype(c, argv[ai]) == TY_POLY)
            buf_printf(b, " if (_t%d.tag != SP_TAG_OBJ || !sp_poly_is_hash_kind(_t%d.cls_id))"
                          " sp_raise_cls(\"TypeError\", sp_sprintf(\"no implicit conversion of %%s into Hash\", sp_convert_src_name(_t%d)));",
                       to, to, to);
          if (tb >= 0) {
            int tk = ++g_tmp, tv = ++g_tmp;
            buf_printf(b, " sp_int _t%d = sp_poly_arr_len_ex(_t%d);"
                          " for (sp_int _i9 = 0; _i9 < _t%d; _i9++) {"
                          " sp_RbVal _t%d = sp_poly_each_elem(_t%d, _i9);"
                          " sp_RbVal _t%d = sp_PolyArray_get((sp_PolyArray *)_t%d.v.p, 0);"
                          " sp_RbVal _t%d = sp_PolyArray_get((sp_PolyArray *)_t%d.v.p, 1);"
                          " if (_t%d.tag != SP_TAG_NIL && sp_PolyPolyHash_has_key(_t%d, _t%d)) {"
                          " _sp_proc_poly_args[0] = _t%d; _sp_proc_poly_args[1] = sp_PolyPolyHash_get(_t%d, _t%d);"
                          " _sp_proc_poly_args[2] = _t%d;"
                          " sp_int _sl[3] = { sp_poly_slot_i(_sp_proc_poly_args[0]), sp_poly_slot_i(_sp_proc_poly_args[1]),"
                          " sp_poly_slot_i(_sp_proc_poly_args[2]) };"
                          " _t%d = sp_poly_callable_call(_t%d, 3, _sl); }"
                          " sp_PolyPolyHash_set(_t%d, _t%d, _t%d); }",
                       ti, to, ti, tp, to, tk, tp, tv, tp,
                       tb, tr, tk, tk, tr, tk, tv, tv, tb, tr, tk, tv);
            continue;
          }
          buf_printf(b, " sp_int _t%d = sp_poly_arr_len_ex(_t%d);"
                        " for (sp_int _i9 = 0; _i9 < _t%d; _i9++) {"
                        " sp_RbVal _t%d = sp_poly_each_elem(_t%d, _i9);"
                        " sp_PolyPolyHash_set(_t%d,"
                        " sp_PolyArray_get((sp_PolyArray *)_t%d.v.p, 0),"
                        " sp_PolyArray_get((sp_PolyArray *)_t%d.v.p, 1)); }",
                     ti, to, ti, tp, to, tr, tp, tp);
        }
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      if ((is_hash_merge_bang(name)) && argc == 1) {
        /* In-place merge: insert each key of `other` into the receiver (a
           conflict-resolution block, if present, picks the kept value), then
           yield the receiver. A typed receiver can't change variant in place,
           so only a same-variant argument is accepted; any other argument
           (including a poly-boxed hash, which can't be variant-checked here
           without risking type confusion) falls through to the unsupported
           path rather than silently dropping or mistyping. */
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        /* merging an empty hash literal changes nothing; yield the receiver,
           after the frozen check. (An empty `{}` has no inferable variant, so
           it can't take the loop.) */
        const char *aty0 = nt_type(nt, argv[0]);
        if (aty0 && (sp_streq(aty0, "HashNode") || sp_streq(aty0, "KeywordHashNode"))) {
          int en = 0; nt_arr(nt, argv[0], "elements", &en);
          if (en == 0) {
            /* nothing to merge, but a frozen receiver refuses it all the same */
            int te = ++g_tmp;
            buf_printf(b, "({ %s _t%d = ", c_type_name(rt), te); emit_expr(c, recv, b);
            buf_printf(b, "; if (_t%d && sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s); _t%d; })",
                       te, te, te, hash_box_cls(rt), te);
            return 1;
          }
        }
        Repr ar = repr_of(c, argv[0]);
        TyKind at = ar.as_ty;
        /* a forwarded `&blk` is the caller's block: none where it passed none
           (a plain merge), its literal where it passed one (whose parameters
           the collision binds); a real proc handed in its place stays */
        int blk0 = nt_ref(nt, id, "block");
        int blk = resolve_forwarded_block(c, blk0);
        if (blk < 0 && forwarded_real_proc(blk0, blk)) blk = blk0;
        /* A receiver whose values are boxed takes another variant's pairs
           boxed -- the key too when its keys are (a local or ivar widened
           because a merged value or the conflict block's did not fit the
           literal's variant). The argument's order[] holds its keys, which
           a PolyPoly argument's does not, so that one stays out. */
        if (ar.key != TY_UNKNOWN && !repr_hash_is(ar, kt, vt) && !repr_hash_is(ar, TY_POLY, TY_POLY) &&
            vt == TY_POLY && (kt == TY_POLY || kt == ar.key)) {
          TyKind akt = ar.key, avt = ar.val;
          const char *ahn = ty_hash_cname(at);
          int tr = ++g_tmp, to = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp, trk = ++g_tmp;
          char akey[32], aval[128];
          snprintf(akey, sizeof akey, "_t%d", tk);
          snprintf(aval, sizeof aval, "sp_%sHash_get(_t%d, _t%d)", ahn, to, tk);
          buf_printf(b, "({ %s _t%d = ", c_type_name(rt), tr); emit_expr(c, recv, b); buf_puts(b, ";");
          buf_printf(b, " if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", tr, tr, hash_box_cls(rt));
          buf_printf(b, " %s _t%d = ", c_type_name(at), to); emit_expr(c, argv[0], b); buf_puts(b, ";");
          buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, to, ti);
          buf_printf(b, " %s _t%d = _t%d->order[_t%d];", c_type_name(akt), tk, to, ti);
          buf_printf(b, " %s _t%d = ", c_type_name(kt), trk);
          if (kt == TY_POLY) emit_boxed_text(c, akt, akey, b); else buf_puts(b, akey);
          buf_puts(b, ";");
          if (blk >= 0) {
            const char *bp0 = block_param_name(c, blk, 0);
            const char *bp1 = block_param_name(c, blk, 1);
            const char *bp2 = block_param_name(c, blk, 2);
            buf_printf(b, " if (sp_%sHash_has_key(_t%d, _t%d)) {", hn, tr, trk);
            char k1[24], v1[96];
            snprintf(k1, sizeof k1, "_t%d", trk);
            snprintf(v1, sizeof v1, "sp_%sHash_get(_t%d, _t%d)", hn, tr, trk);
            emit_merge_blk_param(c, blk, bp0, kt, k1, NULL, b);
            emit_merge_blk_param(c, blk, bp1, vt, v1, NULL, b);
            if (bp2) {
              buf_printf(b, " lv_%s = ", rename_local(bp2));
              emit_boxed_text(c, avt, aval, b);
              buf_puts(b, ";");
            }
            buf_printf(b, " sp_%sHash_set(_t%d, _t%d, ", hn, tr, trk);
            emit_blk_value_as(c, blk, vt, b);
            buf_puts(b, "); }\nelse");
          }
          buf_printf(b, " { sp_%sHash_set(_t%d, _t%d, ", hn, tr, trk);
          emit_boxed_text(c, avt, aval, b);
          buf_printf(b, "); } } _t%d; })", tr);
          return 1;
        }
        int mdef = -1;
        if (at != rt && (!ty_is_object(at) || obj_container_conv(c, at, "to_hash", &mdef) != rt)) return 0;
        /* values that may be nil are read and stored with their nil (D3b-ii) */
        int mvn = oint_kind(vt) && hash_vals_nullable(c, recv);
        const char *mset = mvn ? "oset" : "set", *mget = mvn ? "vget" : "get";
        int tr = ++g_tmp, to = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), tr); emit_expr(c, recv, b); buf_puts(b, ";");
        int tob = at != rt ? ++g_tmp : 0, byv = repr_of(c, argv[0]).kind == RK_VOBJ;
        char ob[32]; snprintf(ob, sizeof ob, "_t%d", tob);
        if (at != rt) {
          buf_printf(b, " SP_GC_ROOT(_t%d); ", tr); emit_ctype(c, at, b); buf_printf(b, " _t%d = ", tob);
          emit_expr(c, argv[0], b); buf_puts(b, ";");
          if (!byv) buf_printf(b, " SP_GC_ROOT(_t%d);", tob);
        }
        buf_printf(b, " if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", tr, tr, hash_box_cls(rt));   /* (#3001) */
        buf_printf(b, " %s _t%d = ", c_type_name(rt), to);
        if (at != rt) {
          emit_obj_conv_call(c, mdef, "to_hash", byv, ob, b);
          buf_printf(b, "; if (!_t%d) { const char *_cn%d = sp_poly_class_name(", to, to); emit_boxed_text(c, at, ob, b);
          buf_printf(b, "); sp_raise_cls(\"TypeError\", sp_sprintf(\"can't convert %%s to Hash (%%s#to_hash gives NilClass)\", _cn%d, _cn%d)); }"
                        " SP_GC_ROOT(_t%d)", to, to, to);
        } else emit_expr(c, argv[0], b);
        buf_puts(b, ";");
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, to, ti);
        /* a PolyPoly hash's order[] holds slot indices: its keys are keys[] */
        if (rt == TY_POLY_POLY_HASH)
          buf_printf(b, " sp_RbVal _t%d = _t%d->keys[_t%d->order[_t%d]];", tk, to, to, ti);
        else
          buf_printf(b, " %s _t%d = _t%d->order[_t%d];", c_type_name(kt), tk, to, ti);
        if (blk >= 0) {
          const char *bp0 = block_param_name(c, blk, 0);
          const char *bp1 = block_param_name(c, blk, 1);
          const char *bp2 = block_param_name(c, blk, 2);
          buf_printf(b, " if (sp_%sHash_has_key(_t%d, _t%d)) {", hn, tr, tk);
          char k2[24], v2[96], w2[96];
          snprintf(k2, sizeof k2, "_t%d", tk);
          snprintf(v2, sizeof v2, "sp_%sHash_%s(_t%d, _t%d)", hn, mget, tr, tk);
          snprintf(w2, sizeof w2, "sp_%sHash_%s(_t%d, _t%d)", hn, mget, to, tk);
          emit_merge_blk_param(c, blk, bp0, kt, k2, NULL, b);
          emit_merge_blk_param(c, blk, bp1, vt, v2, mvn ? v2 : NULL, b);
          emit_merge_blk_param(c, blk, bp2, vt, w2, mvn ? w2 : NULL, b);
          buf_printf(b, " sp_%sHash_%s(_t%d, _t%d, ", hn, mset, tr, tk);
          if (mvn) emit_blk_value_as_o(c, blk, vt, b); else emit_blk_value_as(c, blk, vt, b);
          buf_printf(b, "); }\nelse { sp_%sHash_%s(_t%d, _t%d, sp_%sHash_%s(_t%d, _t%d)); }", hn, mset, tr, tk, hn, mget, to, tk);
        }
        else {
          buf_printf(b, " sp_%sHash_%s(_t%d, _t%d, sp_%sHash_%s(_t%d, _t%d));", hn, mset, tr, tk, hn, mget, to, tk);
        }
        buf_printf(b, " } _t%d; })", tr);
        return 1;
      }
      if (sp_streq(name, "merge") && argc == 1 && nt_ref(nt, id, "block") >= 0 &&
          rt == TY_POLY_POLY_HASH) {
        /* merge(other) { |k, o, n| } on a PolyPolyHash (e.g. a reduce({})
           accumulator). PolyPolyHash is open-addressing: order[i] is a table
           slot, so the key is keys[order[i]] -- the generic order[i]-as-key
           path below miscompiles it. `other` can be any boxed hash (an element
           read out of a poly Array loses its concrete variant), so iterate it
           through sp_poly_hash_pair. Block params are declared locally here so
           a merge nested inside a fold (where they are not scope locals) still
           resolves them (#3100). */
        int blk = nt_ref(nt, id, "block");
        const char *bp0 = block_param_name(c, blk, 0);
        const char *bp1 = block_param_name(c, blk, 1);
        const char *bp2 = block_param_name(c, blk, 2);
        int tr = ++g_tmp, tc = ++g_tmp, tj = ++g_tmp, to = ++g_tmp,
            tn = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp, tv = ++g_tmp;
        buf_printf(b, "({ sp_PolyPolyHash *_t%d = sp_PolyPolyHash_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " sp_PolyPolyHash *_t%d = ", tc); emit_expr(c, recv, b); buf_puts(b, ";");
        buf_printf(b, " _t%d->default_v = _t%d->default_v; _t%d->dproc = _t%d->dproc; _t%d->dproc_self = _t%d->dproc_self;", tr, tc, tr, tc, tr, tc);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {"
                      " sp_int _ix = _t%d->order[_t%d];"
                      " sp_PolyPolyHash_set(_t%d, _t%d->keys[_ix], _t%d->vals[_ix]); }",
                   tj, tj, tc, tj, tc, tj, tr, tc, tc);
        buf_printf(b, " sp_RbVal _t%d = ", to); emit_boxed(c, argv[0], b); buf_puts(b, ";");
        if (bp0) buf_printf(b, " sp_RbVal lv_%s;", rename_local(bp0));
        if (bp1) buf_printf(b, " sp_RbVal lv_%s;", rename_local(bp1));
        if (bp2) buf_printf(b, " sp_RbVal lv_%s;", rename_local(bp2));
        buf_printf(b, " sp_int _t%d = sp_poly_length(_t%d);", tn, to);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) {", ti, ti, tn, ti);
        buf_printf(b, " sp_RbVal _t%d, _t%d; sp_poly_hash_pair(_t%d, _t%d, &_t%d, &_t%d);",
                   tk, tv, to, ti, tk, tv);
        buf_printf(b, " if (sp_PolyPolyHash_has_key(_t%d, _t%d)) {", tr, tk);
        if (bp0) buf_printf(b, " lv_%s = _t%d;", rename_local(bp0), tk);
        if (bp1) buf_printf(b, " lv_%s = sp_PolyPolyHash_get(_t%d, _t%d);", rename_local(bp1), tr, tk);
        if (bp2) buf_printf(b, " lv_%s = _t%d;", rename_local(bp2), tv);
        buf_printf(b, " sp_PolyPolyHash_set(_t%d, _t%d, ", tr, tk);
        if (!emit_blk_value_via_next(c, blk, TY_POLY, b)) {
          buf_puts(b, "({ ");
          int bbody = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
          int bval = bn > 0 ? bb[bn - 1] : -1;
          /* redirect g_pre so an allocating value expression (e.g. a `[o, n]`
             array literal) drains its setup INSIDE this stmt-expr -- after the
             block params were assigned above -- rather than being hoisted
             before the merge loop, which would read the params' initial nil
             (#3100 follow-up). */
          Buf lpre; memset(&lpre, 0, sizeof lpre);
          Buf lval; memset(&lval, 0, sizeof lval);
          Buf *saved_pre = g_pre; g_pre = &lpre;
          for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], &lval, 0);
          if (bval >= 0) {
            if (repr_of(c, bval).kind != RK_BOXED) emit_boxed(c, bval, &lval);
            else emit_expr(c, bval, &lval);
          }
          else buf_puts(&lval, "sp_box_nil()");
          g_pre = saved_pre;
          if (lpre.p) buf_puts(b, lpre.p);
          if (lval.p) buf_puts(b, lval.p);
          free(lpre.p); free(lval.p);
          buf_puts(b, "; })");
        }
        buf_printf(b, "); }\nelse { sp_PolyPolyHash_set(_t%d, _t%d, _t%d); } }", tr, tk, tv);
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      /* A merge with a block typed as the general boxed hash (a block value
         or an argument the receiver's variant cannot hold) builds that hash.
         The boxed-receiver arm usually takes it first; it stands aside when
         a user class defines or reads `merge`. */
      if (sp_streq(name, "merge") && argc == 1 && nt_ref(nt, id, "block") >= 0 &&
          repr_hash_is(repr_of(c, id), TY_POLY, TY_POLY) && rt != TY_POLY_POLY_HASH &&
          emit_merge_block_boxed(c, id, recv, argv[0], nt_ref(nt, id, "block"), b))
        return 1;
      /* merge with a block, typed as the receiver's own variant */
      if (sp_streq(name, "merge") && argc == 1 && nt_ref(nt, id, "block") >= 0 &&
          repr_hash_is(repr_of(c, id), ty_hash_key(rt), ty_hash_val(rt))) {
        return emit_merge_block_same_variant(c, id, recv, argv[0], rt, hn, b);
      }
      /* merge(*hashes): fold each member of the splatted list in, through the
         universal boxed merge (#3561) */
      if (sp_streq(name, "merge") && argc == 1 && nt_ref(nt, id, "block") < 0 &&
          nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "SplatNode")) {
        int sx = nt_ref(nt, argv[0], "expression");
        int tacc = ++g_tmp, tls = ++g_tmp, tmi = ++g_tmp;
        buf_printf(b, "({ sp_PolyPolyHash *_t%d = sp_poly_hash_merge(", tacc);
        emit_boxed(c, recv, b);
        buf_printf(b, ", sp_box_nil()); SP_GC_ROOT(_t%d);", tacc);
        buf_printf(b, " sp_PolyArray *_t%d = sp_poly_to_poly_array(", tls);
        if (sx >= 0) emit_boxed(c, sx, b); else buf_puts(b, "sp_box_nil()");
        buf_printf(b, "); SP_GC_ROOT(_t%d);", tls);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                      " _t%d = sp_poly_hash_merge(sp_box_nullable_obj((void *)_t%d, SP_BUILTIN_POLY_POLY_HASH),"
                      " sp_PolyArray_get(_t%d, _t%d));",
                   tmi, tmi, tls, tmi, tacc, tacc, tls, tmi);
        buf_printf(b, " _t%d; })", tacc);
        return 1;
      }
      if (sp_streq(name, "merge") && argc == 1 &&
          (rt == TY_STR_INT_HASH || rt == TY_STR_POLY_HASH || rt == TY_SYM_POLY_HASH ||
           rt == TY_STR_STR_HASH || rt == TY_POLY_POLY_HASH || rt == TY_INT_INT_HASH ||
           rt == TY_INT_STR_HASH)) {
        TyKind at = comp_ntype(c, argv[0]);
        /* an empty Hash literal settles at its own default variant, which is
           rarely the receiver's; merging it passed one hash struct as another
           (#3597). It contributes nothing, so the merge is a copy. */
        { const char *aty0 = nt_type(nt, argv[0]); int aen = 0;
          if (aty0 && sp_streq(aty0, "HashNode") &&
              (nt_arr(nt, argv[0], "elements", &aen), aen == 0)) {
            buf_printf(b, "sp_%sHash_dup(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
            return 1;
          } }
        /* cross-variant str merge: promote both sides to str_poly_hash */
        if ((rt == TY_STR_INT_HASH || rt == TY_STR_STR_HASH) &&
            ty_is_hash(at) && ty_hash_key(at) == TY_STRING && at != rt) {
          buf_puts(b, "sp_StrPolyHash_merge(");
          const char *rfn = rt == TY_STR_INT_HASH ? "sp_StrPolyHash_from_str_int_hash("
                                                   : "sp_StrPolyHash_from_str_str_hash(";
          buf_puts(b, rfn); emit_expr(c, recv, b); buf_puts(b, "), ");
          const char *afn = at == TY_STR_INT_HASH ? "sp_StrPolyHash_from_str_int_hash("
                          : at == TY_STR_STR_HASH  ? "sp_StrPolyHash_from_str_str_hash("
                                                   : NULL;
          if (afn) { buf_puts(b, afn); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
          else { emit_expr(c, argv[0], b); }
          buf_puts(b, ")");
          return 1;
        }
        /* any other cross-variant merge (mismatched key or value layout):
           fold both sides through the universal boxed merge -- passing the
           argument raw into the receiver-layout helper read it through the
           wrong struct (#3261). Matches the TY_POLY_POLY_HASH inference. */
        if (ty_is_hash(at) && at != rt &&
            !(rt == TY_STR_POLY_HASH && (at == TY_STR_STR_HASH || at == TY_STR_INT_HASH))) {
          buf_puts(b, "sp_poly_hash_merge("); emit_boxed(c, recv, b);
          buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
          return 1;
        }
        /* a BOXED argument holds whichever variant the value really is: casting
           it to the receiver's layout read a Sym-keyed hash through a Str-keyed
           struct, and the Symbol key was then dereferenced as a char * (#3975).
           Fold through the universal boxed merge instead, as a cross-variant
           merge already does. */
        if (at == TY_POLY) {
          buf_puts(b, "sp_poly_hash_merge("); emit_boxed(c, recv, b);
          buf_puts(b, ", sp_poly_hash_merge_arg("); emit_boxed(c, argv[0], b); buf_puts(b, "))");
          return 1;
        }
        if (!ty_is_hash(at) && at != TY_POLY && at != TY_UNKNOWN) {
          /* an object answering #to_hash of the receiver's own layout merges
             as that Hash; any other class is CRuby's TypeError */
          int mdef = -1;
          TyKind mk = obj_container_conv(c, at, "to_hash", &mdef);
          if (mk == rt) {
            buf_printf(b, "sp_%sHash_merge(", hn); emit_expr(c, recv, b); buf_puts(b, ", ");
            emit_obj_container_conv(c, argv[0], mdef, "to_hash", b); buf_puts(b, ")");
            return 1;
          }
          if (mk != TY_UNKNOWN) {
            /* the analysis typed this call as the receiver's layout; a
               #to_hash of another layout would widen it, which is an
               inference question, so say so rather than miscompile */
            unsupported_feature(c, id, "Hash#merge with a #to_hash of another layout than the receiver");
            return 0;
          }
          /* conv_cls_name_of leaves a Complex and a Rational to the numeric
             slots, which convert them; neither has a #to_hash */
          const char *mcn = at == TY_COMPLEX || at == TY_RATIONAL ? conv_builtin_class_name(at)
                                                                  : conv_cls_name_of(c, at);
          if (mcn) {
            buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)("); emit_expr(c, argv[0], b);
            buf_printf(b, "); sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into Hash\"); (%s)0; })", mcn, c_type_name(rt));
            return 1;
          }
        }
        buf_printf(b, "sp_%sHash_merge(", hn); emit_expr(c, recv, b); buf_puts(b, ", ");
        /* a str_poly receiver may be merged with a concrete str-keyed hash;
           coerce the argument to the receiver's variant first */
        if (rt == TY_STR_POLY_HASH && (at == TY_STR_STR_HASH || at == TY_STR_INT_HASH)) {
          buf_printf(b, "sp_StrPolyHash_from_%s(", at == TY_STR_STR_HASH ? "str_str_hash" : "str_int_hash");
          emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else if (at == TY_POLY && rt == TY_POLY_POLY_HASH) {
          /* A boxed argument holds whichever variant the value really is, so
             the pointer cast below would read a Sym-keyed hash through a
             Poly-keyed struct. The general hash can be rebuilt from any of
             them: merge it into an empty one. */
          buf_puts(b, "sp_poly_hash_merge("); emit_expr(c, argv[0], b); buf_puts(b, ", sp_box_nil())");
        }
        else if (at == TY_POLY) {
          /* poly arg: unbox to the receiver's hash type */
          int t = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_expr(c, argv[0], b);
          buf_printf(b, "; (sp_%sHash*)_t%d.v.p; })", hn, t);
        }
        /* an unresolved argument (a name nothing defines) is the gate's
           raise token: coerced to the receiver's layout, it raises before
           the merge reads it (#7276, #7277) */
        else if (at == TY_UNKNOWN) emit_unresolved_coerced(c, argv[0], rt, b);
        else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      /* except(*keys): a copy of the hash without the given keys (the variants
         that have a runtime delete: str/sym/poly-keyed). */
      /* Hash#slice(k, ...): a fresh hash of the present keys, in argument
         order (CRuby keeps hash order; argument order matches for the common
         literal-key use). Same variants as #except below. */
      if (sp_streq(name, "slice") && hn && argc >= 1 &&
          (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_STR_STR_HASH ||
           rt == TY_STR_INT_HASH || rt == TY_POLY_POLY_HASH ||
           rt == TY_INT_INT_HASH || rt == TY_INT_STR_HASH)) {
        int th = ++g_tmp, tr = ++g_tmp;
        buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th);
        emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);", th, hn, tr, hn, tr);
        TyKind skt = ty_hash_key(rt);
        int svn = ty_hash_val(rt) == TY_INT && hash_vals_nullable(c, recv);   /* values copied with their nil (D3b-ii) */
        for (int i = 0; i < argc; i++) {
          /* A splatted key list contributes each of its members, not one key.
             `except` has had this arm since #3561; `slice` never did, so
             `h.slice(*ATTRS)` handed the whole array to the key coercion and
             kept whatever that answered -- no keys for a Symbol-keyed hash,
             the first one for a local (#4164). */
          if (nt_type(nt, argv[i]) && sp_streq(nt_type(nt, argv[i]), "SplatNode")) {
            int sx = nt_ref(nt, argv[i], "expression");
            int tsa = ++g_tmp, tsi = ++g_tmp;
            buf_printf(b, " sp_PolyArray *_t%d = sp_poly_to_poly_array(", tsa);
            if (sx >= 0) emit_boxed(c, sx, b); else buf_puts(b, "sp_box_nil()");
            buf_printf(b, "); SP_GC_ROOT(_t%d);", tsa);
            buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {",
                       tsi, tsi, tsa, tsi);
            { char el[64]; snprintf(el, sizeof el, "sp_PolyArray_get(_t%d, _t%d)", tsa, tsi);
              int tsk = ++g_tmp;
              if (rt == TY_POLY_POLY_HASH) buf_printf(b, " sp_RbVal _t%d = %s;", tsk, el);
              else if (skt == TY_SYMBOL) buf_printf(b, " sp_sym _t%d = (sp_sym)sp_poly_to_i(%s);", tsk, el);
              else if (skt == TY_INT) buf_printf(b, " sp_int _t%d = sp_poly_to_i(%s);", tsk, el);
              else buf_printf(b, " const char *_t%d = %ssp_poly_to_s(%s)%s;", tsk, env_key_open(nt, argv[i]), el, env_key_close(nt, argv[i]));
              buf_printf(b, " if (sp_%sHash_has_key(_t%d, _t%d)) sp_%sHash_%s(_t%d, _t%d, sp_%sHash_%s(_t%d, _t%d)); }",
                         hn, th, tsk, hn, svn ? "oset" : "set", tr, tsk, hn, svn ? "vget" : "get", th, tsk);
            }
            continue;
          }
          int tk = ++g_tmp;
          if (rt != TY_POLY_POLY_HASH && emit_slice_okey_key(c, argv[i], skt, hn, th, tr, tk, svn, b)) continue;
          if (rt == TY_POLY_POLY_HASH) {
            buf_printf(b, " { sp_RbVal _t%d = ", tk); emit_boxed(c, argv[i], b);
          }
          else if (skt == TY_SYMBOL) {
            buf_printf(b, " { sp_sym _t%d = ", tk); emit_hash_key(c, argv[i], skt, b);
          }
          else if (skt == TY_INT) {
            buf_printf(b, " { sp_int _t%d = ", tk); emit_hash_key(c, argv[i], skt, b);
          }
          else {
            buf_printf(b, " { const char *_t%d = ", tk); emit_hash_key(c, argv[i], skt, b);
          }
          buf_printf(b, "; if (sp_%sHash_has_key(_t%d, _t%d)) sp_%sHash_%s(_t%d, _t%d, sp_%sHash_%s(_t%d, _t%d)); }",
                     hn, th, tk, hn, svn ? "oset" : "set", tr, tk, hn, svn ? "vget" : "get", th, tk);
        }
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      if (sp_streq(name, "except") && hn &&
          (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_STR_STR_HASH ||
           rt == TY_STR_INT_HASH || rt == TY_POLY_POLY_HASH ||
           rt == TY_INT_INT_HASH || rt == TY_INT_STR_HASH)) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sHash *_t%d = sp_%sHash_dup(", hn, t, hn);
        emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d);", t);
        /* except answers a fresh hash: no default, no default proc */
        {
          TyKind evt = ty_hash_val(rt);
          if (evt == TY_POLY)
            buf_printf(b, " _t%d->default_v = sp_box_nil(); _t%d->dproc = NULL; _t%d->dproc_self = NULL;", t, t, t);
          else if (evt == TY_STRING) buf_printf(b, " _t%d->default_v = NULL;", t);
          else buf_printf(b, " _t%d->default_v = 0; _t%d->default_nil = 1;", t, t);
        }
        for (int i = 0; i < argc; i++) {
          /* a splatted key list deletes each of its members (#3561) */
          if (nt_type(nt, argv[i]) && sp_streq(nt_type(nt, argv[i]), "SplatNode")) {
            int sx = nt_ref(nt, argv[i], "expression");
            int tsa = ++g_tmp, tsi = ++g_tmp;
            buf_printf(b, " sp_PolyArray *_t%d = sp_poly_to_poly_array(", tsa);
            if (sx >= 0) emit_boxed(c, sx, b); else buf_puts(b, "sp_box_nil()");
            buf_printf(b, "); SP_GC_ROOT(_t%d);", tsa);
            buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                          " sp_%sHash_delete%s(_t%d, ", tsi, tsi, tsa, tsi, hn, rt == TY_STR_POLY_HASH ? "_bytes" : "", t);
            {
              char el[64]; snprintf(el, sizeof el, "sp_PolyArray_get(_t%d, _t%d)", tsa, tsi);
              TyKind kt2 = ty_hash_key(rt);
              if (rt == TY_POLY_POLY_HASH) buf_puts(b, el);
              else if (kt2 == TY_SYMBOL) buf_printf(b, "(sp_sym)sp_poly_to_i(%s)", el);
              else if (kt2 == TY_STRING) buf_printf(b, "sp_poly_to_s(%s)", el);
              else buf_printf(b, "sp_poly_to_i(%s)", el);
            }
            buf_puts(b, ");");
            continue;
          }
          /* a key no entry matches (hash_okey_miss) deletes nothing (_okey) */
          buf_printf(b, " sp_%sHash_delete%s(_t%d, ", hn,
                     rt == TY_POLY_POLY_HASH ? "" : rt == TY_STR_POLY_HASH ? "_bytes" : hash_okey_sfx(c, argv[i], ty_hash_key(rt)), t);
          if (rt == TY_POLY_POLY_HASH) emit_boxed(c, argv[i], b); else emit_hash_key_o(c, argv[i], ty_hash_key(rt), b);
          buf_puts(b, ");");
        }
        buf_printf(b, " _t%d; })", t);
        return 1;
      }
      if (sp_streq(name, "delete") && argc == 1 && nt_ref(nt, id, "block") >= 0 &&
          (rt == TY_STR_INT_HASH || rt == TY_STR_STR_HASH || rt == TY_SYM_POLY_HASH ||
           rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH ||
           rt == TY_INT_INT_HASH || rt == TY_INT_STR_HASH)) {
        /* returns the deleted value (or nil on a miss), then removes the key */
        TyKind vt = ty_hash_val(rt);
        int th = ++g_tmp, tk = ++g_tmp, tv = ++g_tmp;
        if (nt_ref(nt, id, "block") >= 0 && hash_key_misses(c, argv[0], ty_hash_key(rt))) {
          /* the block receives the missing key, typed as the table's key kind */
          unsupported_feature(c, id, "Hash#delete with a block and a key of another class than the hash's keys");
          return 0;
        }
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        buf_printf(b, "; if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", th, th, hash_box_cls(rt));   /* (#3001) */
        buf_printf(b, " %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
        /* delete(key) { |k| fallback }: the block's value stands in for a
           missing key (boxed: the fallback can be any type). Without a
           block literal it is a row. */
        int hd_blk = nt_ref(nt, id, "block");
        const char *dp0 = block_param_name(c, hd_blk, 0);
        int hdb = nt_ref(nt, hd_blk, "body");
        int hdn = 0; const int *hdv = hdb >= 0 ? nt_arr(nt, hdb, "body", &hdn) : NULL;
        int tvv = ++g_tmp;
        buf_printf(b, "; sp_RbVal _t%d; if (sp_%sHash_has_key(_t%d, _t%d)) { _t%d = ",
                   tvv, hn, th, tk, tvv);
        { char getx[96]; snprintf(getx, sizeof getx, "sp_%sHash_get(_t%d, _t%d)", hn, th, tk);
          if (vt == TY_POLY) buf_puts(b, getx);
          else { char hs[24], ks[24]; snprintf(hs, sizeof hs, "_t%d", th); snprintf(ks, sizeof ks, "_t%d", tk); emit_hash_val_boxed(c, recv, vt, hn, hs, ks, b); } }
        buf_printf(b, "; sp_%sHash_delete%s(_t%d, _t%d); }\nelse {", hn, rt == TY_STR_POLY_HASH ? "_bytes" : "", th, tk);
        Buf dbind; memset(&dbind, 0, sizeof dbind);
        if (dp0) {
          char keytmp[32]; snprintf(keytmp, sizeof keytmp, "_t%d", tk);
          buf_printf(&dbind, "lv_%s = ", rename_local(dp0));
          if (ty_hash_key(rt) == TY_POLY) buf_puts(&dbind, keytmp);
          else emit_boxed_text(c, ty_hash_key(rt), keytmp, &dbind);
          buf_puts(&dbind, "; ");
        }
        buf_printf(b, " _t%d = ", tvv);
        emit_fallback_block_value(c, hdv, hdn, dbind.p, 1, "sp_box_nil()", 0, b);
        free(dbind.p);
        buf_puts(b, "; }");
        buf_printf(b, " _t%d; })", tvv);
        return 1;
      }
    }
  }
  return 0;
}

/* True when nodes a and b are the same side-effect-free lvalue -- the same local
   or instance variable read. Used to resolve `x.equal?(x)` (object identity) for
   receivers whose value identity is not otherwise modeled: re-reading one
   variable yields the same object, so the reflexive case is certainly true,
   while method calls / literals (which would produce fresh objects, or which the
   C compiler merges) are excluded. */
/* String#upcase and friends take an optional casemap symbol. `:ascii`
   restricts folding to A-Z/a-z; return the "_ascii" runtime suffix for it so
   non-ASCII bytes pass through. Full-Unicode folding (no arg) returns "". */
const char *case_map_suffix(Compiler *c, int argc, const int *argv) {
  if (argc >= 1 && nt_type(c->nt, argv[0]) &&
      sp_streq(nt_type(c->nt, argv[0]), "SymbolNode") &&
      nt_str(c->nt, argv[0], "value") &&
      sp_streq(nt_str(c->nt, argv[0], "value"), "ascii"))
    return "_ascii";
  return "";
}

static int same_sefree_lvalue(Compiler *c, int a, int b) {
  if (a < 0 || b < 0) return 0;
  const char *ta = nt_type(c->nt, a), *tb = nt_type(c->nt, b);
  if (!ta || !tb || !sp_streq(ta, tb)) return 0;
  if (!sp_streq(ta, "LocalVariableReadNode") && !sp_streq(ta, "InstanceVariableReadNode")) return 0;
  const char *na = nt_str(c->nt, a, "name"), *nb = nt_str(c->nt, b, "name");
  return na && nb && sp_streq(na, nb);
}

/* Does this subtree read the regexp match globals ($~, $1..$9, $&, $`, $')?
   A scan block that does needs the match registers refreshed per iteration,
   which the pre-computed rows alone do not do (#3601). */
int subtree_reads_match_globals(Compiler *c, int root) {
  if (root < 0) return 0;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, root);
  if (ty) {
    if (sp_streq(ty, "BackReferenceReadNode") || sp_streq(ty, "NumberedReferenceReadNode"))
      return 1;
    if (sp_streq(ty, "GlobalVariableReadNode")) {
      const char *gn = nt_str(nt, root, "name");
      if (gn && (sp_streq(gn, "$~") || sp_streq(gn, "$&") || sp_streq(gn, "$`") ||
                 sp_streq(gn, "$'") || sp_streq(gn, "$+")))
        return 1;
    }
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) if (subtree_reads_match_globals(c, nt_ref_at(nt, root, i))) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *el = nt_arr_at(nt, root, i, &n);
    for (int j = 0; j < n; j++) if (subtree_reads_match_globals(c, el[j])) return 1;
  }
  return 0;
}

/* A byte-offset search takes a String needle. A poly one is a String at run
   time, or the conversion protocol's TypeError -- either way emit_str_expr
   makes it a `const char *` -- so it belongs on the same arm as a static
   String rather than falling through to "no such method" (#4004's family). */
static int str_needle_p(Compiler *c, int a) {
  TyKind t = comp_ntype(c, a);
  return t == TY_STRING || t == TY_STRBUF || t == TY_POLY;
}

/* The names CRuby's nil answers: NilClass's own methods plus the Object /
   Kernel surface every object carries. Everything else on a nil receiver is a
   NoMethodError, which is what makes this a list of exceptions rather than a
   list of rules -- a name missing from here raises, the safe direction. */
/* Can this expression answer nil? call_returns_nullable_int is
   the boxing side's answer and is deliberately narrow -- widening it would put
   the nil-aware box on optcarrot's pixel path -- so a container read is asked
   here instead. A miss on a specialized Array or Hash answers the element
   type's own C nil (#4070), which is exactly the shape the receiver guard is
   for. */
int recv_may_be_sentinel(Compiler *c, int node) {
  if (node < 0) return 0;
  if (call_returns_nullable_int(c, node)) return 1;
  const NodeTable *nt = c->nt;
  const char *nty = nt_type(nt, node);
  if (!nty || !sp_streq(nty, "CallNode")) return 0;
  const char *nm = nt_str(nt, node, "name");
  if (!nm) return 0;
  int rr = nt_ref(nt, node, "receiver");
  if (rr < 0) return 0;
  TyKind rrt = comp_ntype(c, rr);
  if (!ty_is_array(rrt) && !ty_is_hash(rrt)) return 0;
  int aa = 0; (void)call_args(nt, node, &aa);
  /* fetch(k, default) and dig with a default never miss into nil */
  if (sp_streq(nm, "[]") || sp_streq(nm, "at") || sp_streq(nm, "dig") ||
      sp_streq(nm, "first") || sp_streq(nm, "last") ||
      sp_streq(nm, "find") || sp_streq(nm, "detect") ||
      (sp_streq(nm, "fetch") && aa == 1))
    return 1;
  return 0;
}

int nil_answers_name(const char *n) {
  static const char *const names[] = {
    "to_s", "inspect", "to_i", "to_f", "to_r", "to_c", "to_a", "to_h",
    "nil?", "hash", "class", "object_id", "frozen?", "dup", "clone", "freeze",
    "itself", "tap", "then", "yield_self", "display",
    "==", "!=", "===", "eql?", "equal?", "=~", "!",
    "is_a?", "kind_of?", "instance_of?", "respond_to?",
    "&", "|", "^",
    "send", "__send__", "public_send", "method", "methods",
    "instance_variables", "instance_variable_get", "instance_variable_set",
    "instance_variable_defined?", "singleton_class", "define_singleton_method",
    "extend", "enum_for", "to_enum", "pretty_print",
  };
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
    if (sp_streq(n, names[i])) return 1;
  return 0;
}

/* nullable_scalar_nil_only_call's names: a slot whose nil flag is set is
   nil, so it answers nil's value, and any other value raises the receiver
   class's NoMethodError the gate below would have. */
int emit_nullable_scalar_nil_only(Compiler *c, int id, Buf *b) {
  if (!nullable_scalar_nil_only_call(c, id)) return 0;
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  TyKind rt = repr_of(c, recv).as_ty;
  int argc = 0; const int *argv = call_args(nt, id, &argc);
  TyKind ct = repr_of(c, id).as_ty;
  const char *ans = sp_streq(nm, "to_a") ? "sp_box_poly_array(sp_PolyArray_new())"
                  : sp_streq(nm, "to_h") ? "sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH)"
                  : sp_streq(nm, "=~") ? "sp_box_nil()"
                  : sp_streq(nm, "!~") ? "1"
                  : NULL;
  /* the Float's boolean operators: `&` is false, `|` and `^` the argument's truth */
  int bool_op = ans ? 0 : sp_streq(nm, "&") ? 1 : 2;
  if (ct != (sp_streq(nm, "!~") ? TY_BOOL : TY_POLY)) return 0;
  int tr = ++g_tmp, ta = -1;
  buf_printf(b, "({ %s _t%d = (", oint_ctype(rt), tr);
  emit_oint_expr(c, recv, rt, b);
  buf_puts(b, "); ");
  if (argc == 1) {
    /* the argument after the receiver, setup and all (emit_split_pre) */
    Buf ap; memset(&ap, 0, sizeof ap);
    Buf av; memset(&av, 0, sizeof av);
    emit_split_pre(c, argv[0], emit_boxed, &ap, &av);
    ta = ++g_tmp;
    buf_printf(b, "%ssp_RbVal _t%d = %s; (void)_t%d; ", ap.p ? ap.p : "", ta, av.p ? av.p : "sp_box_nil()", ta);
    free(ap.p); free(av.p);
  }
  buf_printf(b, "_t%d.nil ? ", tr);
  if (bool_op == 1) buf_puts(b, "sp_box_bool(0)");
  else if (bool_op == 2) buf_printf(b, "sp_box_bool(sp_poly_truthy(_t%d))", ta);
  else buf_puts(b, ans);
  /* Object#!~ is =~ negated, and it is =~ that the number lacks */
  buf_printf(b, " : (sp_raise_nomethod(sp_nomethod_msg(\"%s\", %s(_t%d))), %s); })",
             sp_streq(nm, "!~") ? "=~" : nm, oint_box(rt), tr,
             ct == TY_BOOL ? "0" : "sp_box_nil()");
  return 1;
}

/* A String slot of the pattern family (String#split's separator): a
   String or nil passes as the String slot does, but a value of any other
   class -- true and false included -- is CRuby's "wrong argument type X
   (expected Regexp)", not the implicit-conversion wording. A Regexp is the
   one class the family is not wrong about, and it belongs to the arm's own
   Regexp path, never here. */
void emit_str_pattern_expr(Compiler *c, int node, Buf *b) {
  TyKind t = comp_ntype(c, node);
  if (t == TY_REGEX) unsupported_feature(c, node, "a Regexp separator reached the String pattern slot");
  if (t == TY_BOOL) {
    int tb = ++g_tmp;
    buf_printf(b, "({ int _t%d = (", tb); emit_expr(c, node, b);
    buf_printf(b, "); sp_raise_cls(\"TypeError\", _t%d"
                  " ? \"wrong argument type true (expected Regexp)\""
                  " : \"wrong argument type false (expected Regexp)\");"
                  " (const char *)0; })", tb);
    return;
  }
  const char *cn = conv_wrong_cls_name(t);
  if (cn && t != TY_STRING && t != TY_STRBUF) {
    buf_puts(b, "({ (void)("); emit_expr(c, node, b);
    buf_printf(b, "); sp_raise_cls(\"TypeError\", \"wrong argument type %s (expected Regexp)\"); (const char *)0; })", cn);
    return;
  }
  /* nil is split's documented whitespace mode: evaluate for side effects and
     pass NULL, which every sp_str_split_* entry answers as that mode (#4223).
     A separator that is only nil AT RUN TIME (a poly slot) has to keep the
     same answer, where the loose string conversion below renders nil as ""
     and silently turns the call into a character split; a non-nil non-string
     in that slot still raises through the strict conversion. */
  if (t == TY_NIL) {
    buf_puts(b, "({ (void)("); emit_expr(c, node, b);
    buf_puts(b, "); (const char *)NULL; })");
    return;
  }
  if (t == TY_POLY || t == TY_UNKNOWN) {
    buf_puts(b, "sp_poly_arg_str_or_null(");
    emit_boxed(c, node, b);
    buf_puts(b, ")");
    return;
  }
  emit_str_expr_nilable(c, node, b);
}

static void emit_strbuf_force_encoding(Compiler *c, const char *name, const char *ref, int hr, const int *argv, int argc, Buf *b);
static int emit_scalar_call_arms(Compiler *c, int id, Buf *b);
/* The arms evaluate a scalar receiver into text before they look at the
   method name, and its prelude (`Foo.new` hoisted into a temp for
   `Foo.new.v.zork`) lands in g_pre then. When no arm takes the call, the
   arm that does (the NoMethodError gate among them) emits the receiver
   again, so that prelude is dropped here: left in place it ran the
   receiver's inner call a second time. */
/* String#squeeze / #delete / #count over several character sets: every
   set is a String, handed over as one C array (a String chain row) */
int emit_op_str_set_n(Compiler *c, const BopCtx *x, Buf *b) {
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  if (!x->rtext) return 0;
  int literals = 1;
  for (int a = 0; a < argc; a++)
    if (nt_kind(c->nt, argv[a]) != NK_StringNode) literals = 0;
  if (literals) {
    buf_printf(b, "sp_str_%s_n(%s, (const char *[]){", x->name, x->rtext);
    for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_str_expr(c, argv[a], b); }
    buf_printf(b, "}, %d)", argc);
    return 1;
  }
  int tr = ++g_tmp;
  buf_printf(b, "({ const char *_t%d = %s; SP_GC_ROOT_STR(_t%d); ", tr, x->rtext, tr);
  int first = emit_rooted_arg_list(c, argv, argc, "const char *", "SP_GC_ROOT_STR", emit_str_expr, NULL, b);
  buf_printf(b, "sp_str_%s_n(_t%d, (const char *[]){", x->name, tr);
  for (int a = 0; a < argc; a++) buf_printf(b, "%s_t%d", a ? ", " : "", first + a);
  buf_printf(b, "}, %d); })", argc);
  return 1;
}

/* String#start_with? / #end_with? over several candidates: true when any
   matches, the receiver bound once (a String chain row) */
int emit_op_str_affix_any(Compiler *c, const BopCtx *x, Buf *b) {
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  if (!x->rtext) return 0;
  int tv = ++g_tmp;
  const char *fn = sp_streq(x->name, "start_with?") ? "sp_str_start_with" : "sp_str_end_with";
  buf_printf(b, "({ const char *_t%d = %s; (", tv, x->rtext);
  for (int j = 0; j < argc; j++) {
    if (j) buf_puts(b, " || ");
    buf_printf(b, "%s(_t%d, ", fn, tv);
    emit_str_expr(c, argv[j], b);
    buf_puts(b, ")");
  }
  buf_puts(b, "); })");
  return 1;
}

int emit_scalar_call(Compiler *c, int id, Buf *b) {
  Buf *pre = g_pre;
  size_t pre0 = pre ? pre->len : 0;
  int done = emit_scalar_call_arms(c, id, b);
  if (!done && pre && pre == g_pre && pre->len > pre0) {
    pre->len = pre0;
    pre->p[pre0] = 0;
  }
  return done;
}
/* scrub! mutates in place, so a frozen receiver raises -- but only when
   it would actually replace something: CRuby returns a frozen string
   with no invalid bytes unchanged (#3333, #3338). The scrubbed text goes
   back into the receiver, a shared String's buffer (a reader call's
   handle among them) or an assignable one, as the other bang methods'
   does; it only answered the scrubbed copy. */
static int emit_scrub_bang(Compiler *c, int recv, TyKind rt, int argc, const int *argv, Buf *b) {
  char srefS[1024];
  int tsc = ++g_tmp;
  Buf rpl; memset(&rpl, 0, sizeof rpl);
  if (argc == 1) emit_str_expr_nilable(c, argv[0], &rpl); else buf_puts(&rpl, "0");
  const char *rp = rpl.p ? rpl.p : "0";
  int done = 1;
  if (strbuf_slot_ref(c, recv, srefS, sizeof srefS))
    buf_printf(b, "({ sp_String *_t%d = %s; const char *_t%dr = sp_str_scrub_bang(sp_String_cstr(_t%d), %s);"
                  " if (_t%dr != sp_String_cstr(_t%d)) sp_String_set_bin(_t%d, _t%dr); _t%dr; })",
               tsc, srefS, tsc, tsc, rp, tsc, tsc, tsc, tsc, tsc);
  else if (rt != TY_STRING) done = 0;
  else if (str_mut_var_recv(c, recv) || sb_shadowed_reader(recv)) {
    buf_printf(b, "({ const char *_t%d = sp_str_scrub_bang(", tsc);
    emit_expr(c, recv, b); buf_printf(b, ", %s); ", rp);
    emit_expr(c, recv, b); buf_printf(b, " = _t%d; _t%d; })", tsc, tsc);
  }
  else {
    buf_puts(b, "sp_str_scrub_bang("); emit_expr(c, recv, b); buf_printf(b, ", %s)", rp);
  }
  free(rpl.p);
  return done;
}

/* A String receiver's conversions and comparisons: lines with keywords,
   bytes, codepoints, unpack / unpack1, chars, to_i, eql? and equal?
   (emit_scalar_recv_arms's String chain; answers 1 when a branch was taken) */
static int str_arms_convert(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind a0, const char *r) {
  /* lines(sep, chomp: true): a separator and the keyword together (#3546) */
  if (sp_streq(name, "lines") && argc == 2 &&
           comp_ntype(c, argv[0]) == TY_STRING && nt_type(nt, argv[1]) &&
           sp_streq(nt_type(nt, argv[1]), "KeywordHashNode")) {
    int chv = struct_kwarg_value(c, argv[1], "chomp");
    int isc = kw_flag_static(c, chv);
    if (isc < 0) { buf_puts(b, "("); emit_cond(c, chv, b); buf_puts(b, " ? "); }
    if (isc != 0) { buf_printf(b, "sp_str_lines_sep_chomp(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    if (isc < 0) buf_puts(b, " : ");
    if (isc != 1) { buf_printf(b, "sp_str_lines_sep(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    if (isc < 0) buf_puts(b, ")");
  }
  else if (sp_streq(name, "lines") && argc == 1 && nt_type(nt, argv[0]) &&
           sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) {
    int chomp_v = struct_kwarg_value(c, argv[0], "chomp");
    int is_chomp = kw_flag_static(c, chomp_v);
    if (is_chomp < 0) {
      buf_puts(b, "("); emit_cond(c, chomp_v, b);
      buf_printf(b, " ? sp_str_lines_chomp(%s) : sp_str_lines(%s))", r, r);
    }
    else buf_printf(b, "%s(%s)", is_chomp ? "sp_str_lines_chomp" : "sp_str_lines", r);
  }
  else if (sp_streq(name, "bytes") && argc == 0)   buf_printf(b, "sp_str_bytes(%s)", r);
  else if (sp_streq(name, "codepoints") && argc == 0) buf_printf(b, "sp_str_codepoints(%s)", r);
  /* unpack(fmt, offset: n): a trailing KeywordHashNode carries the offset. */
  else if ((sp_streq(name, "unpack") || sp_streq(name, "unpack1")) && argc == 2 &&
           nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "KeywordHashNode") &&
           struct_kwarg_value(c, argv[1], "offset") >= 0) {
    int offv = struct_kwarg_value(c, argv[1], "offset");
    int one = sp_streq(name, "unpack1");
    TyKind u1t = one ? repr_of(c, id).as_ty : TY_POLY;
    if (one && oint_kind(u1t)) { oint_open(c, id, u1t, b); buf_printf(b, "%s(sp_PolyArray_get(", oint_unbox(u1t)); }
    else if (one)                    buf_puts(b, "sp_PolyArray_get(");
    buf_printf(b, "sp_str_unpack_off(%s, ", r); emit_str_expr(c, argv[0], b);
    buf_puts(b, ", "); emit_int_expr(c, offv, b); buf_puts(b, ")");
    if (one) buf_puts(b, oint_kind(u1t) ? ", 0))" : ", 0)");
    if (one && oint_kind(u1t)) oint_close(c, id, b);
  }
  else if (sp_streq(name, "unpack1") && argc == 1) {
    /* A literal single-directive numeric format fixes the value's type
       (the analyzer's an_unpack1_lit_type): unbox the extracted element
       (int, or float? -- the _opt keeps a padded nil from short input
       as float-nil instead of 0.0). */
    TyKind u1t = repr_of(c, id).as_ty;
    if (oint_kind(u1t)) { oint_open(c, id, u1t, b); buf_printf(b, "%s(sp_PolyArray_get(sp_str_unpack(%s, ", oint_unbox(u1t), r); }
    else                buf_printf(b, "sp_PolyArray_get(sp_str_unpack(%s, ", r);
    emit_str_expr(c, argv[0], b);
    buf_puts(b, oint_kind(u1t) ? "), 0))" : "), 0)");
    if (oint_kind(u1t)) oint_close(c, id, b);
  }
  else if (sp_streq(name, "chars") && argc == 0)   buf_printf(b, "sp_str_chars(%s)", r);
  /* promote mode types the call poly: a Bignum past sp_int */
  else if (sp_streq(name, "to_i") && argc <= 1 && repr_of(c, id).kind == RK_BOXED) {
    buf_printf(b, "sp_str_to_i_promote(%s, ", r);
    if (argc == 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "-1");
    buf_puts(b, ", 0)");
  }
  else if (sp_streq(name, "to_i") && argc == 0)    buf_printf(b, "sp_str_to_i_cruby(%s)", r);
  else if (sp_streq(name, "to_i") && argc == 1)    { buf_printf(b, "sp_str_to_i_base(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
  /* String#eql?(x): byte-equal only when x is itself String-typed (no
     coercion, unlike ==). A poly arg checks its tag; any other concrete
     type is never equal. */
  else if (sp_streq(name, "eql?") && argc == 1) {
    /* the receiver may be a fresh copy (a shared slot's read): rooted
       when the argument, evaluated beside it, may allocate -- as == does */
    if (a0 == TY_STRING && operand_may_allocate(c, argv[0])) {
      int te = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = %s; SP_GC_ROOT(_t%d); sp_str_eq(_t%d, ", te, r, te, te);
      emit_expr(c, argv[0], b); buf_puts(b, "); })");
    }
    else if (a0 == TY_STRING) { buf_printf(b, "sp_str_eq(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (a0 == TY_POLY) {
      /* a boxed shared String handle is a String too: read its text. The
         receiver is bound first, as Ruby evaluates it: rendered after the
         argument, a shared slot's copy allocated while the argument's
         fresh String sat in an unrooted temp. It is rooted when the
         argument may allocate. */
      int te = ++g_tmp, trc = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = %s; ", trc, r);
      if (operand_may_allocate(c, argv[0])) buf_printf(b, "SP_GC_ROOT(_t%d); ", trc);
      buf_printf(b, "sp_RbVal _t%d = sp_poly_strbuf_deref(", te); emit_boxed(c, argv[0], b);
      buf_printf(b, "); _t%d.tag == SP_TAG_STR && sp_str_eq(_t%d.v.s, _t%d); })", te, te, trc);
    }
    else { buf_printf(b, "((void)(%s), (void)(", r); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
  }
  /* String#equal?(x): object identity. A String is a `const char *` whose
     literals the C compiler merges at -O2, so raw pointer equality would
     wrongly equate distinct equal-valued literals (`a = "x"; b = "x"`).
     Only the unambiguous reflexive case -- the same side-effect-free local
     or ivar read on both sides (`x.equal?(x)`) -- is certainly identity-
     true; every other form is conservatively false, still evaluating the
     argument for its side effects. */
  else if (sp_streq(name, "equal?") && argc == 1) {
    TyKind eqa = comp_ntype(c, argv[0]);
    if (string_identity_inline(c, recv, argv[0])) {
      emit_string_identity(c, recv, argv[0], b);
      return 1;
    }
    /* A narrowed box keeps its identity in the original box, whether the
       payload is a plain String or a shared handle. */
    if (repr_share_rule(c) && (repr_of(c, recv).strbuf_src == RS_SLOT_POLY ||
                             repr_of(c, argv[0]).strbuf_src == RS_SLOT_POLY)) {
      buf_puts(b, "sp_poly_equal("); emit_boxed(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* a mutable StringBuffer local as the argument: compare the buffer's
       OWN cstr pointer -- the plain read emits a defensive snapshot copy
       (sp_str_concat(cstr, "")), which would break `(s << "x").equal?(s)`
       (#2307). Hoist the receiver first so its in-place append lands
       before the argument's cstr is read. */
    int eq_sblv = 0;
    /* a demand-marked reader-call argument already emits the handle */
    if (!eq_sblv && repr_of(c, argv[0]).as_ty == TY_STRBUF &&
        nt_kind(nt, argv[0]) == NK_CallNode) {
      char rrefE2[192];
      if (strbuf_slot_ref(c, recv, rrefE2, sizeof rrefE2)) {
        buf_printf(b, "(%s == ", rrefE2);
        emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        eq_sblv = 1;
      }
    }
    /* strbuf receiver vs a POLY operand (a container read): runtime
       handle identity against the boxed value (#3227 P6) */
    if (!eq_sblv && comp_ntype(c, argv[0]) == TY_POLY) {
      char rrefE3[192];
      if (strbuf_slot_ref(c, recv, rrefE3, sizeof rrefE3)) {
        int teq3 = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", teq3);
        emit_boxed(c, argv[0], b);
        if (repr_share_rule(c))
          buf_printf(b, "; sp_poly_equal(sp_box_nullable_obj(%s, SP_BUILTIN_STRBUF), _t%d); })", rrefE3, teq3);
        else
          buf_printf(b, "; (sp_bool)(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_STRBUF"
                        " && (sp_String *)_t%d.v.p == %s); })",
                     teq3, teq3, teq3, rrefE3);
        eq_sblv = 1;
      }
    }
    /* `String(s)` or `+s` against a handle, either side: the String the
       route answers is s's own, so the handles are compared (the route's
       read face is a copy). The other side is a variable's slot, and the
       route's operand is one: nothing runs but the reads. */
    if (!eq_sblv) {
      char hx[192], hy[192];
      int up = 0;
      int fwd = strbuf_self_route_slot(c, recv, &up, hx, sizeof hx) && strbuf_slot_ref(c, argv[0], hy, sizeof hy);
      if (!fwd) fwd = -(strbuf_self_route_slot(c, argv[0], &up, hx, sizeof hx) && strbuf_slot_ref(c, recv, hy, sizeof hy));
      if (fwd) {
        int th = ++g_tmp;
        buf_printf(b, "({ sp_String *_t%d = %s; ", th, hx);
        /* +nil raises, String(nil) is "" (a new String) */
        if (up) buf_printf(b, "if (SP_UNLIKELY(!_t%d)) sp_raise_nomethod(sp_nomethod_msg(\"+@\", sp_box_nil())); "
                              "(sp_bool)(sp_String_uplus(_t%d) == %s); })", th, th, hy);
        /* A route whose answer can be nil keeps nil's identity too. */
        else if (repr_of(c, fwd > 0 ? recv : argv[0]).may_nil)
          buf_printf(b, "(sp_bool)(_t%d == %s); })", th, hy);
        else buf_printf(b, "(sp_bool)(_t%d && _t%d == %s); })", th, th, hy);
        eq_sblv = 1;
      }
    }
    /* --share-strings: a String Range's endpoint (strbuf_route_srange_end)
       against a handle: the handle the Range keeps is compared (#8321) */
    if (!eq_sblv) {
      char sx[192];
      int rse = strbuf_route_srange_end(c, recv) && strbuf_slot_ref(c, argv[0], sx, sizeof sx);
      int ase = !rse && strbuf_route_srange_end(c, argv[0]) && strbuf_slot_ref(c, recv, sx, sizeof sx);
      if (rse || ase) {
        int th = ++g_tmp;
        buf_printf(b, "({ sp_String *_t%d = ", th);
        emit_strbuf_route(c, rse ? recv : argv[0], b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); (sp_bool)(_t%d == %s); })", th, th, sx);
        eq_sblv = 1;
      }
    }
    if (!eq_sblv) {
      char arefE[192];
      if (strbuf_slot_ref(c, argv[0], arefE, sizeof arefE)) {
        /* If the receiver is ALSO a strbuf slot (local or ivar), compare
           the two sp_String handles directly: a shared alias is one
           object, so `s1.equal?(s2)` is true (#3227). Otherwise `r` is a
           live-buffer expr (e.g. `(s << "x")`) and its cstr is compared. */
        char rrefE[192];
        if (strbuf_slot_ref(c, recv, rrefE, sizeof rrefE))
          buf_printf(b, "(%s == %s)", rrefE, arefE);
        else {
          int teq = ++g_tmp;
          buf_printf(b, "({ const char *_t%d = %s; "
                        "(const void *)_t%d == (const void *)sp_String_cstr(%s); })",
                     teq, r, teq, arefE);
        }
        eq_sblv = 1;
      }
    }
    if (eq_sblv) { /* emitted above */ }
    else if (eqa == TY_STRING) {
      /* string identity IS pointer identity (s.freeze.equal?(s) must be
         true: freeze marks in place and returns the same pointer). Under
         --share-strings a side that is a handle answers its identity
         (sp_String_identity): a frozen literal's handle is its bytes' */
      int hr = repr_share_rule(c) && repr_of(c, recv).as_ty == TY_STRBUF;
      int ha = repr_share_rule(c) && repr_of(c, argv[0]).as_ty == TY_STRBUF;
      buf_printf(b, "((const void *)%s(%s) == (const void *)%s(", hr ? "sp_String_identity" : "", r,
                 ha ? "sp_String_identity" : "");
      emit_expr(c, argv[0], b);
      buf_puts(b, "))");
    }
    else if (eqa == TY_POLY) {
      /* a boxed operand can hold this very String: its payload is the same
         pointer, as an element read of it is (`[s, 1][0]`). Under
         --share-strings it can hold a frozen literal's handle, which the
         boxed comparison reads as the literal's bytes. */
      int trq = ++g_tmp, teq = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = %s; sp_RbVal _t%d = ", trq, r, teq);
      emit_boxed(c, argv[0], b);
      if (repr_share_rule(c))
        buf_printf(b, "; sp_poly_equal(sp_box_str(_t%d), _t%d); })", trq, teq);
      else
        buf_printf(b, "; (sp_bool)(_t%d.tag == SP_TAG_STR && (const void *)_t%d.v.s == (const void *)_t%d); })",
                   teq, teq, trq);
    }
    else if (same_sefree_lvalue(c, recv, argv[0])) { buf_puts(b, "(("); emit_expr(c, argv[0], b); buf_puts(b, "), 1)"); }
    /* another kind is another object; the receiver and the argument
       still run */
    else { buf_printf(b, "((void)(%s), (void)(", r); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
  }
  else return 0;
  return 1;
}

/* A String receiver's slicing and its byte and encoding methods: [] / slice
   by regexp, range, index or substring, split, clamp, force_encoding /
   encode!, =~ / !~ against nil, encode, casecmp / casecmp?, setbyte and
   getbyte (emit_scalar_recv_arms's String chain; answers 1 when a branch
   was taken) */
static int str_arms_slice_encode(Compiler *c, int id, Buf *b, const char *name, int recv, int argc, const int *argv, const char *r) {
  if ((is_slice_alias(name)) && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
    /* s[/re/] -> the matched substring, or nil (NULL) on no match */
    buf_printf(b, "(sp_re_match(sp_re_pat_%d, %s) >= 0 ? sp_re_match_str : NULL)", re_lit_index(c, argv[0]), r);
  }
  else if ((is_slice_alias(name)) && argc == 2 && re_lit_index(c, argv[0]) >= 0 &&
           nt_type(c->nt, argv[1]) &&
           (sp_streq(nt_type(c->nt, argv[1]), "SymbolNode") ||
            sp_streq(nt_type(c->nt, argv[1]), "StringNode") ||
            comp_ntype(c, argv[1]) == TY_STRING)) {
    /* s[/(?<g>...)/, :g] or s[/(?<g>...)/, "g"] -> the named group, or nil (#3082) */
    int pi = re_lit_index(c, argv[0]);
    const char *nty = nt_type(c->nt, argv[1]);
    if (sp_streq(nty, "SymbolNode")) {
      const char *gname = nt_str(c->nt, argv[1], "value");
      buf_printf(b, "(sp_re_match(sp_re_pat_%d, %s) >= 0 ? sp_re_named_capture(sp_re_pat_%d, \"%s\") : NULL)",
                 pi, r, pi, gname ? gname : "");
    }
    else {
      /* a String name (literal or dynamic): evaluate it and look it up */
      int tnm = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", tnm); emit_str_expr(c, argv[1], b);
      buf_printf(b, "; sp_re_match(sp_re_pat_%d, %s) >= 0 ? sp_re_named_capture(sp_re_pat_%d, _t%d) : NULL; })",
                 pi, r, pi, tnm);
    }
  }
  else if ((is_slice_alias(name)) && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
    /* s[/re/, n] -> capture group n (0 = whole match), or nil */
    int pi = re_lit_index(c, argv[0]);
    int tn = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = ", tn); emit_int_expr(c, argv[1], b);
    buf_printf(b, "; sp_re_match(sp_re_pat_%d, %s) >= 0 ? "
                  "(_t%d == 0 ? sp_re_match_str : (_t%d >= 1 && _t%d <= 9 ? sp_re_captures[_t%d] : NULL)) : NULL; })",
               pi, r, tn, tn, tn, tn);
  }
  /* The same three forms with the Regexp arriving as a VALUE -- a
     parameter, a constant, a local -- whose class the type already
     says. They went to the integer slice arms, where the Regexp operand
     was a hard TypeError (#4457: the useragent port's `c[pattern, 0]`,
     on every request through campfire's browser gate). */
  else if ((is_slice_alias(name)) && argc == 1 &&
           comp_ntype(c, argv[0]) == TY_REGEX) {
    int tp = ++g_tmp;
    buf_printf(b, "({ mrb_regexp_pattern *_t%d = ", tp); emit_expr(c, argv[0], b);
    buf_printf(b, "; _t%d && sp_re_match(_t%d, %s) >= 0 ? sp_re_match_str : NULL; })", tp, tp, r);
  }
  else if ((is_slice_alias(name)) && argc == 2 &&
           comp_ntype(c, argv[0]) == TY_REGEX && nt_type(c->nt, argv[1]) &&
           (sp_streq(nt_type(c->nt, argv[1]), "SymbolNode") ||
            sp_streq(nt_type(c->nt, argv[1]), "StringNode") ||
            comp_ntype(c, argv[1]) == TY_STRING)) {
    int tp = ++g_tmp, tnm = ++g_tmp;
    const char *nty = nt_type(c->nt, argv[1]);
    buf_printf(b, "({ mrb_regexp_pattern *_t%d = ", tp); emit_expr(c, argv[0], b);
    buf_printf(b, "; const char *_t%d = ", tnm);
    if (sp_streq(nty, "SymbolNode")) buf_printf(b, "\"%s\"", nt_str(c->nt, argv[1], "value") ? nt_str(c->nt, argv[1], "value") : "");
    else emit_str_expr(c, argv[1], b);
    buf_printf(b, "; _t%d && sp_re_match(_t%d, %s) >= 0 ? sp_re_named_capture(_t%d, _t%d) : NULL; })",
               tp, tp, r, tp, tnm);
  }
  else if ((is_slice_alias(name)) && argc == 2 &&
           comp_ntype(c, argv[0]) == TY_REGEX) {
    int tp = ++g_tmp, tn = ++g_tmp;
    buf_printf(b, "({ mrb_regexp_pattern *_t%d = ", tp); emit_expr(c, argv[0], b);
    buf_printf(b, "; sp_int _t%d = ", tn); emit_int_expr(c, argv[1], b);
    buf_printf(b, "; _t%d && sp_re_match(_t%d, %s) >= 0 ? "
                  "(_t%d == 0 ? sp_re_match_str : (_t%d >= 1 && _t%d <= 9 ? sp_re_captures[_t%d] : NULL)) : NULL; })",
               tp, tp, r, tn, tn, tn, tn);
  }
  else if ((is_slice_alias(name)) && argc == 1 &&
           comp_ntype(c, argv[0]) == TY_RANGE &&
           !(nt_type(c->nt, argv[0]) && sp_streq(nt_type(c->nt, argv[0]), "RangeNode"))) {
    /* a Range VALUE (variable / expression): slice through the runtime
       bounds (the literal form keeps its specialized arm below) */
    int trg2 = ++g_tmp;
    buf_printf(b, "({ sp_Range _t%d = sp_range_ix(", trg2); emit_expr(c, argv[0], b); buf_puts(b, ")");
    buf_printf(b, "; sp_str_sub_range_r(%s, _t%d.first, _t%d.last, (int)_t%d.excl); })",
               r, trg2, trg2, trg2);
  }
  else if ((is_slice_alias(name)) && argc == 1 && nt_type(c->nt, argv[0]) &&
           sp_streq(nt_type(c->nt, argv[0]), "RangeNode")) {
    /* s[a..b] / s[a...b]; beginless/endless ranges use 0 / length */
    int rn = argv[0];
    int excl = (int)(nt_int(c->nt, rn, "flags", 0) & 4) ? 1 : 0;
    int lo = nt_ref(c->nt, rn, "left"), hi = nt_ref(c->nt, rn, "right");
    /* the end may read the receiver's length (an endless Range, or a nil
       end), so the receiver is bound once: evaluated twice, a receiver
       with a side effect ran twice */
    int trs = ++g_tmp;
    char none_hi[64];
    snprintf(none_hi, sizeof none_hi, "(sp_int)sp_str_length(_t%d)", trs);
    buf_printf(b, "({ const char *_t%d = %s; SP_GC_ROOT_STR(_t%d); sp_str_sub_range_r(_t%d, ",
               trs, r, trs, trs);
    if (lo >= 0) emit_int_expr_bound(c, lo, "0", b); else buf_puts(b, "0");
    buf_puts(b, ", ");
    if (hi >= 0) { emit_int_expr_bound(c, hi, none_hi, b); buf_printf(b, ", %d); })", excl); }
    else buf_printf(b, "%s, 0); })", none_hi);  /* endless: to the end */
  }
  else if ((is_slice_alias(name)) && argc == 2) {
    /* s[start, len] */
    buf_printf(b, "sp_str_sub_range(%s, ", r);
    emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
  }
  else if ((is_slice_alias(name)) && argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
    /* s["sub"] -> the substring if present, else nil */
    int tsub = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = ", tsub); emit_str_expr(c, argv[0], b);
    buf_printf(b, "; (strstr(%s, _t%d) ? sp_str_dup(_t%d) : NULL); })", r, tsub, tsub);
  }
  else if ((is_slice_alias(name)) && argc == 1) {
    buf_printf(b, "sp_str_char_at_or_nil(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "split") && argc == 0) buf_printf(b, "sp_str_split_ws(%s)", r);
  else if (sp_streq(name, "split") && argc == 1) {
    /* split(nil) and split(" ") are whitespace-mode; split(sep) drops trailing empties */
    const char *aty = nt_type(c->nt, argv[0]);
    int nil_arg = aty && sp_streq(aty, "NilNode");
    int ws = nil_arg || (aty && sp_streq(aty, "StringNode") && nt_str(c->nt, argv[0], "content") &&
             sp_streq(nt_str(c->nt, argv[0], "content"), " ") && nt_str_len(c->nt, argv[0], "content") == 1);
    if (ws) buf_printf(b, "sp_str_split_ws(%s)", r);
    else { buf_printf(b, "sp_str_split_drop_trailing(%s, ", r); emit_str_pattern_expr(c, argv[0], b); buf_puts(b, ")"); }
  }
  else if (sp_streq(name, "split") && argc == 2) {
    buf_printf(b, "sp_str_split_limit(%s, ", r); emit_str_pattern_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
  }
  /* a bound of run-time type (boxed) is compared by the runtime's <=>: a
     String one by bytes, anything else is CRuby's comparison error. The
     plain arm below declared it a C string, and it did not compile. */
  else if (sp_streq(name, "clamp") && argc == 2 &&
           (comp_ntype(c, argv[0]) == TY_POLY || comp_ntype(c, argv[1]) == TY_POLY)) {
    Buf cb; memset(&cb, 0, sizeof cb);
    buf_printf(&cb, "sp_obj_clamp(sp_box_str(%s), ", r);
    emit_boxed(c, argv[0], &cb); buf_puts(&cb, ", ");
    emit_boxed(c, argv[1], &cb); buf_puts(&cb, ")");
    if (repr_of(c, id).kind == RK_BOXED) buf_puts(b, cb.p);
    else emit_unbox_text(c, TY_STRING, cb.p, b);
    free(cb.p);
  }
  else if (sp_streq(name, "clamp") && argc == 2 &&
           ((str_cmp_bound_foreign(c, argv[0]) && comp_ntype(c, argv[0]) != TY_NIL && !ty_is_object(comp_ntype(c, argv[0]))) ||
            (str_cmp_bound_foreign(c, argv[1]) && comp_ntype(c, argv[1]) != TY_NIL && !ty_is_object(comp_ntype(c, argv[1]))))) {
    int tc = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = %s; SP_GC_ROOT_STR(_t%d); sp_RbVal _t%d = ", tc, r, tc, tlo);
    emit_boxed(c, argv[0], b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_RbVal _t%d = ", tlo, thi);
    emit_boxed(c, argv[1], b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); if (_t%d.tag != SP_TAG_NIL && _t%d.tag != SP_TAG_NIL && !sp_poly_cmp_le0(_t%d, _t%d))"
                  " sp_raise_cls(\"ArgumentError\", \"min argument must be less than or equal to max argument\");"
                  " if (_t%d.tag != SP_TAG_NIL) (void)sp_poly_cmp_ge0(sp_box_str(_t%d), _t%d);"
                  " if (_t%d.tag != SP_TAG_NIL) (void)sp_poly_cmp_le0(sp_box_str(_t%d), _t%d); _t%d; })",
               thi, tlo, thi, tlo, thi, tlo, tc, tlo, thi, tc, thi, tc);
  }
  else if (sp_streq(name, "clamp") && (argc == 2 ||
           (argc == 1 && nt_type(c->nt, argv[0]) && sp_streq(nt_type(c->nt, argv[0]), "RangeNode")) ||
           (argc == 1 && comp_ntype(c, argv[0]) == TY_STR_RANGE))) {
    int ops[3];
    if (strbuf_route_clamp(c, id, ops)) {
      /* Publish the selected handle for a method's return pickup too. */
      int demand = repr_of(c, id).demand;
      if (!demand) buf_puts(b, "sp_strbuf_read_pub(");
      emit_strbuf_route(c, id, b);
      if (!demand) buf_puts(b, ")");
      return 1;
    }
    /* a Range held in a variable (or any other String Range value) reads
       its endpoints off the value, where only a literal's were taken */
    int rvar = argc == 1 && !sp_streq(nt_type(c->nt, argv[0]), "RangeNode");
    int lo_n = -1, hi_n = -1;
    if (argc == 2) { lo_n = argv[0]; hi_n = argv[1]; }
    else if (!rvar) { int rn = argv[0]; lo_n = nt_ref(c->nt, rn, "left"); hi_n = nt_ref(c->nt, rn, "right"); }
    /* an exclusive Range with an end has no greatest member to clamp to,
       and a two-argument min above max is out of order: both raise (#3593).
       An endless one clamps from below, exclusive or not. */
    int excl_r = (argc == 1 && !rvar && (nt_int(c->nt, argv[0], "flags", 0) & 4)) ? 1 : 0;
    int tc = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp, trg = rvar ? ++g_tmp : 0;
    buf_printf(b, "({ const char *_t%d = %s; ", tc, r);
    if (rvar) {
      buf_printf(b, "SP_GC_ROOT_STR(_t%d); sp_StrRange _t%d = ", tc, trg); emit_expr(c, argv[0], b);
      buf_printf(b, "; const char *_t%d = _t%d.first; const char *_t%d = _t%d.last;", tlo, trg, thi, trg);
      buf_printf(b, " if (_t%d.excl && _t%d)", trg, thi);
      buf_puts(b, " sp_raise_cls(\"ArgumentError\", \"cannot clamp with an exclusive range\");");
    }
    else {
      buf_printf(b, "const char *_t%d = ", tlo);
      if (lo_n >= 0) emit_expr(c, lo_n, b); else buf_puts(b, "NULL");
      buf_printf(b, "; const char *_t%d = ", thi);
      if (hi_n >= 0) emit_expr(c, hi_n, b); else buf_puts(b, "NULL");
      buf_puts(b, ";");
      if (excl_r)
        buf_printf(b, " if (_t%d) sp_raise_cls(\"ArgumentError\", \"cannot clamp with an exclusive range\");", thi);
    }
    buf_printf(b, " if (_t%d && _t%d && sp_str_cmp_bytes(_t%d, _t%d) > 0)"
                  " sp_raise_cls(\"ArgumentError\", \"min argument must be less than or equal to max argument\");",
               tlo, thi, tlo, thi);
    /* a one-sided Range clamps on the side it has (#3593) */
    buf_printf(b, " (_t%d && sp_str_cmp_bytes(_t%d, _t%d) < 0) ? _t%d :"
                  " ((_t%d && sp_str_cmp_bytes(_t%d, _t%d) > 0) ? _t%d : _t%d); })",
               tlo, tc, tlo, tlo, thi, tc, thi, thi, tc);
  }
  /* force_encoding / encode! set state ON the receiver: CRuby raises on a
     frozen string whether or not the call would change anything (#3334).
     `b` and non-bang `encode` return a NEW string, so they never raise. */
  else if ((is_encoding_mutator(name)) && argc <= 2) {
    char feref[1024];
    int hr = strbuf_recv_handle(c, id, recv, feref, sizeof feref);
    if (hr) emit_strbuf_force_encoding(c, name, feref, hr, argv, argc, b);
    else emit_str_force_encoding(c, name, r, argv, argc, b);
  }
  else if ((is_match_operator(name)) && argc == 1 &&
           comp_ntype(c, argv[0]) == TY_NIL) {
    /* `str =~ nil` is nil in CRuby (and `!~` its negation), not a missing
       method; the operand still evaluates (it can be a nil-typed call) */
    buf_printf(b, "((void)(%s), (void)(", r);
    emit_expr(c, argv[0], b);
    if (sp_streq(name, "!~")) buf_puts(b, "), (sp_bool)1)");
    else if (repr_of(c, id).kind == RK_BOXED) buf_puts(b, "), sp_box_nil())");
    else buf_printf(b, "), %s)", raise_tail_value(repr_of(c, id).as_ty));
  }
  /* encode with no argument is the receiver; with a destination it is a
     transcode between the two encodings the runtime models (#4439) */
  else if (sp_streq(name, "encode") && argc <= 3) emit_str_encode_call(c, r, argv, argc, b);
  /* an operand whose class answers #to_str: CRuby converts it and
     compares, where the arm below discarded it and answered nil. The
     answer is boxed because the conversion can still come back empty --
     a #to_str that answers nil is CRuby's nil casecmp, not a comparison
     with "" -- so the call is typed TY_POLY, as it is for a poly operand
     above (analyze_infer.c, analyze_infer_recv.c). */
  else if ((is_casecmp_family(name)) && argc == 1 &&
           str_cmp_conv_shape(c, argv[0])) {
    int tr, to, ts;
    emit_str_cmp_prologue(c, r, argv[0], &tr, &to, &ts, b);
    if (sp_streq(name, "casecmp"))
      buf_printf(b, "sp_box_int(sp_str_casecmp(_t%d, _t%d))", tr, ts);
    else
      buf_printf(b, "sp_box_bool(sp_str_casecmp_p(_t%d, _t%d))", tr, ts);
    buf_puts(b, " : sp_box_nil(); })");
  }
  else if ((is_casecmp_family(name)) && argc == 1 &&
           comp_ntype(c, argv[0]) != TY_STRING && !repr_of(c, argv[0]).untyped) {
    /* statically non-string argument: nil (the call typed TY_NIL); the
       argument still evaluates for effect */
    buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
  }
  else if (sp_streq(name, "setbyte") && argc == 2) {
    /* copy-on-write: rebind an lvalue receiver to the mutated copy
       (a literal's bytes live in static storage, #2029) */
    int lvw = str_mut_var_recv(c, recv) || sb_shadowed_reader(recv);
    int tv2 = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = ", tv2); emit_int_expr(c, argv[1], b);
    buf_puts(b, "; ");
    if (lvw) { emit_expr(c, recv, b); buf_puts(b, " = "); }
    buf_printf(b, "sp_str_setbyte_cow(%s, ", r); emit_int_expr(c, argv[0], b);
    buf_printf(b, ", _t%d); _t%d; })", tv2, tv2);
  }
  else if (sp_streq(name, "getbyte") && argc == 1) {
    /* Bounds/negative-correct: a negative index counts from the end and an
       out-of-range index is nil -- getbyte answers an sp_oint.
       A String whose bytes the loop being emitted holds (hc_string) reads
       an index in range there and takes this call for anything else. */
    char hd[48], hl[48];
    if (hc_string(c, recv, hd, hl, sizeof hd)) {
      int tk = ++g_tmp;
      buf_printf(b, "({ sp_int _t%d = ", tk); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; SP_LIKELY((unsigned long long)_t%d < (unsigned long long)%s) ? sp_oint_of((sp_int)(unsigned char)%s[_t%d]) : sp_str_getbyte_opt(%s, _t%d); })",
                 tk, hl, hd, tk, r, tk);
    }
    else { buf_printf(b, "sp_str_getbyte_opt(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
  }
  else return 0;
  return 1;
}

/* A String receiver's length, case mapping (upcase, downcase, capitalize,
   swapcase), chomp, dup / clone, and its searches: start_with?, index /
   rindex, byteindex / byterindex, partition / rpartition, scrub
   (emit_scalar_recv_arms's String chain; answers 1 when a branch was taken) */
static int str_arms_case_search(Compiler *c, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, const char *r);
/* A partition's middle element is its String separator on a match. The
   byte splitter supplies fresh outer pieces; its empty middle on a miss
   must remain a new String, including for an empty receiver. */
static int str_partition_shared(Compiler *c, int id, Buf *b, const char *name, int recv,
                                int argc, const int *argv) {
  if (!repr_share_rule(c) || argc != 1 || !is_partition_family(name) ||
      repr_of(c, id).elem != TY_POLY) return 0;
  TyKind at = comp_ntype(c, argv[0]);
  if (at == TY_STRBUF) at = TY_STRING;
  if (at != TY_STRING && at != TY_REGEX) return 0;
  int tr = ++g_tmp, ts = ++g_tmp, tp = ++g_tmp, to = ++g_tmp;
  buf_puts(b, "({ ");
  tr = hold_operand(c, recv, TY_STRING, 0, tr, 1, " ", b);
  if (at == TY_STRING) ts = hold_operand(c, argv[0], TY_POLY, 1, ts, 1, " ", b);
  buf_printf(b, "sp_StrArray *_t%d = ", tp);
  if (at == TY_STRING) buf_printf(b, "sp_str_%s(_t%d, sp_poly_arg_str_or_null(_t%d))", name, tr, ts);
  else {
    char r[24]; snprintf(r, sizeof r, "_t%d", tr);
    str_arms_case_search(c, b, c->nt, name, recv, argc, argv, r);
  }
  buf_printf(b, "; SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); "
                "for (int i = 0; i < 3; i++) sp_PolyArray_push(_t%d, sp_box_obj(sp_String_new_shared(sp_StrArray_get(_t%d, i)), SP_BUILTIN_STRBUF)); ",
             tp, to, to, to, tp);
  if (at == TY_STRING)
    buf_printf(b, "if (sp_str_byte_len(sp_poly_arg_str_or_null(_t%d)) == 0 || sp_str_byte_len(sp_StrArray_get(_t%d, 1)) > 0) "
                  "sp_PolyArray_set(_t%d, 1, _t%d); ", ts, tp, to, ts);
  buf_printf(b, "_t%d; })", to);
  return 1;
}

static int str_arms_case_search(Compiler *c, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, const char *r) {
  if (is_len_alias(name)) {
    if (g_hoist_len_var && g_hoist_len_recv && recv >= 0 && nt_type(nt, recv) &&
        sp_streq(nt_type(nt, recv), "LocalVariableReadNode") && nt_str(nt, recv, "name") &&
        sp_streq(nt_str(nt, recv, "name"), g_hoist_len_recv))
      buf_puts(b, g_hoist_len_var);
    else buf_printf(b, "sp_str_length_m(%s)", r);
  }
  else if (sp_streq(name, "upcase"))     buf_printf(b, "sp_str_upcase%s(%s)", case_map_suffix(c, argc, argv), r);
  else if (sp_streq(name, "downcase"))   buf_printf(b, "sp_str_downcase%s(%s)", case_map_suffix(c, argc, argv), r);
  else if (sp_streq(name, "capitalize")) buf_printf(b, "sp_str_capitalize%s(%s)", case_map_suffix(c, argc, argv), r);
  else if (sp_streq(name, "swapcase"))   buf_printf(b, "sp_str_swapcase%s(%s)", case_map_suffix(c, argc, argv), r);
  else if (sp_streq(name, "chomp") && argc == 1) {
    const char *a0ty = nt_type(nt, argv[0]);
    if (a0ty && sp_streq(a0ty, "NilNode")) {
      /* chomp(nil) returns the string unchanged */
      buf_puts(b, r);
    }
    else {
      buf_printf(b, "sp_str_chomp_sep(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
    }
  }
  else if ((is_copy_alias(name)) &&
           (argc == 0 ||
            (argc == 1 && sp_streq(name, "clone") && nt_type(nt, argv[0]) &&
             sp_streq(nt_type(nt, argv[0]), "KeywordHashNode") &&
             ({ int _fv = kwh_lookup(nt, argv[0], "freeze");
                const char *_ft = _fv >= 0 ? nt_type(nt, _fv) : NULL;
                _ft && (sp_streq(_ft, "FalseNode") || sp_streq(_ft, "TrueNode") ||
                        sp_streq(_ft, "NilNode")); })))) {
    /* sp_str_dup, not dup_external: the receiver is a spinel string, and
       the byte_len-aware copy carries embedded NULs (dup_external is for
       unmarked C pointers and must stay strlen-based). clone's literal
       freeze: keyword forces the copy's frozen state (nil/absent keeps
       clone's default); a non-literal value stays a loud reject. */
    int fz1 = 0;
    if (argc == 1) {
      int fv = kwh_lookup(nt, argv[0], "freeze");
      const char *ft = fv >= 0 ? nt_type(nt, fv) : NULL;
      fz1 = ft && sp_streq(ft, "TrueNode");
    }
    if (fz1) buf_printf(b, "sp_str_freeze_val(sp_str_dup(%s))", r);
    else buf_printf(b, "sp_str_dup(%s)", r);
  }
  else if (sp_streq(name, "start_with?") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
    /* s.start_with?(/re/): true when the pattern matches at index 0 */
    buf_printf(b, "(sp_re_match(sp_re_pat_%d, %s) == 0)", re_lit_index(c, argv[0]), r);
  }
  else if (sp_streq(name, "start_with?") && argc == 1 && comp_ntype(c, argv[0]) == TY_REGEX) {
    /* the same with the Regexp in a value; the receiver is read first */
    int ts = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = %s; SP_GC_ROOT(_t%d); sp_re_match(", ts, r, ts);
    emit_expr(c, argv[0], b); buf_printf(b, ", _t%d) == 0; })", ts);
  }
  else if (sp_streq(name, "start_with?") && argc == 1) {
    buf_printf(b, "sp_str_start_with(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "index") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
    /* an sp_oint (nil on a miss), matching the inferred type -- the
       poly-boxed form broke a variable-regexp argument */
    int tmi = ++g_tmp, tsi = ++g_tmp;
    /* report the match position in characters, not bytes (#3056) */
    buf_printf(b, "({ const char *_t%d = %s; sp_int _t%d = sp_re_match(sp_re_pat_%d, _t%d);"
                  " _t%d < 0 ? sp_oint_nil() : sp_oint_of(sp_str_byte_to_char(_t%d, _t%d)); })",
               tsi, r, tmi, re_lit_index(c, argv[0]), tsi, tmi, tsi, tmi);
  }
  /* a Regexp held in a variable or parameter rather than written inline */
  else if ((is_string_index(name)) && (argc == 1 || argc == 2) &&
           comp_ntype(c, argv[0]) == TY_REGEX) {
    int tsr = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = %s; sp_re_%sindex_%s(", tsr, r,
               sp_streq(name, "rindex") ? "r" : "", argc == 2 || name[0] == 'i' ? "from_opt" : "opt");
    emit_expr(c, argv[0], b); buf_printf(b, ", _t%d", tsr);
    if (argc == 2) { buf_puts(b, ", "); emit_int_expr(c, argv[1], b); }
    else if (name[0] == 'i') buf_puts(b, ", 0");
    buf_puts(b, "); })");
  }
  /* a pattern that is a Regexp or a String only at run time (an element of
     a mixed array): the tag picks the regexp search or the substring one,
     whose conversion raises CRuby's TypeError for anything else */
  else if ((is_string_index(name)) && (argc == 1 || argc == 2) &&
           comp_ntype(c, argv[0]) == TY_POLY) {
    int ri = sp_streq(name, "rindex");
    int ts = ++g_tmp, tp = ++g_tmp, tn = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = %s; SP_GC_ROOT(_t%d); sp_RbVal _t%d = ", ts, r, ts, tp);
    emit_boxed(c, argv[0], b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", tp);
    if (argc == 2) { buf_printf(b, "sp_int _t%d = ", tn); emit_int_expr(c, argv[1], b); buf_puts(b, "; "); }
    buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_REGEX) ? ", tp, tp);
    if (argc == 2)
      buf_printf(b, "sp_re_%sindex_from_opt((mrb_regexp_pattern *)_t%d.v.p, _t%d, _t%d) : ", ri ? "r" : "", tp, ts, tn);
    else if (ri) buf_printf(b, "sp_re_rindex_opt((mrb_regexp_pattern *)_t%d.v.p, _t%d) : ", tp, ts);
    else buf_printf(b, "sp_re_index_from_opt((mrb_regexp_pattern *)_t%d.v.p, _t%d, 0) : ", tp, ts);
    if (argc == 2)
      buf_printf(b, "sp_str_%s(_t%d, sp_poly_arg_str_chk(_t%d), _t%d); })",
                 ri ? "rindex_from" : "index_from_opt", ts, tp, tn);
    else buf_printf(b, "sp_str_%sindex_opt(_t%d, sp_poly_arg_str_chk(_t%d)); })", ri ? "r" : "", ts, tp);
  }
  else if (sp_streq(name, "index") && argc == 1) {
    /* nil on a miss: an sp_oint */
    buf_printf(b, "sp_str_index_opt(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "index") && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
    buf_printf(b, "sp_re_index_from_opt(sp_re_pat_%d, %s, ", re_lit_index(c, argv[0]), r);
    emit_int_expr(c, argv[1], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "index") && argc == 2) {
    buf_printf(b, "sp_str_index_from_opt(%s, ", r);
    emit_str_expr(c, argv[0], b); buf_puts(b, ", ");
    emit_int_expr(c, argv[1], b); buf_puts(b, ")");
  }
  /* byteindex/byterindex over a String needle: BYTE-offset search (result +
     start are byte offsets). The runtime helpers answer an sp_oint. A Regexp
     needle is a separate feature -- not handled here,
     so it falls through to the unsupported-call reject. */
  else if (sp_streq(name, "byteindex") && (argc == 1 || argc == 2) &&
           re_lit_index(c, argv[0]) >= 0) {
    buf_printf(b, "sp_re_byteindex_opt(sp_re_pat_%d, %s, ", re_lit_index(c, argv[0]), r);
    if (argc == 2) emit_int_expr(c, argv[1], b); else buf_puts(b, "0");
    buf_puts(b, ")");
  }
  else if (sp_streq(name, "byterindex") && (argc == 1 || argc == 2) &&
           re_lit_index(c, argv[0]) >= 0) {
    int tsr = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = %s; sp_re_byterindex_opt(sp_re_pat_%d, _t%d, ",
               tsr, r, re_lit_index(c, argv[0]), tsr);
    if (argc == 2) emit_int_expr(c, argv[1], b);
    else buf_printf(b, "(sp_int)sp_str_byte_len(_t%d)", tsr);
    buf_puts(b, "); })");
  }
  else if (sp_streq(name, "byteindex") && argc == 1 && str_needle_p(c, argv[0])) {
    buf_printf(b, "sp_str_byteindex(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "byteindex") && argc == 2 && str_needle_p(c, argv[0])) {
    buf_printf(b, "sp_str_byteindex_from(%s, ", r); emit_str_expr(c, argv[0], b);
    buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "byterindex") && argc == 1 && str_needle_p(c, argv[0])) {
    buf_printf(b, "sp_str_byterindex(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "byterindex") && argc == 2 && str_needle_p(c, argv[0])) {
    buf_printf(b, "sp_str_byterindex_from(%s, ", r); emit_str_expr(c, argv[0], b);
    buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
  }
  else if ((is_partition_family(name)) && argc == 1 &&
           re_lit_index(c, argv[0]) < 0 && comp_ntype(c, argv[0]) != TY_REGEX) {
    buf_printf(b, "sp_str_%s(%s, ", name, r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "partition") && argc == 1 &&
           (re_lit_index(c, argv[0]) >= 0 || comp_ntype(c, argv[0]) == TY_REGEX)) {
    /* [before, match, after] from the first regex match, else [s, "", ""];
       a Regexp in a value is read after the receiver */
    int tr = ++g_tmp, ts = ++g_tmp, tp = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = %s; SP_GC_ROOT(_t%d); mrb_regexp_pattern *_t%d = ", ts, r, ts, tp);
    if (re_lit_index(c, argv[0]) >= 0) buf_printf(b, "sp_re_pat_%d", re_lit_index(c, argv[0]));
    else emit_expr(c, argv[0], b);
    /* rooted: both arms allocate (the match pieces, the unmatched copies)
       while the array is only in _t */
    buf_printf(b, "; sp_StrArray *_t%d = sp_StrArray_new(); SP_GC_ROOT(_t%d);"
                  " if (sp_re_match(_t%d, _t%d) >= 0) {"
                  " sp_StrArray_push(_t%d, sp_re_pre_match()); sp_StrArray_push(_t%d, sp_re_match_str);"
                  " sp_StrArray_push(_t%d, sp_re_post_match()); }\nelse {"
                  " sp_StrArray_push(_t%d, sp_str_dup(_t%d)); sp_StrArray_push(_t%d, sp_str_dup(sp_str_empty));"
                  " sp_StrArray_push(_t%d, sp_str_dup(sp_str_empty)); }"
                  " _t%d; })",
               tr, tr, tp, ts, tr, tr, tr, tr, ts, tr, tr, tr);
  }
  else if (sp_streq(name, "rpartition") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
    buf_printf(b, "sp_re_rpartition(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
  }
  else if (sp_streq(name, "rpartition") && argc == 1 && comp_ntype(c, argv[0]) == TY_REGEX) {
    int ts = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = %s; SP_GC_ROOT(_t%d); sp_re_rpartition(", ts, r, ts);
    emit_expr(c, argv[0], b); buf_printf(b, ", _t%d); })", ts);
  }
  else if (sp_streq(name, "rindex") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
    buf_printf(b, "sp_re_rindex_opt(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
  }
  else if (sp_streq(name, "rindex") && argc == 1) { buf_printf(b, "sp_str_rindex_opt(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")"); }
  else if (sp_streq(name, "rindex") && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
    buf_printf(b, "sp_re_rindex_from_opt(sp_re_pat_%d, %s, ", re_lit_index(c, argv[0]), r);
    emit_int_expr(c, argv[1], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "rindex") && argc == 2) { buf_printf(b, "sp_str_rindex_from(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")"); }
  else if (sp_streq(name, "scrub") && argc == 1) { buf_printf(b, "sp_str_scrub(%s, ", r); emit_str_expr_nilable(c, argv[0], b); buf_puts(b, ")"); }
  else return 0;
  return 1;
}

/* A String receiver's pattern methods: a nil / true / false pattern's
   TypeError, sub / gsub with a regexp literal, an interpolated regexp, a
   Regexp value or a poly pattern, split by a regexp, and scan
   (emit_scalar_recv_arms's String chain; answers 1 when a branch was taken) */
static int str_arms_pattern(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int argc, const int *argv, const char *r) {
  /* a nil / true / false PATTERN in the regexp-expected family is CRuby's
     TypeError ("wrong argument type nil (expected Regexp)") -- it used to
     fall past every pattern-typed arm into NoMethodError, or silently
     skip the substitution */
  if ((sp_streq(name, "sub") || sp_streq(name, "sub!") ||
            sp_streq(name, "gsub") || sp_streq(name, "gsub!") ||
            sp_streq(name, "match") || sp_streq(name, "match?") ||
            sp_streq(name, "scan")) && argc >= 1 &&
           (comp_ntype(c, argv[0]) == TY_NIL || comp_ntype(c, argv[0]) == TY_BOOL)) {
    TyKind prty = repr_of(c, id).as_ty;
    int prb = ++g_tmp;
    buf_printf(b, "({ (void)(%s); ", r);
    /* every argument evaluates in order before the raise, as a real
       dispatch would */
    if (comp_ntype(c, argv[0]) == TY_NIL) {
      buf_puts(b, "(void)("); emit_expr(c, argv[0], b); buf_puts(b, "); ");
    }
    else {
      buf_printf(b, "int _t%d = (", prb); emit_expr(c, argv[0], b); buf_puts(b, "); ");
    }
    for (int pa = 1; pa < argc; pa++) {
      buf_puts(b, "(void)("); emit_expr(c, argv[pa], b); buf_puts(b, "); ");
    }
    if (comp_ntype(c, argv[0]) == TY_NIL)
      buf_puts(b, "sp_raise_cls(\"TypeError\", \"wrong argument type nil (expected Regexp)\");");
    else
      buf_printf(b, "sp_raise_cls(\"TypeError\", _t%d"
                    " ? \"wrong argument type true (expected Regexp)\""
                    " : \"wrong argument type false (expected Regexp)\");", prb);
    buf_printf(b, " %s; })", raise_tail_c(c, id, prty));
  }
  /* string methods taking a regex-literal argument route to the engine */
  else if ((is_substitution(name)) && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
    Repr hr = repr_of(c, argv[1]);
    int str_pairs = repr_hash_is(hr, TY_STRING, TY_STRING);
    const char *hconv = repl_hash_to_s_fn(hr);
    const char *suf = (str_pairs || hconv) ? "_str_str_hash" : g_sub_bang_id == id ? "_own" : "";
    buf_printf(b, "sp_re_%s%s(sp_re_pat_%d, %s, ", name, suf, re_lit_index(c, argv[0]), r);
    if (str_pairs) emit_expr(c, argv[1], b);
    else if (hconv) { buf_printf(b, "%s(", hconv); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
    else emit_str_expr(c, argv[1], b);
    buf_puts(b, ")");
  }
  else if ((is_substitution(name)) && argc == 2 &&
           nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "InterpolatedRegularExpressionNode")) {
    /* a Hash replacement takes the Hash overload, as with a literal pattern */
    Repr hr = repr_of(c, argv[1]);
    int str_pairs = repr_hash_is(hr, TY_STRING, TY_STRING);
    const char *hconv = repl_hash_to_s_fn(hr);
    const char *suf = (str_pairs || hconv) ? "_str_str_hash" : g_sub_bang_id == id ? "_own" : "";
    Buf rp; memset(&rp, 0, sizeof rp);
    emit_regex_pat_to_buf(c, argv[0], &rp);
    buf_printf(b, "sp_re_%s%s(%s, %s, ", name, suf, rp.p ? rp.p : "NULL", r);
    if (str_pairs) emit_expr(c, argv[1], b);
    else if (hconv) { buf_printf(b, "%s(", hconv); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
    else emit_str_expr(c, argv[1], b);
    buf_puts(b, ")");
    free(rp.p);
  }
  else if ((is_substitution(name)) && argc == 2 &&
           comp_ntype(c, argv[0]) == TY_REGEX) {
    /* pattern held in a regex-typed value (e.g. a local bound to an
       interpolated /.../); dispatch to the compiled-pattern overload
       rather than the string-pattern one. */
    Repr hr = repr_of(c, argv[1]);
    int str_pairs = repr_hash_is(hr, TY_STRING, TY_STRING);
    const char *hconv = repl_hash_to_s_fn(hr);
    const char *suf = (str_pairs || hconv) ? "_str_str_hash" : g_sub_bang_id == id ? "_own" : "";
    buf_printf(b, "sp_re_%s%s(", name, suf);
    emit_expr(c, argv[0], b); buf_printf(b, ", %s, ", r);
    if (str_pairs) emit_expr(c, argv[1], b);
    else if (hconv) { buf_printf(b, "%s(", hconv); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
    else emit_str_expr(c, argv[1], b);
    buf_puts(b, ")");
  }
  else if ((is_substitution(name)) && argc == 2 &&
           comp_ntype(c, argv[0]) == TY_POLY && !repr_hash_is(repr_of(c, argv[1]), TY_STRING, TY_STRING)) {
    /* a pattern that is a Regexp or a String only at runtime (an inflection
       rule read out of a [pattern, replacement] pair): the runtime picks
       the engine by its tag. The string-pattern path coerced the Regexp. */
    buf_printf(b, "sp_poly_pat_gsub%s(", g_sub_bang_id == id ? "_own" : ""); emit_boxed(c, argv[0], b);
    buf_printf(b, ", %s, ", r); emit_str_expr(c, argv[1], b);
    buf_printf(b, ", %d)", sp_streq(name, "sub") ? 1 : 0);
  }
  else if (sp_streq(name, "split") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
    buf_printf(b, "sp_re_split(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
  }
  else if (sp_streq(name, "split") && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
    buf_printf(b, "sp_re_split_limit(sp_re_pat_%d, %s, ", re_lit_index(c, argv[0]), r);
    emit_int_expr(c, argv[1], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "split") && argc == 1 && comp_ntype(c, argv[0]) == TY_REGEX) {
    buf_puts(b, "sp_re_split("); emit_expr(c, argv[0], b);
    buf_printf(b, ", %s)", r);
  }
  else if (sp_streq(name, "split") && argc == 2 && comp_ntype(c, argv[0]) == TY_REGEX) {
    buf_puts(b, "sp_re_split_limit("); emit_expr(c, argv[0], b);
    buf_printf(b, ", %s, ", r); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "scan") && argc == 1 &&
           (re_lit_index(c, argv[0]) >= 0 || comp_ntype(c, argv[0]) == TY_STRING ||
            comp_ntype(c, argv[0]) == TY_REGEX || comp_ntype(c, argv[0]) == TY_POLY) &&
           nt_ref(nt, id, "block") >= 0) {
    /* value-form scan { }: iterate in the prelude; the value is the
       receiver string (CRuby returns self from the block form). With
       capture groups the rows come from sp_re_scan_poly: one param
       binds the group row itself, several destructure it (a group that
       did not participate binds nil). */
    int blk = nt_ref(nt, id, "block");
    int re_idx = re_lit_index(c, argv[0]);
    int has_cap = re_idx >= 0 && an_re_has_captures(re_lit_src(c, argv[0]));
    int runtime_cap = re_idx < 0 && (comp_ntype(c, argv[0]) == TY_REGEX ||
                                   comp_ntype(c, argv[0]) == TY_POLY);
    int np = 0; while (block_param_name(c, blk, np)) np++;
    int body = nt_ref(nt, blk, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    int tr = ++g_tmp, tm = ++g_tmp, ti = ++g_tmp, tpat = -1;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "const char *_t%d = %s;\n", tr, r);
    if (runtime_cap) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "SP_GC_ROOT_STR(_t%d);\n", tr);
    }
    emit_indent(g_pre, g_indent);
    if (has_cap)
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_re_scan_poly(sp_re_pat_%d, _t%d); SP_GC_ROOT(_t%d);\n",
                 tm, re_idx, tr, tm);
    else if (re_idx >= 0)
      buf_printf(g_pre, "sp_StrArray *_t%d = sp_re_scan(sp_re_pat_%d, _t%d); SP_GC_ROOT(_t%d);\n",
                 tm, re_idx, tr, tm);
    /* pattern only known at run time (an inline `Regexp.new(s)`, a local
       holding one): the value already IS the mrb_regexp_pattern*. has_cap
       is 0 for such a pattern, so the block param stays a whole-match
       String -- the same shape a local bound to a capturing literal
       already yields here (#3389). Runtime patterns now retain the capture
       rows too, with their shape checked when binding the parameters. */
    else if (comp_ntype(c, argv[0]) == TY_REGEX) {
      /* render the pattern to a scratch buffer: `Regexp.new(s)` roots its
         own argument, and those decls go to g_pre, which must receive them
         as whole statements rather than spliced into this initializer */
      Buf eb; memset(&eb, 0, sizeof eb);
      emit_expr(c, argv[0], &eb);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_re_scan_poly(%s, _t%d); SP_GC_ROOT(_t%d);\n",
                 tm, eb.p ? eb.p : "NULL", tr, tm);
      free(eb.p);
    }
    else if (comp_ntype(c, argv[0]) == TY_POLY) {
      /* the pattern arrives boxed (read out of a table): a Regexp or a
         String, told apart at run time (sp_scan_boxed) */
      Buf pb2; memset(&pb2, 0, sizeof pb2);
      emit_boxed(c, argv[0], &pb2);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_scan_boxed_poly(_t%d, %s); SP_GC_ROOT(_t%d);\n",
                 tm, tr, pb2.p ? pb2.p : "sp_box_nil()", tm);
      free(pb2.p);
    }
    /* a String pattern the body walks the subject with (below) is held
       in a temp, read by every turn */
    else if (subtree_reads_match_globals(c, body)) {
      Buf sb; memset(&sb, 0, sizeof sb);
      emit_expr(c, argv[0], &sb);
      tpat = ++g_tmp;
      buf_printf(g_pre, "const char *_t%d = %s; SP_GC_ROOT_STR(_t%d);\n",
                 tpat, sb.p ? sb.p : "NULL", tpat);
      free(sb.p);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_StrArray *_t%d = sp_str_scan(_t%d, _t%d); SP_GC_ROOT(_t%d);\n",
                 tm, tr, tpat, tm);
    }
    else {
      buf_printf(g_pre, "sp_StrArray *_t%d = sp_str_scan(_t%d, ", tm, tr);
      emit_expr(c, argv[0], g_pre);
      buf_printf(g_pre, "); SP_GC_ROOT(_t%d);\n", tm);
    }
    /* the rows are pre-computed, so the match registers hold the last
       match; walk the subject again per iteration when the body reads $~
       or a capture global (#3601). The walk reads the subject after the
       body has run, so it is rooted. */
    int sc_pos = ((re_idx >= 0 || tpat >= 0) && subtree_reads_match_globals(c, body)) ? ++g_tmp : -1;
    if (sc_pos >= 0) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_int _t%d = 0; SP_GC_ROOT_STR(_t%d);\n", sc_pos, tr);
    }
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, tm, ti);
    if (sc_pos >= 0 && tpat >= 0) {
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "_t%d = sp_str_scan_at(_t%d, _t%d, _t%d);\n", sc_pos, tr, tpat, sc_pos);
    }
    else if (sc_pos >= 0) {
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "if (sp_re_match_at(sp_re_pat_%d, _t%d, _t%d) >= 0)"
                        " _t%d = sp_re_caps[1] > sp_re_caps[0] ? sp_re_caps[1] : sp_re_caps[1] + 1;\n",
                 re_idx, tr, sc_pos, sc_pos);
    }
    if (runtime_cap) {
      for (int pj = 0; pj < np; pj++) {
        const char *pn = block_param_name(c, blk, pj);
        Scope *sc = comp_scope_of(c, blk);
        LocalVar *lv = sc ? scope_local(sc, pn) : NULL;
        char row[64], value[256];
        snprintf(row, sizeof row, "_t%d->data[_t%d]", tm, ti);
        if (np == 1) snprintf(value, sizeof value, "%s", row);
        else snprintf(value, sizeof value, "%s.tag == SP_TAG_OBJ ? sp_poly_arr_get(%s, %d) : %s",
                      row, row, pj, pj == 0 ? row : "sp_box_nil()");
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "lv_%s = ", rename_local(pn));
        emit_coerce_text(c, id, TY_POLY, lv ? repr_of_slot(c, lv).as_ty : TY_POLY,
                         CO_HOLD, value, "a runtime scan parameter", g_pre);
        buf_puts(g_pre, ";\n");
        emit_block_param_share(c, sc, lv, rename_local(pn), g_pre, 1);
      }
    }
    else if (has_cap && np >= 2) {
      int trow = ++g_tmp;
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "sp_PolyArray *_t%d = (sp_PolyArray *)_t%d->data[_t%d].v.p;\n", trow, tm, ti);
      for (int pj = 0; pj < np; pj++) {
        const char *pn = rename_local(block_param_name(c, blk, pj));
        emit_indent(g_pre, g_indent + 1);
        char value[256];
        snprintf(value, sizeof value, "(_t%d && _t%d->len > %d && _t%d->data[%d].tag == SP_TAG_STR) ? _t%d->data[%d].v.s : NULL",
                 trow, trow, pj, trow, pj, trow, pj);
        Scope *sc = comp_scope_of(c, blk);
        LocalVar *lv = sc ? scope_local(sc, block_param_name(c, blk, pj)) : NULL;
        buf_printf(g_pre, "lv_%s = ", pn);
        emit_coerce_text(c, id, TY_STRING, lv ? repr_of_slot(c, lv).as_ty : TY_STRING,
                         CO_HOLD, value, "a scan capture parameter", g_pre);
        buf_puts(g_pre, ";\n");
        emit_block_param_share(c, sc, lv, pn, g_pre, 1);
      }
    }
    else if (block_param_name(c, blk, 0)) {
      const char *p0r = rename_local(block_param_name(c, blk, 0));
      emit_indent(g_pre, g_indent + 1);
      if (has_cap) {
        char value[80]; snprintf(value, sizeof value, "(sp_PolyArray *)_t%d->data[_t%d].v.p", tm, ti);
        Scope *sc = comp_scope_of(c, blk);
        LocalVar *lv = sc ? scope_local(sc, block_param_name(c, blk, 0)) : NULL;
        buf_printf(g_pre, "lv_%s = ", p0r);
        emit_coerce_text(c, id, TY_POLY_ARRAY, lv ? repr_of_slot(c, lv).as_ty : TY_POLY_ARRAY,
                         CO_HOLD, value, "a scan capture row", g_pre);
        buf_puts(g_pre, ";\n");
      }
      else {
        /* --share-strings: a parameter that is the shared handle takes the
           match, a fresh String, as a handle of its own */
        Scope *sc = comp_scope_of(c, blk);
        LocalVar *lv = sc ? scope_local(sc, block_param_name(c, blk, 0)) : NULL;
        char value[64]; snprintf(value, sizeof value, "_t%d->data[_t%d]", tm, ti);
        buf_printf(g_pre, "lv_%s = ", p0r);
        emit_strbuf_param_bind(c, lv, TY_STRING, value, g_pre);
        buf_puts(g_pre, ";\n");
      }
    }
    int svind = g_indent; g_indent++;
    for (int j = 0; j < bn; j++) emit_stmt(c, bb[j], g_pre, g_indent);
    g_indent = svind;
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
    buf_printf(b, "_t%d", tr);
  }
  else if (sp_streq(name, "scan") && argc == 1 && re_lit_index(c, argv[0]) >= 0 &&
           !an_re_has_captures(re_lit_src(c, argv[0]))) {
    buf_printf(b, "sp_re_scan(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
  }
  else if (sp_streq(name, "scan") && argc == 1 && re_lit_index(c, argv[0]) >= 0 &&
           an_re_has_captures(re_lit_src(c, argv[0]))) {
    buf_printf(b, "sp_re_scan_poly(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
  }
  else if (sp_streq(name, "scan") && argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
    buf_printf(b, "sp_str_scan(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
  }
  /* scan against a regex VALUE the arms above could not resolve to a
     precompiled literal (an interpolated pattern, a local holding one, an
     inline `Regexp.new(s)`): the value already IS the
     mrb_regexp_pattern*. Without this arm the call fell through to the
     unresolved-call gate and raised NoMethodError on the String (#3389).
     The result shape follows the type analyze settled on, so the two stay
     in step: an unresolvable pattern is typed poly_array and
     sp_re_scan_poly decides per match whether the row is the whole match
     or its captures. */
  else if (sp_streq(name, "scan") && argc == 1 && comp_ntype(c, argv[0]) == TY_REGEX &&
           nt_ref(nt, id, "block") < 0) {
    buf_printf(b, "%s(", repr_of(c, id).elem == TY_POLY ? "sp_re_scan_poly" : "sp_re_scan");
    emit_expr(c, argv[0], b); buf_printf(b, ", %s)", r);
  }
  else return 0;
  return 1;
}

/* An Integer receiver's clamp, digits, allbits? / anybits? / nobits?,
   ceildiv, pow, coerce, eql? and equal? (emit_scalar_recv_arms's Integer
   chain; answers 1 when a branch was taken) */
/* --int-overflow=promote typed the call poly: its value can leave the word
   (2**63 - 1 + 1, |-2**63|, a power), so the boxed helper answers it, ahead
   of the rows that compute in sp_int. Answers 1 when it emitted. */
static int int_arms_promote(Compiler *c, int id, Buf *b, const char *name, int argc, const int *argv, const char *r) {
  if (!g_promote_mode || repr_of(c, id).kind != RK_BOXED) return 0;
  if (argc == 0 && (sp_streq(name, "succ") || sp_streq(name, "next"))) { buf_printf(b, "sp_poly_succ_m(sp_box_int(%s), 0)", r); return 1; }
  if (argc == 0 && sp_streq(name, "pred")) { buf_printf(b, "sp_poly_int_pred(sp_box_int(%s))", r); return 1; }
  if (argc == 0 && (sp_streq(name, "abs") || sp_streq(name, "magnitude"))) { buf_printf(b, "sp_poly_abs(sp_box_int(%s))", r); return 1; }
  if (sp_streq(name, "pow") && argc == 1 && (comp_ntype(c, argv[0]) == TY_INT || comp_ntype(c, argv[0]) == TY_POLY)) {
    buf_printf(b, "sp_poly_int_pow(sp_box_int(%s), ", r); emit_boxed(c, argv[0], b); buf_puts(b, ")"); return 1;
  }
  return 0;
}

static int int_arms_clamp_pow(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind a0, const char *r) {
  /* a nil bound is an open side: clamp one-sided (or return the receiver),
     boxed so the chosen operand keeps its class (#2588) */
  if (sp_streq(name, "clamp") && argc == 2 &&
           (comp_ntype(c, argv[0]) == TY_NIL || comp_ntype(c, argv[1]) == TY_NIL)) {
    buf_printf(b, "sp_num_clamp_open(sp_box_int(%s), ", r); emit_boxed(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
  }
  /* A Float (or runtime-typed poly) bound makes the applied bound or the
     in-range receiver decide the result class at runtime, so box the
     operands and return whichever is chosen unchanged via sp_num_clamp. */
  else if (sp_streq(name, "clamp") && argc == 2 &&
           (comp_ntype(c, argv[0]) == TY_FLOAT || comp_ntype(c, argv[1]) == TY_FLOAT ||
            comp_ntype(c, argv[0]) == TY_POLY || comp_ntype(c, argv[1]) == TY_POLY ||
            comp_ntype(c, argv[0]) == TY_RATIONAL || comp_ntype(c, argv[1]) == TY_RATIONAL)) {
    /* a Rational bound (like a Float bound) makes the applied bound decide
       the result class at runtime; box the operands and let sp_num_clamp
       return whichever is chosen unchanged (#3232) */
    buf_printf(b, "sp_num_clamp(sp_box_int(%s), ", r); emit_boxed(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
  }
  /* clamp(lo, hi) with a Bignum bound: an sp_int receiver is inside any
     Bignum bound on that side, so only the sp_int side can bind (#3006) */
  else if (sp_streq(name, "clamp") && argc == 2 &&
           (repr_of(c, argv[0]).big || repr_of(c, argv[1]).big)) {
    int tlo = repr_of(c, argv[0]).big, thi = repr_of(c, argv[1]).big;
    buf_puts(b, "({ ");
    if (tlo) { buf_puts(b, "(void)("); emit_expr(c, argv[0], b); buf_puts(b, "); "); }
    if (thi) { buf_puts(b, "(void)("); emit_expr(c, argv[1], b); buf_puts(b, "); "); }
    if (tlo && thi) buf_printf(b, "(sp_int)(%s); })", r);
    else if (tlo) {
      /* a Bignum LOW bound is above every sp_int receiver... unless it is
         negative, in which case the receiver already exceeds it */
      int tb2 = ++g_tmp;
      buf_printf(b, "sp_Bigint *_t%d = ", tb2); emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_bigint_cmp(_t%d, sp_bigint_new_int(%s)) > 0"
                    " ? sp_bigint_to_int(_t%d) : (sp_int)(%s); })", tb2, r, tb2, r);
    }
    else {
      int tb2 = ++g_tmp;
      buf_printf(b, "sp_Bigint *_t%d = ", tb2); emit_expr(c, argv[1], b);
      buf_printf(b, "; sp_bigint_cmp(_t%d, sp_bigint_new_int(%s)) < 0"
                    " ? sp_bigint_to_int(_t%d) : sp_int_clamp_ck(%s, ", tb2, r, tb2, r);
      emit_expr(c, argv[0], b);
      buf_printf(b, ", %s); })", r);
    }
  }
  else if (sp_streq(name, "clamp") && argc == 2) { buf_printf(b, "sp_int_clamp_ck(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
  else if (sp_streq(name, "clamp") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT_RANGE) {
    /* int.clamp(float_range): the clamped-to bound is the Float endpoint (a
       boxed result); an in-range Int receiver stays Int. */
    int tv3 = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = (%s); sp_FloatRange _fr%d = ", tv3, r, tv3); emit_expr(c, argv[0], b);
    /* the receiver against the bounds exactly (sp_int_flt_cmp, #7505) */
    buf_printf(b, "; (sp_int_flt_cmp(_t%d, _fr%d.first) < 0) ? sp_box_float(_fr%d.first)"
                  " : (sp_int_flt_cmp(_t%d, _fr%d.last) == 1) ? sp_box_float(_fr%d.last)"
                  " : sp_box_int(_t%d); })", tv3, tv3, tv3, tv3, tv3, tv3, tv3);
  }
  else if (sp_streq(name, "clamp") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE &&
           nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode") &&
           ((nt_ref(nt, argv[0], "left") >= 0 && comp_ntype(c, nt_ref(nt, argv[0], "left")) == TY_FLOAT) ||
            (nt_ref(nt, argv[0], "right") >= 0 && comp_ntype(c, nt_ref(nt, argv[0], "right")) == TY_FLOAT))) {
    /* float bounds cannot ride sp_Range's int fields: compare as doubles,
       the clamped-to bound is the Float endpoint itself */
    int lo3 = nt_ref(nt, argv[0], "left"), hi3 = nt_ref(nt, argv[0], "right");
    int tv3 = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = (%s);", tv3, r);
    buf_printf(b, " double _lo%d = ", tv3);
    if (lo3 >= 0) emit_float_expr(c, lo3, b); else buf_puts(b, "-HUGE_VAL");
    buf_printf(b, "; double _hi%d = ", tv3);
    if (hi3 >= 0) emit_float_expr(c, hi3, b); else buf_puts(b, "HUGE_VAL");
    buf_printf(b, "; (sp_int_flt_cmp(_t%d, _lo%d) < 0) ? sp_box_float(_lo%d)"
                  " : (sp_int_flt_cmp(_t%d, _hi%d) == 1) ? sp_box_float(_hi%d)"
                  " : sp_box_int(_t%d); })",
               tv3, tv3, tv3, tv3, tv3, tv3, tv3);
  }
  else if (sp_streq(name, "clamp") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
    /* the helper raises on an exclusive range with a real end (CRuby) */
    buf_printf(b, "sp_int_clamp_range_ck(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
  }
  /* digits is migrated to builtins/integer.rb for every STATIC concrete
     call site (desugar_builtin_scalar_calls rewrites it to
     __int_digits__N before codegen ever sees a plain "digits" name
     here) -- these arms are dead for that case by construction, kept
     only as the poly "face table"'s own re-entry target (below,
     "unbox to the kind that owns the name, retype, re-enter"): a
     run-time-typed value whose actual class turns out to be Integer,
     reached only when some OTHER class in the program also defines a
     method literally named digits (the migration's own poly receiver
     deliberately stays on sp_poly_int_digits / this face table rather
     than an is_a? split, measured too costly; see
     desugar_builtin_scalar_calls's own comment). */
  else if (sp_streq(name, "digits") && argc == 1 && repr_of(c, argv[0]).big) {
    int tdb = ++g_tmp;
    buf_printf(b, "({ (void)("); emit_expr(c, argv[0], b);
    buf_printf(b, "); if ((%s) < 0) sp_raise_cls(\"Math::DomainError\", \"out of domain\");", r);
    buf_printf(b, " sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);", tdb, tdb);
    buf_printf(b, " sp_IntArray_push(_t%d, %s); _t%d; })", tdb, r, tdb);
  }
  else if (sp_streq(name, "digits") && argc == 1) { buf_printf(b, "sp_int_digits(%s, ", r); emit_to_int_expr(c, argv[0], b); buf_puts(b, ")"); }
  else if (is_bits_query(name) &&
           argc == 1 && repr_of(c, argv[0]).big) {
    /* A Bignum mask exceeds int64, so an int receiver can never cover all
       its bits (allbits? is always false); anybits?/nobits? test the
       receiver against the mask's low 64 bits -- the only ones an int
       receiver can share (#2470). */
    if (sp_streq(name, "allbits?")) {
      buf_printf(b, "((void)(%s), (void)(", r); emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
    }
    else {
      buf_printf(b, "(((%s) & sp_bigint_to_int(", r); emit_expr(c, argv[0], b);
      buf_printf(b, ")) %s 0)", sp_streq(name, "anybits?") ? "!=" : "==");
    }
  }
  else if (sp_streq(name, "allbits?") && argc == 1) { int t = ++g_tmp; buf_printf(b, "({ sp_int _t%d = ", t); emit_int_expr(c, argv[0], b); buf_printf(b, "; (((%s) & _t%d) == _t%d); })", r, t, t); }
  else if (sp_streq(name, "anybits?") && argc == 1) { buf_printf(b, "(((%s) & (", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")) != 0)"); }
  else if (sp_streq(name, "nobits?") && argc == 1) { buf_printf(b, "(((%s) & (", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")) == 0)"); }
  else if (sp_streq(name, "ceildiv") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
    buf_printf(b, "((sp_int)ceil((double)(%s) / (", r); emit_expr(c, argv[0], b); buf_puts(b, ")))");  /* (#2425) */
  }
  else if (sp_streq(name, "ceildiv") && argc == 1) { buf_printf(b, "sp_ceildiv(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
  /* pow(exp, mod) with a Bignum modulus: the result is bounded by the
     modulus but the intermediates are not, so run it in bigint (#3006) */
  else if (sp_streq(name, "pow") && argc == 2 && repr_of(c, argv[1]).big) {
    buf_printf(b, "sp_bigint_powmod(sp_bigint_new_int(%s), ", r);
    emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "pow") && argc == 2) { buf_printf(b, "sp_powmod(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")"); }
  /* pow with a literal negative exponent is the exact Rational
     1 / base**|exp|, computed as ** computes it (emit_complex_rational_call):
     sp_rational_pow raises ZeroDivisionError for a zero base, where
     sp_rational_new(1, 0) answered (1/0) */
  else if (sp_streq(name, "pow") && argc == 1 &&
           nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "IntegerNode") &&
           nt_int(nt, argv[0], "value", 0) < 0) {
    buf_printf(b, "sp_rational_pow(sp_rational_new((sp_int)(%s), 1), %lldLL)",
               r, (long long)nt_int(nt, argv[0], "value", 0));
  }
  /* pow with a Float exponent is real exponentiation -> Float (#2604) */
  else if (sp_streq(name, "pow") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
    buf_printf(b, "pow((double)(%s), ", r); emit_float_expr(c, argv[0], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "pow") && argc == 1) { buf_printf(b, "sp_int_pow(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
  else if (sp_streq(name, "coerce") && argc == 1) {
    TyKind a0 = comp_ntype(c, argv[0]);
    if (repr_of(c, argv[0]).big) {
      /* [big_arg, receiver promoted to Bignum] -- a poly pair (#2419) */
      int ta = ++g_tmp, o = ++g_tmp;
      /* the operand is held across the pair's allocation, which can collect */
      buf_printf(b, "({ sp_Bigint *_t%d = ", ta); emit_expr(c, argv[0], b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " sp_PolyArray_push(_t%d, sp_box_bigint(_t%d));"
                    " sp_PolyArray_push(_t%d, sp_box_bigint(sp_bigint_new_int(%s))); _t%d; })",
                 ta, o, o, o, ta, o, r, o);
    }
    else if (a0 == TY_FLOAT) {
      int ta = ++g_tmp, o = ++g_tmp;
      buf_printf(b, "({ sp_float _t%d = ", ta); emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_FloatArray *_t%d = sp_FloatArray_new();"
                    " sp_FloatArray_push(_t%d, _t%d);"
                    " sp_FloatArray_push(_t%d, (sp_float)(%s)); _t%d; })", o, o, ta, o, r, o);
    }
    /* coerce against a Rational computes in floats: [Float(other), Float(self)] (#2606) */
    else if (a0 == TY_RATIONAL) {
      int ta = ++g_tmp, o = ++g_tmp;
      buf_printf(b, "({ sp_float _t%d = sp_rational_to_f(", ta); emit_expr(c, argv[0], b);
      buf_printf(b, "); sp_FloatArray *_t%d = sp_FloatArray_new();"
                    " sp_FloatArray_push(_t%d, _t%d);"
                    " sp_FloatArray_push(_t%d, (sp_float)(%s)); _t%d; })", o, o, ta, o, r, o);
    }
    /* an Integer can't coerce with a Complex -> RangeError (#2606) */
    else if (a0 == TY_COMPLEX) {
      buf_puts(b, "((void)("); emit_expr(c, argv[0], b);
      buf_puts(b, "), (sp_raise_cls(\"RangeError\", \"can't convert Complex into Integer\"), (sp_FloatArray *)0))");
    }
    /* Only a NUMBER coerces to an Integer pair. Everything else is
       `[Float(other), Float(self)]`, which is where CRuby's messages come
       from -- and the argument went into the sp_int slot as itself before,
       so a String stopped the C build and a nil answered a coerced 0
       (#4011). */
    else if (a0 != TY_INT && a0 != TY_POLY && a0 != TY_UNKNOWN) {
      int o = ++g_tmp;
      buf_printf(b, "({ sp_float _tc%d = sp_poly_Float(", o); emit_boxed(c, argv[0], b);
      buf_printf(b, "); sp_FloatArray *_t%d = sp_FloatArray_new();"
                    " sp_FloatArray_push(_t%d, _tc%d);"
                    " sp_FloatArray_push(_t%d, (sp_float)(%s)); _t%d; })", o, o, o, o, r, o);
    }
    else if (a0 == TY_POLY) {
      /* the tag decides at run time, through the same helper the boxed
         receiver path uses */
      int o = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = sp_poly_coerce(sp_box_int(%s), ", o, r);
      emit_boxed(c, argv[0], b);
      buf_printf(b, "); sp_poly_to_poly_array(_t%d); })", o);
    }
    else {
      int ta = ++g_tmp, o = ++g_tmp;
      buf_printf(b, "({ sp_int _t%d = ", ta); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new();"
                    " sp_IntArray_push(_t%d, _t%d);"
                    " sp_IntArray_push(_t%d, (%s)); _t%d; })", o, o, ta, o, r, o);
    }
  }
  /* Integer#eql?/equal?(x): value-equal only when x is itself Integer-typed
     (no numeric coercion -- 1.eql?(1.0) is false). For a fixnum receiver
     equal? is value identity, so it behaves the same as eql?. A Float or
     any other concrete arg is never equal; a poly arg checks its tag. */
  else if ((is_eql_or_equal(name)) && argc == 1) {
    /* a receiver whose nil flag is set is nil, which is eql? and
       equal? to nil alone: ask the boxed pair, as nil where it is one */
    if (call_returns_nullable_int(c, recv)) {
      buf_printf(b, "%s(sp_box_oint(", sp_streq(name, "eql?") ? "sp_poly_eql" : "sp_poly_equal");
      emit_oint_expr(c, recv, TY_INT, b); buf_puts(b, "), ");
      emit_boxed(c, argv[0], b); buf_puts(b, ")");
    }
    else if (a0 == TY_INT) { buf_printf(b, "((%s) == (", r); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
    else if (a0 == TY_POLY) {
      int te = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", te); emit_boxed(c, argv[0], b);
      buf_printf(b, "; _t%d.tag == SP_TAG_INT && _t%d.v.i == (%s); })", te, te, r);
    }
    else { buf_puts(b, "(("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
  }
  else return 0;
  return 1;
}

/* Integer#div / #modulo given an operand with no number in it (a String,
   a Symbol, nil, a boolean, an Array, a Hash): CRuby's coerce failure, as
   the operators raise it (emit_int_operand_fail), over the receiver's text
   `r`. The arms handed such an operand to sp_idiv's or sp_imod's sp_int
   slot, which did not build. */
static int int_divisor_coerce_fail(Compiler *c, const char *r, int arg, Buf *b) {
  TyKind at = comp_ntype(c, arg);
  if (!(at == TY_STRING || at == TY_NIL || at == TY_SYMBOL || at == TY_BOOL ||
        ty_is_array(at) || ty_is_hash(at))) return 0;
  int ta = ++g_tmp;
  buf_printf(b, "({ (void)(%s); sp_RbVal _t%d = ", r, ta); emit_boxed(c, arg, b);
  buf_printf(b, "; sp_raise_cls(\"TypeError\", sp_sprintf(\"%%s can't be coerced into Integer\", "
                "sp_cmperr_desc(_t%d))); (sp_int)0; })", ta);
  return 1;
}

/* Keep Float digits unconverted until the keyword expressions have run. */
static int emit_round_precision_bind(Compiler *c, int node, int temp, Buf *b) {
  int is_float = comp_ntype(c, node) == TY_FLOAT;
  buf_printf(b, "%s _t%d = ", is_float ? "double" : "sp_int", temp);
  if (is_float) emit_scalar_operand(c, node, "0", b);
  else emit_int_expr(c, node, b);
  buf_puts(b, "; ");
  return is_float;
}

/* An Integer receiver's round(half:) and the rounding family, chr, [] / bit
   reads, and its division family: divmod, div, gcd / lcm, modulo,
   remainder, gcdlcm (emit_scalar_recv_arms's Integer chain; answers 1 when
   a branch was taken) */
static int int_arms_round_divide(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int argc, const int *argv, const char *r) {
  /* `round(half: mode)`, with or without a digit count. Only #round takes
     a tie-break mode; the other three reject the hash outright, and with
     a digit count as well it is the arity CRuby complains about first. */
  if ((argc == 1 || argc == 2) && nt_type(nt, argv[argc - 1]) &&
           sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode") &&
           is_round_family(name)) {
    RoundKw kw; round_kw_read(c, argv[argc - 1], &kw);
    int tr = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = (%s); ", tr, r);
    if (argc == 2) {
      int tn = ++g_tmp;
      int nd_float = emit_round_precision_bind(c, argv[0], tn, b);
      if (!sp_streq(name, "round")) {
        /* the hash is built before the call rejects it */
        emit_round_kw_effects(c, &kw, b);
        buf_printf(b, "(void)_t%d; (void)_t%d;"
                      " sp_raise_cls(\"ArgumentError\", \"wrong number of"
                      " arguments (given 2, expected 0..1)\"); (sp_int)0; })", tr, tn);
      }
      else {
        int tm = emit_round_kw_binds(c, &kw, b);
        buf_printf(b, "sp_int_round_half_v(_t%d, ", tr);
        if (nd_float) buf_printf(b, "sp_float_arg_i(_t%d), ", tn);
        else buf_printf(b, "_t%d, ", tn);
        if (tm >= 0) buf_printf(b, "_t%d", tm); else buf_puts(b, "sp_box_nil()");
        buf_puts(b, "); })");
      }
    }
    else if (!sp_streq(name, "round")) {
      emit_round_kw_effects(c, &kw, b);
      buf_printf(b, "(void)_t%d; ", tr);
      buf_puts(b, "sp_raise_cls(\"TypeError\", \"no implicit conversion of Hash"
                  " into Integer\"); (sp_int)0; })");
    }
    else {
      /* Integer#round with no digit count answers the receiver without
         reading the keywords at all -- `1.round(half: :bogus)` is 1,
         where `1.round(0, half: :bogus)` is an ArgumentError. They are
         still evaluated: the hash is built before the call ignores it. */
      emit_round_kw_effects(c, &kw, b);
      buf_printf(b, "_t%d; })", tr);
    }
  }
  else if (is_round_family(name) && argc == 1) {
    buf_printf(b, "sp_int_%s(%s, ", name, r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "chr") && argc == 1) {
    /* Integer#chr(Encoding::X): the encoding argument is resolved at
       compile time from the constant path (Encoding values barely exist
       as runtime objects). UTF_8 encodes the codepoint (1-4 bytes);
       the single-byte encodings keep byte semantics. A dynamic or
       unknown encoding is a loud reject, not a silent byte-truncation
       (which is what this arm previously did for EVERY chr(enc)). */
    const char *enm = NULL, *parnm = NULL;
    if (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ConstantPathNode")) {
      enm = nt_str(nt, argv[0], "name");
      int par = nt_ref(nt, argv[0], "parent");
      parnm = (par >= 0 && nt_type(nt, par) &&
               sp_streq(nt_type(nt, par), "ConstantReadNode"))
              ? nt_str(nt, par, "name") : NULL;
    }
    if (parnm && sp_streq(parnm, "Encoding") && enm && sp_streq(enm, "UTF_8"))
      buf_printf(b, "sp_int_chr_utf8(%s)", r);
    else if (parnm && sp_streq(parnm, "Encoding") && enm &&
             (sp_streq(enm, "US_ASCII") || sp_streq(enm, "ASCII_8BIT") ||
              sp_streq(enm, "BINARY")))
      buf_printf(b, "sp_int_chr(%s)", r);
    else
      unsupported(c, id, "Integer#chr with a non-constant or unsupported encoding");
  }
  else if (sp_streq(name, "[]") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
    /* bit-slice: n[lo..hi] extracts hi-lo+1 bits starting at lo; an
       endless range keeps everything above lo; a beginless range raises
       like CRuby (the field below bit 0 is infinite) */
    int trb = ++g_tmp;
    buf_printf(b, "({ sp_Range _t%d = sp_range_ix(", trb); emit_expr(c, argv[0], b); buf_puts(b, ")");
    buf_printf(b, "; sp_int _lo%d = _t%d.nobeg"
                  " ? (sp_raise_cls(\"ArgumentError\","
                  " \"The beginless range for Integer#[] results in infinity\"), 0)"
                  " : _t%d.first;"
                  /* the shift and the field width in unsigned: a bound at an
                     end of sp_int (a clamped Float end) overflowed `last - lo`,
                     and a shift by 64 or more is undefined */
                  " sp_int _v%d = (%s);"
                  " sp_int _sh%d = _lo%d >= 64 ? (_v%d < 0 ? -1 : 0) : (_v%d >> _lo%d);"
                  /* CRuby's: an end below the start keeps every bit above
                     it, an end at the start is that one bit (none if excluded) */
                  " _t%d.noend || _t%d.last < _lo%d ? _sh%d : _t%d.last == _lo%d ? (_t%d.excl ? 0 : (_sh%d & 1))"
                  " : ({ uintptr_t _w%d = (uintptr_t)_t%d.last - (uintptr_t)_lo%d + (_t%d.excl ? 0 : 1);"
                  " (sp_int)((uintptr_t)_sh%d & (_w%d >= 64 ? ~(uintptr_t)0 : (((uintptr_t)1 << _w%d) - 1))); }); })",
               trb, trb, trb,
               trb, r,
               trb, trb, trb, trb, trb,
               trb, trb, trb, trb, trb, trb, trb, trb,
               trb, trb, trb, trb,
               trb, trb, trb);
  }
  else if (sp_streq(name, "[]") && argc == 1) {
    /* clamped: a literal-folded out-of-range index was an undefined C
       shift (right answer on x86's masked shifts, garbage elsewhere).
       A Bignum index is far past the receiver's width, so the bit is the
       sign bit: 0 for a non-negative receiver, 1 for a negative one. */
    if (repr_of(c, argv[0]).big) {
      buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
      buf_printf(b, "); (sp_int)((%s) < 0 ? 1 : 0); })", r);
    }
    else {
      buf_printf(b, "sp_int_bit((%s), ", r);
      Repr ar = repr_of(c, argv[0]);
      if (ar.as_ty == TY_FLOAT || ar.kind == RK_BOXED) {
        buf_puts(b, "sp_poly_bit_index_arg("); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      }
      else emit_to_int_expr(c, argv[0], b);
      buf_puts(b, ")");
    }
  }
  else if (sp_streq(name, "[]") && argc == 2) {
    /* n[start, len]: the len-bit field starting at bit `start`. Routed
       through a runtime helper that clamps an out-of-range start/len so
       the shift never goes undefined. */
    buf_printf(b, "sp_int_bit_range((%s), ", r); emit_to_int_expr(c, argv[0], b);
    buf_puts(b, ", "); emit_to_int_expr(c, argv[1], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "divmod") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
    /* a Float divisor divides as floats, CRuby's flodivmod (sp_flo_divmod):
       [Integer quotient, Float mod] */
    int tb = ++g_tmp, tq = ++g_tmp, tm = ++g_tmp, o = ++g_tmp;
    buf_printf(b, "({ double _t%d = ", tb); emit_expr(c, argv[0], b);
    buf_printf(b, "; double _t%d, _t%d; sp_flo_divmod((double)(%s), _t%d, &_t%d, &_t%d);"
                  " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                  " sp_PolyArray_push(_t%d, sp_box_int(sp_float_fit_i(_t%d)));"
                  " sp_PolyArray_push(_t%d, sp_box_float(_t%d)); _t%d; })",
               tq, tm, r, tb, tq, tm, o, o, o, tq, o, tm, o);
  }
  /* promote typed the pair poly: the quotient can be the Bignum 2**63 */
  else if (sp_streq(name, "divmod") && argc == 1 && repr_of(c, id).as_ty == TY_POLY_ARRAY &&
           comp_ntype(c, argv[0]) == TY_INT) {
    /* the pair as the slot holds it: an Array, not its box */
    buf_printf(b, "sp_poly_to_poly_array(sp_poly_divmod(sp_box_int(%s), ", r); emit_boxed(c, argv[0], b); buf_puts(b, "))");
  }
  /* a divisor known only at run time: the pair Numeric#divmod makes for its
     kind (a Rational's is exact, sp_rat_mod_v). It was read as an Integer,
     and 7.divmod(Rational(-3, 2)) answered [-7, 0] */
  else if (is_divmod_name(name) && argc == 1 &&
           repr_of(c, argv[0]).kind == RK_BOXED && comp_ntype(c, id) == TY_POLY_ARRAY) {
    buf_printf(b, "sp_poly_to_poly_array(sp_poly_divmod(sp_box_int(%s), ", r);
    emit_expr(c, argv[0], b); buf_puts(b, "))");
  }
  else if (sp_streq(name, "divmod") && argc == 1 &&
           comp_ntype(c, argv[0]) != TY_RATIONAL) {
    int tb = ++g_tmp, o = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = ", tb); emit_int_expr(c, argv[0], b);
    buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new(); sp_IntArray_push(_t%d, sp_idiv(%s, _t%d));"
                  " sp_IntArray_push(_t%d, sp_imod(%s, _t%d)); _t%d; })", o, o, r, tb, o, r, tb, o);
  }
  else if (sp_streq(name, "div") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
    /* Integer#div(Float) floors the real quotient (7.div(2.5) == 2) (#2425);
       a zero divisor is ZeroDivisionError, a NaN one FloatDomainError, and
       a quotient past the Integer range sp_float_fit_i's RangeError -- or,
       in the boxed slot promote mode gives the call, its Bignum */
    int tx = ++g_tmp, tn = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = (%s); sp_float _t%d = ", tx, r, tn);
    emit_expr(c, argv[0], b);
    buf_printf(b, "; sp_float_div_%s((double)_t%d, _t%d); })",
               repr_of(c, id).kind == RK_BOXED ? "v" : "i", tx, tn);
  }
  /* int receiver, Bignum divisor: the receiver always fits an sp_int, but
     the quotient has to be computed in bigint since the divisor cannot
     narrow to one -- emit_int_divisor's plain sp_int cast handed
     sp_idiv a pointer where it wanted a machine int, and the call never
     compiled (not merely truncated). Dividing something that fits int64
     by something that does not always answers -1, 0, or a small
     quotient bounded by the receiver, so narrow the ANSWER instead,
     the same shape gcd/lcm's own TY_BIGINT arms below use. */
  else if (sp_streq(name, "div") && argc == 1 && repr_of(c, argv[0]).big) {
    buf_printf(b, "sp_bigint_to_int(sp_bigint_div(sp_bigint_new_int(%s), ", r);
    emit_expr(c, argv[0], b); buf_puts(b, "))");
  }
  /* a boxed divisor answers by its run-time kind: a Float floors the real
     quotient as the typed arm above does. It was converted to an Integer
     first, and 17.div(2.5) answered 8 where CRuby answers 6. A Bignum or a
     Rational divides as Numeric#div does (sp_int_div_poly). */
  else if (sp_streq(name, "div") && argc == 1 && repr_of(c, argv[0]).kind == RK_BOXED) {
    /* the boxed slot promote gives the call holds a quotient past the word */
    if (repr_of(c, id).kind == RK_BOXED) buf_printf(b, "sp_poly_div_m(sp_box_int(%s), ", r);
    else buf_printf(b, "sp_int_div_poly(%s, ", r);
    emit_expr(c, argv[0], b); buf_puts(b, ")");
  }
  else if (is_div_or_modulo(name) && argc == 1 &&
           int_divisor_coerce_fail(c, r, argv[0], b)) {}
  else if (sp_streq(name, "div") && argc == 1) { buf_printf(b, "sp_idiv(%s, ", r); emit_int_divisor(c, argv[0], b); buf_puts(b, ")"); }
  else if ((sp_streq(name, "gcd") || sp_streq(name, "lcm")) && argc == 1 &&
           (comp_ntype(c, argv[0]) == TY_FLOAT ||
            comp_ntype(c, argv[0]) == TY_STRING ||
            comp_ntype(c, argv[0]) == TY_NIL ||
            comp_ntype(c, argv[0]) == TY_BOOL ||
            comp_ntype(c, argv[0]) == TY_SYMBOL ||
            ty_is_array(comp_ntype(c, argv[0])) ||
            ty_is_hash(comp_ntype(c, argv[0])))) {
    /* every non-Integer argument is CRuby's "not an integer" TypeError;
       only a Float was caught, so a String went into sp_gcd's sp_int slot
       as a pointer (#3644) */
    buf_puts(b, "({ (void)(");
    emit_expr(c, argv[0], b);
    buf_printf(b, "); sp_raise_cls(\"TypeError\", \"not an integer\"); (sp_int)(%s); })", r);
  }
  else if (sp_streq(name, "gcd") && argc == 1 && repr_of(c, argv[0]).big) {
    /* gcd(int, bignum) divides the int receiver, so it always fits an
       sp_int; compute via the bigint gcd then narrow (#3006) */
    buf_printf(b, "sp_bigint_to_int(sp_bigint_gcd(sp_bigint_new_int(%s), ", r);
    emit_expr(c, argv[0], b); buf_puts(b, "))");
  }
  else if (sp_streq(name, "gcd") && argc == 1) { buf_printf(b, "sp_gcd(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
  /* lcm(bignum) is at least as large as the argument, so it stays big */
  else if (sp_streq(name, "lcm") && argc == 1 && repr_of(c, argv[0]).big) {
    buf_printf(b, "sp_bigint_lcm(sp_bigint_new_int(%s), ", r);
    emit_expr(c, argv[0], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "lcm") && argc == 1) { buf_printf(b, "sp_lcm(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
  else if (sp_streq(name, "modulo") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
    int tb = ++g_tmp;
    buf_printf(b, "({ double _t%d = ", tb); emit_expr(c, argv[0], b);
    buf_printf(b, "; (double)(%s) - _t%d * floor((double)(%s) / _t%d); })",
               r, tb, r, tb);
  }
  else if ((sp_streq(name, "modulo") || sp_streq(name, "%%")) && argc == 1 &&
           comp_ntype(c, argv[0]) == TY_RATIONAL) {
    /* Integer % Rational lifts the receiver to n/1 (floor modulo) */
    buf_printf(b, "sp_int_rat_mod(%s, ", r);
    emit_expr(c, argv[0], b); buf_puts(b, ")");
  }
  /* a boxed Float divisor answers a Float CRuby's way (17.modulo(2.5) is
     2.0), which the Integer-typed call cannot hold: raise rather than answer
     the modulo of a divisor cut to an Integer (it answered 1) */
  else if (sp_streq(name, "modulo") && argc == 1 && repr_of(c, argv[0]).kind == RK_BOXED) {
    /* typed boxed, as `%` is: the modulo of the divisor's run-time kind */
    if (repr_of(c, id).kind == RK_BOXED) buf_printf(b, "sp_poly_modulo(sp_box_int(%s), ", r);
    else buf_printf(b, "sp_int_modulo_boxed(%s, ", r);
    emit_expr(c, argv[0], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "modulo") && argc == 1) { buf_printf(b, "sp_imod(%s, ", r); emit_int_divisor(c, argv[0], b); buf_puts(b, ")"); }
  else if (sp_streq(name, "remainder") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
    /* x - y * (x/y).truncate, in doubles (7.remainder(2.5) is 2.0); a zero
       divisor raises like every other division-derived operation (#3649) */
    int tb = ++g_tmp;
    buf_printf(b, "({ double _t%d = ", tb); emit_expr(c, argv[0], b);
    buf_printf(b, "; _t%d == 0 ? (sp_raise_cls(\"ZeroDivisionError\", \"divided by 0\"), 0.0)"
                  " : (double)(%s) - _t%d * trunc((double)(%s) / _t%d); })",
               tb, r, tb, r, tb);
  }
  else if (sp_streq(name, "remainder") && argc == 1 &&
           comp_ntype(c, argv[0]) == TY_RATIONAL) {
    buf_printf(b, "sp_int_rat_rem(%s, ", r);
    emit_expr(c, argv[0], b); buf_puts(b, ")");
  }
  else if (sp_streq(name, "remainder") && argc == 1) { buf_printf(b, "sp_iremainder(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
  else if (sp_streq(name, "divmod") && argc == 1 && comp_ntype(c, argv[0]) == TY_RATIONAL) {
    /* [floor quotient (Integer), self - q*b (Rational)]; the quotient
       past the word a Bignum (sp_int_rat_divmod) */
    int ta = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = (%s); sp_int_rat_divmod(_t%d, ", ta, r, ta);
    emit_expr(c, argv[0], b); buf_puts(b, "); })");
  }
  else if (sp_streq(name, "gcdlcm") && argc == 1 &&
           comp_ntype(c, argv[0]) == TY_FLOAT) {
    buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
    buf_printf(b, "); (void)(%s); sp_raise_cls(\"TypeError\", \"not an integer\");"
                  " sp_IntArray_new(); })", r);
  }
  else if (sp_streq(name, "gcdlcm") && argc == 1) {
    int ta = ++g_tmp, o = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = ", ta); emit_int_expr(c, argv[0], b);
    buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new(); sp_IntArray_push(_t%d, sp_gcd(%s, _t%d));"
                  " sp_IntArray_push(_t%d, sp_lcm(%s, _t%d)); _t%d; })", o, o, r, ta, o, r, ta, o);
  }
  else return 0;
  return 1;
}

/* The String arms that read only the receiver text and the arguments, in
   emit_scalar_recv_arms's order. */
static int str_arms_text(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, const char *r) {
  return str_arms_pattern(c, id, b, nt, name, argc, argv, r) ||
         emit_builtin_op_text(c, id, recv, TY_STRING, name, r, b) ||
         str_arms_case_search(c, b, nt, name, recv, argc, argv, r) ||
         str_arms_slice_encode(c, id, b, name, recv, argc, argv, r);
}

/* split, partition, rpartition, start_with? and [] with a pattern that is
   a Regexp or a String only at run time (an element of a mixed Array or
   Hash, Rack's COMMON_SEP[sep]). The boxed arms took a Regexp for a String
   and raised TypeError. The tag now picks the Regexp or the String arms;
   any other value takes the boxed arms as before. */
static int g_poly_pattern_open;
static int nt_is_literal_arg(const NodeTable *nt, int n) {
  const char *t = nt_type(nt, n);
  return t && (sp_streq(t, "IntegerNode") || sp_streq(t, "SymbolNode") || sp_streq(t, "StringNode") ||
               sp_streq(t, "NilNode"));
}
static int str_poly_pattern_name(const char *name, int argc) {
  if (sp_streq(name, "split")) return argc == 1 || argc == 2;
  if (is_slice_alias(name)) return argc == 1 || argc == 2;
  return argc == 1 && (is_partition_family(name) || sp_streq(name, "start_with?"));
}
static int str_arms_poly_pattern(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, const char *r) {
  if (g_poly_pattern_open || !str_poly_pattern_name(name, argc) || comp_ntype(c, argv[0]) != TY_POLY) return 0;
  int ts = ++g_tmp, tp = ++g_tmp;
  char rs[24];
  snprintf(rs, sizeof rs, "_t%d", ts);
  Buf eb; memset(&eb, 0, sizeof eb);
  /* a conversion of the pattern belongs inside its branch: hoisted to the
     call, it would read the temp before it is declared */
  ConvHold *held = g_conv_hold;
  g_conv_hold = NULL;
  size_t pre_mark = g_pre ? g_pre->len : 0;
  buf_printf(&eb, "({ const char *_t%d = %s; SP_GC_ROOT(_t%d); sp_RbVal _t%d = ", ts, r, ts, tp);
  emit_boxed(c, argv[0], &eb);
  buf_printf(&eb, "; SP_GC_ROOT_RBVAL(_t%d); _t%d = sp_poly_strbuf_deref(_t%d); ", tp, tp, tp);
  /* the limit or the second index is read once, after the pattern; every
     branch below renders it again. A literal stays as it is, since the
     arms read its node (s[re, :name]). */
  int bind1 = g_n_argov;
  if (argc == 2 && !nt_is_literal_arg(nt, argv[1])) {
    int t1 = hold_operand(c, argv[1], comp_ntype(c, argv[1]), 0, ++g_tmp, 1, " ", &eb);
    bind1 = view_bind(argv[1], "_t%d", t1);
  }
  buf_printf(&eb, "(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_REGEX) ? (", tp, tp);
  int bind = view_bind(argv[0], "((mrb_regexp_pattern *)_t%d.v.p)", tp);
  int v = view_push(c, argv[0], TY_REGEX);
  int ok = str_arms_text(c, id, &eb, nt, name, recv, argc, argv, rs);
  view_pop(c, v);
  view_unbind(bind);
  /* a String takes the String arms: s[str] read it as an Integer index */
  buf_printf(&eb, ") : _t%d.tag == SP_TAG_STR ? (", tp);
  bind = view_bind(argv[0], "_t%d.v.s", tp);
  v = view_push(c, argv[0], TY_STRING);
  ok = ok && str_arms_text(c, id, &eb, nt, name, recv, argc, argv, rs);
  view_pop(c, v);
  view_unbind(bind);
  buf_puts(&eb, ") : (");
  /* anything else: split and partition say CRuby's "expected Regexp" */
  if (sp_streq(name, "split") || is_partition_family(name))
    bind = view_bind(argv[0], sp_streq(name, "split") ? "sp_poly_pattern_chk(_t%d, 1)" : "sp_poly_pattern_chk(_t%d, 0)", tp);
  else bind = view_bind(argv[0], "_t%d", tp);
  g_poly_pattern_open = 1;
  ok = ok && str_arms_text(c, id, &eb, nt, name, recv, argc, argv, rs);
  /* only ever entered closed, so closing is restoring */
  g_poly_pattern_open = 0;
  view_unbind(bind);
  view_unbind(bind1);
  buf_puts(&eb, "); })");
  g_conv_hold = held;
  if (ok) buf_puts(b, eb.p);
  /* a dropped dispatch takes back what its branches hoisted */
  else if (g_pre && g_pre->len > pre_mark) { g_pre->len = pre_mark; g_pre->p[pre_mark] = '\0'; }
  free(eb.p);
  return ok;
}

/* A String, Integer or Float receiver, evaluated once into rs and spliced into each arm (emit_scalar_call_arms's arms, in their order) */
/* Does the prelude text added since `mark` name the temp `_t<id>` (a whole
   token, not a prefix of a longer one)? */
static int pre_names_tmp(const Buf *pre, size_t mark, int id) {
  if (!pre->p || pre->len <= mark) return 0;
  char nm[24]; int n = snprintf(nm, sizeof nm, "_t%d", id);
  for (const char *q = pre->p + mark; (q = strstr(q, nm)) != NULL; q += n) {
    char after = q[n];
    if (!((after >= '0' && after <= '9') || after == '_' || (after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z'))) return 1;
  }
  return 0;
}
/* insert `text` into `b` at byte offset `at` */
static void buf_insert_at(Buf *b, size_t at, const char *text) {
  size_t tl = strlen(text);
  Buf nb; memset(&nb, 0, sizeof nb);
  if (b->p && at) { char save = b->p[at]; b->p[at] = 0; buf_puts(&nb, b->p); b->p[at] = save; }
  buf_puts(&nb, text);
  if (b->p) buf_puts(&nb, b->p + at);
  free(b->p); *b = nb; (void)tl;
}
static int emit_scalar_recv_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind a0, int *out) {
  /* scalar receiver methods: evaluate the receiver once into rs, then
     splice its text (so a literal/complex receiver isn't rebuilt). */
  if (!(recv >= 0 && (rt == TY_STRING || rt == TY_INT || rt == TY_FLOAT))) return 0;
  if (rt == TY_STRING && str_partition_shared(c, id, b, name, recv, argc, argv)) { *out = 1; return 1; }
  Buf rs; memset(&rs, 0, sizeof rs);
  /* Reading a shared-mutable string as a value copies its whole buffer so
     the value cannot alias the handle (#3227). A method that only looks at
     the bytes and answers a scalar or a freshly built string keeps nothing,
     so it can read the live buffer instead -- `text[i]` in a scan loop was
     copying the whole subject on every character. */
  if (rt == TY_STRING && name && is_string_read_only_method(name))
    emit_strbuf_read_ref(c, recv, &rs);
  /* a receiver that can be nil is rendered as its unwrapped oint, never as
     the checked plain read: the `$N` arms take the oint back out of it */
  if (!rs.p && (rt == TY_INT || rt == TY_FLOAT) && node_is_oint(c, recv)) {
    buf_printf(&rs, "%s(", oint_arg(rt)); emit_oint_expr(c, recv, rt, &rs); buf_puts(&rs, ")");
  }
  if (!rs.p) emit_expr(c, recv, &rs);
  const char *r = rs.p ? rs.p : "";
  /* A Float receiver that can be nil answers a Float's methods only where
     NilClass has the name:
     to_s/inspect/nil?/to_i/to_f and the identity family. The rest raise
     NoMethodError, where the payload used to ride through (`nil.nan?` was
     true, `nil.abs` read back as nil, `nil.round` a FloatDomainError).
     Only where the #3505 marking says the slot can hold it. */
  Buf rfg; memset(&rfg, 0, sizeof rfg);
  if (rt == TY_FLOAT && name && nullable_int_value(c, recv) &&
      !(sp_streq(name, "to_s") || sp_streq(name, "inspect") || sp_streq(name, "nil?") ||
        sp_streq(name, "to_i") || sp_streq(name, "to_f") || sp_streq(name, "==") ||
        sp_streq(name, "!=") || sp_streq(name, "eql?") || sp_streq(name, "equal?") ||
        sp_streq(name, "hash") || sp_streq(name, "frozen?") || sp_streq(name, "class") ||
        sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?") ||
        sp_streq(name, "respond_to?") || sp_streq(name, "object_id") || sp_streq(name, "dup") ||
        sp_streq(name, "clone") || sp_streq(name, "itself") || sp_streq(name, "!") ||
        sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^") ||
        sp_streq(name, "to_a") || sp_streq(name, "to_h") || sp_streq(name, "<=>") ||
        sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") || sp_streq(name, ">="))) {
    buf_printf(&rfg, "sp_ofloat_val(");
    emit_oint_expr(c, recv, TY_FLOAT, &rfg);
    buf_printf(&rfg, ", \"%s\")", name);
    r = rfg.p;
  }
  else if (rt == TY_FLOAT && name && nullable_int_value(c, recv) &&
           (is_numeric_conversion(name)) && argc == 0) {
    /* nil answers these itself: nil.to_i is 0, nil.to_f is 0.0 */
    int tfr = ++g_tmp;
    buf_printf(b, "({ sp_ofloat _t%d = ", tfr); emit_oint_expr(c, recv, TY_FLOAT, b);
    if (sp_streq(name, "to_i"))
      if (repr_of(c, id).kind == RK_BOXED)
        buf_printf(b, "; _t%d.nil ? sp_box_int(0) : sp_box_f_to_int(_t%d.v); })", tfr, tfr);
      else
        buf_printf(b, "; _t%d.nil ? (sp_int)0 : sp_float_to_i_checked(_t%d.v); })", tfr, tfr);
    else
      buf_printf(b, "; _t%d.nil ? 0.0 : _t%d.v; })", tfr, tfr);
    free(rs.p);
    { *out = 1; return 1; }
  }
  /* A String-typed receiver that resolved to a poly nil -- e.g. an
     unresolvable chain like `Rails.application.class.to_s` in a method that
     is compiled but never called -- emits sp_box_nil(); coerce it to a
     const char* (yields "" at runtime) so the string ops below type-check. */
  if (rt == TY_STRING && sp_streq(share_check_unmarked(r), "sp_box_nil()")) r = "sp_poly_to_s(sp_box_nil())";
  /* Same shape, but the unresolved-call gate raised (SPINEL_GATE_RAISE): its
     sp_raise_nomethod(...) is a side-effecting poly value, so coerce it (the
     raise diverges before the result is read) rather than feed the raw
     sp_RbVal into a const char* string op. */
  else if (rt == TY_STRING && strncmp(share_check_unmarked(r), "sp_raise_nomethod(", 18) == 0) {
    Buf cb; memset(&cb, 0, sizeof cb); buf_printf(&cb, "sp_poly_to_s(%s)", r); r = cb.p ? cb.p : r;
  }
  /* A receiver whose nil flag is set IS nil, and CRuby's nil answers only
     the names NilClass defines -- every other name is a NoMethodError. The
     arms below read the value as an ordinary number, so `h["zz"].succ`
     answered -9223372036854775807 and `h["zz"].bit_length` answered 63,
     silently. #4070 spelled the check out per name (to_s,
     inspect, to_i, to_f) and the names it did not reach kept the old
     behaviour; this asks once, in front of all of them. Only a receiver the
     compiler already knows to be nullable pays for the test, so the hot int
     path is unchanged, and a safe-navigation call is left alone -- there the
     nil arm is the point. */
  Buf gbody; memset(&gbody, 0, sizeof gbody);
  Buf *g_outer_b = NULL; int g_tmpid = 0; char g_rname[24];
  /* where the statement prelude stood when the call's body began: an arm
     that hoists a loop (a block call) binds the receiver there, so the held
     temp is declared at that point rather than inside the expression */
  size_t g_pre_mark = g_pre ? g_pre->len : 0;
  /* A String receiver that is a fresh copy (a shared slot's reader) and is
     never nil still needs the bound, rooted temp when an argument may
     allocate: the guard below is the only path that holds it (g_noguard:
     the hold without the nil test) */
  int g_noguard = 0;
  if (rt == TY_STRING && name && recv >= 0 && operand_may_allocate(c, recv) &&
      repr_of(c, recv).as_ty != TY_STRBUF &&   /* a handle is no fresh copy, and no const char * */
      !((!nil_answers_name(name)) && recv_may_be_sentinel(c, recv))) {
    for (int ai = 0; ai < argc && !g_noguard; ai++) g_noguard = operand_may_allocate(c, argv[ai]);
    const char *sop_n = nt_str(nt, id, "call_operator");
    if (sop_n && sp_streq(sop_n, "&.")) g_noguard = 0;
    if (g_noguard) {
      g_tmpid = ++g_tmp;
      snprintf(g_rname, sizeof g_rname, "_t%d", g_tmpid);
      g_outer_b = b; b = &gbody; r = g_rname;
      if (g_conv_hold) g_conv_hold->guarded = 1;
    }
  }
  if (!g_noguard && (rt == TY_INT || rt == TY_STRING) && name && recv >= 0 && !nil_answers_name(name) &&
      recv_may_be_sentinel(c, recv)) {
    const char *sop_g = nt_str(nt, id, "call_operator");
    if (!(sop_g && sp_streq(sop_g, "&."))) {
      /* Bind the receiver once and let every arm below read the temp: some
         of them fold the call to a constant (`size` is sizeof(sp_int)) or to
         the receiver itself (`numerator`) and never render the receiver
         text at all, so a guard spliced into that text would vanish. The
         arms emit into gbody and the guard wraps whatever they produced. */
      g_tmpid = ++g_tmp;
      snprintf(g_rname, sizeof g_rname, "_t%d", g_tmpid);
      g_outer_b = b; b = &gbody; r = g_rname;
      /* conversions the arms emit belong BELOW the guard's nil check --
         CRuby raises its NoMethodError without asking #to_str -- so the
         call-level hold, which would hoist them above it, stands down and
         they render inline inside the guarded body */
      if (g_conv_hold) g_conv_hold->guarded = 1;
    }
  }
  int handled = 1;

  if (rt == TY_STRING) {
    /* blockless "a".upto("c") materializes the succ-sequence as an array */
    if (sp_streq(name, "upto") && argc == 1 && nt_ref(nt, id, "block") < 0) {
      buf_printf(b, "sp_StrArray_from_string_range(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ", 0)");
    }
    else if (str_arms_poly_pattern(c, id, b, nt, name, recv, argc, argv, r)) ;
    else if (str_arms_pattern(c, id, b, nt, name, argc, argv, r)) ;
    /* the receiver is a spinel string, so its own byte length is what the
       symbol's name is -- a NUL in it is a byte of the name (#nul) */
    /* the arms that read only the receiver text and the arguments:
       builtin-op rows (builtin_ops.c) */
    else if (emit_builtin_op_text(c, id, recv, TY_STRING, name, r, b)) ;
    /* a length is never nil: lifted where the call's slot holds its nil (a
       site of a call whose other receivers can answer nil) */
    else if (is_len_alias(name) && argc == 0 && node_is_oint(c, id)) {
      buf_puts(b, "sp_oint_of("); str_arms_case_search(c, b, nt, name, recv, argc, argv, r); buf_puts(b, ")");
    }
    else if (str_arms_case_search(c, b, nt, name, recv, argc, argv, r)) ;
    else if (str_arms_slice_encode(c, id, b, name, recv, argc, argv, r)) ;
    else if (sp_streq(name, "delete") && argc == 0) { buf_printf(b, "(%s)", r); { *out = 1; return 1; } }
    else if (sp_streq(name, "count") && argc == 0) { buf_printf(b, "(sp_raise_cls(\"TypeError\", \"no implicit conversion of nil into String\"), 0LL)"); { *out = 1; return 1; } }
    else if (str_arms_convert(c, id, b, nt, name, recv, argc, argv, a0, r)) ;
    else handled = 0;
  }
  else if (rt == TY_INT) {
    /* the arms that read only the receiver and the arguments: builtin-op
       rows (builtin_ops.c), behind the promote-mode answers that leave the word */
    if (int_arms_promote(c, id, b, name, argc, argv, r)) ;
    else if (emit_builtin_op_text(c, id, recv, rt, name, r, b)) ;
    else if (int_arms_round_divide(c, id, b, nt, name, argc, argv, r)) ;
    else if (int_arms_clamp_pow(c, id, b, nt, name, recv, argc, argv, a0, r)) ;
    else handled = 0;
  }
  else { /* TY_FLOAT */
    /* round/ceil/floor/truncate(n>0) -> Float to n decimals; else Integer.
       A non-literal ndigits can't be classified statically; compute the exact
       value at runtime, typed Float (see infer_method_name_type / FLOAT-ROUNDING). */
    int ndig = 0;
    int nonlit = 0;
    /* round(half: :even/:down/:up): tie-break mode as a trailing keyword,
       with or without a digits argument. The keyword hash is peeled off
       the positional view. */
    const char *half_fn = NULL;
    int half_dyn = -1;
    int eff_argc = argc;
    RoundKw kw; memset(&kw, 0, sizeof kw); kw.half = -1;
    int has_kwh = (argc == 1 || argc == 2) && nt_type(c->nt, argv[argc - 1]) &&
                  sp_streq(nt_type(c->nt, argv[argc - 1]), "KeywordHashNode") &&
                  is_round_family(name);
    if (has_kwh) round_kw_read(c, argv[argc - 1], &kw);
    /* Only #round takes a tie-break mode; the other three reject the hash
       outright, and with a digit count as well it is the arity CRuby
       complains about first (#3646). The receiver, the digit count and
       every keyword value are still evaluated: the hash is built before
       the call rejects it. */
    if (has_kwh && !sp_streq(name, "round")) {
      buf_printf(b, "({ (void)(%s); ", r);
      if (argc == 2) { buf_puts(b, "(void)("); emit_int_expr(c, argv[0], b); buf_puts(b, "); "); }
      emit_round_kw_effects(c, &kw, b);
      if (argc == 2)
        buf_puts(b, "sp_raise_cls(\"ArgumentError\", \"wrong number of arguments"
                    " (given 2, expected 0..1)\"); 0.0; })");
      else buf_puts(b, "sp_raise_cls(\"TypeError\","
                       " \"no implicit conversion of Hash into Integer\"); 0.0; })");
      { *out = 1; return 1; }
    }
    if (has_kwh) {
      eff_argc = argc - 1;
      /* A mode written as a literal :even / :down / :up is settled here and
         the plain arms below answer the call. Everything else -- a String, a
         Symbol out of a variable, a `**` source, an unknown keyword -- is
         settled at run time, by the same helpers #4701 gave the boxed path,
         so the two spellings of a mode cannot disagree. */
      const char *hty = kw.half >= 0 ? nt_type(c->nt, kw.half) : NULL;
      int lit = !kw.nunknown && kw.nelem <= 1 && kw.nsplat == 0 &&
                (kw.half < 0 ||
                 (hty && (sp_streq(hty, "SymbolNode") || sp_streq(hty, "NilNode"))));
      const char *hm = (lit && hty && sp_streq(hty, "SymbolNode"))
                         ? nt_str(c->nt, kw.half, "value") : NULL;
      /* promote widens `round(half: …)` with no digit count (or a literal
         0) to a boxed Integer, as it widens the keyword-less `round`
         (#4688). The literal-mode arms below answer a raw sp_int, so that
         shape takes the run-time route, which is the one that can hand
         back a Bignum. */
      int pv_nd0 = eff_argc == 0 ||
                   (eff_argc == 1 && nt_type(c->nt, argv[0]) &&
                    sp_streq(nt_type(c->nt, argv[0]), "IntegerNode") &&
                    nt_int(c->nt, argv[0], "value", 0) == 0);
      if (!lit || (g_promote_mode && pv_nd0)) half_dyn = 1;
      else if (!hm) { /* no mode, or `half: nil`: the plain half-up default */ }
      else if (sp_streq(hm, "even")) half_fn = "sp_round_half_even";
      else if (sp_streq(hm, "down")) half_fn = "sp_round_half_down";
      else if (sp_streq(hm, "up")) half_fn = "round";
      else {
        /* any other name is CRuby's ArgumentError, not the default (#3647) */
        buf_printf(b, "({ (void)(%s); sp_raise_cls(\"ArgumentError\","
                      " sp_sprintf(\"invalid rounding mode: %%s\", ", r);
        emit_str_literal(b, hm);
        buf_puts(b, ")); 0.0; })");
        { *out = 1; return 1; }
      }
    }
    if (half_dyn >= 0) {
      /* CRuby evaluates the receiver, the digit count and every keyword
         value before the call decides anything, so they are bound in that
         order and only then read. */
      int tv = ++g_tmp, tn = -1, nd_float = 0;
      buf_printf(b, "({ double _t%d = (%s); ", tv, r);
      if (eff_argc == 1) {
        tn = ++g_tmp;
        nd_float = emit_round_precision_bind(c, argv[0], tn, b);
      }
      int tm = emit_round_kw_binds(c, &kw, b);
      int pv_wide = g_promote_mode && (eff_argc == 0 ||
                      (nt_type(c->nt, argv[0]) &&
                       sp_streq(nt_type(c->nt, argv[0]), "IntegerNode") &&
                       nt_int(c->nt, argv[0], "value", 0) == 0));
      /* the widened form rounds at the decimal point, so a literal 0 digit
         count is bound (CRuby evaluates it) and then has nothing to say */
      if (pv_wide && tn >= 0) buf_printf(b, "(void)_t%d; ", tn);
      const char *ndl = (eff_argc == 1 && nt_type(c->nt, argv[0]) &&
                         sp_streq(nt_type(c->nt, argv[0]), "IntegerNode"))
                        ? nt_type(c->nt, argv[0]) : NULL;
      int nd_lit = ndl ? (int)nt_int(c->nt, argv[0], "value", 0) : 0;
      /* the class follows the digit count exactly as the literal-mode arms
         below choose it: Float above the decimal point, Integer at or below
         it, and a boxed choice when the count is only known at run time */
      const char *fn = pv_wide       ? "sp_float_round_half_p"
                     : eff_argc == 0 ? "sp_float_round_half_i"
                     : !ndl          ? "sp_float_round_half_v"
                     : nd_lit > 0    ? "sp_float_round_half_f"
                                     : "sp_float_round_half_i";
      buf_printf(b, "%s(_t%d", fn, tv);
      if (!pv_wide) {
        buf_puts(b, ", ");
        if (tn >= 0) buf_printf(b, nd_float ? "sp_float_arg_i(_t%d)" : "_t%d", tn); else buf_puts(b, "0");
      }
      if (tm >= 0) buf_printf(b, ", _t%d); })", tm);
      else buf_puts(b, ", sp_box_nil()); })");
      { *out = 1; return 1; }
    }
    if (is_round_family(name) && eff_argc == 1) {
      const char *aty = nt_type(c->nt, argv[0]);
      if (aty && sp_streq(aty, "IntegerNode")) ndig = (int)nt_int(c->nt, argv[0], "value", 0);
      else nonlit = 1;
    }
    const char *cfn = sp_streq(name, "floor") ? "floor" : sp_streq(name, "ceil") ? "ceil"
                    : sp_streq(name, "truncate") ? "trunc" : "round";
    /* A POSITIVE digit count goes through the runtime helper: scaling by a
       power of ten and rounding the product answers a decimal short when the
       product's own representation error crosses the tie (#3983). */
    const char *precop = sp_streq(name, "floor") ? "SP_PREC_FLOOR"
                       : sp_streq(name, "ceil") ? "SP_PREC_CEIL"
                       : sp_streq(name, "truncate") ? "SP_PREC_TRUNC" : "SP_PREC_ROUND";
    if (half_fn) cfn = half_fn;
    if (half_fn && sp_streq(half_fn, "sp_round_half_even")) precop = "SP_PREC_HALF_EVEN";
    else if (half_fn && sp_streq(half_fn, "sp_round_half_down")) precop = "SP_PREC_HALF_DOWN";
    /* Float#div into the boxed slot promote mode gives it: the Integer
       floor, a Bignum past the word, where the row's sp_float_div_i raises */
    if (is_div_name(name) && argc == 1 && repr_of(c, id).kind == RK_BOXED) {
      int tx = ++g_tmp, ty = ++g_tmp;
      buf_printf(b, "({ sp_float _t%d = (%s); sp_float _t%d = ", tx, r, ty);
      emit_float_expr(c, argv[0], b);
      buf_printf(b, "; sp_float_div_v(_t%d, _t%d); })", tx, ty);
    }
    /* the arms that read only the receiver and the arguments: builtin-op
       rows (builtin_ops.c) */
    else if (emit_builtin_op_text(c, id, recv, rt, name, r, b)) ;
    else if (is_round_family(name)) {
      if (nonlit) {
        /* The class depends on the runtime ndigits: Float when n > 0, Integer
           when n <= 0 (CRuby). Choose at runtime and return a boxed poly. */
        int tn = ++g_tmp, tv = ++g_tmp;
        buf_printf(b, "({ sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; double _t%d = (%s); (_t%d > 0)", tv, r, tn);
        buf_printf(b, " ? sp_box_float(sp_float_prec_op(_t%d, _t%d, %s))", tv, tn, precop);
        buf_printf(b, " : ({ if (isinf(_t%d)) sp_raise_cls(\"FloatDomainError\", _t%d > 0 ? \"Infinity\" : \"-Infinity\");"
                      " if (isnan(_t%d)) sp_raise_cls(\"FloatDomainError\", \"NaN\");"
                      " double _f = pow(10, (double)(-_t%d)); sp_box_int(isinf(_f) ? 0 : sp_float_fit_i(%s(_t%d / _f) * _f)); }); })",
                   tv, tv, tv, tn, cfn, tv);
      }
      else if (ndig > 0 && sp_streq(name, "round")) {
        /* CRuby normalizes a nonzero value that rounds to zero to +0.0
           (a genuine -0.0 input keeps its sign) (#3235). */
        int tx = ++g_tmp;
        /* the tie-break mode applies here too: this branch hard-coded the
           default rounding, so `half:` was silently ignored (#3647) */
        buf_printf(b, "({ double _t%d = (%s);"
                      " double _r = sp_float_prec_op(_t%d, %d, %s);"
                      " (_t%d != 0.0 && _r == 0.0) ? 0.0 : _r; })",
                   tx, r, tx, ndig, precop, tx);
      }
      else if (ndig > 0)
        buf_printf(b, "sp_float_prec_op((%s), %d, %s)", r, ndig, precop);
      else if (ndig < 0) {  /* round to a power of ten left of the decimal -> Integer */
        int tg = ++g_tmp;
        buf_printf(b, "({ double _t%d = (%s);"
                      " if (isinf(_t%d)) sp_raise_cls(\"FloatDomainError\", _t%d > 0 ? \"Infinity\" : \"-Infinity\");"
                      " if (isnan(_t%d)) sp_raise_cls(\"FloatDomainError\", \"NaN\");"
                      " double _f = pow(10, %d); sp_float_fit_i(%s(_t%d / _f) * _f); })",
                   tg, r, tg, tg, tg, -ndig, cfn, tg);
      }
      else {
        int tg = ++g_tmp;
        buf_printf(b, "({ double _t%d = (%s);"
                      " if (isinf(_t%d)) sp_raise_cls(\"FloatDomainError\", _t%d > 0 ? \"Infinity\" : \"-Infinity\");"
                      " if (isnan(_t%d)) sp_raise_cls(\"FloatDomainError\", \"NaN\");"
                      " %s(%s(_t%d)); })",
                   tg, r, tg, tg, tg,
                   repr_of(c, id).kind == RK_BOXED ? "sp_box_f_to_int" : "sp_float_fit_i",
                   cfn, tg);
      }
    }
    else if (sp_streq(name, "clamp") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT_RANGE &&
             nt_type(nt, unwrap_parens(c, argv[0])) && !sp_streq(nt_type(nt, unwrap_parens(c, argv[0])), "RangeNode")) {
      /* Float#clamp(float_range) held in a variable: clamp against sp_FloatRange
         (boxed, matching the literal path and TY_POLY inference). */
      int tf = ++g_tmp;
      buf_printf(b, "({ double _t%d = (%s); sp_FloatRange _fr%d = ", tf, r, tf); emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_box_float(_t%d < _fr%d.first ? _fr%d.first : (_t%d > _fr%d.last ? _fr%d.last : _t%d)); })",
                 tf, tf, tf, tf, tf, tf, tf);
    }
    else if (sp_streq(name, "clamp") && argc == 1 &&
             (comp_ntype(c, argv[0]) == TY_RANGE || comp_ntype(c, argv[0]) == TY_FLOAT_RANGE)) {
      /* the clamped-to bound is the range's endpoint itself (keeping its
         own class); an in-range receiver stays the Float. A literal range
         with a Float bound cannot ride sp_Range (sp_int bounds truncate
         it), so it clamps against typed endpoint temps directly. */
      int rn3 = unwrap_parens(c, argv[0]);
      int is_lit = rn3 >= 0 && nt_type(nt, rn3) && sp_streq(nt_type(nt, rn3), "RangeNode");
      int flo = is_lit ? nt_ref(nt, rn3, "left") : -1;
      int fhi = is_lit ? nt_ref(nt, rn3, "right") : -1;
      int any_f = is_lit && (comp_ntype(c, argv[0]) == TY_FLOAT_RANGE ||
                             (flo >= 0 && comp_ntype(c, flo) == TY_FLOAT) ||
                             (fhi >= 0 && comp_ntype(c, fhi) == TY_FLOAT));
      if (any_f) {
        int excl3 = (int)(nt_int(nt, rn3, "flags", 0) & 4) ? 1 : 0;
        int tf3 = ++g_tmp, tlo = -1, thi = -1;
        int lo_f = flo >= 0 && comp_ntype(c, flo) == TY_FLOAT;
        int hi_f = fhi >= 0 && comp_ntype(c, fhi) == TY_FLOAT;
        buf_printf(b, "({ double _t%d = (%s);", tf3, r);
        if (flo >= 0) {
          tlo = ++g_tmp;
          buf_printf(b, " %s _t%d = ", lo_f ? "double" : "sp_int", tlo);
          emit_expr(c, flo, b); buf_puts(b, ";");
        }
        if (fhi >= 0) {
          thi = ++g_tmp;
          buf_printf(b, " %s _t%d = ", hi_f ? "double" : "sp_int", thi);
          emit_expr(c, fhi, b); buf_puts(b, ";");
        }
        if (excl3 && fhi >= 0)
          buf_puts(b, " sp_raise_cls(\"ArgumentError\", \"cannot clamp with an exclusive range\");");
        /* a NaN receiver compares with no bound: CRuby names the begin, or
           the end of a beginless range */
        if (flo >= 0 || fhi >= 0) {
          int nb = flo >= 0 ? tlo : thi, nf = flo >= 0 ? lo_f : hi_f;
          buf_printf(b, " if (_t%d != _t%d) sp_raise_cls(\"ArgumentError\", sp_sprintf(\"comparison of Float with %%s failed\", %s(_t%d)));",
                     tf3, tf3, nf ? "sp_float_to_s" : "sp_int_to_s", nb);
        }
        buf_puts(b, " ");
        if (flo >= 0)
          buf_printf(b, "(_t%d < (double)_t%d) ? %s(_t%d) : ", tf3, tlo,
                     lo_f ? "sp_box_float" : "sp_box_int", tlo);
        if (fhi >= 0)
          buf_printf(b, "(_t%d > (double)_t%d) ? %s(_t%d) : ", tf3, thi,
                     hi_f ? "sp_box_float" : "sp_box_int", thi);
        buf_printf(b, "sp_box_float(_t%d); })", tf3);
      }
      else {
        int tf2 = ++g_tmp, trg2 = ++g_tmp;
        /* sp_float_clamp_range: an end written as a Float clamps to that Float */
        buf_printf(b, "({ double _t%d = (%s); sp_Range _t%d = ", tf2, r, trg2);
        emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_float_clamp_range(_t%d, _t%d); })", tf2, trg2);
      }
    }
    else if (sp_streq(name, "to_i"))  buf_printf(b, repr_of(c, id).kind == RK_BOXED ? "sp_box_f_to_int(%s)" : "sp_float_to_i_checked(%s)", r);
    else if (sp_streq(name, "divmod") && argc == 1) {
      /* Float#divmod(n) -> [Integer quotient, Float mod], CRuby's flodivmod */
      int tx = ++g_tmp, tn = ++g_tmp, tq = ++g_tmp, tm = ++g_tmp, o = ++g_tmp;
      buf_printf(b, "({ sp_float _t%d = (%s); sp_float _t%d = ", tx, r, tn);
      emit_coerce(c, argv[0], TY_FLOAT, CO_CONVERT, "a Float operand", b);
      buf_printf(b, "; if (isnan(_t%d) || isnan(_t%d)) sp_raise_cls(\"FloatDomainError\", \"NaN\");"
                    /* an infinite dividend has no quotient: FloatDomainError (#3008) */
                    " if (isinf(_t%d)) sp_raise_cls(\"FloatDomainError\", _t%d > 0 ? \"Infinity\" : \"-Infinity\");"
                    " if (_t%d == 0.0) sp_raise_cls(\"ZeroDivisionError\", \"divided by 0\");"
                    " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " if (isinf(_t%d)) {"
                    /* an infinite divisor: same sign -> [0, x], opposite -> [-1, divisor] */
                    " if (_t%d == 0.0 || (_t%d > 0) == (_t%d > 0)) {"
                    " sp_PolyArray_push(_t%d, sp_box_int(0)); sp_PolyArray_push(_t%d, sp_box_float(_t%d)); }"
                    "\nelse { sp_PolyArray_push(_t%d, sp_box_int(-1)); sp_PolyArray_push(_t%d, sp_box_float(_t%d)); } }"
                    "\nelse {"
                    /* CRuby's flodivmod: the quotient follows the remainder */
                    " double _t%d, _t%d; sp_flo_divmod(_t%d, _t%d, &_t%d, &_t%d);"
                    " sp_PolyArray_push(_t%d, sp_box_int(sp_float_fit_i(_t%d)));"
                    " sp_PolyArray_push(_t%d, sp_box_float(_t%d)); } _t%d; })",
                 tx, tn, tx, tx, tn,
                 o, o,
                 tn,
                 tx, tx, tn,
                 o, o, tx,
                 o, o, tn,
                 tq, tm, tx, tn, tq, tm,
                 o, tq,
                 o, tm, o);
    }
    else if (sp_streq(name, "to_int")) buf_printf(b, repr_of(c, id).kind == RK_BOXED ? "sp_box_f_to_int(%s)" : "sp_float_to_i_checked(%s)", r);  /* alias of to_i (#2317); raises on Inf/NaN */
    /* a nil bound is an open side: clamp one-sided (or return the receiver),
       boxed so the chosen operand keeps its class (#2588) */
    else if (sp_streq(name, "clamp") && argc == 2 &&
             (comp_ntype(c, argv[0]) == TY_NIL || comp_ntype(c, argv[1]) == TY_NIL)) {
      buf_printf(b, "sp_num_clamp_open(sp_box_float(%s), ", r); emit_boxed(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
    }
    /* a Rational bound: box the operands and clamp through sp_num_clamp, which
       understands Rational and returns the applied operand unchanged (#3232) */
    else if (sp_streq(name, "clamp") && argc == 2 &&
             (comp_ntype(c, argv[0]) == TY_RATIONAL || comp_ntype(c, argv[1]) == TY_RATIONAL)) {
      buf_printf(b, "sp_num_clamp(sp_box_float(%s), ", r); emit_boxed(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
    }
    /* Float#clamp with float bounds always yields a float (the returned bound
       is itself a float), so emit only when both bounds are float-typed; the
       mixed-bound case (int bound returned as Integer) is poly and left alone.
       Mirrors the inference condition in analyze_infer.c. */
    else if (sp_streq(name, "clamp") && argc == 2 &&
             comp_ntype(c, argv[0]) == TY_FLOAT && comp_ntype(c, argv[1]) == TY_FLOAT) {
      buf_printf(b, "sp_float_clamp_ck(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
    }
    else if (sp_streq(name, "clamp") && argc == 2 &&
             (comp_ntype(c, argv[0]) == TY_INT || comp_ntype(c, argv[0]) == TY_FLOAT) &&
             (comp_ntype(c, argv[1]) == TY_INT || comp_ntype(c, argv[1]) == TY_FLOAT)) {
      /* mixed-class bounds: the applied bound keeps its own class, so the
         result is boxed (0.5.clamp(1, 3) is the Integer 1) */
      int lo_f2 = comp_ntype(c, argv[0]) == TY_FLOAT;
      int hi_f2 = comp_ntype(c, argv[1]) == TY_FLOAT;
      int tf4 = ++g_tmp, tlo2 = ++g_tmp, thi2 = ++g_tmp;
      buf_printf(b, "({ double _t%d = (%s); %s _t%d = ", tf4, r, lo_f2 ? "double" : "sp_int", tlo2);
      emit_expr(c, argv[0], b);
      buf_printf(b, "; %s _t%d = ", hi_f2 ? "double" : "sp_int", thi2);
      emit_expr(c, argv[1], b);
      buf_printf(b, "; if ((double)_t%d > (double)_t%d)"
                    " sp_raise_cls(\"ArgumentError\", \"min argument must be less than or equal to max argument\");"
                    " (_t%d < (double)_t%d) ? %s(_t%d)"
                    " : (_t%d > (double)_t%d) ? %s(_t%d)"
                    " : sp_box_float(_t%d); })",
                 tlo2, thi2,
                 tf4, tlo2, lo_f2 ? "sp_box_float" : "sp_box_int", tlo2,
                 tf4, thi2, hi_f2 ? "sp_box_float" : "sp_box_int", thi2,
                 tf4);
    }
    else if (sp_streq(name, "coerce") && argc == 1) {
      TyKind a0 = comp_ntype(c, argv[0]);
      int ta = ++g_tmp, o = ++g_tmp;
      if (a0 == TY_RATIONAL) {
        buf_printf(b, "({ sp_float _t%d = sp_rational_to_f(", ta); emit_expr(c, argv[0], b);
        buf_printf(b, "); sp_FloatArray *_t%d = sp_FloatArray_new();"
                      " sp_FloatArray_push(_t%d, _t%d);"
                      " sp_FloatArray_push(_t%d, (%s)); _t%d; })", o, o, ta, o, r, o);
      }
      else if (a0 == TY_COMPLEX) {
        /* a real-valued Complex coerces to its real part; an imaginary
           component can't become a Float (CRuby raises RangeError) */
        int tc9 = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", tc9); emit_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d.im != 0) sp_raise_cls(\"RangeError\", \"can't convert complex into Float\");"
                      " sp_FloatArray *_t%d = sp_FloatArray_new();"
                      " sp_FloatArray_push(_t%d, _t%d.re);"
                      " sp_FloatArray_push(_t%d, (%s)); _t%d; })", tc9, o, o, tc9, o, r, o);
      }
      else if (a0 == TY_INT) {
        buf_printf(b, "({ sp_int _t%d = ", ta); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; sp_FloatArray *_t%d = sp_FloatArray_new();"
                      " sp_FloatArray_push(_t%d, (sp_float)_t%d);"
                      " sp_FloatArray_push(_t%d, (%s)); _t%d; })", o, o, ta, o, r, o);
      }
      /* Float#coerce is [Float(other), self], and Float() is where CRuby's
         errors come from: a nil answered a coerced 0.0 before (#4011). */
      else if (a0 != TY_FLOAT && a0 != TY_BIGINT && a0 != TY_UNKNOWN) {
        buf_printf(b, "({ sp_float _t%d = sp_poly_Float(", ta); emit_boxed(c, argv[0], b);
        buf_printf(b, "); sp_FloatArray *_t%d = sp_FloatArray_new();"
                      " sp_FloatArray_push(_t%d, _t%d);"
                      " sp_FloatArray_push(_t%d, (%s)); _t%d; })", o, o, ta, o, r, o);
      }
      else {
        buf_printf(b, "({ sp_float _t%d = ", ta);
        emit_coerce(c, argv[0], TY_FLOAT, CO_CONVERT, "a Float operand", b);
        buf_printf(b, "; sp_FloatArray *_t%d = sp_FloatArray_new();"
                      " sp_FloatArray_push(_t%d, _t%d);"
                      " sp_FloatArray_push(_t%d, (%s)); _t%d; })", o, o, ta, o, r, o);
      }
    }
    /* fdiv(Complex) is self / c, as Float#/ divides by one: a Complex
       argument went into the Float coercion as a struct */
    else if (sp_streq(name, "fdiv") && argc == 1 && a0 == TY_COMPLEX) {
      buf_printf(b, "sp_complex_div(((sp_Complex){(%s), 0, SP_CPLX_RE_F}), ", r);
      emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (sp_streq(name, "fdiv") && argc == 1) { buf_printf(b, "((%s) / (", r); emit_float_coerce_expr(c, argv[0], b); buf_puts(b, "))"); }
    /* Float#eql?(x): true only when x is itself a Float of equal value (no
       numeric coercion, unlike ==). A float-typed arg compares directly; any
       other arg is boxed and rejected unless it is tagged float at runtime. */
    /* Float#equal?: an unboxed double is an immediate value -- identity IS
       the value, exactly CRuby's flonum behavior (1.0.equal?(1.0) is true). */
    else if ((is_eql_or_equal(name)) && argc == 1) {
      TyKind a0 = comp_ntype(c, argv[0]);
      /* a receiver whose nil flag is set is nil (see Integer#eql?) */
      if (call_returns_nullable_int(c, recv)) {
        buf_printf(b, "sp_poly_%s(sp_box_ofloat(", sp_streq(name, "eql?") ? "eql" : "equal");
        emit_oint_expr(c, recv, TY_FLOAT, b); buf_puts(b, "), ");
        emit_boxed(c, argv[0], b); buf_puts(b, ")");
      }
      else if (a0 == TY_FLOAT) { buf_printf(b, "((%s) == (", r); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
      else {
        int te = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", te); emit_boxed(c, argv[0], b);
        buf_printf(b, "; _t%d.tag == SP_TAG_FLT && _t%d.v.f == (%s); })", te, te, r);
      }
    }
    /* Float#===(x) is #== -- numeric compare for a numeric arg, false for
       anything else (nil / Rational / Complex compare by value) (#2400) */
    else if (sp_streq(name, "===") && argc == 1) {
      TyKind a0q = comp_ntype(c, argv[0]);
      if (a0q == TY_FLOAT || a0q == TY_INT) {
        buf_printf(b, "((%s) == (", r); emit_expr(c, argv[0], b); buf_puts(b, "))");
      }
      else if (a0q == TY_RATIONAL) {
        int tq = ++g_tmp;
        buf_printf(b, "({ sp_Rational _t%d = ", tq); emit_expr(c, argv[0], b);
        buf_printf(b, "; ((double)_t%d.num / (double)_t%d.den) == (%s); })", tq, tq, r);
      }
      else if (a0q == TY_COMPLEX) {
        int tq = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", tq); emit_expr(c, argv[0], b);
        buf_printf(b, "; _t%d.im == 0.0 && _t%d.re == (%s); })", tq, tq, r);
      }
      else if (a0q == TY_POLY) {
        int tq = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", tq); emit_boxed(c, argv[0], b);
        buf_printf(b, "; sp_poly_eq(_t%d, sp_box_float(%s)); })", tq, r);
      }
      else {
        buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
      }
    }
    else handled = 0;
  }
  if (g_outer_b) {
    Buf *ib = b; b = g_outer_b;
    if (handled) {
      /* the String's nil is the NULL pointer; an Integer's is its oint's
         flag, read through sp_oint_val (nil's NoMethodError). A String
         receiver can be a fresh copy (a shared slot's reader, a method's
         result) that only this temp holds: it is rooted when an argument,
         evaluated inside the call below, may allocate. */
      int g_root = 0;
      if (rt == TY_STRING)
        for (int ai = 0; ai < argc && !g_root; ai++) g_root = operand_may_allocate(c, argv[ai]);
      if (rt == TY_STRING && g_pre && pre_names_tmp(g_pre, g_pre_mark, g_tmpid)) {
        /* the body's prelude reads the held temp: declare it there */
        Buf hd; memset(&hd, 0, sizeof hd);
        buf_printf(&hd, "const char *_t%d = (%s); SP_GC_ROOT(_t%d);", g_tmpid, rs.p ? rs.p : "", g_tmpid);
        if (!g_noguard) buf_printf(&hd, " if (!_t%d) sp_raise_nomethod(sp_nomethod_msg(\"%s\", sp_box_nil()));", g_tmpid, name);
        buf_puts(&hd, "\n");
        buf_insert_at(g_pre, g_pre_mark, hd.p);
        free(hd.p);
        buf_puts(b, "({ ");
      }
      else if (rt == TY_STRING) {
        buf_printf(b, "({ const char *_t%d = (%s); ", g_tmpid, rs.p ? rs.p : "");
        if (g_root) buf_printf(b, "SP_GC_ROOT(_t%d); ", g_tmpid);
        if (!g_noguard) buf_printf(b, "if (!_t%d) sp_raise_nomethod(sp_nomethod_msg(\"%s\", sp_box_nil())); ", g_tmpid, name);
      }
      else {
        buf_printf(b, "({ sp_int _t%d = ", g_tmpid);
        emit_oint_unwrap_ck(c, recv, TY_INT, name, b);
        buf_puts(b, "; ");
      }
      /* a nil arm that left the test to this guard (emit_nil_target_own)
         learns it was written */
      if (c->nil_tested && c->nil_tested[recv] == 3) c->nil_tested[recv] = 4;
      if (ib->p) buf_puts(b, ib->p);
      buf_puts(b, "; })");
    }
    else if (ib->p) buf_puts(b, ib->p);
    free(gbody.p);
  }
  free(rs.p);
  free(rfg.p);
  if (handled) { *out = 1; return 1; }
  return 0;
}

static int emit_scalar_call_arms(Compiler *c, int id, Buf *b) {
  /* Shared-mutable shim (#3227): setbyte on a strbuf local -- shadow-copy
     re-entry, same as emit_array_call's. */
  {
    const NodeTable *ntS = c->nt;
    const char *nmS = nt_str(ntS, id, "name");
    int recvS = nt_ref(ntS, id, "receiver");
    /* setbyte on a handle writes one byte into the handle's OWN buffer. The
       shims below read the whole String out, mutate that copy and append it
       back -- two O(len) passes to write one byte, so a per-row column write
       over a large buffer is quadratic in it. sp_str_setbyte_cow already
       mutates a heap string in place and copies only a static literal, so the
       handle's buffer serves directly and the republish is needed only in
       that one case. */
    /* (--share-strings: a route over a handle, `(+s).setbyte(0, 90)`, too) */
    if (nmS && recvS >= 0 && sp_streq(nmS, "setbyte") &&
        (repr_of(c, recvS).as_ty == TY_STRBUF ||
         (comp_ntype(c, recvS) == TY_STRING && (strbuf_local_name(c, recvS) || repr_share_rule(c))))) {
      int aS = nt_ref(ntS, id, "arguments"); int acS = 0;
      const int *avS = aS >= 0 ? nt_arr(ntS, aS, "arguments", &acS) : NULL;
      char srefB[1024];
      const char *sbnB = strbuf_local_name(c, recvS);
      /* a local's slot too: a captured local's is its cell */
      int haveB = !avS || acS != 2 ? 0
                : sbnB ? strbuf_slot_ref(c, recvS, srefB, sizeof srefB)
                : strbuf_recv_handle(c, id, recvS, srefB, sizeof srefB);
      /* a route's receiver (strbuf_recv_handle's 2) runs here, ahead of the
         arguments, which then run in their order -- the index, then the
         value -- before the frozen check, as in the call */
      if (haveB == 2) {
        int tH = ++g_tmp;
        /* rooted: a route can answer a String of its own (`(+f)` of a frozen
           f), which nothing else holds while the arguments allocate (when
           they can: emit_route_recv_root) */
        buf_printf(b, "({ sp_String *_t%d = %s;", tH, srefB);
        emit_route_recv_root(c, haveB, tH, acS, avS, TY_INT, b);
        buf_printf(b, " sp_int _i%d = ", tH);
        emit_int_expr(c, avS[0], b);
        buf_printf(b, "; sp_int _v%d = ", tH);
        emit_int_expr(c, avS[1], b);
        buf_printf(b, "; if (sp_String_is_frozen(_t%d)) sp_raise_frozen_str(_t%d->data);"
                      " const char *_p%d = sp_String_cstr(_t%d);"
                      " const char *_q%d = sp_str_setbyte_cow(_p%d, _i%d, _v%d);"
                      " if (_q%d != _p%d) sp_String_set_bin(_t%d, _q%d); _v%d; })",
                   tH, tH, tH, tH, tH, tH, tH, tH, tH, tH, tH, tH, tH);
        return 1;
      }
      if (haveB) {
        int tH = ++g_tmp;
        buf_printf(b, "({ sp_String *_t%d = %s;"
                      " if (sp_String_is_frozen(_t%d)) sp_raise_frozen_str(_t%d->data);"
                      " const char *_p%d = sp_String_cstr(_t%d); sp_int _v%d = ",
                   tH, srefB, tH, tH, tH, tH, tH);
        emit_int_expr(c, avS[1], b);
        buf_printf(b, "; const char *_q%d = sp_str_setbyte_cow(_p%d, ", tH, tH);
        emit_int_expr(c, avS[0], b);
        buf_printf(b, ", _v%d);"
                      " if (_q%d != _p%d) sp_String_set_bin(_t%d, _q%d); _v%d; })",
                   tH, tH, tH, tH, tH, tH);
        return 1;
      }
    }
    if (nmS && recvS >= 0 && repr_of(c, recvS).as_ty == TY_STRBUF &&
        sp_streq(nmS, "setbyte") &&
        sb_reader_expr_shim(c, id, recvS, b, emit_scalar_call)) return 1;
    if (nmS && recvS >= 0 && comp_ntype(c, recvS) == TY_STRING &&
        sp_streq(nmS, "setbyte")) {
      if (sb_iv_expr_shim(c, id, recvS, b, emit_scalar_call)) return 1;
      if (repr_static_read_kind(nt_kind(ntS, recvS)) &&
          sb_reader_expr_shim(c, id, recvS, b, emit_scalar_call)) return 1;
      const char *sbn = strbuf_local_name(c, recvS);
      /* the handle's slot through the holder (a captured local's cell) */
      char srefL[1024];
      if (sbn && g_nren < MAX_RENAME && strbuf_slot_ref(c, recvS, srefL, sizeof srefL)) {
        Scope *shs = comp_scope_of(c, recvS);
        LocalVar *shlv = scope_local(shs, sbn);
        int tH = ++g_tmp;
        Buf armb; memset(&armb, 0, sizeof armb);
        snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", sbn);
        snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_sb%d", tH);
        g_nren++;
        SbShimSave sv_sh = sb_shim_enter(shlv);
        int handled = emit_scalar_call(c, id, &armb);
        sb_shim_leave(shlv, sv_sh);
        g_nren--;
        if (!handled) { free(armb.p); }
        else {
          buf_printf(b, "({ sp_String *_t%d = %s;"
                        " if (sp_String_is_frozen(_t%d)) sp_raise_frozen_str(_t%d->data);"
                        " const char *lv__sb%d = sp_str_concat(sp_String_cstr(_t%d), (&(\"\\xff\")[1]));"
                        " SP_GC_ROOT(lv__sb%d);"
                        " sp_int _res%d = %s;"
                        " sp_String_set_bin(_t%d, lv__sb%d); _res%d; })",
                     tH, srefL, tH, tH, tH, tH, tH,
                     tH, armb.p ? armb.p : "0", tH, tH, tH);
          free(armb.p);
          return 1;
        }
      }
    }
  }
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = comp_recv_type(c, recv);
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  if (recv >= 0 && (rt == TY_STRING || rt == TY_STRBUF) && name &&
      sp_streq(name, "scrub!") && argc <= 1 && emit_scrub_bang(c, recv, rt, argc, argv, b))
    return 1;
  { int r; if (emit_scalar_recv_arms(c, id, b, nt, name, recv, argc, argv, rt, a0, &r)) return r; }
  return 0;
}

/* The class operand for a hierarchy check on `recv`. A synthesized singleton
   subclass is NOT the object's class until the extend / `def obj.m` that made
   it has run, so read the id the object actually carries rather than folding
   the static one (#4084). Everything else keeps the fold, with the receiver
   evaluated for its effects. */
/* Does any user class have `cid` in its superclass chain? Then a slot typed
   `cid` can hold one of them at run time. */
static int class_has_descendants(Compiler *c, int cid) {
  for (int k = 0; k < c->nclasses; k++) {
    if (k == cid) continue;
    for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
      if (p == cid) return 1;
  }
  return 0;
}

/* The class to test a receiver's `is_a?` against. The receiver's STATIC type is
   only an upper bound: a `Base`-typed slot legitimately holds a `Sub`, which is
   the whole point of a subclass, so a class with descendants has to be asked at
   run time. Using the static id there made `is_a?(Sub)` inside a method defined
   on Base answer false for every Sub -- silently, and the identical test written
   at the call site answered true, because there the receiver's type IS Sub
   (#4142). A leaf class is exact, and a value type carries no tag, so both keep
   the constant. */
static void emit_isa_self_class(Compiler *c, int recv, int cid, Buf *b) {
  if (cid >= 0 && cid < c->nclasses && !c->classes[cid].is_value_type &&
      (c->classes[cid].is_singleton_of || class_has_descendants(c, cid))) {
    buf_puts(b, "((sp_Class){("); emit_expr(c, recv, b); buf_puts(b, ")->cls_id})");
    return;
  }
  buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), (sp_Class){%d})", cid);
}

/* `obj.x = v` is an assignment expression: its value is v as written, whatever
   the writer's body returns (`def x=(v); @x = v.to_s; end` still yields v). The
   argument is evaluated once, after the receiver, into a rooted temp; the
   dispatch reads the temp through g_argov, and the temp is the result. A call
   emit_stmt is lowering (g_setter_stmt_id) has no reader for the value and
   emits as before. Returns the temp, or -1 when the call is left alone, and
   the temp's type in *vt_out. */
/* the setter-value temps that hold an oint (a small ring, as fold's) */
static int g_sv_oint[32], g_sv_oint_n = 0;
static int sv_oint_is(int tv) { for (int i = 0; i < 32; i++) if (tv && g_sv_oint[i] == tv) return 1; return 0; }
static int setter_value_open(Compiler *c, int id, Buf *b, TyKind *vt_out) {
  const NodeTable *nt = c->nt;
  int argc; const int *argv = call_args(nt, id, &argc);
  if (id == g_setter_stmt_id || argc != 1 || nt_ref(nt, id, "block") >= 0 ||
      !call_is_setter_assign(nt, id) || g_n_argov >= MAX_ARG_OVERRIDE)
    return -1;
  TyKind vt = repr_of(c, argv[0]).as_ty;
  if (vt == TY_UNKNOWN) return -1;
  /* nil and void have no C storage type of their own: hold them boxed */
  int boxed = (vt == TY_NIL || vt == TY_VOID);
  /* an Integer or Float that can be nil is held with its nil (view_bind_o):
     the writer gets it, and so does the assignment's value */
  int so = !boxed && oint_kind(vt) && node_has_oint_form(c, argv[0]);
  Buf ab; memset(&ab, 0, sizeof ab);
  if (boxed) emit_boxed(c, argv[0], &ab);
  else if (so) emit_oint_expr(c, argv[0], vt, &ab);
  else emit_expr(c, argv[0], &ab);
  int tv = ++g_tmp;
  emit_indent(g_pre, g_indent);
  if (so) buf_puts(g_pre, oint_ctype(vt)); else emit_ctype(c, boxed ? TY_POLY : vt, g_pre);
  buf_printf(g_pre, " _t%d = ", tv);
  buf_puts(g_pre, ab.p ? ab.p : "sp_box_nil()"); buf_puts(g_pre, ";\n");
  free(ab.p);
  if (boxed || vt == TY_POLY) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", tv); }
  else if (needs_root(vt)) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tv); }
  if (so) { view_bind_o(argv[0], "_t%d", tv); g_sv_oint[g_sv_oint_n++ & 31] = tv; }
  else view_bind(argv[0], "_t%d", tv);
  buf_puts(b, "({ (void)(");
  *vt_out = boxed ? TY_POLY : vt;
  return tv;
}
/* The temp holds the argument as the dispatch reads it; the expression
   answers in the CALL's type, which the inference may have widened to poly
   (a class with both an attr_accessor and a `def x=` for the name) -- box
   the temp on the way out then. */
static void setter_value_close(Compiler *c, int id, TyKind vt, Buf *b, int tv) {
  if (tv < 0) return;
  view_unbind(g_n_argov - 1);
  buf_puts(b, "); ");
  if (sv_oint_is(tv)) {
    /* the oint: boxed with its nil, as it is where the consumer takes the
       oint, unwrapped for a plain one */
    if (repr_of(c, id).kind == RK_BOXED) buf_printf(b, "%s(_t%d)", oint_box(vt), tv);
    else if (node_is_oint(c, id) || node_has_oint_form(c, id)) buf_printf(b, "_t%d", tv);
    else buf_printf(b, "%s(_t%d)", oint_arg(vt), tv);
  }
  else if (repr_of(c, id).kind == RK_BOXED && vt != TY_POLY) {
    char tn[32]; snprintf(tn, sizeof tn, "_t%d", tv);
    emit_boxed_text(c, vt, tn, b);
  }
  else buf_printf(b, "_t%d", tv);
  buf_puts(b, "; })");
}

/* sp_obj_clamp takes three boxed operands, and each of them can be a fresh
   allocation: `Ver.new(12).clamp(Ver.new(1)..Ver.new(9))` allocates all three.
   C leaves the evaluations unsequenced, so whichever runs first is held by
   nothing while the others allocate, and a collection there took it -- the
   user `<=>` then ran against freed memory and clamp answered the receiver
   unchanged. Bind the operands to rooted temps in order, and only when more
   than one of them can allocate: a lone allocating operand has nothing to
   outlive. Same rule as #4049. A -1 side is an absent range endpoint, which
   sp_obj_clamp skips as nil. */
static void emit_obj_clamp3(Compiler *c, int recv, int lo, int hi, Buf *b) {
  int na = (recv >= 0 && subtree_may_allocate(c->nt, recv))
         + (lo >= 0 && subtree_may_allocate(c->nt, lo))
         + (hi >= 0 && subtree_may_allocate(c->nt, hi));
  if (na < 2) {
    buf_puts(b, "sp_obj_clamp(");
    emit_boxed(c, recv, b); buf_puts(b, ", ");
    if (lo >= 0) emit_boxed(c, lo, b); else buf_puts(b, "sp_box_nil()");
    buf_puts(b, ", ");
    if (hi >= 0) emit_boxed(c, hi, b); else buf_puts(b, "sp_box_nil()");
    buf_puts(b, ")");
    return;
  }
  int t1 = ++g_tmp, t2 = ++g_tmp;
  buf_puts(b, "({ "); t1 = hold_operand(c, recv, TY_POLY, 1, t1, 1, " ", b);
  buf_printf(b, "sp_RbVal _t%d = ", t2);
  if (lo >= 0) emit_boxed(c, lo, b); else buf_puts(b, "sp_box_nil()");
  buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_obj_clamp(_t%d, _t%d, ", t2, t1, t2);
  if (hi >= 0) emit_boxed(c, hi, b); else buf_puts(b, "sp_box_nil()");
  buf_puts(b, "); })");
}

static void emit_identity_equal(Compiler *c, int recv, int arg, TyKind rt, TyKind a0, Buf *b) {
  if (a0 == rt) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == (");
    emit_expr(c, arg, b); buf_puts(b, "))");
  }
  else if (a0 == TY_POLY) {
    int te = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = ", te); emit_boxed(c, arg, b);
    buf_printf(b, "; _t%d.tag == SP_TAG_OBJ && _t%d.v.p == (void*)(", te, te);
    emit_expr(c, recv, b); buf_puts(b, "); })");
  }
  else { buf_puts(b, "(("); emit_expr(c, arg, b); buf_puts(b, "), 0)"); }
}
/* The root of a Struct receiver's temp `_tN`, for an arm that runs no Ruby
   code between the receiver and its last member read. A receiver made in
   place is held by nothing but the temp; a read of a local, an ivar, self
   or a constant is held where it is and cannot be dropped meanwhile. */
static void emit_struct_recv_root(Compiler *c, int recv, int t, Buf *b) {
  if (!expr_is_held_ref(c, recv)) buf_printf(b, " SP_GC_ROOT(_t%d);", t);
}
/* A Struct instance receiver (emit_object_call's arms, in their order) */
/* The value a Struct `[]=` stores into member slot type mt, from the box
   `vtxt` of a value of static type vt. A boxed member takes the box. A
   member of another type was typed by every store the analysis sees reach
   it (infer_struct_aset_call); a box whose classes it cannot tell types
   none, so there a value that does not fit is refused at run time, a
   TypeError, rather than unboxed as the member's type (an Integer read as a
   String pointer): known by its type where the store is written, tested by
   its tag where it is boxed. A member kind with no such test (a container,
   a Range) is unboxed as before. */
static void emit_struct_member_value(Compiler *c, TyKind mt, TyKind vt, const char *vtxt, Buf *b) {
  if (mt == TY_POLY) { buf_puts(b, vtxt); return; }
  int str = mt == TY_STRING || mt == TY_STRBUF;
  /* a member the share rule holds as the handle: nil is no handle, NULL */
  Buf u; memset(&u, 0, sizeof u);
  if (mt == TY_STRBUF) buf_printf(&u, "(%s.tag == SP_TAG_NIL ? NULL : ", vtxt);
  emit_unbox_text(c, mt, vtxt, &u);
  if (mt == TY_STRBUF) buf_puts(&u, ")");
  int obj = ty_is_object(mt) && !c->classes[ty_object_class(mt)].is_value_type;
  const char *kind = mt == TY_INT ? "Integer" : mt == TY_FLOAT ? "Float" : str ? "String"
                   : mt == TY_SYMBOL ? "Symbol" : mt == TY_BOOL ? "true or false"
                   : obj ? c->classes[ty_object_class(mt)].name : NULL;
  int fits = vt == mt || (vt == TY_NIL && (mt == TY_INT || mt == TY_FLOAT || str || obj)) ||
             (vt == TY_INT && mt == TY_FLOAT) || (str && (vt == TY_STRING || vt == TY_STRBUF)) ||
             (obj && ty_is_object(vt) && is_descendant(c, ty_object_class(vt), ty_object_class(mt)));
  if (fits || !kind) {
    buf_puts(b, u.p ? u.p : "");
    free(u.p);
    return;
  }
  if (vt == TY_POLY || vt == TY_UNKNOWN) {
    buf_puts(b, "((");
    if (mt == TY_INT) buf_printf(b, "%s.tag == SP_TAG_INT || %s.tag == SP_TAG_NIL", vtxt, vtxt);
    else if (mt == TY_FLOAT) buf_printf(b, "%s.tag == SP_TAG_FLT || %s.tag == SP_TAG_INT || %s.tag == SP_TAG_NIL", vtxt, vtxt, vtxt);
    else if (str) buf_printf(b, "%s.tag == SP_TAG_STR || %s.tag == SP_TAG_NIL || sp_poly_is_strbuf(%s)", vtxt, vtxt, vtxt);
    else if (mt == TY_SYMBOL) buf_printf(b, "%s.tag == SP_TAG_SYM", vtxt);
    else if (mt == TY_BOOL) buf_printf(b, "%s.tag == SP_TAG_BOOL", vtxt);
    else {
      buf_printf(b, "%s.tag == SP_TAG_NIL || (%s.tag == SP_TAG_OBJ && (0", vtxt, vtxt);
      for (int k = 0; k < c->nclasses; k++)
        if (k == ty_object_class(mt) || is_descendant(c, k, ty_object_class(mt))) buf_printf(b, " || %s.cls_id == %d", vtxt, k);
      buf_puts(b, "))");
    }
    buf_printf(b, ") ? %s : ", u.p);
  }
  else buf_puts(b, "(");
  buf_printf(b, "(sp_raise_cls(\"TypeError\", sp_sprintf(\"cannot store %%s into a Struct member Spinel typed as %s"
                " (the member was not widened for this store)\", sp_poly_class_name(%s))), %s))", kind, vtxt, u.p);
  free(u.p);
}

static int emit_struct_recv_call(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind res, int *out) {
  /* Struct instance methods (to_h / to_a / values / members / dig). */
  if (!(recv >= 0 && ty_is_object(rt) && c->classes[ty_object_class(rt)].is_struct &&
      /* A method written in the `Struct.new` / `Data.define` block overrides the
         generated one of the same name, as it does in CRuby: `[]` defined there
         has to run instead of the member lookup, which raised NameError for a
         key that is not a member (#3794). A member accessor overrides it too:
         Struct.new(:members) answers the member, not the member names. The
         iterator this file synthesizes for a struct is a method and is served
         by the object path below. */
      comp_resolve_member(c, ty_object_class(rt), name, 0, NULL, NULL) == SP_MEMBER_NONE)) return 0;
  ClassInfo *sc = &c->classes[ty_object_class(rt)];
  /* #inspect / #to_s -> the generated (or user-overridden) struct/data stringifier */
  if ((is_text_conversion(name)) && argc == 0) {
    const char *cn = obj_str_cname(c, ty_object_class(rt), sp_streq(name, "inspect"));
    if (cn) { buf_printf(b, "sp_%s_%s((sp_%s *)", cn, obj_str_mname(c, ty_object_class(rt), sp_streq(name, "inspect")), cn); emit_expr(c, recv, b); buf_puts(b, ")"); { *out = 1; return 1; } }
  }
  int is_to_a = (sp_streq(name, "to_a") || sp_streq(name, "values") || sp_streq(name, "deconstruct"));
  /* CRuby's Data has neither #to_a nor #values (Struct has both); only
     #deconstruct answers its members, and asking for the others is a
     NoMethodError rather than the member list. */
  if (is_to_a && sc->is_data && !sp_streq(name, "deconstruct")) is_to_a = 0;
  if (is_to_a && argc == 0) {
    int t = ++g_tmp; int rt2 = ++g_tmp;
    Buf rb = expr_buf(c, recv);
    buf_printf(b, "({ sp_%s *_t%d = %s;", sc->name, t, rb.p ? rb.p : "");
    emit_struct_recv_root(c, recv, t, b);
    buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", rt2, rt2);
    for (int i = 0; i < sc->nmembers; i++) {
      buf_printf(b, " sp_PolyArray_push(_t%d, ", rt2);
      Buf fb; memset(&fb, 0, sizeof fb); buf_printf(&fb, "_t%d->iv_%s", t, iv_c(sc->ivars[i] + 1));
      emit_member_box(c, sc, i, fb.p ? fb.p : "", b); free(fb.p);
      buf_puts(b, ");");
    }
    buf_printf(b, " _t%d; })", rt2);
    free(rb.p);
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "to_h") && argc == 0) {
    int block = nt_ref(nt, id, "block");
    int t = ++g_tmp, rh = ++g_tmp;
    Buf rb = expr_buf(c, recv);
    Repr hr = repr_of(c, id);
    const char *hn = ty_hash_cname(hr.as_ty);
    if (!hn) hn = "SymPoly";
    buf_printf(b, "({ sp_%s *_t%d = %s; SP_GC_ROOT(_t%d); sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);",
               sc->name, t, rb.p ? rb.p : "", t, hn, rh, hn, rh);
    free(rb.p);
    if (block >= 0) {
      /* to_h { |k, v| [nk, nv] }: per member, bind k/v then set hash[nk] = nv */
      const char *kp = block_param_name(c, block, 0); if (kp) kp = rename_local(kp);
      const char *vp = block_param_name(c, block, 1); if (vp) vp = rename_local(vp);
      int bbody = nt_ref(nt, block, "body");
      int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
      int last = bn > 0 ? bb[bn - 1] : -1;
      int ke = -1, ve = -1;
      if (last >= 0 && nt_type(nt, last) && sp_streq(nt_type(nt, last), "ArrayNode")) {
        int en = 0; const int *els = nt_arr(nt, last, "elements", &en);
        if (en == 2) { ke = els[0]; ve = els[1]; }
      }
      TyKind kt = hr.key, vt = hr.val;
      for (int i = 0; i < sc->nmembers; i++) {
        if (kp) buf_printf(b, " lv_%s = (sp_sym)%d;", kp, comp_sym_intern(c, sc->ivars[i] + 1));
        if (vp) {
          char fb[300]; snprintf(fb, sizeof fb, "_t%d->iv_%s", t, iv_c(sc->ivars[i] + 1));
          buf_printf(b, " lv_%s = ", vp); emit_member_box(c, sc, i, fb, b); buf_puts(b, ";");
        }
        /* a composite key/value (an Array or Hash literal built from the
           block parameters) hoists its construction into the prelude, which
           runs BEFORE these per-member assignments -- so it read stale
           parameters. Emit that setup here, after them (#3603). */
        Buf kpre; memset(&kpre, 0, sizeof kpre);
        Buf kbuf; memset(&kbuf, 0, sizeof kbuf);
        Buf vbuf; memset(&vbuf, 0, sizeof vbuf);
        Buf *sv_pre = g_pre; g_pre = &kpre;
        /* the step's setup and the body's leading statements, which ran
           nowhere */
        emit_block_locals_reset(c, block, &kpre, 0);
        int rd_lbl = emit_iter_step_stmts(c, bbody, &kpre, 0, NULL);
        if (ke >= 0) { if (kt == TY_POLY && repr_of(c, ke).kind != RK_BOXED) emit_boxed(c, ke, &kbuf); else emit_expr(c, ke, &kbuf); }
        if (ve >= 0) { if (vt == TY_POLY && repr_of(c, ve).kind != RK_BOXED) emit_boxed(c, ve, &vbuf); else emit_expr(c, ve, &vbuf); }
        if (rd_lbl) g_redo_depth--;
        g_pre = sv_pre;
        if (kpre.p) { buf_puts(b, " "); buf_puts(b, kpre.p); }
        free(kpre.p);
        buf_printf(b, " sp_%sHash_set(_t%d, ", hn, rh);
        buf_puts(b, kbuf.p ? kbuf.p : "0"); free(kbuf.p);
        buf_puts(b, ", ");
        buf_puts(b, vbuf.p ? vbuf.p : "0"); free(vbuf.p);
        buf_puts(b, ");");
      }
    }
    else {
      for (int i = 0; i < sc->nmembers; i++) {
        buf_printf(b, " sp_SymPolyHash_set(_t%d, (sp_sym)%d, ", rh, comp_sym_intern(c, sc->ivars[i] + 1));
        char fb[300]; snprintf(fb, sizeof fb, "_t%d->iv_%s", t, iv_c(sc->ivars[i] + 1));
        emit_member_box(c, sc, i, fb, b);
        buf_puts(b, ");");
      }
    }
    buf_printf(b, " _t%d; })", rh);
    { *out = 1; return 1; }
  }
  /* values_at with no keys selects nothing, as Array#values_at does */
  if (sp_streq(name, "values_at") && argc == 0) {
    buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_PolyArray_new())");
    { *out = 1; return 1; }
  }
  /* values_at(i, j, ... / range): member values by index, boxed */
  if (sp_streq(name, "values_at") && argc >= 1) {
    int tv4 = ++g_tmp, to4 = ++g_tmp;
    Buf rb4 = expr_buf(c, recv);
    /* built aside: a key the literal walk cannot resolve falls back to the
       runtime form below, and appending to the caller's buffer first would
       leave the abandoned prefix in it */
    Buf lit4; memset(&lit4, 0, sizeof lit4);
    Buf *b4 = &lit4;
    buf_printf(b4, "({ sp_%s *_t%d = %s;", sc->c_name, tv4, rb4.p ? rb4.p : "");
    emit_struct_recv_root(c, recv, tv4, b4);
    buf_printf(b4, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", to4, to4);
    int ok4 = 1;
    for (int a4 = 0; a4 < argc && ok4; a4++) {
      const char *aty4 = nt_type(nt, argv[a4]);
      if (aty4 && sp_streq(aty4, "IntegerNode")) {
        long long ix = nt_int(nt, argv[a4], "value", 0);
        if (ix < 0) ix += sc->nmembers;
        if (ix < 0 || ix >= sc->nmembers) { ok4 = 0; break; }
        char fb4[300]; snprintf(fb4, sizeof fb4, "_t%d->iv_%s", tv4, iv_c(sc->ivars[(int)ix] + 1));
        buf_printf(b4, " sp_PolyArray_push(_t%d, ", to4);
        emit_member_box(c, sc, (int)ix, fb4, b4);
        buf_puts(b4, ");");
      }
      else if (aty4 && sp_streq(aty4, "RangeNode")) {
        int rl4 = nt_ref(nt, argv[a4], "left"), rr4 = nt_ref(nt, argv[a4], "right");
        long long lo4 = rl4 >= 0 && nt_type(nt, rl4) && sp_streq(nt_type(nt, rl4), "IntegerNode")
                          ? nt_int(nt, rl4, "value", 0) : 0;
        long long hi4 = rr4 >= 0 && nt_type(nt, rr4) && sp_streq(nt_type(nt, rr4), "IntegerNode")
                          ? nt_int(nt, rr4, "value", 0) : sc->nmembers - 1;
        if (nt_int(nt, argv[a4], "flags", 0) & 4) hi4--;
        if (lo4 < 0) lo4 += sc->nmembers;
        if (hi4 < 0) hi4 += sc->nmembers;
        /* a Range that runs past the last member pads with nil, the way
           Array#values_at does; the walk used to stop at the last member */
        for (long long ix = lo4; ix <= hi4; ix++) {
          if (ix < 0) continue;
          if (ix >= sc->nmembers) { buf_printf(b4, " sp_PolyArray_push(_t%d, sp_box_nil());", to4); continue; }
          char fb4[300]; snprintf(fb4, sizeof fb4, "_t%d->iv_%s", tv4, iv_c(sc->ivars[(int)ix] + 1));
          buf_printf(b4, " sp_PolyArray_push(_t%d, ", to4);
          emit_boxed_text(c, sc->ivar_types[(int)ix], fb4, b4);
          buf_puts(b4, ");");
        }
      }
      else ok4 = 0;
    }
    if (ok4) {
      buf_printf(b4, " _t%d; })", to4);
      buf_puts(b, lit4.p ? lit4.p : "");
      free(lit4.p); free(rb4.p);
      { *out = 1; return 1; }
    }
    free(lit4.p);
    /* A key the loop above could not resolve at compile time (a local, an
       out-of-range offset, a name) resolves at run time instead of taking
       the whole file down (#3849). The partial output above is discarded by
       re-emitting from scratch. */
    if (!ok4) {
      int tv5 = ++g_tmp, to5 = ++g_tmp;
      Buf rb5; memset(&rb5, 0, sizeof rb5); buf_puts(&rb5, rb4.p ? rb4.p : "");
      free(rb4.p);
      char rtxt[32]; snprintf(rtxt, sizeof rtxt, "_t%d", tv5);
      buf_printf(b, "({ sp_%s *_t%d = %s; SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);",
                 sc->c_name, tv5, rb5.p ? rb5.p : "", tv5, to5, to5);
      free(rb5.p);
      for (int a5 = 0; a5 < argc; a5++) {
        buf_printf(b, " sp_PolyArray_push(_t%d, ", to5);
        emit_struct_member_by_key(c, sc, rtxt, argv[a5], 1, 0, b);
        buf_puts(b, ");");
      }
      buf_printf(b, " _t%d; })", to5);
      { *out = 1; return 1; }
    }
  }
  /* #hash: combine the boxed member hashes so equal-valued structs agree.
     A member literally named `hash` shadows this with its reader (#2975). */
  if (sp_streq(name, "hash") && argc == 0 && comp_ivar_index(sc, "@hash") < 0) {
    int tv5 = ++g_tmp, th5 = ++g_tmp;
    Buf rb5 = expr_buf(c, recv);
    buf_printf(b, "({ sp_%s *_t%d = %s; uint64_t _t%d = 1469598103934665603ULL;",
               sc->c_name, tv5, rb5.p ? rb5.p : "", th5);
    free(rb5.p);
    for (int i5 = 0; i5 < sc->nmembers; i5++) {
      char fb5[300]; snprintf(fb5, sizeof fb5, "_t%d->iv_%s", tv5, iv_c(sc->ivars[i5] + 1));
      buf_printf(b, " _t%d = (_t%d ^ (uint64_t)sp_rbval_hash_key(", th5, th5);
      emit_member_box(c, sc, i5, fb5, b);
      buf_puts(b, ")) * 1099511628211ULL;");
    }
    buf_printf(b, " (sp_int)(_t%d >> 1); })", th5);
    { *out = 1; return 1; }
  }
  if (is_len_alias(name) && argc == 0 && !sc->is_data) {
    char szn[272]; snprintf(szn, sizeof szn, "@%s", name);
    if (comp_ivar_index(sc, szn) < 0) {
      Buf rb = expr_buf(c, recv);
      buf_printf(b, "((void)(%s), %dLL)", rb.p ? rb.p : "0", sc->nmembers);
      free(rb.p);
      { *out = 1; return 1; }
    }
  }
  /* deconstruct_keys([:a, :b]) / deconstruct_keys(nil): the requested
     members (all for nil) as a symbol-keyed hash. */
  if (sp_streq(name, "deconstruct_keys") && argc == 1) {
    int keyed[64]; int nkey = 0; int ok = 1;
    const char *aty = nt_type(nt, argv[0]);
    if (aty && sp_streq(aty, "NilNode")) {
      for (int i = 0; i < sc->nmembers && nkey < 64; i++) keyed[nkey++] = i;
    }
    else if (aty && sp_streq(aty, "ArrayNode")) {
      int en = 0; const int *els = nt_arr(nt, argv[0], "elements", &en);
      for (int e = 0; e < en && ok; e++) {
        const char *ety = nt_type(nt, els[e]);
        if (!ety || !sp_streq(ety, "SymbolNode")) ok = 0;
      }
      /* as CRuby: more keys than members is {}, and the first key naming no
         member ends the hash there -- it is not an error (#2974) */
      for (int e = 0; ok && en <= sc->nmembers && e < en; e++) {
        char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", nt_str(nt, els[e], "value"));
        int mi2 = comp_member_index(sc, ivn);
        if (nkey >= 64) { ok = 0; break; }
        if (mi2 < 0) break;
        keyed[nkey++] = mi2;
      }
    }
    else ok = 0;
    if (ok) {
      int t = ++g_tmp, rh = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      buf_printf(b, "({ sp_%s *_t%d = %s;", sc->c_name, t, rb.p ? rb.p : "");
      emit_struct_recv_root(c, recv, t, b);
      buf_printf(b, " sp_SymPolyHash *_t%d = sp_SymPolyHash_new(); SP_GC_ROOT(_t%d);", rh, rh);
      free(rb.p);
      for (int e = 0; e < nkey; e++) {
        int i = keyed[e];
        buf_printf(b, " sp_SymPolyHash_set(_t%d, (sp_sym)%d, ", rh, comp_sym_intern(c, sc->ivars[i] + 1));
        char fb2[300]; snprintf(fb2, sizeof fb2, "_t%d->iv_%s", t, iv_c(sc->ivars[i] + 1));
        emit_member_box(c, sc, i, fb2, b);
        buf_puts(b, ");");
      }
      buf_printf(b, " _t%d; })", rh);
      { *out = 1; return 1; }
    }
  }
  if ((sp_streq(name, "members")) && argc == 0) {
    int rm = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", rm, rm);
    for (int i = 0; i < sc->nmembers; i++)
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym((sp_sym)%d));", rm, comp_sym_intern(c, sc->ivars[i] + 1));
    buf_printf(b, " _t%d; })", rm);
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "with") && sc->is_data) {
    /* Data#with copy-update: a new instance with the given members
       overridden, the rest copied from the receiver. Members are passed to
       the generated constructor in declaration order. */
    int wargs = nt_ref(nt, id, "arguments");
    int wargc = 0; const int *wargv = wargs >= 0 ? nt_arr(nt, wargs, "arguments", &wargc) : NULL;
    /* no arguments: CRuby answers the receiver itself, not a copy */
    if (wargc == 0) { emit_expr(c, recv, b); { *out = 1; return 1; } }
    int wkwh = -1;
    if (wargv && wargc >= 1) {
      const char *lty = nt_type(nt, wargv[wargc - 1]);
      if (lty && sp_streq(lty, "KeywordHashNode")) wkwh = wargv[wargc - 1];
    }
    /* a `**hash` double-splat in the keyword hash carries member overrides
       only known at run time; look each member up in it (#2972) */
    int wds = -1;
    if (wkwh >= 0) {
      int en = 0; const int *els = nt_arr(nt, wkwh, "elements", &en);
      for (int e = 0; e < en; e++)
        if (nt_type(nt, els[e]) && sp_streq(nt_type(nt, els[e]), "AssocSplatNode"))
          wds = nt_ref(nt, els[e], "value");
    }
    /* Data#with takes keyword arguments only; a positional argument (the only
       arg, or one alongside the keyword hash) is an ArgumentError in CRuby. */
    if (wkwh < 0 || wargc > 1) {
      unsupported(c, id, "Data#with with a positional argument (keywords only)");
      { *out = 0; return 1; }
    }
    if (wkwh >= 0) {
      int en = 0; const int *els = nt_arr(nt, wkwh, "elements", &en);
      /* an unknown member keyword is a runtime ArgumentError in CRuby (not a
         compile error): evaluate the receiver, then raise, naming every
         key no member takes, once each, as #inspect writes it (#2664) */
      char unk[512] = ""; int nunk = 0, spelled = 1;
      for (int e = 0; e < en; e++) {
        if (nt_type(nt, els[e]) && sp_streq(nt_type(nt, els[e]), "AssocSplatNode")) continue;
        int key = nt_ref(nt, els[e], "key");
        const char *kty = key >= 0 ? nt_type(nt, key) : NULL;
        int is_sym = kty && sp_streq(kty, "SymbolNode"), is_str = kty && sp_streq(kty, "StringNode");
        const char *kn = is_sym ? nt_str(nt, key, "value") : is_str ? nt_str(nt, key, "content") : NULL;
        char ivn[256];
        if (kn) snprintf(ivn, sizeof ivn, "@%s", kn);
        if (is_sym && comp_member_index(sc, ivn) >= 0) continue;
        if (!kn) { spelled = 0; continue; }
        int dup = 0;
        for (int e2 = 0; e2 < e && !dup; e2++) {
          int k2 = nt_ref(nt, els[e2], "key");
          const char *t2 = k2 >= 0 ? nt_type(nt, k2) : NULL;
          const char *n2 = t2 && sp_streq(t2, kty) ? nt_str(nt, k2, is_sym ? "value" : "content") : NULL;
          dup = n2 && sp_streq(n2, kn);
        }
        if (dup) continue;
        char iv[300];
        kw_key_inspect(kn, is_sym, iv, sizeof iv);
        kw_names_add(unk, sizeof unk, &nunk, iv);
      }
      if (nunk || !spelled) {
        char km[600];
        if (nunk && spelled) kw_error_message(km, sizeof km, "unknown", nunk, unk);
        else snprintf(km, sizeof km, "unknown keyword: :?");
        buf_puts(b, "({ (void)("); emit_expr(c, recv, b);
        buf_puts(b, "); sp_raise_cls(\"ArgumentError\", ");
        emit_str_literal(b, km);
        buf_printf(b, "); (sp_%s *)NULL; })", sc->c_name);
        { *out = 1; return 1; }
      }
    }
    int t = ++g_tmp;
    int th = wds >= 0 ? ++g_tmp : -1;
    Buf rb = expr_buf(c, recv);
    buf_printf(b, "({ sp_%s *_t%d = %s;", sc->c_name, t, rb.p ? rb.p : ""); free(rb.p);
    if (th >= 0) { buf_printf(b, " sp_RbVal _t%d = ", th); emit_boxed(c, wds, b); buf_puts(b, ";"); }
    buf_printf(b, " sp_%s_new(", sc->c_name);
    for (int i = 0; i < sc->nmembers; i++) {
      if (i) buf_puts(b, ", ");
      int val = wkwh >= 0 ? kwh_lookup(nt, wkwh, sc->ivars[i] + 1) : -1;
      if (val >= 0) {
        TyKind mt = sc->ivar_types[i];
        int val_boxed = repr_of(c, val).kind == RK_BOXED;
        if (mt == TY_POLY && !val_boxed) {
          emit_boxed(c, val, b);  /* box a concrete value into a poly member */
        }
        else if (mt != TY_POLY && val_boxed) {
          /* A poly (sp_RbVal) value into a concrete member: coerce it, mirroring
             the poly-arg path in emit_arg_or_default. The regular `.new` call
             goes through that path; this hand-rolled constructor call did not,
             so it assigned an sp_RbVal straight into a const char* / sp_int /
             sp_<T>* slot (a C type error). */
          const char *mtn = c_type_name(mt);
          if (mt == TY_STRING) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, val, b); buf_puts(b, ")"); }
          else if (mt == TY_FLOAT) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, val, b); buf_puts(b, ")"); }
          else if (mt == TY_SYMBOL) { buf_puts(b, "(sp_sym)sp_poly_to_i("); emit_expr(c, val, b); buf_puts(b, ")"); }
          else if (mt == TY_BOOL) { buf_puts(b, "sp_poly_truthy("); emit_expr(c, val, b); buf_puts(b, ")"); }
          else if (mt == TY_INT) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, val, b); buf_puts(b, ")"); }
          else if (ty_is_object(mt) || (mtn && mtn[0] && mtn[strlen(mtn) - 1] == '*')) {
            Buf ub = expr_buf(c, val);
            emit_unbox_text(c, mt, ub.p ? ub.p : "", b); free(ub.p);
          }
          else emit_expr(c, val, b);
        }
        else {
          emit_expr(c, val, b);
        }
      }
      else if (th >= 0) {
        /* member not given literally: take it from the **hash if present,
           else copy from the receiver (#2972) */
        buf_printf(b, "({ sp_bool _f = 0; sp_RbVal _v = sp_poly_hash_get_pair_val(_t%d, "
                      "sp_box_sym(sp_sym_intern(\"%s\")), &_f); _f ? (", th, sc->ivars[i] + 1);
        if (sc->ivar_types[i] == TY_POLY) buf_puts(b, "_v");
        else emit_unbox_text(c, sc->ivar_types[i], "_v", b);
        buf_printf(b, ") : _t%d->iv_%s; })", t, iv_c(sc->ivars[i] + 1));
      }
      else {
        buf_printf(b, "_t%d->iv_%s", t, iv_c(sc->ivars[i] + 1));
      }
    }
    buf_puts(b, "); })");
    { *out = 1; return 1; }
  }
  /* CRuby's Data defines no #dig at all (Struct does), so digging into one
     is a NoMethodError on a direct call and a TypeError through an
     intermediate -- not a member read (#3919). */
  if (sp_streq(name, "dig") && sc->is_data) {
    TyKind dgr = repr_of(c, id).as_ty;
    const char *dgv = default_value_from_compiler(c, dgr);
    buf_puts(b, "({ (void)("); emit_expr(c, recv, b);
    buf_printf(b, "); sp_raise_nomethod(sp_nomethod_msg(\"dig\", sp_box_obj((void *)0, %d))); %s; })",
               ty_object_class(rt), dgv ? dgv : "0");
    { *out = 1; return 1; }
  }
  /* CRuby's Data defines no #[] either: a member is read by name only,
     and indexing is a NoMethodError -- not Struct's member access. Nor
     #[]= (a Data is frozen): the Struct's store wrote the member. */
  if (is_element_access(name) && (argc == 2 || !is_index_assign(name)) && sc->is_data) {
    TyKind dar = repr_of(c, id).as_ty;
    buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); ");
    for (int da = 0; da < argc; da++) {
      buf_puts(b, "(void)("); emit_boxed(c, argv[da], b); buf_puts(b, "); ");
    }
    buf_printf(b, "sp_raise_nomethod(sp_nomethod_msg(\"%s\", sp_box_obj((void *)0, %d))); %s; })",
               name, ty_object_class(rt), raise_tail_c(c, id, dar));
    { *out = 1; return 1; }
  }
  /* a Struct's [] / dig / deconstruct_keys validate like CRuby: a missing
     argument is ArgumentError (dig says "1+"), a nil / bool index is the
     Integer-conversion TypeError. Data keeps its own dispatch above. */
  if (((!sc->is_data && (sp_streq(name, "[]") || sp_streq(name, "dig"))) ||
       sp_streq(name, "deconstruct_keys")) && argc == 0) {
    TyKind z0 = repr_of(c, id).as_ty;
    buf_puts(b, "({ (void)("); emit_expr(c, recv, b);
    buf_printf(b, "); sp_raise_cls(\"ArgumentError\","
                  " \"wrong number of arguments (given 0, expected %s)\"); %s; })",
               sp_streq(name, "dig") ? "1+" : "1",
               raise_tail_c(c, id, z0));
    { *out = 1; return 1; }
  }
  if (!sc->is_data && sp_streq(name, "[]") && argc >= 2) {
    TyKind za = repr_of(c, id).as_ty;
    buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); ");
    for (int sa = 0; sa < argc; sa++) {
      buf_puts(b, "(void)("); emit_boxed(c, argv[sa], b); buf_puts(b, "); ");
    }
    buf_printf(b, "sp_raise_cls(\"ArgumentError\","
                  " \"wrong number of arguments (given %d, expected 1)\"); %s; })",
               argc, raise_tail_c(c, id, za));
    { *out = 1; return 1; }
  }
  if (!sc->is_data && (sp_streq(name, "[]") || sp_streq(name, "dig")) && argc >= 1 &&
      (comp_ntype(c, argv[0]) == TY_NIL || comp_ntype(c, argv[0]) == TY_BOOL)) {
    TyKind z1 = repr_of(c, id).as_ty;
    int zb = ++g_tmp;
    buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); ");
    if (comp_ntype(c, argv[0]) == TY_NIL) {
      buf_puts(b, "(void)("); emit_expr(c, argv[0], b); buf_puts(b, "); ");
    }
    else {
      buf_printf(b, "int _t%d = (", zb); emit_expr(c, argv[0], b); buf_puts(b, "); ");
    }
    for (int sa = 1; sa < argc; sa++) {   /* dig's trailing keys evaluate too */
      buf_puts(b, "(void)("); emit_boxed(c, argv[sa], b); buf_puts(b, "); ");
    }
    if (comp_ntype(c, argv[0]) == TY_NIL)
      buf_puts(b, "sp_raise_cls(\"TypeError\", \"no implicit conversion from nil to integer\");");
    else
      buf_printf(b, "sp_raise_cls(\"TypeError\", _t%d"
                    " ? \"no implicit conversion of true into Integer\""
                    " : \"no implicit conversion of false into Integer\");", zb);
    buf_printf(b, " %s; })", raise_tail_c(c, id, z1));
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "dig") && argc >= 1) {
    /* literal key resolves a member at compile time */
    int mi = -1;
    const char *kty = nt_type(nt, argv[0]);
    if (kty && (sp_streq(kty, "SymbolNode") || sp_streq(kty, "StringNode"))) {
      /* a String names a member too, and inference resolves one: leaving it
         to the runtime walk answered a boxed value into the member-typed
         slot the call site declares (#3892) */
      const char *kv = sp_streq(kty, "SymbolNode") ? nt_str(nt, argv[0], "value")
                                                   : nt_str(nt, argv[0], "content");
      if (kv) { char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", kv);
                mi = comp_member_index(sc, ivn); }
    }
    else if (kty && sp_streq(kty, "IntegerNode")) {
      int v = (int)nt_int(nt, argv[0], "value", -1);
      if (v >= 0 && v < sc->nmembers) mi = v;
    }
    if (mi >= 0) {
      /* nested struct members resolve the remaining literal keys at compile
         time: n.dig(:b, :c) walks member structs field by field */
      {
        char path[512]; path[0] = 0;
        ClassInfo *cur = sc; int cmi = mi; int di = 1; int all = 1;
        while (di < argc) {
          TyKind mt2 = cur->ivar_types[cmi];
          if (!ty_is_object(mt2) || !c->classes[ty_object_class(mt2)].is_struct) { all = 0; break; }
          ClassInfo *nx = &c->classes[ty_object_class(mt2)];
          const char *k2ty = nt_type(nt, argv[di]);
          int nmi = -1;
          if (k2ty && sp_streq(k2ty, "SymbolNode")) {
            char ivn2[256]; snprintf(ivn2, sizeof ivn2, "@%s", nt_str(nt, argv[di], "value"));
            nmi = comp_member_index(nx, ivn2);
          }
          else if (k2ty && sp_streq(k2ty, "IntegerNode")) {
            int v2 = (int)nt_int(nt, argv[di], "value", -1);
            if (v2 >= 0 && v2 < nx->nmembers) nmi = v2;
          }
          if (nmi < 0) { all = 0; break; }
          size_t pl = strlen(path);
          snprintf(path + pl, sizeof path - pl, "->iv_%s", iv_c(cur->ivars[cmi] + 1));
          cur = nx; cmi = nmi; di++;
        }
        if (all && di == argc && argc >= 2) {
          int t2 = ++g_tmp;
          Buf rb2 = expr_buf(c, recv);
          buf_printf(b, "({ sp_%s *_t%d = %s; ", sc->c_name, t2, rb2.p ? rb2.p : "");
          char leaf[700]; snprintf(leaf, sizeof leaf, "_t%d%s->iv_%s", t2, path, iv_c(cur->ivars[cmi] + 1));
          if (cur->ivar_types[cmi] == TY_STRBUF) emit_strbuf_node_read(c, id, leaf, b);
          else buf_puts(b, leaf);
          buf_puts(b, "; })");
          free(rb2.p);
          { *out = 1; return 1; }
        }
      }
      int t = ++g_tmp;
      char fld[300]; snprintf(fld, sizeof fld, "_t%d->iv_%s", t, iv_c(sc->ivars[mi] + 1));
      TyKind mt = sc->ivar_types[mi];
      /* the receiver is rooted across the later keys, which may allocate;
         a single key reads the member with nothing emitted in between */
      buf_printf(b, "({ sp_%s *_t%d = ", sc->c_name, t);
      if (argc == 1) { emit_expr(c, recv, b); buf_puts(b, "; "); }
      else emit_recv_rooted(c, recv, t, "SP_GC_ROOT", b);
      if (argc == 1 && mt == TY_STRBUF) emit_strbuf_node_read(c, id, fld, b);
      /* one key on a number member with a nil bit: read with the bit -- the
         oint where the call's consumer takes one, else unwrapped */
      else if (argc == 1 && oint_kind(mt) && ivar_has_nilbit(c, (int)(sc - c->classes), mi)) {
        char objp[40]; snprintf(objp, sizeof objp, "_t%d->", t);
        char bit[400]; ivar_nilbit_test(c, (int)(sc - c->classes), mi, objp, bit, sizeof bit);
        if (!node_is_oint(c, id)) buf_printf(b, "%s(", oint_arg(mt));
        buf_printf(b, "((%s){ %s, (%s) != 0 })", oint_ctype(mt), fld, bit);
        if (!node_is_oint(c, id)) buf_puts(b, ")");
      }
      else if (argc == 1) buf_puts(b, fld);
      else if (ty_is_hash(mt) && argc == 2) {
        const char *hn = ty_hash_cname(mt);
        int oint = ty_hash_val(mt) == TY_INT;   /* a miss answers nil beside the value */
        if (oint) oint_open(c, id, TY_INT, b);
        buf_printf(b, "sp_%sHash_%s(%s, ", hn, oint ? "oget" : "get", fld);
        emit_expr(c, argv[1], b); buf_puts(b, ")");
        if (oint) oint_close(c, id, b);
      }
      else if (ty_is_array(mt) && argc == 2) {
        /* array_kind has no name for a poly array (nor for the pointer-array
           kinds), and the NULL went straight into the C symbol (#3574) */
        const char *ak = (mt == TY_POLY_ARRAY) ? "Poly" : array_kind(mt);
        if (ak) {
          buf_printf(b, "sp_%sArray_get(%s, ", ak, fld); emit_expr(c, argv[1], b); buf_puts(b, ")");
        }
        else {
          buf_puts(b, "sp_poly_dig_step_key(");
          emit_boxed_text(c, mt, fld, b);
          buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
        }
      }
      else if (argc >= 2) {
        /* every other remaining key walks at run time: the arms above cover
           one step into a member, and the rest were silently dropped, which
           emitted the member itself where a dug value was wanted (#3881) */
        Buf vb = {0}; emit_boxed_text(c, mt, fld, &vb);
        emit_rooted_key_call(c, "sp_poly_dig_n", vb.p, argv + 1, argc - 1, b); free(vb.p);
      }
      else buf_puts(b, fld);
      buf_puts(b, "; })");
      { *out = 1; return 1; }
    }
    /* a key no literal member matches (a local, an offset, a name) resolves
       at run time; each further key then digs from that value (#3849) */
    if (sc->nmembers > 0) {
      int td = ++g_tmp;
      char rtxt[32]; snprintf(rtxt, sizeof rtxt, "_t%d", td);
      /* the receiver is rooted across the keys, which may allocate */
      buf_printf(b, "({ sp_%s *_t%d = ", sc->c_name, td);
      emit_recv_rooted(c, recv, td, "SP_GC_ROOT", b);
      if (argc == 1) emit_struct_member_by_key(c, sc, rtxt, argv[0], 0, 1, b);
      else {
        Buf vb = {0}; emit_struct_member_by_key(c, sc, rtxt, argv[0], 0, 1, &vb);
        emit_rooted_key_call(c, "sp_poly_dig_n", vb.p, argv + 1, argc - 1, b); free(vb.p);
      }
      buf_puts(b, "; })");
      { *out = 1; return 1; }
    }
  }
  /* struct[key] = v: the member the key names takes the value. Only an
     in-range literal member name had an emitter, so a variable key, an
     out-of-range offset or a missing name was refused outright (#3849). */
  if (sp_streq(name, "[]=") && argc == 2) {
    int tw = ++g_tmp, tk = ++g_tmp, tk0 = ++g_tmp, tv = ++g_tmp;
    Buf rbw = expr_buf(c, recv);
    buf_printf(b, "({ sp_%s *_t%d = %s; sp_RbVal _t%d = ", sc->c_name, tw, rbw.p ? rbw.p : "", tk);
    free(rbw.p);
    emit_boxed(c, argv[0], b);
    buf_printf(b, "; sp_RbVal _t%d = _t%d;", tk0, tk);
    /* A Float key is cut to an Integer only where every member takes a value
       of the type stored: the store below unboxes the value as the member's
       type, so another type would be written as garbage. A Float key is a
       NameError otherwise, as it was. */
    {
      TyKind fvt = repr_of(c, argv[1]).as_ty;
      int fok = fvt != TY_POLY && fvt != TY_UNKNOWN;
      for (int i = 0; i < sc->nmembers && fok; i++)
        if (sc->ivar_types[i] != TY_POLY && sc->ivar_types[i] != fvt) fok = 0;
      if (fok) emit_struct_float_offset(b, tk, tk0);
    }
    buf_printf(b, " if (_t%d.tag == SP_TAG_INT && _t%d.v.i < 0) _t%d = sp_box_int(_t%d.v.i + %d);",
               tk, tk, tk, tk, sc->nmembers);
    /* The assignment's own value is the right-hand side in ITS type -- that
       is what the call site is typed for -- so keep it, and box a copy for
       the per-member stores (#3897). A nil has no C value of its own (its
       C type is void): it is the boxed nil, as an untyped value is. */
    TyKind vt = repr_of(c, argv[1]).as_ty;
    int tvraw = ++g_tmp;
    if (vt != TY_POLY && vt != TY_UNKNOWN && vt != TY_NIL) {
      buf_printf(b, " "); emit_ctype(c, vt, b);
      buf_printf(b, " _t%d = ", tvraw); emit_expr(c, argv[1], b); buf_puts(b, ";");
      char rawtxt[32]; snprintf(rawtxt, sizeof rawtxt, "_t%d", tvraw);
      /* a String a shared boxed member takes is its handle (emit_boxed's lift) */
      int lift = repr_share_rule(c) && c->poly_strbuf_lift[argv[1]] && vt == TY_STRING;
      buf_printf(b, " sp_RbVal _t%d = %s", tv, lift ? "sp_poly_strbuf_lift(" : "");
      emit_boxed_text(c, vt, rawtxt, b);
      buf_puts(b, lift ? ");" : ";");
    }
    else {
      buf_printf(b, " sp_RbVal _t%d = ", tv); emit_boxed(c, argv[1], b); buf_puts(b, ";");
      tvraw = tv;
    }
    char obj[32]; snprintf(obj, sizeof obj, "_t%d", tw);
    emit_frozen_obj_guard(c, ty_object_class(rt), obj, b);
    for (int i = 0; i < sc->nmembers; i++) {
      buf_printf(b, " if(sp_rbval_eql_key(_t%d,sp_box_sym((sp_sym)%d))||sp_rbval_eql_key(_t%d,sp_box_int(%lldLL))"
                    "||sp_rbval_eql_key(_t%d,sp_box_str(\"%s\"))){ _t%d->iv_%s = ",
                 tk, comp_sym_intern(c, sc->ivars[i] + 1), tk, (long long)i,
                 tk, sc->ivars[i] + 1, tw, iv_c(sc->ivars[i] + 1));
      char vtxt[32]; snprintf(vtxt, sizeof vtxt, "_t%d", tv);
      int scid = ty_object_class(rt);
      /* a member with a nil bit takes the nil into its bit */
      if (oint_kind(sc->ivar_types[i]) && ivar_has_nilbit(c, scid, i)) {
        char ot[64], pfx[48];
        snprintf(ot, sizeof ot, "%s(_t%d)", oint_unbox(sc->ivar_types[i]), tv);
        snprintf(pfx, sizeof pfx, "_t%d->", tw);
        emit_ivar_text_nilbit(c, scid, i, pfx, ot, b);
      }
      else emit_struct_member_value(c, sc->ivar_types[i], vt, vtxt, b);
      buf_puts(b, ";}\nelse");
    }
    /* The value is the right-hand side in its own type -- except where the
       call site is typed boxed (the analyzer answers the store poly when
       the member is; under --int-overflow=promote every int slot is), in
       which case it is the boxed copy: the raw sp_int landed in an
       sp_RbVal local otherwise (#4733). */
    buf_printf(b, " { if (_t%d.tag == SP_TAG_INT)"
                  " sp_raise_cls(\"IndexError\", sp_sprintf(\"offset %%lld too %%s for struct(size:%d)\","
                  " (long long)_t%d.v.i, _t%d.v.i < 0 ? \"small\" : \"large\"));"
                  " sp_raise_cls(\"NameError\", sp_sprintf(\"no member '%%s' in struct\", sp_poly_to_s(_t%d)));"
                  " } _t%d; })",
               tk0, sc->nmembers, tk0, tk0, tk0,
               (repr_of(c, id).kind == RK_BOXED && tvraw != tv) ? tv : tvraw);
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "[]") && argc == 1) {
    /* struct[:sym] or struct[int_literal]: return member value boxed to poly */
    int mi = -1;
    const char *kty = nt_type(nt, argv[0]);
    if (kty && (sp_streq(kty, "SymbolNode") || sp_streq(kty, "StringNode"))) {
      const char *kv = sp_streq(kty, "SymbolNode") ? nt_str(nt, argv[0], "value")
                                                   : nt_str(nt, argv[0], "content");
      if (kv) {
        char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", kv);
        mi = comp_member_index(sc, ivn);
      }
    }
    else if (kty && sp_streq(kty, "IntegerNode")) {
      long long v = (long long)nt_int(nt, argv[0], "value", 0);
      if (v < 0) v += (long long)sc->nmembers;
      if (v >= 0 && v < sc->nmembers) mi = (int)v;
    }
    if (mi >= 0) {
      int t = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      buf_printf(b, "({ sp_%s *_t%d = %s; ", sc->c_name, t, rb.p ? rb.p : ""); free(rb.p);
      /* a String member that is a handle reads as its ivar does: the handle
         where the caller asks for it, else the String */
      if (sc->ivar_types[mi] == TY_STRBUF) {
        char sref[300]; snprintf(sref, sizeof sref, "_t%d->iv_%s", t, iv_c(sc->ivars[mi] + 1));
        emit_strbuf_node_read(c, id, sref, b);
        buf_puts(b, "; })");
      }
      /* a number member with a nil bit reads with it: the oint where the
         call's consumer takes one, else unwrapped (or boxed) */
      else if (oint_kind(sc->ivar_types[mi]) && ivar_has_nilbit(c, (int)(sc - c->classes), mi)) {
        char objp[40]; snprintf(objp, sizeof objp, "_t%d->", t);
        char bit[400]; ivar_nilbit_test(c, (int)(sc - c->classes), mi, objp, bit, sizeof bit);
        TyKind mt = sc->ivar_types[mi];
        char ot[600]; snprintf(ot, sizeof ot, "((%s){ _t%d->iv_%s, (%s) != 0 })", oint_ctype(mt), t, iv_c(sc->ivars[mi] + 1), bit);
        if (repr_of(c, id).kind == RK_BOXED) buf_printf(b, "%s(%s); })", oint_box(mt), ot);
        else if (node_is_oint(c, id)) buf_printf(b, "%s; })", ot);
        else buf_printf(b, "%s(%s); })", oint_arg(mt), ot);
      }
      else buf_printf(b, "_t%d->iv_%s; })", t, iv_c(sc->ivars[mi] + 1));
      { *out = 1; return 1; }
    }
    /* general: generate chain of comparisons. Each arm has to ASSIGN into a
       result temp -- written as bare statements the chain is a void
       expression, which is not a value the caller can read (#3572). */
    if (sc->nmembers > 0) {
      int t = ++g_tmp, tk = ++g_tmp, tr = ++g_tmp, tk0 = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      buf_printf(b, "({ sp_%s *_t%d = %s; sp_RbVal _t%d = ", sc->c_name, t, rb.p ? rb.p : "", tk);
      free(rb.p);
      emit_boxed(c, argv[0], b);
      /* a negative offset counts from the end; keep the original for the
         error message */
      buf_printf(b, "; sp_RbVal _t%d = _t%d;", tk0, tk);
      emit_struct_float_offset(b, tk, tk0);
      buf_printf(b, " if (_t%d.tag == SP_TAG_INT && _t%d.v.i < 0) _t%d = sp_box_int(_t%d.v.i + %d);",
                 tk, tk, tk, tk, sc->nmembers);
      buf_printf(b, " sp_RbVal _t%d = sp_box_nil();", tr);
      for (int i = 0; i < sc->nmembers; i++) {
        buf_printf(b, " if(sp_rbval_eql_key(_t%d,sp_box_sym((sp_sym)%d))||sp_rbval_eql_key(_t%d,sp_box_int(%lldLL))){ _t%d = ",
                   tk, comp_sym_intern(c, sc->ivars[i]+1), tk, (long long)i, tr);
        char fld2[300]; snprintf(fld2, sizeof fld2, "_t%d->iv_%s", t, iv_c(sc->ivars[i] + 1));
        emit_member_box(c, sc, i, fld2, b);
        buf_printf(b, ";}\nelse");
      }
      /* a miss is an error: IndexError for an offset, NameError for a name */
      buf_printf(b, " { if (_t%d.tag == SP_TAG_INT)"
                    " sp_raise_cls(\"IndexError\", sp_sprintf(\"offset %%lld too %%s for struct(size:%d)\","
                    " (long long)_t%d.v.i, _t%d.v.i < 0 ? \"small\" : \"large\"));"
                    " sp_raise_cls(\"NameError\", sp_sprintf(\"no member '%%s' in struct\", sp_poly_to_s(_t%d)));"
                    " } _t%d; })",
                 tk0, sc->nmembers, tk0, tk0, tk0, tr);
      { *out = 1; return 1; }
    }
  }
  return 0;
}

static int emit_object_kind_nil(Compiler *c, int id, Buf *b, const NodeTable *nt,
                                const char *name, int recv, int argc, const int *argv, TyKind rt) {
  /* A heap object slot can hold nil, the NULL pointer, and nil is an
     instance of NilClass, Object, Kernel and BasicObject and of nothing
     else: the arm below answers for the class alone, so `v.is_a?(Shape)` on
     a `v` holding nil said true. The receiver is read once into a temp; the
     arm below answers for a live object, reading the temp, and nil answers
     as nil does. The live arm re-enters the call with its receiver viewed
     as tested (VR_NIL_TESTED), so the re-entry of this call is not armed
     twice; a kind query nested in its class argument has a receiver of its
     own and still gets its arm. */
  if (recv >= 0 && !repr_of(c, recv).nil_tested && ty_is_object(rt) && argc == 1 &&
      !comp_ty_value_obj(c, rt) && nt_kind(nt, recv) != NK_SelfNode &&
      is_kind_query(name) &&
      comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0 &&
      g_n_argov < MAX_ARG_OVERRIDE) {
    const char *ncn = isa_const_name(nt, argv[0]);
    int dyn = !ncn && (comp_ntype(c, argv[0]) == TY_CLASS ||
                       comp_ntype(c, argv[0]) == TY_POLY);
    if (ncn || dyn) {
      int exact = sp_streq(name, "instance_of?");
      int tr = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tr, rb.p ? rb.p : "NULL");
      free(rb.p);
      /* both arms read the class argument: one that runs code is read once,
         after the receiver, so a call in it -- a nested kind query's
         receiver its arm hoists, too -- runs once */
      int mark = g_n_argov;
      if (dyn && subtree_has_side_effect(c, argv[0])) {
        emit_gc_root_tmp(c, rt, tr, g_pre);
        TyKind at = comp_ntype(c, argv[0]);
        int ta = ++g_tmp;
        Buf ab = expr_buf(c, argv[0]);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, at, g_pre); buf_printf(g_pre, " _t%d = %s;\n", ta, ab.p ? ab.p : "");
        free(ab.p);
        view_bind(argv[0], "_t%d", ta);
      }
      view_bind(recv, "_t%d", tr);
      Buf ib; memset(&ib, 0, sizeof ib);
      int vt = view_push_repr(c, recv, VR_NIL_TESTED, 1);
      int ok = emit_object_call(c, id, &ib);
      view_pop(c, vt);
      if (ok) {
        buf_printf(b, "(_t%d ? (%s) : ", tr, ib.p ? ib.p : "0");
        if (dyn) {
          int nk = builtin_class_id("NilClass");
          buf_puts(b, "({ sp_Class _nc = ");
          if (comp_ntype(c, argv[0]) == TY_POLY) buf_puts(b, "sp_isa_class_arg(");
          emit_expr(c, argv[0], b);
          if (comp_ntype(c, argv[0]) == TY_POLY) buf_puts(b, ")");
          if (exact) buf_printf(b, "; _nc.cls_id == %d; })", nk);
          else buf_printf(b, "; sp_class_le((sp_Class){%d}, _nc); })", nk);
        }
        else {
          int yes = !comp_const(c, ncn) && comp_class_index(c, ncn) < 0 &&
                    (sp_streq(ncn, "NilClass") ||
                     (!exact && is_object_root(ncn)));
          buf_printf(b, "%d", yes);
        }
        buf_puts(b, ")");
        view_unbind(mark);
        free(ib.p);
        return 1;
      }
      view_unbind(mark);
      free(ib.p);
    }
  }
  return 0;
}

int emit_object_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = comp_recv_type(c, recv);
  TyKind res = comp_ntype(c, id);
  /* Object#equal? -- reference identity. A heap instance IS its pointer, so
     this is a plain pointer comparison; a poly argument unwraps to tag +
     pointer; an argument of any other concrete type is never identical. A
     value-type instance is copied inline and has no stable identity, so only
     the reflexive same-lvalue read is knowably true (the string arm's rule). */
  /* Object#eql? default (no user override) is identity, exactly equal? --
     route it through the same arm (#2361) */
  /* Object#frozen? / #freeze on a concurrency handle. A Mutex, Fiber, Thread
     or ConditionVariable is an ordinary heap instance and freezes like one --
     the answer used to be a flat "never frozen", so `freeze` was a no-op and
     `frozen?` stayed false after it (#3483). A Queue is the exception Ruby
     itself makes: freezing one raises, because a frozen queue could never be
     pushed to again. (A Fiber reached the front-end reject where its Thread
     sibling did not, #3470.) */
  if (recv >= 0 && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (rt == TY_MUTEX || rt == TY_QUEUE || rt == TY_CONDVAR ||
       rt == TY_FIBER || rt == TY_THREAD) &&
      (is_freeze_family(name)) &&
      !user_defines_or_reads(c, name)) {
    int tq = ++g_tmp;
    if (rt == TY_QUEUE) {
      if (sp_streq(name, "frozen?")) {
        buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (sp_bool)0)");
      }
      else {
        buf_printf(b, "({ sp_queue *_t%d = ", tq); emit_expr(c, recv, b);
        buf_printf(b, "; sp_raise_cannot_freeze(sp_Queue_class_name(_t%d), (void *)_t%d); _t%d; })",
                   tq, tq, tq);
      }
      return 1;
    }
    if (sp_streq(name, "frozen?")) {
      buf_puts(b, "sp_gc_is_frozen((void *)("); emit_expr(c, recv, b); buf_puts(b, "))");
    }
    else {
      buf_puts(b, "(("); emit_ctype(c, rt, b); buf_puts(b, ")sp_gc_freeze((void *)(");
      emit_expr(c, recv, b); buf_puts(b, ")))");
    }
    return 1;
  }
  /* The concurrency handles are heap instances too -- a Mutex, Queue,
     SizedQueue, ConditionVariable or Fiber IS its pointer -- so identity is
     the same pointer comparison. They are not ty_is_object (no user class
     behind them), which left equal?/eql? on them refused by the front end,
     where it could not even be rescued (#3470). */
  if (recv >= 0 && argc == 1 &&
      (rt == TY_MUTEX || rt == TY_QUEUE || rt == TY_CONDVAR ||
       rt == TY_FIBER || rt == TY_THREAD) &&
      (is_eql_or_equal(name)) &&
      !user_defines_or_reads(c, name)) {
    TyKind a0 = comp_ntype(c, argv[0]);
    emit_identity_equal(c, recv, argv[0], rt, a0, b);
    return 1;
  }
  if (recv >= 0 && ty_is_object(rt) && argc == 1 &&
      (is_eql_or_equal(name)) &&
      comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0 &&
      comp_method_in_chain(c, ty_object_class(rt), "eql?", NULL) < 0) {
    TyKind a0 = comp_ntype(c, argv[0]);
    if (!c->classes[ty_object_class(rt)].is_value_type) {
      emit_identity_equal(c, recv, argv[0], rt, a0, b);
      return 1;
    }
    if (same_sefree_lvalue(c, recv, argv[0])) { buf_puts(b, "(("); emit_expr(c, argv[0], b); buf_puts(b, "), 1)"); }
    else { buf_puts(b, "(("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
    return 1;
  }

  /* Object#freeze / #frozen? on a user instance: the frozen state lives in
     the object's GC header bit (shared with the container freeze paths).
     freeze sets it and returns self; frozen? reads it. Mutation of a frozen
     plain object's ivars is NOT trapped (raw C stores); the flag round-trip
     is what reflection-driven code observes. */
  if (recv >= 0 && ty_is_object(rt) && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (is_freeze_family(name)) &&
      comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0 &&
      /* a generated reader of the name owns it, as in CRuby (#4190) */
      comp_resolve_member(c, ty_object_class(rt), name, 0, NULL, NULL) != SP_MEMBER_ATTR) {
    if (comp_ty_value_obj(c, rt)) {
      /* A value-type object (sp_X by value, no heap GC header to carry the
         frozen bit): freeze is a self-returning no-op and frozen? is false,
         matching the pre-stateful-freeze behavior for these unboxed classes. */
      if (sp_streq(name, "freeze")) { emit_expr(c, recv, b); return 1; }
      /* frozen? folds to constant-false, but the receiver may carry side
         effects (e.g. get_point().frozen?), so evaluate and discard it via a
         comma expression, mirroring the is_a?/instance_of? value-fold below. */
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0)");
      return 1;
    }
    if (sp_streq(name, "freeze")) {
      buf_puts(b, "((");
      emit_ctype(c, rt, b);
      buf_puts(b, ")sp_gc_freeze("); emit_expr(c, recv, b); buf_puts(b, "))");
      return 1;
    }
    buf_puts(b, "sp_gc_is_frozen("); emit_expr(c, recv, b); buf_puts(b, ")");
    return 1;
  }

  if (emit_object_kind_nil(c, id, b, nt, name, recv, argc, argv, rt)) return 1;
  /* obj.is_a?/kind_of?/instance_of?(Class): resolved via sp_class_le for
     correctness with module includes; falls back to constant for builtins. */
  if (recv >= 0 && ty_is_object(rt) && argc == 1 &&
      is_kind_query(name) &&
      comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0) {
    const char *cn = isa_const_name(nt, argv[0]);
    if (cn) {
      int cid = ty_object_class(rt);
      int target = comp_class_index(c, cn);
      /* an exception-subclass instance walks its carried cls_name chain --
         the class-index fold below answers 0 for builtin targets like
         StandardError, which have no user class index. Exception class names
         are registered fully qualified ("PG::Error"), so a nested-path
         argument must compare with the whole path (#3260). */
      if (class_is_exc_subclass(c, cid)) {
        char qbuf[192];
        const char *qn = isa_const_qualname(nt, argv[0], qbuf, sizeof qbuf);
        if (!qn) qn = cn;
        /* When two modules each hold a class of the same leaf name, the
           colliding one is carried in the AST already flattened (`A::Error`
           arrives as `A__Error`), and prefixing its module again produced
           "A::A__Error" -- a name no exception answers to, so is_a? was
           silently false for both while `rescue` and #ancestors stayed right
           (#4133, found under net/http's Timeout::Error beside URI::Error).
           The class table's own qualified name is the one the raise site
           uses, so ask it whenever the argument names a class we know. */
        {
          int qi = comp_class_index(c, qn);
          if (qi < 0) qi = comp_class_index(c, cn);
          if (qi >= 0) {
            const char *rn = class_ruby_name(c, qi);
            /* COPIED, not pointed at: class_ruby_name hands back a shared
               static buffer, and the emit_expr for the receiver below asks it
               for the receiver's own name -- which overwrote the target and
               made every check compare the receiver against itself. */
            if (rn) { snprintf(qbuf, sizeof qbuf, "%s", rn); qn = qbuf; }
          }
        }
        if (sp_streq(name, "instance_of?")) {
          buf_puts(b, "(strcmp(((sp_Exception *)(");
          emit_expr(c, recv, b);
          buf_printf(b, "))->cls_name, \"%s\") == 0)", qn);
        }
        else {
          buf_puts(b, "sp_exc_is_a((volatile sp_Exception *)(");
          emit_expr(c, recv, b);
          buf_printf(b, "), \"%s\")", qn);
        }
        return 1;
      }
      if (target >= 0) {
        if (sp_streq(name, "instance_of?")) {
          /* a synthesized singleton subclass is instance_of? its parent
             (CRuby hides the singleton class) */
          if (cid >= 0 && cid < c->nclasses && !c->classes[cid].is_value_type &&
              class_has_descendants(c, cid)) {
            /* Same upper-bound problem as is_a? just below, and exactness is
               what instance_of? is FOR: ask the object. The ids that answer
               are the target and any singleton class of it, which is a set the
               compiler can enumerate (#4142). */
            int t9 = ++g_tmp;
            buf_printf(b, "({ sp_int _t%d = (", t9); emit_expr(c, recv, b);
            buf_printf(b, ")->cls_id; ");
            int first = 1;
            for (int k = 0; k < c->nclasses; k++) {
              if (singleton_visible_ci(c, k) != target) continue;
              buf_printf(b, "%s_t%d == %d", first ? "" : " || ", t9, k);
              first = 0;
            }
            if (first) buf_puts(b, "0");
            buf_puts(b, "; })");
          }
          else {
            buf_puts(b, "((void)("); emit_expr(c, recv, b);
            buf_printf(b, "), %d)", singleton_visible_ci(c, cid) == target);
          }
        }
        else {
          /* use sp_class_le_mod (via macro) so includes chain is checked */
          buf_puts(b, "sp_class_le(");
          emit_isa_self_class(c, recv, cid, b);
          buf_printf(b, ",((sp_Class){%d}))", target);
        }
        return 1;
      }
      else {
        /* no user class index: universal ancestors still answer true for the
           hierarchy predicates (every object is_a? Object/BasicObject/Kernel);
           instance_of? stays exact and answers false */
        int uni = !sp_streq(name, "instance_of?") &&
                  is_object_root(cn);
        /* a builtin CLASS ancestor in the superclass chain (Data -146, Struct
           -145, Numeric ...): check the object's class against its cls_id so a
           Data/Struct instance is_a? Data/Struct (#2662). */
        int bid = builtin_class_id(cn);
        if (!uni && !sp_streq(name, "instance_of?") && bid < 0) {
          buf_puts(b, "sp_class_le(");
          emit_isa_self_class(c, recv, cid, b);
          buf_printf(b, ",((sp_Class){%d}))", bid);
          return 1;
        }
        buf_puts(b, "(("); emit_expr(c, recv, b); buf_printf(b, "), %d)", uni);
        return 1;
      }
    }
    /* Dynamic klass argument typed as TY_CLASS: runtime sp_class_le check */
    if (comp_ntype(c, argv[0]) == TY_CLASS ||
        (comp_ntype(c, argv[0]) == TY_POLY && nt_kind(nt, argv[0]) != NK_ConstantReadNode)) {
      int cid = ty_object_class(rt);
      int k = ++g_tmp;
      buf_printf(b, "({ sp_Class _t%d = ", k);
      /* a class read out of a boxed slot is checked: CRuby's TypeError */
      if (repr_of(c, argv[0]).kind == RK_BOXED) { buf_puts(b, "sp_isa_class_arg("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[0], b);
      buf_printf(b, "; ");
      if (sp_streq(name, "instance_of?")) {
        if (cid >= 0 && cid < c->nclasses && !c->classes[cid].is_value_type && class_has_descendants(c, cid)) {
          /* the object's own class, which a subclass instance running an
             inherited method (`instance_of?(w(Animal))` in a Dog) does not
             share with the method's: read off the object, a synthesized
             singleton class answering as its parent (#4142) */
          int o9 = ++g_tmp;
          buf_printf(b, "sp_int _t%d = (", o9); emit_expr(c, recv, b); buf_puts(b, ")->cls_id; (");
          for (int kk = 0; kk < c->nclasses; kk++)
            if (singleton_visible_ci(c, kk) != kk) buf_printf(b, "_t%d == %d ? %d : ", o9, kk, singleton_visible_ci(c, kk));
          buf_printf(b, "_t%d) == _t%d.cls_id; })", o9, k);
        }
        else buf_printf(b, "((sp_Class){%d}).cls_id == _t%d.cls_id; })", singleton_visible_ci(c, cid), k);
      }
      else {
        buf_puts(b, "sp_class_le(");
        emit_isa_self_class(c, recv, cid, b);
        buf_printf(b, ",_t%d); })", k);
      }
      return 1;
    }
  }

  /* Comparable#clamp(lo, hi) on a user object: dispatch the user `<=>` through
     sp_obj_clamp. The result is self or the APPLIED BOUND, so it keeps the
     receiver's class only when both bounds are statically that class (the
     inference arm matches); otherwise it stays boxed. */
  if (recv >= 0 && ty_is_object(rt) && sp_streq(name, "clamp") && argc == 2 &&
      comp_method_in_chain(c, ty_object_class(rt), "<=>", NULL) >= 0) {
    TyKind clo = comp_ntype(c, argv[0]), chi = comp_ntype(c, argv[1]);
    int same_cls = (clo == rt || clo == TY_NIL) && (chi == rt || chi == TY_NIL);
    /* a by-value receiver class unboxes by dereferencing the heap copy the
       boxing made (v.p is always a pointer); a ref class casts the pointer */
    if (same_cls && comp_ty_value_obj(c, rt))
      buf_printf(b, "(*(sp_%s *)", c->classes[ty_object_class(rt)].c_name);
    else if (same_cls) { buf_puts(b, "(("); emit_ctype(c, rt, b); buf_puts(b, ")"); }
    emit_obj_clamp3(c, recv, argv[0], argv[1], b);
    if (same_cls) buf_puts(b, ".v.p)");
    return 1;
  }
  /* Comparable#clamp(lo_obj..hi_obj) with same-class endpoints in a literal
     range: unfold to the two-argument object clamp (an sp_Range cannot carry
     the endpoints' class, so the range helper would compare raw pointers). */
  if (recv >= 0 && ty_is_object(rt) && sp_streq(name, "clamp") && argc == 1 &&
      comp_method_in_chain(c, ty_object_class(rt), "<=>", NULL) >= 0) {
    int rn2 = unwrap_parens(c, argv[0]);
    if (rn2 >= 0 && nt_type(nt, rn2) && sp_streq(nt_type(nt, rn2), "RangeNode")) {
      int rlo = nt_ref(nt, rn2, "left"), rhi = nt_ref(nt, rn2, "right");
      /* a side is present unless it is absent (-1) or an explicit nil */
      int has_lo = rlo >= 0 && !(nt_type(nt, rlo) && sp_streq(nt_type(nt, rlo), "NilNode"));
      int has_hi = rhi >= 0 && !(nt_type(nt, rhi) && sp_streq(nt_type(nt, rhi), "NilNode"));
      int lo_obj = has_lo && comp_ntype(c, rlo) == rt;
      int hi_obj = has_hi && comp_ntype(c, rhi) == rt;
      /* At least one endpoint is the receiver's class and no present endpoint is
         a different type -- covers two-sided (`lo..hi`), beginless (`..hi`), and
         endless (`lo..`) object ranges. An sp_Range cannot carry the endpoints'
         class (its bounds are sp_int), so unfold to sp_obj_clamp with a nil
         bound for the missing side; sp_obj_clamp skips a nil side. */
      if ((lo_obj || hi_obj) && (!has_lo || lo_obj) && (!has_hi || hi_obj)) {
        /* an exclusive range with a real end (`lo...hi`, `...hi`) cannot clamp
           -- CRuby raises ArgumentError regardless of whether the end would be
           applied (#2587). An endless `lo...` has no end, so it is fine.
           Evaluate the operands in order, then raise. */
        if (has_hi && (int)(nt_int(nt, rn2, "flags", 0) & 4)) {
          const char *ccn = c->classes[ty_object_class(rt)].c_name;
          buf_puts(b, "({ (void)("); emit_boxed(c, recv, b);
          if (has_lo) { buf_puts(b, "); (void)("); emit_boxed(c, rlo, b); }
          buf_puts(b, "); (void)("); emit_boxed(c, rhi, b);
          buf_puts(b, "); sp_raise_cls(\"ArgumentError\", \"cannot clamp with an exclusive range\"); ");
          /* dead default in the receiver's own C type (value vs pointer) */
          if (comp_ty_value_obj(c, rt)) buf_printf(b, "(sp_%s){0}; })", ccn);
          else buf_printf(b, "(sp_%s *)NULL; })", ccn);
          return 1;
        }
        if (comp_ty_value_obj(c, rt))
          buf_printf(b, "(*(sp_%s *)", c->classes[ty_object_class(rt)].c_name);
        else { buf_puts(b, "(("); emit_ctype(c, rt, b); buf_puts(b, ")"); }
        emit_obj_clamp3(c, recv, lo_obj ? rlo : -1, hi_obj ? rhi : -1, b);
        buf_puts(b, ".v.p)");
        return 1;
      }
    }
  }
  /* Comparable#clamp(range) on a user object: int endpoints become bounds fed
     to the user `<=>`; beginless/endless clamp one-sided; an exclusive range
     with a real end raises (CRuby). A clamped result IS the Integer endpoint
     itself, so the value stays boxed (inference: TY_POLY). */
  if (recv >= 0 && ty_is_object(rt) && sp_streq(name, "clamp") && argc == 1 &&
      comp_ntype(c, argv[0]) == TY_RANGE &&
      comp_method_in_chain(c, ty_object_class(rt), "<=>", NULL) >= 0) {
    buf_puts(b, "sp_obj_clamp_range(");
    emit_boxed(c, recv, b); buf_puts(b, ", ");
    emit_expr(c, argv[0], b);
    buf_puts(b, ")");
    return 1;
  }

  /* Default Object#to_s / #inspect on a plain user object with no override:
     box and route through the poly renderers, which produce CRuby's
     "#<Name:0x...>" (inspect appends the ivar list via the registered
     per-class walker). A by-value class has no boxable pointer, so its
     renderer is emitted inline over a stack temp. */
  if (recv >= 0 && ty_is_object(rt) && !c->classes[ty_object_class(rt)].is_struct &&
      (is_text_conversion(name)) && argc == 0 &&
      !obj_str_cname(c, ty_object_class(rt), sp_streq(name, "inspect")) &&
      /* a native class may bind its own to_s/inspect (IO::Buffer does);
         that declared method wins over the default renderer */
      comp_native_method_find(c, ty_object_class(rt), name, 0, 0) < 0) {
    int cid2 = ty_object_class(rt);
    ClassInfo *ci2 = &c->classes[cid2];
    int want_ins = sp_streq(name, "inspect");
    if (ci2->is_value_type) {
      char rn2[512], rarg2[96];
      obj_default_name(c, cid2, rn2, sizeof rn2, rarg2, sizeof rarg2);
      int tv2 = ++g_tmp;
      buf_printf(b, "({ sp_%s _t%d = ", ci2->c_name, tv2); emit_expr(c, recv, b);
      buf_printf(b, "; sp_sprintf(\"#<%s:0x%%016llx", rn2);
      if (want_ins)
        for (int vi = 0; vi < ci2->nivars; vi++)
          buf_printf(b, "%s %s=%%s", vi ? "," : "", ci2->ivars[vi]);
      buf_printf(b, ">\", %s(unsigned long long)(uintptr_t)&_t%d", rarg2, tv2);
      if (want_ins)
        for (int vi = 0; vi < ci2->nivars; vi++) {
          char fb2[300]; snprintf(fb2, sizeof fb2, "_t%d.iv_%s", tv2, iv_c(ci2->ivars[vi] + 1));
          buf_puts(b, ", sp_poly_inspect(");
          emit_boxed_text(c, ci2->ivar_types[vi], fb2, b);
          buf_puts(b, ")");
        }
      buf_puts(b, "); })");
      return 1;
    }
    buf_printf(b, "sp_poly_%s(", want_ins ? "inspect" : "to_s");
    emit_boxed(c, recv, b);
    buf_puts(b, ")");
    return 1;
  }

  { int r; if (emit_struct_recv_call(c, id, b, nt, name, recv, argc, argv, rt, res, &r)) return r; }

  /* object method call: sp_<DefClass>_<m>((sp_<DefClass>*)&recv, args) */
  if (recv >= 0 && ty_is_object(rt)) {
    int cid = ty_object_class(rt);
    /* native (C-backed) class: dispatch a declared instance method to its C
       symbol, receiver first. `string?` returns are wrapped nil-safe. Overload
       selection is type-keyed (putc(65) vs putc("A")). */
    if (c->classes[cid].is_native_class) {
      /* IO::Buffer fast path: get_value/set_value with a LITERAL type symbol
         lowers to the typed accessor (sp_IOBuffer_get_i and friends) --
         no symbol decode, and no boxing the typed slot can hold. The
         wasm-memory / binary-protocol access pattern this class exists for
         is exactly this shape, in a hot loop. The OFFSET need not be
         statically int: emit_int_expr routes a boxed one through
         sp_poly_arg_int_chk, and under --int-overflow=promote every int
         local is boxed, so requiring it lost the lowering everywhere.
         Conditions mirror the analyzer's special-case
         (analyze_infer_recv.c); any declined shape falls through to the
         generic boxed binding below. */
      if (c->classes[cid].c_struct && sp_streq(c->classes[cid].c_struct, "sp_IOBuffer") &&
          argc >= 2 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "SymbolNode") &&
          (comp_ntype(c, argv[1]) == TY_INT || comp_ntype(c, argv[1]) == TY_POLY)) {
        int it = comp_iob_sym_type(nt_str(nt, argv[0], "value"));
        if (it >= 0 && sp_streq(name, "get_value") && argc == 2) {
          if (comp_iob_ty_is_64(it)) {
            buf_puts(b, "sp_IOBuffer_get_x(");
            emit_expr(c, recv, b);
            buf_printf(b, ", %d, ", it);
          }
          else {
            buf_printf(b, "sp_IOBuffer_get_%s(", comp_iob_ty_is_float(it) ? "f" : "i");
            emit_expr(c, recv, b);
            buf_printf(b, ", %d, ", it);
          }
          emit_int_expr(c, argv[1], b);
          buf_puts(b, ")");
          return 1;
        }
        if (it >= 0 && sp_streq(name, "set_value") && argc == 3) {
          TyKind vt = comp_ntype(c, argv[2]);
          int f = comp_iob_ty_is_float(it);
          if ((f && (vt == TY_FLOAT || vt == TY_INT)) || (!f && vt == TY_INT)) {
            buf_printf(b, "sp_IOBuffer_set_%s(", f ? "f" : "i");
            emit_expr(c, recv, b);
            buf_printf(b, ", %d, ", it);
            emit_int_expr(c, argv[1], b);
            buf_puts(b, ", ");
            if (f) emit_float_expr(c, argv[2], b);
            else emit_int_expr(c, argv[2], b);
            buf_puts(b, ")");
            return 1;
          }
          /* A boxed value keeps the lowering but not the typed setter: an
             integer type takes a Float, and u64/s64 take a Bignum, neither of
             which survives sp_int. sp_IOBuffer_set_v hands the value to the
             same core the generic binding would, without the symbol decode. */
          if (vt == TY_POLY) {
            buf_puts(b, "sp_IOBuffer_set_v(");
            emit_expr(c, recv, b);
            buf_printf(b, ", %d, ", it);
            emit_int_expr(c, argv[1], b);
            buf_puts(b, ", ");
            emit_boxed(c, argv[2], b);
            buf_puts(b, ")");
            return 1;
          }
        }
      }
      if (emit_native_splat_call(c, id, cid, name, recv, argc, argv, b)) return 1;
      TyKind natys[8];
      int nta = argc < 8 ? argc : 8;
      for (int a = 0; a < nta; a++) natys[a] = comp_ntype(c, argv[a]);
      int nm = comp_native_method_find_typed(c, cid, name, argc, 0, nta == argc ? natys : NULL);
      if (nm >= 0) {
        /* a :regexp arg binds to a regex literal at the call site (it
           compiles to the generated sp_re_pat_<n> pattern) or to any value
           typed Regexp, which is the same pattern pointer: a parameter or a
           constant holding one (`@s.scan(pat)`, Crass's RE_WHITESPACE,
           #5360); anything else falls through to the generic paths. */
        NativeMethod *mre = &c->native_methods[nm];
        for (int ai = 0; ai < mre->nargs && ai < argc; ai++) {
          if (!sp_streq(mre->args[ai], "regexp") || re_lit_index(c, argv[ai]) >= 0 ||
              comp_ntype(c, argv[ai]) == TY_REGEX) continue;
          /* a nil / true / false where the binding wants a pattern is CRuby's
             TypeError (StringScanner accepts String patterns, so its wording
             is the String one), not a missing method */
          TyKind pat = comp_ntype(c, argv[ai]);
          if (pat == TY_NIL || pat == TY_BOOL) {
            TyKind nrty = repr_of(c, id).as_ty;
            int nrb = ++g_tmp;
            buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); ");
            /* every argument evaluates in order before the raise */
            for (int na = 0; na < argc; na++) {
              if (na == ai && pat == TY_BOOL) {
                buf_printf(b, "int _t%d = (", nrb); emit_expr(c, argv[na], b); buf_puts(b, "); ");
              }
              else {
                buf_puts(b, "(void)("); emit_boxed(c, argv[na], b); buf_puts(b, "); ");
              }
            }
            if (pat == TY_NIL)
              buf_puts(b, "sp_raise_cls(\"TypeError\", \"no implicit conversion of nil into String\");");
            else
              buf_printf(b, "sp_raise_cls(\"TypeError\", _t%d"
                            " ? \"no implicit conversion of true into String\""
                            " : \"no implicit conversion of false into String\");", nrb);
            buf_printf(b, " %s; })", raise_tail_c(c, id, nrty));
            return 1;
          }
          nm = -1; break;
        }
      }
      if (nm >= 0 && emit_native_count_mismatch(c, id, cid, name, 0, recv, argc, argv, b)) return 1;
      if (nm >= 0) {
        NativeMethod *m = &c->native_methods[nm];
        native_arg_check(c, id, "native method", m, argc, argv);
        int wrap = sp_streq(m->ret, "string?");
        /* The two borrowed-buffer return modes, which the native_func arm
           (codegen_call.c) has carried since they were introduced and this
           one never did: a `cstring` is the callee's own storage, clobbered
           by its next call, so it is duplicated onto the GC string heap
           before it escapes into Ruby; a `cbinstr` is the same buffer
           holding raw BYTES, so exactly the count the callee published in
           sp_ffi_bin_len is copied -- strlen stops at the first NUL, and a
           PNG came back as the 8 bytes of its signature (#4821). The result
           is tagged binary for the reason the other arm states: declaring
           this return mode is the callee saying its answer is bytes. */
        int cstr_ret = sp_streq(m->ret, "cstring");
        int bin_ret = sp_streq(m->ret, "cbinstr");
        int bin_tmp = bin_ret ? ++g_tmp : 0;
        if (wrap) buf_puts(b, "sp_box_nullable_str(");
        if (cstr_ret) buf_puts(b, "sp_str_dup_external(");
        /* Sequence the call before the sp_ffi_bin_len read: C leaves
           argument evaluation order unspecified. */
        if (bin_ret) buf_printf(b, "({ const char *_t%d = ", bin_tmp);
        buf_puts(b, m->csym); buf_puts(b, "("); emit_expr(c, recv, b);
        for (int ai = 0; ai < m->nargs && ai < argc; ai++) {
          buf_puts(b, ", ");
          TyKind aw = ffi_spec_to_ty(m->args[ai]);
          if (sp_streq(m->args[ai], "any")) emit_boxed(c, argv[ai], b);
          else if (sp_streq(m->args[ai], "regexp") && re_lit_index(c, argv[ai]) >= 0)
            buf_printf(b, "sp_re_pat_%d", re_lit_index(c, argv[ai]));
          else if (sp_streq(m->args[ai], "regexp")) emit_expr(c, argv[ai], b);
          /* a write payload is the operand's #to_s, as IO#write takes it */
          else if (sp_streq(m->args[ai], "text")) emit_to_s_expr(c, argv[ai], b);
          /* the typed-slot emitters carry the implicit conversion protocol
             (poly unboxing, #to_str / #to_int on a user object) */
          else if (aw == TY_STRING) emit_str_expr(c, argv[ai], b);
          else if (aw == TY_INT) emit_int_expr(c, argv[ai], b);
          else emit_expr(c, argv[ai], b);
        }
        emit_native_rest_args(c, m, argc, argv, b);
        buf_puts(b, ")");
        if (cstr_ret) buf_puts(b, ")");
        if (bin_ret)
          buf_printf(b, "; sp_str_as_binary(sp_str_from_bytes(_t%d, (size_t)(sp_ffi_bin_len < 0 ? 0 : sp_ffi_bin_len))); })",
                     bin_tmp);
        if (wrap) buf_puts(b, ")");
        return 1;
      }
    }
    /* undef'd method: raise NoMethodError */
    if (comp_is_undeffed_in_chain(c, cid, name)) {
      TyKind ret_ty = repr_of(c, id).as_ty;
      {
        char head[200]; snprintf(head, sizeof head, "undefined method '%s' for an instance of ", name);
        buf_puts(b, "(sp_raise_cls(\"NoMethodError\",");
        if (!anon_class_text(c, cid, head, b)) buf_printf(b, "\"%s%s\"", head, c->classes[cid].name);
        buf_printf(b, "),%s)", ret_ty == TY_RANGE ? "(sp_Range){0}" : default_value_from_compiler(c, ret_ty));
      }
      return 1;
    }
    /* instance_variable_get(:@x) / instance_variable_set(:@x, v) with a literal
       symbol or string name. A name present in the known layout lowers to a
       direct field read/write. An undefined-but-valid `@`-name reads as nil
       (get), matching CRuby; a name without a leading `@` raises NameError at
       runtime, also matching CRuby. A dynamic name -- or instance_variable_set
       to a valid name absent from the fixed object layout (no field to write) --
       cannot be represented and is diagnosed. */
    /* instance_variables lists the assigned slots from the class layout. */
    if (sp_streq(name, "instance_variables") && argc == 0 && ty_is_object(rt)) {
      int ivcid = ty_object_class(rt);
      if (ivcid >= 0 && ivcid < c->nclasses) {
        return emit_object_ivar_list(c, recv, ivcid, b);
      }
    }
    if (ty_is_object(rt) &&
        (sp_streq(name, "methods") || sp_streq(name, "public_methods") ||
         sp_streq(name, "singleton_methods")) &&
        an_object_methods_all_arg(c, ty_object_class(rt), argc, argv) >= 0 &&
        emit_object_methods_reflection(c, recv, ty_object_class(rt), name,
                                       an_object_methods_all_arg(c, ty_object_class(rt), argc, argv),
                                       argc > 0 ? argv[0] : -1, b))
      return 1;
    if (emit_object_ivar_call(c, id, name, recv, rt, cid, argc, argv, b)) return 1;
    /* remove_instance_variable(:@x) returns the removed value. The fixed object
       layout can't truly undefine a slot, so a later read still sees the field;
       an undefined name raises NameError, matching CRuby (#3020). */
    if (sp_streq(name, "remove_instance_variable") && argc == 1 && nt_type(nt, argv[0]) &&
        (sp_streq(nt_type(nt, argv[0]), "SymbolNode") || sp_streq(nt_type(nt, argv[0]), "StringNode"))) {
      const char *a0ty = nt_type(nt, argv[0]);
      const char *sym = sp_streq(a0ty, "SymbolNode")
                          ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
      int mi = -1;
      if (sym && sym[0] == '@')
        for (int i = c->classes[cid].is_struct ? c->classes[cid].nmembers : 0; i < c->classes[cid].nivars; i++)
          if (sp_streq(c->classes[cid].ivars[i], sym)) { mi = i; break; }
      /* a boxed field that starts with the unset mark (poly_ivar_unset_marked)
         can be undefined again: an unset one raises NameError, a set one
         answers its value and takes the mark back, so defined? and
         instance_variable_defined? read it as never set, as CRuby does */
      if (mi >= 0 && !comp_ty_value_obj(c, rt) && poly_ivar_unset_marked(c, cid, mi)) {
        int to = ++g_tmp, tv = ++g_tmp;
        buf_printf(b, "({ sp_%s *_t%d = ", c->classes[cid].c_name, to); emit_expr(c, recv, b);
        buf_printf(b, "; sp_RbVal _t%d = _t%d->iv_%s;", tv, to, iv_c(sym + 1));
        buf_printf(b, " if (_t%d.tag == SP_TAG_NIL && _t%d.cls_id == 0x%x)"
                      " sp_raise_cls(\"NameError\", \"instance variable %s not defined\");",
                   tv, tv, SP_IVAR_UNSET_MARK, sym);
        buf_printf(b, " _t%d->iv_%s = ((sp_RbVal){SP_TAG_NIL, 0x%x, {0}}); _t%d; })",
                   to, iv_c(sym + 1), SP_IVAR_UNSET_MARK, tv);
      }
      /* the rest through the layout slot (an Integer or Float field with a
         nil byte answers its nil) */
      else if (mi >= 0) emit_object_ivar_remove(c, id, recv, rt, cid, sym, b);
      else {
        if (recv >= 0) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), "); }
        else buf_puts(b, "(");
        buf_printf(b, "sp_raise_cls(\"NameError\", \"instance variable %s not defined\"), sp_box_nil())",
                   sym ? sym : "");
      }
      return 1;
    }

    /* attr reader -> field access (recv).iv_x, UNLESS an explicit method of
       the same name overrides it at an equal-or-more-derived class. CRuby:
       attr_reader defines an ordinary method, so a subclass `def x` (or a
       same-class `def x`) overrides it via normal dispatch rather than
       reading the field. Whichever definition sits in the more-derived class
       wins; on a same-class tie the explicit method wins. */
    int rdc = -1, mdc = -1;
    if (comp_reader_in_chain(c, cid, name, &rdc)) {
      int reader_wins = comp_resolve_member(c, cid, name, 0, &mdc, NULL) == SP_MEMBER_ATTR;
      if (reader_wins) {
        /* a reader is zero-arity: excess arguments are CRuby's ArgumentError
           (a Struct member read with an argument answered the member). A
           splat / keyword-hash / forwarding argument can be empty at run
           time -- zero arguments to CRuby -- so those stay unguarded. */
        int rdr_dynamic = 0;
        for (int ra = 0; ra < argc; ra++) {
          const char *rat = nt_type(nt, argv[ra]);
          if (rat && (sp_streq(rat, "SplatNode") || sp_streq(rat, "KeywordHashNode") ||
                      nt_kind(nt, argv[ra]) == NK_ForwardingArgumentsNode ||
                      sp_streq(rat, "BlockArgumentNode")))
            { rdr_dynamic = 1; break; }
        }
        if (argc > 0 && !rdr_dynamic) {
          TyKind rrty2 = repr_of(c, id).as_ty;
          buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); ");
          for (int ra = 0; ra < argc; ra++) {
            buf_puts(b, "(void)("); emit_expr(c, argv[ra], b); buf_puts(b, "); ");
          }
          buf_printf(b, "sp_raise_cls(\"ArgumentError\","
                        " \"wrong number of arguments (given %d, expected 0)\"); %s; })",
                     argc, raise_tail_c(c, id, rrty2));
          return 1;
        }
        const char *rn2 = comp_resolve_alias(c, cid, name);
        /* A receiver typed for this class can hold an instance of a subclass
           whose `def` overrides the reader: `self` in one of the class's own
           methods, or an object such a method answers. The read then
           dispatches on the runtime class, as a receiverless call to the
           reader does, and the receiver is evaluated once. */
        TyKind ovty = comp_ty_value_obj(c, rt) ? TY_UNKNOWN : reader_override_ty(c, id, cid, name);
        if (ovty != TY_UNKNOWN) {
          const char *ovrt = nt_type(nt, recv);
          int ov_simple = ovrt && (sp_streq(ovrt, "LocalVariableReadNode") ||
                                   sp_streq(ovrt, "InstanceVariableReadNode") || sp_streq(ovrt, "SelfNode"));
          int ovt = ov_simple ? -1 : ++g_tmp;
          Buf ovs; memset(&ovs, 0, sizeof ovs);
          if (ov_simple) ovs = expr_buf(c, recv);
          else buf_printf(&ovs, "_t%d", ovt);
          Buf ovr; memset(&ovr, 0, sizeof ovr);
          buf_printf(&ovr, "(%s)->iv_%s", ovs.p, iv_c(rn2));
          if (!ov_simple) {
            buf_puts(b, "({ "); emit_ctype(c, rt, b);
            buf_printf(b, " _t%d = ", ovt); emit_expr(c, recv, b);
            buf_printf(b, "; SP_GC_ROOT(_t%d); ", ovt);
          }
          emit_reader_override_dispatch(c, id, cid, name, ovs.p, ovr.p, ovty, b);
          if (!ov_simple) buf_puts(b, "; })");
          free(ovs.p); free(ovr.p);
          return 1;
        }
        /* a shared-mutable string slot reads out as a GC COPY of the current
           contents (the raw sp_String* handle must not leak into a plain
           string context); a demand-marked read hands out the handle (#3227) */
        char ivfull[300]; snprintf(ivfull, sizeof ivfull, "@%s", rn2);
        int rdiv = comp_ivar_index(&c->classes[rdc >= 0 ? rdc : cid], ivfull);
        if (rdiv >= 0 &&
            c->classes[rdc >= 0 ? rdc : cid].ivar_types[rdiv] == TY_STRBUF) {
          int tvR = ++g_tmp;
          buf_printf(b, "({ sp_String *_t%d = (", tvR);
          emit_expr(c, recv, b);
          buf_printf(b, ")%siv_%s; ", comp_ty_value_obj(c, rt) ? "." : "->", iv_c(rn2));
          /* either mark is the same demand: strbuf_box carries it with the
             node's type, strbuf_handle_demand without it (see compiler.h) */
          if (repr_of(c, id).handle || repr_of(c, id).demand)
            buf_printf(b, "_t%d; })", tvR);
          /* the only consumer READS the bytes and keeps no pointer past the
             call, so the live buffer serves it. The copy below is O(len) and
             this read is typically in a loop: `ctx.buf.getbyte(i)` over a
             200 KB buffer copied the whole string once per row. Same type,
             same const char * -- the live buffer rather than a snapshot. */
          else if (repr_of(c, id).read_raw && decide_node(c->nt, id, "strbuf-raw", NULL))
            buf_printf(b, "_t%d ? sp_String_cstr(_t%d) : NULL; })", tvR, tvR);
          else
            buf_printf(b, "sp_strbuf_read(_t%d); })", tvR);
          return 1;
        }
        /* a field with a nil bit is read with it where the call's consumer
           wants the oint (node_is_oint answers the reader by that bit) */
        if (rdiv >= 0 && node_is_oint(c, id) &&
            ivar_has_nilbit(c, rdc >= 0 ? rdc : cid, rdiv)) {
          int rcls = rdc >= 0 ? rdc : cid;
          TyKind rivt = c->classes[rcls].ivar_types[rdiv];
          int trd = ++g_tmp;
          char objp[48]; snprintf(objp, sizeof objp, "_t%d%s", trd, comp_ty_value_obj(c, rt) ? "." : "->");
          char bit[400]; ivar_nilbit_test(c, rcls, rdiv, objp, bit, sizeof bit);
          buf_puts(b, "({ "); emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", trd); emit_expr(c, recv, b);
          buf_printf(b, "; ((%s){ %siv_%s, (%s) != 0 }); })", oint_ctype(rivt), objp, iv_c(rn2), bit);
          return 1;
        }
        buf_puts(b, "("); emit_expr(c, recv, b);
        buf_printf(b, ")%siv_%s", comp_ty_value_obj(c, rt) ? "." : "->", iv_c(rn2));
        return 1;
      }
    }
    /* the method is the call's plan (call_plan.c) when the plan is the
       receiver class's own lookup of the name; otherwise, and under
       --plan-check as the assertion, the arm looks it up itself */
    const CallPlan *opl = cplan_user(c, id);
    int mi = opl->chain && opl->via == UC_INST && opl->owner_ci == cid ? opl->mi : -1;
    if (g_plan_check && mi >= 0) cplan_served("object-call");
    if (g_plan_check || mi < 0) {
      int omi = comp_method_in_chain(c, cid, name, NULL);
      if (mi < 0) {
        if (g_plan_check && omi >= 0)
          fprintf(stderr, "plan-check: cplan-fallback: object-call node %d %s\n", id, name);
        mi = omi;
      }
      else if (omi != mi)
        fprintf(stderr, "plan-check: cplan-conflict: object-call node %d %s: plan %d, lookup %d\n", id, name, mi, omi);
    }
    /* a demand-marked read through a simple hand-written reader
       (`def body = @body`) hands out the ivar HANDLE via a field access:
       the C reader function returns the safe copy (#3227 P5). Either mark
       is the demand, as for the attr reader above: strbuf_handle_demand
       alone (an `equal?` operand, mark_reader_identity_operands) left the
       call to the reader, which answered a copy, and `b.v.equal?(w)` was
       false for one String. */
    if (mi >= 0 && (repr_of(c, id).handle || repr_of(c, id).demand)) {
      int lastH = scope_body_last(c, mi);
      if (lastH >= 0 && nt_kind(nt, lastH) == NK_InstanceVariableReadNode) {
        const char *ivnH = nt_str(nt, lastH, "name");
        int defcH = c->scopes[mi].class_id;
        int ivH = (ivnH && defcH >= 0) ? comp_ivar_index(&c->classes[defcH], ivnH) : -1;
        if (ivH >= 0 && c->classes[defcH].ivar_types[ivH] == TY_STRBUF) {
          /* a body with statements before the read (`@reads += 1; @s`)
             runs them first, then the slot is the handle it answered */
          int nH = 0;
          nt_arr(nt, c->scopes[mi].body, "body", &nH);
          if (nH > 1 && !comp_ty_value_obj(c, rt)) {
            int tH = ++g_tmp;
            Buf rbH = expr_buf(c, recv);
            buf_puts(b, "({ "); emit_ctype(c, rt, b);
            buf_printf(b, " _t%d = %s; SP_GC_ROOT(_t%d); (void)", tH, rbH.p ? rbH.p : "", tH);
            free(rbH.p);
            char selfH[32]; snprintf(selfH, sizeof selfH, "_t%d", tH);
            emit_dispatch(c, cid, name, selfH, nt_ref(nt, id, "arguments"), nt_ref(nt, id, "block"), b);
            buf_printf(b, "; _t%d->iv_%s; })", tH, iv_c(ivnH + 1));
            return 1;
          }
          buf_puts(b, "(");
          emit_expr(c, recv, b);
          buf_printf(b, ")%siv_%s", comp_ty_value_obj(c, rt) ? "." : "->", iv_c(ivnH + 1));
          return 1;
        }
      }
    }
    /* a memoizing reader (`def s = (@s ||= +"")`) has to run to fill the
       slot first; then the slot is the handle */
    if (mi >= 0 && (repr_of(c, id).handle || repr_of(c, id).demand)) {
      const char *ivnM = an_memo_reader_ivar(c, mi);
      int defcM = c->scopes[mi].class_id;
      int ivM = (ivnM && defcM >= 0) ? comp_ivar_index(&c->classes[defcM], ivnM) : -1;
      if (ivM >= 0 && c->classes[defcM].ivar_types[ivM] == TY_STRBUF && !comp_ty_value_obj(c, rt)) {
        int tM = ++g_tmp;
        Buf rbM = expr_buf(c, recv);
        buf_puts(b, "({ "); emit_ctype(c, rt, b);
        buf_printf(b, " _t%d = %s; SP_GC_ROOT(_t%d); (void)", tM, rbM.p ? rbM.p : "", tM);
        free(rbM.p);
        char selfM[32]; snprintf(selfM, sizeof selfM, "_t%d", tM);
        emit_dispatch(c, cid, name, selfM, nt_ref(nt, id, "arguments"), nt_ref(nt, id, "block"), b);
        buf_printf(b, "; _t%d->iv_%s; })", tM, iv_c(ivnM + 1));
        return 1;
      }
    }
    /* Keep the reader shortcuts above. Other marked String calls still
       return bytes: take the proved return channel before boxing them,
       using the same fact as strbuf_marked_yields_handle. */
    if (repr_share_rule(c) && repr_of(c, id).handle && repr_call_returns_handle(c, id))
      return emit_strbuf_route(c, id, b);
    /* Without sharing, the same handle demand wraps the method's byte
       result as the container's mutable copy. Reader shortcuts have
       already handed out their slots; other calls keep their byte ABI. */
    if (!repr_share_rule(c) && mi >= 0 && repr_of(c, id).handle && strbuf_marked_yields_handle(c, id)) {
      int sv = view_push_repr(c, id, VR_STRBUF_BOX, 0);
      buf_puts(b, "sp_String_new_shared(");
      emit_str_expr(c, id, b);
      buf_puts(b, ")");
      view_pop(c, sv);
      return 1;
    }
    if (mi >= 0) {
      /* a value-type receiver is passed by value; an ordinary object by
         pointer. For a value recv we hand emit_dispatch the value expression
         (lvalue or hoisted temp); the method takes `self` by value. */
      if (comp_ty_value_obj(c, rt)) {
        /* The receiver's C text lives on the heap: a local's C name is as
           long as its Ruby name, and a 64-byte buffer cut a 61-character
           name down to another local's name with no diagnostic. */
        Buf selfv; memset(&selfv, 0, sizeof selfv);
        const char *rty = nt_type(nt, recv);
        if (rty && (sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode")))
          selfv = expr_buf(c, recv);
        else {
          int t = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = ", t); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";"); free(rb.p);
          /* The rest of the statement may collect before it reads self. */
          if (ty_gc_holds_refs(c, rt)) { buf_puts(g_pre, " "); emit_gc_root_tmp_refs(c, rt, t, g_pre); }
          buf_puts(g_pre, "\n"); buf_printf(&selfv, "_t%d", t);
        }
        TyKind svt = TY_UNKNOWN;
        int sv = setter_value_open(c, id, b, &svt);
        emit_dispatch(c, cid, name, selfv.p ? selfv.p : "", nt_ref(nt, id, "arguments"), nt_ref(nt, id, "block"), b);
        setter_value_close(c, id, svt, b, sv);
        free(selfv.p);
        return 1;
      }
      /* receiver is a pointer; reuse it directly if it's a simple lvalue,
         else stash in a temp (the virtual-dispatch switch references it
         multiple times) */
      Buf selfptr; memset(&selfptr, 0, sizeof selfptr);   /* heap text, as above */
      const char *rty = nt_type(nt, recv);
      if (rty && (sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode")))
        selfptr = expr_buf(c, recv);
      else {
        int t = ++g_tmp;
        /* emit the receiver first so any setup it pushes into g_pre is fully
           flushed before we write this temp's declaration line */
        Buf rb = expr_buf(c, recv);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, rt, g_pre);
        buf_printf(g_pre, " _t%d = ", t);
        buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
        /* Root the hoisted receiver: a freshly constructed object (e.g.
           Scene.new.render(...)) must survive any GC the call triggers. */
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
        buf_printf(&selfptr, "_t%d", t);
      }
      TyKind svt = TY_UNKNOWN;
      int sv = setter_value_open(c, id, b, &svt);
      emit_dispatch(c, cid, name, selfptr.p ? selfptr.p : "", nt_ref(nt, id, "arguments"), nt_ref(nt, id, "block"), b);
      setter_value_close(c, id, svt, b, sv);
      free(selfptr.p);
      return 1;
    }
  }
  return 0;
}


/* String#encode(dst [, src] [, invalid:, undef:, replace:]): the destination
   and source (a name or an Encoding) and the three keywords travel boxed, and
   the runtime decides the pair. `recv_txt` is the receiver's `const char *`
   expression. */

/* String#force_encoding / #encode! on `recv_txt`, a `const char *` expression.
   The argument was once ignored entirely, so `force_encoding("ASCII-8BIT")`
   left the string naming UTF-8 -- and spinel's one tag is exactly what that
   argument asks for. A constant path (Encoding::BINARY) or a string literal
   both name it; anything else keeps the no-op, since spinel has no third
   encoding to move to.
   The receiver is evaluated ONCE, into a temp: it feeds both the mutability
   check and the retag, and a receiver that is a call with effects --
   campfire's `request.body.read.force_encoding("UTF-8")`, where `read`
   advances a cursor -- ran twice and retagged the second, empty, read. */
/* The encoding a force_encoding / encode! call names as a literal: 1 for
   ASCII-8BIT, 0 for UTF-8, -1 for any other or a computed one, which only
   checks that the receiver may change. */
/* Encoding::X or ::Encoding::X; a user constant (M::E) is a value */
static int enc_const_path(const NodeTable *nt, int n) {
  const char *at = nt_type(nt, n);
  if (!at || !sp_streq(at, "ConstantPathNode")) return 0;
  int p = nt_ref(nt, n, "parent");
  const char *pt = p >= 0 ? nt_type(nt, p) : NULL;
  const char *pn = p >= 0 ? nt_str(nt, p, "name") : NULL;
  if (!pt || !pn || !sp_streq(pn, "Encoding")) return 0;
  return sp_streq(pt, "ConstantReadNode") || (sp_streq(pt, "ConstantPathNode") && nt_ref(nt, p, "parent") < 0);
}
static int str_force_encoding_mode(Compiler *c, const int *argv, int argc) {
  const NodeTable *nt = c->nt;
  const char *fe_nm = NULL;
  if (argc >= 1) {
    const char *at = nt_type(nt, argv[0]);
    if (enc_const_path(nt, argv[0])) fe_nm = nt_str(nt, argv[0], "name");
    else if (at && sp_streq(at, "StringNode")) {
      fe_nm = nt_str(nt, argv[0], "unescaped");
      if (!fe_nm) fe_nm = nt_str(nt, argv[0], "content");
    }
  }
  int fe_bin = 0, fe_txt = 0;
  if (fe_nm) {
    char fe_up[32]; size_t fl = 0;
    for (; fe_nm[fl] && fl < sizeof fe_up - 1; fl++) {
      char ch = fe_nm[fl];
      fe_up[fl] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : (ch == '_' ? '-' : ch);
    }
    fe_up[fl] = 0;
    fe_bin = sp_streq(fe_up, "ASCII-8BIT") || sp_streq(fe_up, "BINARY");
    /* US-ASCII has no tag of its own: it is text, so a high byte reads as
       the invalid character it is there rather than as a binary byte */
    fe_txt = sp_streq(fe_up, "UTF-8") || sp_streq(fe_up, "US-ASCII") || sp_streq(fe_up, "ASCII");
  }
  return fe_bin ? 1 : fe_txt ? 0 : -1;
}
/* force_encoding(enc) with an enc that is no literal (a local, a default
   parameter): the runtime picks the tag from the Encoding or name. */
static int str_force_encoding_computed(Compiler *c, const char *name, const int *argv, int argc) {
  if (!sp_streq(name, "force_encoding") || argc != 1) return 0;
  const char *at = nt_type(c->nt, argv[0]);
  return !enc_const_path(c->nt, argv[0]) && !(at && sp_streq(at, "StringNode"));
}
/* The mode argument of sp_String_force_encoding on handle `h`. A frozen
   handle raises before the argument is looked at, as in CRuby. */
static void emit_strbuf_force_encoding_mode(Compiler *c, const char *name, const char *h, const int *argv, int argc, Buf *b) {
  if (!str_force_encoding_computed(c, name, argv, argc)) {
    buf_printf(b, "%d", str_force_encoding_mode(c, argv, argc));
    return;
  }
  buf_printf(b, "sp_String_is_frozen(%s) ? -1 : sp_force_encoding_mode(", h);
  emit_boxed(c, argv[0], b);
  buf_puts(b, ")");
}
void emit_str_force_encoding(Compiler *c, const char *name, const char *r, const int *argv, int argc, Buf *b) {
  int mode = str_force_encoding_mode(c, argv, argc);
  int fe_bin = mode == 1, fe_txt = mode == 0;
  int trc = ++g_tmp;
  /* a nil receiver (a nullable String slot, #4567) is NoMethodError, not the
     FrozenError the mutability check reads a NULL as */
  buf_printf(b, "({ const char *_t%d = %s; if (!_t%d) sp_nil_recv(\"%s\"); sp_str_check_mutable(_t%d); ",
             trc, r, trc, name, trc);
  if (str_force_encoding_computed(c, name, argv, argc)) {
    buf_printf(b, "int _m%d = sp_force_encoding_mode(", trc);
    emit_boxed(c, argv[0], b);
    buf_printf(b, "); if (_m%d == 1) sp_str_as_binary(_t%d); else if (_m%d == 0) sp_str_as_text(_t%d); _t%d; })",
               trc, trc, trc, trc, trc);
  }
  else if (fe_bin) buf_printf(b, "sp_str_as_binary(_t%d); })", trc);
  else if (fe_txt) buf_printf(b, "sp_str_as_text(_t%d); })", trc);
  else buf_printf(b, "_t%d; })", trc);
}
/* The same on a shared String handle (#6179), whose ASCII-8BIT tag lives on
   the handle and is stamped back into its bytes after every growth
   (sp_fd_publish). Marked on the bytes alone, the tag went to a copy -- a
   handle local or ivar reads back as one -- or was dropped by the next
   append that moved the bytes. `ref` is the handle; the value is its bytes. */
static void emit_strbuf_force_encoding(Compiler *c, const char *name, const char *ref, int hr, const int *argv, int argc, Buf *b) {
  int th = ++g_tmp;
  char hv[24]; snprintf(hv, sizeof hv, "_t%d", th);
  buf_printf(b, "({ sp_String *_t%d = %s;", th, ref);
  emit_route_recv_root(c, hr, th, argc, argv, TY_STRING, b);
  buf_printf(b, " if (!_t%d) sp_nil_recv(\"%s\"); sp_String_force_encoding(_t%d, ", th, name, th);
  emit_strbuf_force_encoding_mode(c, name, hv, argv, argc, b);
  buf_printf(b, "); sp_String_cstr(_t%d); })", th);
}
static void emit_str_encode_call(Compiler *c, const char *recv_txt, const int *argv, int argc, Buf *b) {
  const NodeTable *nt = c->nt;
  int kwh = -1, pos = argc;
  if (argc > 0 && nt_type(nt, argv[argc - 1]) && sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
    kwh = argv[argc - 1]; pos = argc - 1;
  }
  buf_printf(b, "sp_str_encode(%s, ", recv_txt);
  if (pos >= 1) emit_boxed(c, argv[0], b); else buf_puts(b, "sp_box_nil()");
  buf_puts(b, ", ");
  if (pos >= 2) emit_boxed(c, argv[1], b); else buf_puts(b, "sp_box_nil()");
  static const char *const KW[] = { "invalid", "undef", "replace" };
  for (int k = 0; k < 3; k++) {
    buf_puts(b, ", ");
    int v = kwh >= 0 ? kwh_lookup(nt, kwh, KW[k]) : -1;
    if (v >= 0) emit_boxed(c, argv[0] == v ? v : v, b); else buf_puts(b, "sp_box_nil()");
  }
  buf_puts(b, ")");
}
int emit_value_recv_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = comp_recv_type(c, recv);
  /* Time instance methods: sp_Time is a value -- splice the receiver once. */
  if (recv >= 0 && rt == TY_TIME) {
    Buf rs = expr_buf(c, recv);
    const char *r = rs.p ? rs.p : "";
    int done = 1;
    /* CRuby's #utc/#gmtime/#localtime mutate the receiver (and return it);
       the get* flavors copy. sp_Time is a value struct, so when the receiver
       is an LVALUE (a local, an ivar slot) the mutation is a write-back
       assignment -- `v.utc` then really updates v (#2637). A temporary
       receiver has nothing to observe afterwards, so the copy serves it. */
    int r_lval = nt_type(nt, recv) && (sp_streq(nt_type(nt, recv), "LocalVariableReadNode") ||
                                       sp_streq(nt_type(nt, recv), "InstanceVariableReadNode"));
    if ((sp_streq(name, "utc") || sp_streq(name, "gmtime")) && r_lval)
      buf_printf(b, "(%s = sp_time_utc(%s))", r, r);
    else if (sp_streq(name, "localtime") && argc == 0 && r_lval)
      buf_printf(b, "(%s = sp_time_localtime(%s))", r, r);
    else if (sp_streq(name, "utc") || sp_streq(name, "gmtime") || sp_streq(name, "getutc")) buf_printf(b, "sp_time_utc(%s)", r);
    /* getlocal(zone)/localtime(zone): a fixed UTC offset, given as seconds or
       a zone string, reinterprets the instant in that zone (#3093) */
    else if ((is_local_time(name)) && argc == 1) {
      int mutate = sp_streq(name, "localtime") && r_lval;
      if (mutate) buf_printf(b, "(%s = ", r);
      /* a String zone ("+01:00", "UTC", "A"), or a user object naming one
         through #to_str, reads as `in:` does; an Integer is a second count;
         any other value is resolved at run time (sp_time_in_zone_v) */
      TyKind ot = comp_ntype(c, argv[0]);
      if (ot == TY_STRING || obj_conv_method(c, ot, "to_str", TY_STRING, NULL) >= 0) {
        buf_printf(b, "sp_time_in_zone_s(%s, ", r); emit_str_expr(c, argv[0], b);
      }
      else if (ot == TY_INT) { buf_printf(b, "sp_time_getlocal_off(%s, ", r); emit_int_expr(c, argv[0], b); }
      else { buf_printf(b, "sp_time_in_zone_v(%s, ", r); emit_boxed(c, argv[0], b); buf_puts(b, ", 1"); }
      buf_puts(b, ")");
      if (mutate) buf_puts(b, ")");
    }
    else if (is_local_time(name)) buf_printf(b, "sp_time_localtime(%s)", r);
    /* the plain readers: builtin-op rows (builtin_ops.c) */
    else if (emit_builtin_op_text(c, id, recv, rt, name, r, b)) ;
    else if (sp_streq(name, "iso8601") && sp_feature_enabled("time")) {
      if (argc == 1) { buf_printf(b, "sp_time_iso8601_frac(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else buf_printf(b, "sp_time_iso8601(%s)", r);
    }
    else if (sp_streq(name, "httpdate") && sp_feature_enabled("time")) buf_printf(b, "sp_time_httpdate(%s)", r);
    else if ((sp_streq(name, "rfc2822") || sp_streq(name, "rfc822")) && sp_feature_enabled("time"))
      buf_printf(b, "sp_time_rfc2822(%s)", r);
    else if (sp_streq(name, "eql?") && argc == 1) {
      if (comp_ntype(c, argv[0]) == TY_TIME) {
        int tt = ++g_tmp, tu = ++g_tmp;
        buf_printf(b, "({ sp_Time _t%d = %s; sp_Time _t%d = ", tt, r, tu); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_time_cmp(_t%d, _t%d) == 0; })", tt, tu);
      }
      /* a poly operand may hold a Time: unwrap and compare by instant */
      else if (repr_of(c, argv[0]).kind == RK_BOXED) {
        int tt = ++g_tmp, tq = ++g_tmp;
        buf_printf(b, "({ sp_Time _t%d = %s; sp_RbVal _t%d = ", tt, r, tq); emit_expr(c, argv[0], b);
        buf_printf(b, "; (sp_bool)(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_TIME && "
                      "sp_time_cmp(_t%d, *(sp_Time *)_t%d.v.p) == 0); })", tq, tq, tt, tq);
      }
      else { buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
    }
    else if (sp_streq(name, "to_a") && argc == 0) {
      /* [sec, min, hour, mday, mon, year, wday, yday, isdst, zone] */
      int tt = ++g_tmp, ta = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = %s; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tt, r, ta, ta);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_time_sec(_t%d)));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_time_min(_t%d)));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_time_hour(_t%d)));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_time_mday(_t%d)));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_time_mon(_t%d)));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_time_year(_t%d)));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_time_wday(_t%d)));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_time_yday(_t%d)));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_bool(sp_time_isdst(_t%d) != 0));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_time_zone(_t%d)));", ta, tt);
      buf_printf(b, " _t%d; })", ta);
    }
    else if (sp_streq(name, "deconstruct_keys") && argc == 1) {
      /* a Hash of the requested keys (or all when the argument is nil). Each
         key's value carries its own boxing (int / bool / string / rational);
         `%d` in `vfmt` is the time temp id. */
      static const struct { const char *k, *vfmt; } TK[] = {
        {"year", "sp_box_int(sp_time_year(_t%d))"}, {"month", "sp_box_int(sp_time_mon(_t%d))"},
        {"mon", "sp_box_int(sp_time_mon(_t%d))"}, {"day", "sp_box_int(sp_time_mday(_t%d))"},
        {"mday", "sp_box_int(sp_time_mday(_t%d))"}, {"hour", "sp_box_int(sp_time_hour(_t%d))"},
        {"min", "sp_box_int(sp_time_min(_t%d))"}, {"sec", "sp_box_int(sp_time_sec(_t%d))"},
        {"wday", "sp_box_int(sp_time_wday(_t%d))"}, {"yday", "sp_box_int(sp_time_yday(_t%d))"},
        {"subsec", "(_t%d.tv_nsec == 0 ? sp_box_int(0) : sp_box_rational(sp_rational_new((sp_int)_t%d.tv_nsec, 1000000000)))"},
        {"dst", "sp_box_bool(sp_time_isdst(_t%d) != 0)"},
        {"zone", "sp_box_str(sp_time_zone(_t%d))"}, {NULL, NULL} };
      /* which keys: a literal array selects them; nil (or non-literal) is all */
      int arr = argv[0];
      int is_arr = nt_type(nt, arr) && sp_streq(nt_type(nt, arr), "ArrayNode");
      int tt = ++g_tmp, th = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = %s; sp_SymPolyHash *_t%d = sp_SymPolyHash_new(); SP_GC_ROOT(_t%d);", tt, r, th, th);
      /* runtime sp_sym_intern for the key: these symbols are synthesized during
         body emission, after the static sp_sym_names table is written, so a
         compile-time id would have no name at run time (rendered as "") (#2866). */
      #define SG_EMIT_KEY(K, VFMT) do { \
        buf_printf(b, " sp_SymPolyHash_set(_t%d, sp_sym_intern(\"%s\"), ", th, (K)); \
        buf_printf(b, (VFMT), tt, tt); buf_puts(b, ");"); \
      } while (0)
      if (is_arr) {
        int en = 0; const int *els = nt_arr(nt, arr, "elements", &en);
        for (int e = 0; e < en; e++) {
          const char *ety = nt_type(nt, els[e]);
          const char *sk = (ety && sp_streq(ety, "SymbolNode")) ? nt_str(nt, els[e], "value") : NULL;
          if (!sk) continue;
          for (int t = 0; TK[t].k; t++)
            if (sp_streq(sk, TK[t].k)) { SG_EMIT_KEY(TK[t].k, TK[t].vfmt); break; }
        }
      }
      else {
        /* CRuby's full key set, in order */
        static const char *const allk[] = {"year","month","day","yday","wday","hour","min","sec","subsec","dst","zone",NULL};
        for (int a = 0; allk[a]; a++)
          for (int t = 0; TK[t].k; t++)
            if (sp_streq(allk[a], TK[t].k)) { SG_EMIT_KEY(TK[t].k, TK[t].vfmt); break; }
      }
      #undef SG_EMIT_KEY
      buf_printf(b, " sp_box_obj(_t%d, SP_BUILTIN_SYM_POLY_HASH); })", th);
    }
    /* Comparable#between? / #clamp compare a Time with a Time; CRuby raises
       ArgumentError ("comparison of Time with 1 failed") for anything else,
       where reading the operand as an sp_Time did not compile (#3865). */
    else if ((sp_streq(name, "between?") || sp_streq(name, "clamp")) && argc == 2 &&
             (comp_ntype(c, argv[0]) != TY_TIME || comp_ntype(c, argv[1]) != TY_TIME)) {
      int tt = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = %s; (void)(", tt, r);
      emit_boxed(c, argv[0], b); buf_puts(b, "); (void)(");
      emit_boxed(c, argv[1], b);
      buf_puts(b, "); sp_raise_cls(\"ArgumentError\", sp_sprintf(\"comparison of Time with %s failed\", sp_poly_inspect(");
      emit_boxed(c, argv[0], b);
      if (sp_streq(name, "clamp")) buf_printf(b, "))); _t%d; })", tt);
      else buf_puts(b, "))); (sp_bool)0; })");
    }
    else if (sp_streq(name, "between?") && argc == 2) {
      int tt = ++g_tmp, ta = ++g_tmp, tb2 = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = %s; sp_Time _t%d = ", tt, r, ta); emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_Time _t%d = ", tb2); emit_expr(c, argv[1], b);
      buf_printf(b, "; sp_time_cmp(_t%d, _t%d) >= 0 && sp_time_cmp(_t%d, _t%d) <= 0; })", tt, ta, tt, tb2);
    }
    else if (sp_streq(name, "clamp") && argc == 2) {
      int tt = ++g_tmp, ta = ++g_tmp, tb2 = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = %s; sp_Time _t%d = ", tt, r, ta); emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_Time _t%d = ", tb2); emit_expr(c, argv[1], b);
      buf_printf(b, "; sp_time_cmp(_t%d, _t%d) < 0 ? _t%d : (sp_time_cmp(_t%d, _t%d) > 0 ? _t%d : _t%d); })",
                 tt, ta, ta, tt, tb2, tb2, tt);
    }
    else if ((sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") ||
              sp_streq(name, ">=") || sp_streq(name, "==") || sp_streq(name, "!=")) && argc == 1 &&
             comp_ntype(c, argv[0]) == TY_TIME) {
      int tt = ++g_tmp, tu = ++g_tmp;
      buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = %s; sp_Time _t%d = ", tt, r, tu);
      emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_time_cmp(_t%d, _t%d) %s 0; })", tt, tu, name);
    }
    /* a poly operand (a `Time | nil` local past its nil guard, #4465) is
       checked at run time: a Time compares, anything else raises as below */
    else if (is_cmp_op(name) && argc == 1 &&
             (comp_ntype(c, argv[0]) == TY_POLY || repr_of(c, argv[0]).untyped)) {
      int tt = ++g_tmp, tu = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = %s; sp_RbVal _t%d = ", tt, r, tu); emit_boxed(c, argv[0], b);
      buf_printf(b, "; sp_poly_time_cmp_arg(_t%d, _t%d) %s 0; })", tt, tu, name);
    }
    /* a relational comparison against a non-Time operand: CRuby's Comparable
       raises ArgumentError (its <=> returned nil). Evaluate the operand first. */
    else if (is_cmp_op(name) && argc == 1) {
      buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
      buf_puts(b, "); sp_raise_cls(\"ArgumentError\", \"comparison of Time with an incompatible value failed\"); 0; })");
    }
    else if (sp_streq(name, "<=>") && argc == 1 && comp_ntype(c, argv[0]) == TY_TIME) {
      int tt = ++g_tmp, tu = ++g_tmp;
      buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = %s; sp_Time _t%d = ", tt, r, tu);
      emit_expr(c, argv[0], b);
      buf_printf(b, "; (sp_int)sp_time_cmp(_t%d, _t%d); })", tt, tu);
    }
    /* Time <=> non-Time is nil (poly). A poly operand is checked at runtime. */
    else if (sp_streq(name, "<=>") && argc == 1) {
      TyKind a0t = comp_ntype(c, argv[0]);
      if (a0t == TY_POLY || a0t == TY_UNKNOWN) {
        int tt = ++g_tmp, tu = ++g_tmp;
        buf_printf(b, "({ sp_Time _t%d = %s; sp_RbVal _t%d = ", tt, r, tu); emit_boxed(c, argv[0], b);
        buf_printf(b, "; (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_TIME) ? "
                      "sp_box_int(sp_time_cmp(_t%d, *(sp_Time *)_t%d.v.p)) : sp_box_nil(); })",
                   tu, tu, tt, tu);
      }
      else { buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), sp_box_nil())"); }
    }
    else done = 0;
    free(rs.p);
    if (done) return 1;
  }

  /* Process::Status readers: builtin-op rows (builtin_ops.c) */
  if (recv >= 0 && rt == TY_PROCESS_STATUS && emit_builtin_op(c, id, recv, rt, name, b)) return 1;

  /* StringScanner instance methods. String-returning methods may yield NULL
     (nil) on a miss; the NULL-aware string output operators render that. */
  /* StringScanner dispatch: native-bound (packages/strscan); no arms here. */
  /* MatchData instance methods (sp_MatchData *, nullable on no-match). */
  if (recv >= 0 && rt == TY_MATCHDATA) {
    Buf rs = expr_buf(c, recv);
    const char *r = rs.p ? rs.p : "";
    /* the plain readers: builtin-op rows (builtin_ops.c) */
    if (emit_builtin_op_text(c, id, recv, rt, name, r, b)) ;
    else if (sp_streq(name, "[]") && argc == 1 &&
        (comp_ntype(c, argv[0]) == TY_RANGE ||
         (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode")))) {
      /* md[range]: the groups over that index range (#2532) */
      int t = ++g_tmp;
      buf_printf(b, "({ sp_Range _t%d = sp_range_ix(", t); emit_expr(c, argv[0], b); buf_puts(b, ")");
      buf_printf(b, "; sp_MatchData_aref_range(%s, _t%d.first, _t%d.last, (int)_t%d.excl); })", r, t, t, t);
    }
    else if (sp_streq(name, "[]") && argc == 1) {
      /* A Symbol/String key selects a named capture group; an Integer key is a
         positional group (the existing path). */
      TyKind kt = repr_of(c, argv[0]).as_ty;
      if (kt == TY_SYMBOL) { buf_printf(b, "sp_MatchData_aref_name(%s, sp_sym_to_s(", r); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
      else if (kt == TY_STRING) { buf_printf(b, "sp_MatchData_aref_name(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (kt == TY_POLY) {
        /* a poly key dispatches at runtime: a Symbol/String resolves by name,
           anything else is an index -- passing the raw sp_RbVal to
           sp_MatchData_aref (sp_int) would be a C type error. The index takes
           the Integer slot's conversion: a Float or a Rational truncates, an
           object answers #to_int, and nil or an Object is CRuby's TypeError,
           where sp_poly_to_i read them as group 0. */
        int mtmp = ++g_tmp, ktmp = ++g_tmp;
        buf_printf(b, "({ sp_MatchData *_t%d = %s; sp_RbVal _t%d = %s", mtmp, r, ktmp, needle_needs_deref(c, argv[0]) ? "sp_poly_strbuf_deref(" : "");
        emit_expr(c, argv[0], b); if (needle_needs_deref(c, argv[0])) buf_puts(b, ")");
        buf_printf(b, "; _t%d.tag == SP_TAG_SYM ? sp_MatchData_aref_name(_t%d, sp_sym_to_s((sp_sym)_t%d.v.i)) :"
                      " _t%d.tag == SP_TAG_STR ? sp_MatchData_aref_name(_t%d, _t%d.v.s) :"
                      " sp_MatchData_aref(_t%d, sp_poly_arg_int_chk(_t%d)); })",
                   ktmp, mtmp, ktmp, ktmp, mtmp, ktmp, mtmp, ktmp);
      }
      else { buf_printf(b, "sp_MatchData_aref(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
    }
    /* md[start, length]: an Array of `length` groups from `start` (#2507) */
    else if (sp_streq(name, "[]") && argc == 2) {
      buf_printf(b, "sp_MatchData_aref_len(%s, ", r); emit_int_expr(c, argv[0], b);
      buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
    }
    /* MatchData#=== is Object's: == (structural, above); #equal? is identity */
    else if ((sp_streq(name, "===") || sp_streq(name, "equal?")) && argc == 1) {
      Buf as = expr_buf(c, argv[0]);
      emit_native_object_protocol_text(c, name, TY_MATCHDATA, r, comp_ntype(c, argv[0]), as.p ? as.p : "0", b);
      free(as.p);
    }
    /* named_captures(symbolize_names: true): symbol keys (#2530) */
    else if (sp_streq(name, "named_captures") && argc == 1) {
      /* symbolize_names: FALSE asks for the string keys the no-argument form
         gives; the argument was ignored and the keys came back symbols (#3640) */
      int sym_on = 1;
      { int kv = kwh_lookup(nt, argv[0], "symbolize_names");
        const char *kvt = kv >= 0 ? nt_type(nt, kv) : NULL;
        if (kvt && sp_streq(kvt, "FalseNode")) sym_on = 0; }
      buf_printf(b, sym_on ? "sp_md_named_captures_sym(%s)" : "sp_md_named_captures(%s)", r);
    }
    /* #deconstruct is the captures array; #deconstruct_keys the named captures
       as a symbol-keyed hash (#2503) */
    else if (sp_streq(name, "values_at") && argc >= 1) {
      /* values_at(i, ...) / values_at(:name, ...) -> a poly array of the
         selected groups (nil when a group did not participate). A Symbol/String
         argument resolves by name against this MatchData's own group table (like
         #[]); an integer argument is a group index. Routing a name through the
         index accessor would consult a wrong (first-seen) global name table. */
      int mt = ++g_tmp, at = ++g_tmp;
      buf_printf(b, "({ sp_MatchData *_t%d = %s; SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);",
                 mt, r, mt, at, at);
      /* With several arguments, every one is evaluated, in source order,
         before any is looked up: a Range begin before the whole match raises
         RangeError and an unknown name IndexError, which must not skip a later
         argument's side effects. Each value is held (rooted) in a temp. */
      int held[64];
      int hold = argc > 1 && argc <= 64;
      for (int i = 0; hold && i < argc; i++) {
        TyKind kt3 = comp_ntype(c, argv[i]);
        held[i] = ++g_tmp;
        switch (kt3) {
        case TY_SYMBOL:
          buf_printf(b, " const char *_t%d = sp_sym_to_s(", held[i]); emit_expr(c, argv[i], b);
          buf_printf(b, "); SP_GC_ROOT_STR(_t%d);", held[i]);
          break;
        case TY_STRING:
          buf_printf(b, " const char *_t%d = ", held[i]); emit_expr(c, argv[i], b);
          buf_printf(b, "; SP_GC_ROOT_STR(_t%d);", held[i]);
          break;
        case TY_RANGE:
          buf_printf(b, " sp_Range _t%d = ", held[i]); emit_expr(c, argv[i], b); buf_puts(b, ";");
          break;
        case TY_POLY:
          buf_printf(b, " sp_RbVal _t%d = ", held[i]); emit_expr(c, argv[i], b);
          buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", held[i]);
          break;
        default:
          buf_printf(b, " sp_int _t%d = ", held[i]); emit_int_expr(c, argv[i], b); buf_puts(b, ";");
          break;
        }
      }
      for (int i = 0; i < argc; i++) {
        TyKind kt3 = comp_ntype(c, argv[i]);
        switch (kt3) {
        case TY_SYMBOL:
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_nullable_str(sp_MatchData_aref_name(_t%d, ", at, mt);
          if (hold) buf_printf(b, "_t%d", held[i]);
          else { buf_puts(b, "sp_sym_to_s("); emit_expr(c, argv[i], b); buf_puts(b, ")"); }
          buf_puts(b, ")));");
          break;
        case TY_STRING:
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_nullable_str(sp_MatchData_aref_name(_t%d, ", at, mt);
          if (hold) buf_printf(b, "_t%d", held[i]);
          else emit_expr(c, argv[i], b);
          buf_puts(b, ")));");
          break;
        case TY_RANGE: {
          /* a Range argument selects a run of groups, as Array#values_at does;
             it went into sp_MatchData_aref's sp_int slot as a struct (#3627).
             Its ends resolve against the group count the way CRuby's do: an
             open end runs to the last group, a negative one counts back from
             the end, a begin before the whole match raises RangeError, and a
             group past the last reads nil. Read raw, an endless range ran the
             loop to INTPTR_MAX, and a negative begin read the groups from the
             end through sp_MatchData_aref's own negative index. */
          int rk = ++g_tmp, rj = ++g_tmp, rlo = ++g_tmp, rhi = ++g_tmp, rn = ++g_tmp;
          buf_printf(b, " sp_Range _t%d = sp_range_ix(", rk);
          if (hold) buf_printf(b, "_t%d", held[i]);
          else emit_expr(c, argv[i], b);
          buf_puts(b, ")");
          buf_printf(b, "; sp_int _t%d = sp_MatchData_length(_t%d);", rn, mt);
          buf_printf(b, " sp_int _t%d = _t%d.nobeg ? 0 : _t%d.first;", rlo, rk, rk);
          buf_printf(b, " if (_t%d < 0) { if (_t%d < -_t%d) sp_raise_cls(\"RangeError\","
                        " sp_sprintf(\"%%s out of range\", sp_range_str(_t%d))); _t%d += _t%d; }",
                     rlo, rlo, rn, rk, rlo, rn);
          buf_printf(b, " sp_int _t%d = _t%d.noend ? _t%d - 1"
                        " : ((_t%d.last < 0 ? _t%d.last + _t%d : _t%d.last) - (_t%d.excl ? 1 : 0));",
                     rhi, rk, rn, rk, rk, rn, rk, rk);
          buf_printf(b, " for (sp_int _t%d = _t%d; _t%d <= _t%d; _t%d++)"
                        " sp_PolyArray_push(_t%d, sp_box_nullable_str(sp_MatchData_aref(_t%d, _t%d)));",
                     rj, rlo, rj, rhi, rj, at, mt, rj);
          break;
        }
        case TY_POLY: {
          /* a poly key dispatches at runtime like #[]: a Symbol/String resolves
             by name, anything else is an index. Passing the raw sp_RbVal to
             sp_MatchData_aref (sp_int) would be a C type error. */
          int kt = ++g_tmp;
          buf_printf(b, " sp_RbVal _t%d = %s", kt, needle_needs_deref(c, argv[i]) ? "sp_poly_strbuf_deref(" : "");
          if (hold) buf_printf(b, "_t%d", held[i]);
          else emit_expr(c, argv[i], b); if (needle_needs_deref(c, argv[i])) buf_puts(b, ")");
          buf_printf(b, "; sp_PolyArray_push(_t%d, sp_box_nullable_str("
                        "_t%d.tag == SP_TAG_SYM ? sp_MatchData_aref_name(_t%d, sp_sym_to_s((sp_sym)_t%d.v.i)) :"
                        " _t%d.tag == SP_TAG_STR ? sp_MatchData_aref_name(_t%d, _t%d.v.s) :"
                        " sp_MatchData_aref(_t%d, sp_poly_arg_int_chk(_t%d))));",
                     at, kt, mt, kt, kt, mt, kt, mt, kt);
          break;
        }
        default:
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_nullable_str(sp_MatchData_aref(_t%d, ", at, mt);
          if (hold) buf_printf(b, "_t%d", held[i]);
          else emit_int_expr(c, argv[i], b);
          buf_puts(b, ")));");
          break;
        }
      }
      buf_printf(b, " _t%d; })", at);
    }
    /* a method the program adds to Object is every MatchData's too (`$~.me`
       under ruby/spec's `$~.should`): leave it to the Object reopening
       dispatch, which boxes the receiver -- a nil $~ included -- instead of
       refusing it as a MatchData method this table lacks */
    else if (comp_class_index(c, "Object") >= 0 &&
             comp_method_in_chain(c, comp_class_index(c, "Object"), name, NULL) >= 0) {
      free(rs.p);
      return 0;
    }
    else unsupported(c, id, "MatchData method");
    free(rs.p);
    return 1;
  }

  /* StringIO instance methods (a non-GC heap buffer behind sp_StringIO *). */
  /* StringIO dispatch: native-bound (packages/stringio); no arms here. */
  return 0;
}

/* Emit the expression that materialises (range).step(k) as a typed array,
   returning its array TyKind. A float step -- or a literal range with float
   bounds -- yields a FloatArray; sp_Range stores sp_int bounds, so a literal
   float-bounded range reads begin/end from the AST to keep the float values.
   Integer steps use the faithful int helper (step 0 raises ArgumentError, a
   negative step descends, an exclusive range drops the endpoint). Shared by the
   no-block materialisation and the block walk so both yield identical values. */
TyKind emit_range_step_array(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (argc < 1) { buf_puts(b, "sp_IntArray_new()"); return TY_INT_ARRAY; }
  int rn = unwrap_parens(c, recv);
  int is_lit = rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode");
  int lo = is_lit ? nt_ref(nt, rn, "left") : -1;
  int hi = is_lit ? nt_ref(nt, rn, "right") : -1;
  int excl = (is_lit && (nt_int(nt, rn, "flags", 0) & 4)) ? 1 : 0;
  int is_float = comp_ntype(c, argv[0]) == TY_FLOAT ||
                 (lo >= 0 && comp_ntype(c, lo) == TY_FLOAT) ||
                 (hi >= 0 && comp_ntype(c, hi) == TY_FLOAT);
  if (is_float && is_lit && lo >= 0 && hi >= 0) {
    buf_puts(b, "sp_FloatArray_from_step(");
    emit_float_expr(c, lo, b); buf_puts(b, ", ");
    emit_float_expr(c, hi, b); buf_puts(b, ", ");
    emit_float_expr(c, argv[0], b); buf_printf(b, ", %d)", excl);
    return TY_FLOAT_ARRAY;
  }
  int t = ++g_tmp;
  Buf rb = expr_buf(c, recv);
  /* a Float stride walks to the end as written (sp_range_end_num); an
     Integer one over a Float end yields Floats in CRuby, which this
     Integer array cannot hold (sp_range_int_only) */
  if (is_float)
    buf_printf(b, "({ sp_Range _t%d = %s; sp_FloatArray_from_step((sp_float)_t%d.first, sp_range_end_num(_t%d), ",
               t, rb.p ? rb.p : "", t, t);
  else
    buf_printf(b, "({ sp_Range _t%d = %s; sp_range_int_only(_t%d, \"Range#step\"); sp_IntArray_from_range_step(_t%d.first, _t%d.last, ",
               t, rb.p ? rb.p : "", t, t, t);
  if (is_float) emit_float_expr(c, argv[0], b); else emit_int_expr(c, argv[0], b);
  if (is_float) buf_printf(b, ", sp_range_excl_end(_t%d)); })", t);
  else buf_printf(b, ", _t%d.excl); })", t);
  free(rb.p);
  return is_float ? TY_FLOAT_ARRAY : TY_INT_ARRAY;
}

int emit_range_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = comp_recv_type(c, recv);
  /* String range ("a".."e"): a distinct sp_StrRange receiver. The endpoints
     answer natively; every traversal materializes the element array (#3064). */
  if (recv >= 0 && rt == TY_STR_RANGE) {
    TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
    int tr = ++g_tmp;
    /* a Range that no name holds sits in its temp while the argument is
       made: when that may allocate, the temp's two ends are rooted first */
    int hold = argc == 1 && !arg_ran_first(recv, 0) && !subtree_is_pure_read(c, recv) &&
               operand_may_allocate(c, argv[0]);
    if (is_range_membership(name) && argc == 1) {
      const char *fn = is_membership_alias(name) ?
                       "sp_srange_include" : "sp_srange_cover";
      if (a0 == TY_STRING) {
        buf_printf(b, "({ sp_StrRange _t%d = ", tr); emit_expr(c, recv, b);
        buf_puts(b, "; ");
        if (hold) { emit_gc_root_tmp_refs(c, rt, tr, b); buf_puts(b, " "); }
        buf_printf(b, "%s(_t%d, ", fn, tr); emit_str_expr(c, argv[0], b);
        buf_puts(b, "); })"); return 1;
      }
      if (a0 == TY_POLY) {
        buf_printf(b, "({ sp_StrRange _t%d = ", tr); emit_expr(c, recv, b);
        buf_puts(b, "; ");
        if (hold) { emit_gc_root_tmp_refs(c, rt, tr, b); buf_puts(b, " "); }
        buf_printf(b, "sp_RbVal _a%d = ", tr); emit_boxed(c, argv[0], b);
        buf_printf(b, "; (sp_bool)(_a%d.tag == SP_TAG_STR &&"
                      " %s(_t%d, _a%d.v.s)); })", tr, fn, tr, tr);
        return 1;
      }
      buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); return 1;
    }
    if ((is_eq_or_eql(name)) && argc == 1) {
      if (a0 == TY_STR_RANGE) {
        int tr2 = ++g_tmp;
        buf_printf(b, "({ sp_StrRange _t%d = ", tr); emit_expr(c, recv, b);
        buf_puts(b, "; ");
        if (hold) { emit_gc_root_tmp_refs(c, rt, tr, b); buf_puts(b, " "); }
        buf_printf(b, "sp_StrRange _t%d = ", tr2); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_srange_eq(_t%d, _t%d); })", tr, tr2); return 1;
      }
      buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); return 1;
    }
    if ((is_to_array_alias(name)) && argc == 0) {
      buf_printf(b, "({ sp_StrRange _t%d = ", tr); emit_expr(c, recv, b);
      if (nt_int(nt, id, "to_set", 0))
        buf_printf(b, "; if (!_t%d.last) sp_raise_cls(\"RangeError\", \"cannot convert endless range to a set\")", tr);
      buf_printf(b, "; sp_srange_to_a(_t%d); })", tr); return 1;
    }
    /* builtin-op rows (builtin_ops.c), after the arms that read the operand */
    if (emit_builtin_op_tmp(c, id, recv, rt, name, tr, b)) return 1;
  }
  /* Float range (1.0..3.0): a distinct sp_FloatRange receiver. It is not
     iterable, so its face is endpoint reads, membership tests, step, and
     bsearch (the fold handles bsearch; the iteration forms raise earlier). */
  if (recv >= 0 && rt == TY_FLOAT_RANGE) {
    int a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
    int tr = ++g_tmp;
    /* a blockless step over an endless one: an Enumerator walks it */
    if (argc == 1 && sp_streq(name, "step") && nt_ref(nt, id, "block") < 0 && range_lit_endless(c, recv)) {
      buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; sp_range_endless_step(sp_box_float(_t%d.first), ", tr);
      emit_boxed(c, argv[0], b); buf_puts(b, "); })");
      return 1;
    }
    /* overlap?: CRuby's range_overlap at run time, as for an Integer Range */
    if (argc == 1 && sp_streq(name, "overlap?")) {
      buf_puts(b, "sp_range_overlap_v(sp_box_frange("); emit_expr(c, recv, b);
      buf_puts(b, "), "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    if (argc == 0 && (is_range_end_reader(name))) {
      int as_int2 = comp_ntype(c, id) == TY_INT;
      buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
      /* an omitted end: #end is nil and #last the RangeError, as CRuby */
      if (as_int2 && node_is_oint(c, id)) buf_printf(b, "; sp_oint_of((sp_int)_t%d.last); })", tr);
      else if (as_int2) buf_printf(b, "; (sp_int)_t%d.last; })", tr);
      else {
        /* #end answers the endpoint with its nil (an endless range);
           #last the plain endpoint (an endless range raises) */
        buf_puts(b, "; ");
        if (sp_streq(name, "end")) {
          oint_open(c, id, TY_FLOAT, b); buf_printf(b, "sp_frange_end_v(_t%d)", tr); oint_close(c, id, b);
        }
        else {
          oint_lift_open(c, id, TY_FLOAT, b); buf_printf(b, "sp_frange_last_v(_t%d)", tr); oint_lift_close(c, id, b);
        }
        buf_puts(b, "; })");
      }
      return 1;
    }
    if (argc == 0 && sp_streq(name, "max")) {
      /* the endpoint the caller wrote: an Integer end answers an Integer,
         whatever the other endpoint made of the range's kind (#3837) */
      int as_int = comp_ntype(c, id) == TY_INT;
      buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_puts(b, "; ");
      /* an Integer end: its nil (an empty range) kept where the consumer takes the oint */
      if (as_int && node_is_oint(c, id))
        buf_printf(b, "sp_ofloat _m%d = sp_frange_max_v(_t%d); _m%d.nil ? sp_oint_nil() : sp_oint_of((sp_int)_m%d.v); })", tr, tr, tr, tr);
      else if (as_int) buf_printf(b, "(sp_int)sp_ofloat_arg(sp_frange_max_v(_t%d)); })", tr);
      else { oint_open(c, id, TY_FLOAT, b); buf_printf(b, "sp_frange_max_v(_t%d)", tr); oint_close(c, id, b); buf_puts(b, "; })"); }
      return 1;
    }
    /* Range#size counts the integers a range enumerates, so it answers only
       for an Integer begin -- and Infinity when the end is unbounded, which is
       exactly the shape an infinite bound puts on the float representation
       (#3670). A Float begin has no enumeration, as CRuby's TypeError says. */
    if ((is_size_or_count(name)) && argc == 0 &&
        nt_ref(nt, id, "block") < 0) {
      int rq2 = unwrap_parens(c, recv);
      int lo2 = (rq2 >= 0 && nt_type(nt, rq2) && sp_streq(nt_type(nt, rq2), "RangeNode"))
                  ? nt_ref(nt, rq2, "left") : -1;
      if (lo2 >= 0 && comp_ntype(c, lo2) == TY_INT) {
        buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
        buf_printf(b, "; _t%d.last == HUGE_VAL ? HUGE_VAL"
                      " : (sp_float)((sp_int)_t%d.last - (sp_int)_t%d.first"
                      " + (_t%d.excl ? 0 : 1)); })", tr, tr, tr, tr);
        return 1;
      }
    }
    /* is_a?/kind_of?/instance_of?/equal? via the boxed value's builtin identity
       (its class is "Range"; the helpers key on the SP_BUILTIN_FLOAT_RANGE tag) */
    if (argc == 1 && is_kind_query(name)) {
      int is_iof = sp_streq(name, "instance_of?");
      char cnq[192];
      const char *cn = isa_match_name(nt, argv[0], cnq, sizeof cnq);
      buf_printf(b, "({ sp_RbVal _t%d = ", tr); emit_boxed(c, recv, b); buf_puts(b, "; ");
      if (cn && is_iof) buf_printf(b, "(sp_bool)(strcmp(sp_poly_class_name(_t%d), \"%s\") == 0); })", tr, cn);
      else if (cn)      buf_printf(b, "sp_poly_kind_of_builtin(_t%d, \"%s\"); })", tr, cn);
      else { buf_printf(b, "sp_poly_is_a_dyn(_t%d, ", tr); emit_boxed(c, argv[0], b);
             buf_printf(b, ", %d); })", is_iof ? 1 : 0); }
      return 1;
    }
    if (sp_streq(name, "equal?") && argc == 1) {
      if (a0 == TY_FLOAT_RANGE) {
        int tr2 = ++g_tmp;
        buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
        buf_printf(b, "; sp_FloatRange _t%d = ", tr2); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_frange_eq(_t%d, _t%d); })", tr, tr2); return 1;
      }
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), ((void)(");
      emit_expr(c, argv[0], b); buf_puts(b, "), (sp_bool)0))"); return 1;
    }
    /* builtin-op rows (builtin_ops.c), after the arms that read the literal
       or the operand: the endpoints, membership, step, and the enumerating
       forms, which raise "can't iterate from Float" like CRuby */
    if (emit_builtin_op_tmp(c, id, recv, rt, name, tr, b)) return 1;
  }
  /* range value methods (evaluate the range once into a temp) */
  if (recv >= 0 && rt == TY_RANGE) {
    int block = nt_ref(nt, id, "block");
    /* (1..5.5): the end readers answer the literal Float, which the sp_int
       fields cannot hold; #to_s renders it too (#3896). */
    if (argc == 0 && block < 0) {
      int fe = range_lit_float_end(c, recv);
      if (fe >= 0 && (is_range_end_reader(name))) {
        /* a bound reader's consumer may take the oint form (node_is_oint) */
        oint_lift_open(c, id, TY_FLOAT, b);
        emit_float_expr(c, fe, b);
        oint_lift_close(c, id, b);
        return 1;
      }
      /* max is nil for an empty one and CRuby's TypeError for an excluded end */
      if (fe >= 0 && sp_streq(name, "max")) {
        int trm = ++g_tmp;
        oint_open(c, id, TY_FLOAT, b);
        buf_printf(b, "({ sp_Range _t%d = ", trm); emit_expr(c, recv, b);
        buf_printf(b, "; sp_range_max_f(_t%d); })", trm);
        oint_close(c, id, b);
        return 1;
      }
    }
    /* find / detect / take_while over an ENDLESS Range: there is no array to
       materialize, so walk up from the bounded end the way `each` does. A
       search that never succeeds does not terminate in CRuby either (#3863). */
    if (block >= 0 && argc == 0 && nt_type(nt, block) &&
        sp_streq(nt_type(nt, block), "BlockNode") &&
        (is_find_or_take_while(name))) {
      int rn8 = unwrap_parens(c, recv);
      if (rn8 >= 0 && nt_type(nt, rn8) && !sp_streq(nt_type(nt, rn8), "RangeNode")) {
        int sl8 = local_sole_range_node(c, rn8);
        if (sl8 >= 0) rn8 = sl8;
      }
      int endless = rn8 >= 0 && nt_type(nt, rn8) && sp_streq(nt_type(nt, rn8), "RangeNode") &&
                    nt_ref(nt, rn8, "right") < 0 && nt_ref(nt, rn8, "left") >= 0;
      const char *bp8 = block_param_name(c, block, 0);
      int body8 = nt_ref(nt, block, "body");
      int bn8 = 0; const int *bb8 = body8 >= 0 ? nt_arr(nt, body8, "body", &bn8) : NULL;
      if (endless && bp8 && bn8 >= 1) {
        int want_take = sp_streq(name, "take_while");
        Scope *bsc8 = comp_scope_of(c, block);
        LocalVar *lv8 = bsc8 ? scope_local(bsc8, bp8) : NULL;
        const char *bpr = rename_local(bp8);
        int tr8 = ++g_tmp, ti8 = ++g_tmp, to8 = ++g_tmp;
        emit_indent(g_pre, g_indent);
        Buf rb8 = expr_buf(c, recv);
        buf_printf(g_pre, "sp_Range _t%d = %s;\n", tr8, rb8.p ? rb8.p : "");
        free(rb8.p);
        emit_indent(g_pre, g_indent);
        if (want_take)
          buf_printf(g_pre, "sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", to8, to8);
        else
          buf_printf(g_pre, "sp_oint _t%d = sp_oint_nil();\n", to8);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (sp_int _t%d = _t%d.first; ; _t%d++) {\n", ti8, tr8, ti8);
        emit_indent(g_pre, g_indent + 1);
        if (lv8 && lv8->type == TY_POLY) buf_printf(g_pre, "lv_%s = sp_box_int(_t%d);\n", bpr, ti8);
        else buf_printf(g_pre, "lv_%s = _t%d;\n", bpr, ti8);
        for (int j = 0; j + 1 < bn8; j++) emit_stmt(c, bb8[j], g_pre, g_indent + 1);
        Buf cb8; memset(&cb8, 0, sizeof cb8);
        { int sv8 = g_indent; g_indent += 1; emit_cond(c, bb8[bn8 - 1], &cb8); g_indent = sv8; }
        emit_indent(g_pre, g_indent + 1);
        if (want_take)
          buf_printf(g_pre, "if (!(%s)) break; sp_IntArray_push(_t%d, _t%d);\n",
                     cb8.p ? cb8.p : "0", to8, ti8);
        else
          buf_printf(g_pre, "if (%s) { _t%d = sp_oint_of(_t%d); break; }\n", cb8.p ? cb8.p : "0", to8, ti8);
        free(cb8.p);
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        if (want_take) buf_printf(b, "_t%d", to8);
        else { oint_open(c, id, TY_INT, b); buf_printf(b, "_t%d", to8); oint_close(c, id, b); }
        return 1;
      }
    }
    /* reverse_each { } over a BEGINLESS Range: there is no array to
       materialize and no lower bound to stop at, so count down from the
       bounded end the way CRuby does -- a `break` in the block is what ends
       it (#3914). */
    if (block >= 0 && argc == 0 && nt_type(nt, block) &&
        sp_streq(nt_type(nt, block), "BlockNode") && sp_streq(name, "reverse_each")) {
      int rn7 = unwrap_parens(c, recv);
      if (rn7 >= 0 && nt_type(nt, rn7) && !sp_streq(nt_type(nt, rn7), "RangeNode")) {
        int sl7 = local_sole_range_node(c, rn7);
        if (sl7 >= 0) rn7 = sl7;
      }
      int beginless = rn7 >= 0 && nt_type(nt, rn7) && sp_streq(nt_type(nt, rn7), "RangeNode") &&
                      nt_ref(nt, rn7, "left") < 0 && nt_ref(nt, rn7, "right") >= 0;
      const char *bp7 = block_param_name(c, block, 0);
      int body7 = nt_ref(nt, block, "body");
      int bn7 = 0; const int *bb7 = body7 >= 0 ? nt_arr(nt, body7, "body", &bn7) : NULL;
      if (beginless && bp7 && bn7 >= 1) {
        Scope *bsc7 = comp_scope_of(c, block);
        LocalVar *lv7 = bsc7 ? scope_local(bsc7, bp7) : NULL;
        const char *bpr7 = rename_local(bp7);
        int tr7 = ++g_tmp, ti7 = ++g_tmp;
        emit_indent(g_pre, g_indent);
        Buf rb7 = expr_buf(c, recv);
        buf_printf(g_pre, "sp_Range _t%d = %s;\n", tr7, rb7.p ? rb7.p : "");
        free(rb7.p);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (sp_int _t%d = _t%d.last - (_t%d.excl ? 1 : 0); ; _t%d--) {\n",
                   ti7, tr7, tr7, ti7);
        emit_indent(g_pre, g_indent + 1);
        if (lv7 && lv7->type == TY_POLY) buf_printf(g_pre, "lv_%s = sp_box_int(_t%d);\n", bpr7, ti7);
        else buf_printf(g_pre, "lv_%s = _t%d;\n", bpr7, ti7);
        /* a real C loop, so a `break` in the body lowers to a C break */
        int sv_lexc7 = g_loop_exc_base, sv_lexc7_rb = g_loop_rescue_base, sv_lens7 = g_loop_ensure_base;
        g_loop_exc_base = g_exc_frame_depth; g_loop_rescue_base = g_rescue_save_depth; g_loop_ensure_base = g_ensure_depth;
        g_c_loop_depth++;
        for (int j = 0; j < bn7; j++) emit_stmt(c, bb7[j], g_pre, g_indent + 1);
        g_c_loop_depth--;
        g_loop_exc_base = sv_lexc7; g_loop_rescue_base = sv_lexc7_rb; g_loop_ensure_base = sv_lens7;
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        /* #reverse_each answers its receiver */
        buf_printf(b, "_t%d", tr7);
        return 1;
      }
    }
    /* endless literal: size is infinite; take/first(n) count from the start
       (an endless range cannot materialize) */
    {
      int rn9 = unwrap_parens(c, recv);
      /* a local holding only such a literal counts too (sole-assignment);
         the arms below never evaluate the receiver, so skipping the local
         read loses no side effect */
      if (rn9 >= 0 && nt_type(nt, rn9) && !sp_streq(nt_type(nt, rn9), "RangeNode")) {
        int sl9 = local_sole_range_node(c, rn9);
        if (sl9 >= 0) rn9 = sl9;
      }
      if (rn9 >= 0 && nt_type(nt, rn9) && sp_streq(nt_type(nt, rn9), "RangeNode") &&
          nt_ref(nt, rn9, "left") < 0 && sp_streq(name, "size") && argc == 0) {
        /* beginless: CRuby cannot iterate from nil */
        buf_puts(b, "({ sp_raise_cls(\"TypeError\", \"can't iterate from NilClass\"); (sp_int)0; })");
        return 1;
      }
      /* a Float begin cannot iterate: the enumerating forms raise like
         CRuby (int begin + float end iterates fine; first/last/minmax read
         endpoints without iterating and stay served below) */
      if (rn9 >= 0 && nt_type(nt, rn9) && sp_streq(nt_type(nt, rn9), "RangeNode") &&
          nt_ref(nt, rn9, "left") >= 0 &&
          comp_ntype(c, nt_ref(nt, rn9, "left")) == TY_FLOAT &&
          ((argc == 0 && (sp_streq(name, "size") || sp_streq(name, "sum") ||
                          sp_streq(name, "count") || sp_streq(name, "to_a"))) ||
           (argc == 1 && (is_endpoint_query(name))))) {
        const char *dflt9 = (sp_streq(name, "to_a") || argc == 1)
                              ? "(sp_IntArray*)0" : "(sp_int)0";
        buf_printf(b, "({ sp_raise_cls(\"TypeError\", \"can't iterate from Float\"); %s; })", dflt9);
        return 1;
      }
      /* an int begin with a finite Float end sizes by the floored span */
      if (rn9 >= 0 && nt_type(nt, rn9) && sp_streq(nt_type(nt, rn9), "RangeNode") &&
          nt_ref(nt, rn9, "left") >= 0 && nt_ref(nt, rn9, "right") >= 0 &&
          comp_ntype(c, nt_ref(nt, rn9, "right")) == TY_FLOAT &&
          !lazy_endpoint_is_infinite(c, nt_ref(nt, rn9, "right")) &&
          sp_streq(name, "size") && argc == 0) {
        int excl9 = (int)(nt_int(nt, rn9, "flags", 0) & 4) ? 1 : 0;
        int tb9 = ++g_tmp, te9 = ++g_tmp;
        /* promote types a Range size poly (it can pass the word) */
        int bx9 = repr_of(c, id).kind == RK_BOXED;
        if (bx9) buf_puts(b, "sp_box_int(");
        buf_printf(b, "({ sp_int _t%d = ", tb9);
        emit_int_expr(c, nt_ref(nt, rn9, "left"), b);
        buf_printf(b, "; double _t%d = ", te9);
        emit_expr(c, nt_ref(nt, rn9, "right"), b);
        buf_printf(b, "; double _d = _t%d - (double)_t%d;"
                      " _d < 0 ? 0 : (%d && _t%d == floor(_t%d)) ? (sp_int)_d : (sp_int)floor(_d) + 1; })",
                   te9, tb9, excl9, te9, te9);
        if (bx9) buf_puts(b, ")");
        return 1;
      }
      /* String-endpoint range accessors: the int-backed sp_Range stores the
         endpoint string POINTERS in its first/last fields, so begin/end/first/
         last/min/max must read them back as strings, not raw ints (#2467). */
      if (rn9 >= 0 && nt_type(nt, rn9) && sp_streq(nt_type(nt, rn9), "RangeNode")) {
        int lo9 = nt_ref(nt, rn9, "left"), hi9 = nt_ref(nt, rn9, "right");
        if (lo9 >= 0 && hi9 >= 0 &&
            comp_ntype(c, lo9) == TY_STRING && comp_ntype(c, hi9) == TY_STRING) {
          int excl9 = (int)(nt_int(nt, rn9, "flags", 0) & 4) ? 1 : 0;
          if (argc == 0 && (sp_streq(name, "begin") || sp_streq(name, "first") ||
                            sp_streq(name, "min"))) { emit_expr(c, lo9, b); return 1; }
          if (argc == 0 && (is_range_end_reader(name))) {
            emit_expr(c, hi9, b); return 1;
          }
          if (argc == 0 && sp_streq(name, "max") && !excl9) { emit_expr(c, hi9, b); return 1; }
          /* blockless count: the succ-sequence length (#3070). Range#size is
             nil for a non-numeric range, so it is not served here. */
          if (argc == 0 && nt_ref(nt, id, "block") < 0 && sp_streq(name, "count")) {
            int ta9 = ++g_tmp;
            buf_printf(b, "({ sp_StrArray *_t%d = sp_StrArray_from_string_range(", ta9);
            emit_expr(c, lo9, b); buf_puts(b, ", "); emit_expr(c, hi9, b);
            buf_printf(b, ", %d); (sp_int)_t%d->len; })", excl9, ta9);
            return 1;
          }
          if (argc == 1 && (is_endpoint_query(name))) {
            int ta9 = ++g_tmp, tn9 = ++g_tmp;
            buf_printf(b, "({ sp_StrArray *_t%d = sp_StrArray_from_string_range(", ta9);
            emit_expr(c, lo9, b); buf_puts(b, ", "); emit_expr(c, hi9, b);
            buf_printf(b, ", %d); sp_int _t%d = ", excl9, tn9); emit_int_expr(c, argv[0], b);
            if (sp_streq(name, "first"))
              buf_printf(b, "; sp_StrArray_slice(_t%d, 0, _t%d); })", ta9, tn9);
            else
              buf_printf(b, "; sp_int _s9 = _t%d->len - _t%d; if (_s9 < 0) _s9 = 0;"
                            " sp_StrArray_slice(_t%d, _s9, _t%d); })", ta9, tn9, ta9, tn9);
            return 1;
          }
        }
      }
      /* an unbounded end has no maximum and an unbounded begin has no minimum
         -- CRuby raises RangeError rather than reading the infinity sentinel
         stored in the endpoint (#3065) */
      if (rn9 >= 0 && nt_type(nt, rn9) && sp_streq(nt_type(nt, rn9), "RangeNode") && argc == 0) {
        int lo9 = nt_ref(nt, rn9, "left"), hi9 = nt_ref(nt, rn9, "right");
        int endless9 = (hi9 < 0 || lazy_endpoint_is_infinite(c, hi9));
        if (endless9 && lo9 >= 0 && sp_streq(name, "max")) {
          buf_puts(b, "({ sp_raise_cls(\"RangeError\", \"cannot get the maximum of endless range\"); (sp_int)0; })");
          return 1;
        }
        if (lo9 < 0 && sp_streq(name, "min")) {
          buf_puts(b, "({ sp_raise_cls(\"RangeError\", \"cannot get the minimum of beginless range\"); (sp_int)0; })");
          return 1;
        }
      }
      /* a beginless one's #count is Infinity as well, as CRuby 4.0 counts it */
      if (rn9 >= 0 && nt_type(nt, rn9) && sp_streq(nt_type(nt, rn9), "RangeNode") &&
          (nt_ref(nt, rn9, "left") < 0 || nt_kind(nt, nt_ref(nt, rn9, "left")) == NK_NilNode) &&
          sp_streq(name, "count") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        buf_puts(b, repr_of(c, id).kind == RK_BOXED ? "sp_box_float(HUGE_VAL)" : "(HUGE_VAL)");
        return 1;
      }
      if (rn9 >= 0 && nt_type(nt, rn9) && sp_streq(nt_type(nt, rn9), "RangeNode") &&
          (nt_ref(nt, rn9, "right") < 0 ||
           lazy_endpoint_is_infinite(c, nt_ref(nt, rn9, "right"))) &&
          nt_ref(nt, rn9, "left") >= 0) {
        /* #count enumerates forever on an endless range, so like #size it
           answers Infinity (#3668) */
        if ((is_size_or_count(name)) && argc == 0 &&
            nt_ref(nt, id, "block") < 0) {
          buf_puts(b, repr_of(c, id).kind == RK_BOXED ? "sp_box_float(HUGE_VAL)" : "(HUGE_VAL)");
          return 1;
        }
        if ((is_first_or_take(name)) && argc == 1) {
          int lo9 = nt_ref(nt, rn9, "left");
          int ts9 = ++g_tmp, tn9 = ++g_tmp, ti9 = ++g_tmp, to9 = ++g_tmp;
          buf_printf(b, "({ sp_int _t%d = ", ts9); emit_int_expr(c, lo9, b);
          buf_printf(b, "; sp_int _t%d = ", tn9); emit_int_expr(c, argv[0], b);
          buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);"
                        " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)"
                        " sp_IntArray_push(_t%d, _t%d + _t%d); _t%d; })",
                     to9, to9, ti9, ti9, tn9, ti9, to9, ts9, ti9, to9);
          return 1;
        }
      }
    }
    if (sp_streq(name, "step") && argc == 1 && block < 0) {
      /* an endless range cannot materialize: an Enumerator walks it */
      if (range_lit_endless(c, recv)) {
        int te = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", te); emit_expr(c, recv, b);
        buf_printf(b, "; sp_range_endless_step(sp_box_int(_t%d.first), ", te);
        emit_boxed(c, argv[0], b); buf_puts(b, "); })");
        return 1;
      }
      emit_range_step_array(c, id, b);
      return 1;
    }
    if (sp_streq(name, "each") && block < 0) {  /* external enumerator, or to_a materialize */
      int t = ++g_tmp;
      /* an endless range is walked as it is read: the step-1 Enumerator */
      if (comp_ntype(c, id) == TY_ENUMERATOR && range_lit_endless(c, recv)) {
        buf_printf(b, "({ sp_Range _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_range_endless_step(sp_box_int(_t%d.first), sp_box_int(1)); })", t);
        return 1;
      }
      Buf rb = expr_buf(c, recv);
      if (comp_ntype(c, id) == TY_ENUMERATOR) {
        /* pass the boxed range itself: sp_enum_items_from expands the members
           and #inspect keeps the range printable as the source */
        buf_printf(b, "sp_Enumerator_new_from(sp_box_range(%s))", rb.p ? rb.p : "");
      }
      else {
        buf_printf(b, "({ sp_Range _t%d = %s; sp_range_to_ia(_t%d); })",
                   t, rb.p ? rb.p : "", t);
      }
      free(rb.p);
      return 1;
    }
    /* to_s / inspect render the range itself ("1..3"); a string-endpoint
       literal renders statically (int-backed sp_Range cannot). */
    if ((is_text_conversion(name)) &&
        argc == 0 && nt_ref(nt, id, "block") < 0) {
      int rq = unwrap_parens(c, recv);
      if (rq >= 0 && nt_type(nt, rq) && !sp_streq(nt_type(nt, rq), "RangeNode")) {
        int sl = local_sole_range_node(c, rq);
        if (sl >= 0) rq = sl;
      }
      int lo_q = rq >= 0 && nt_type(nt, rq) && sp_streq(nt_type(nt, rq), "RangeNode")
                   ? nt_ref(nt, rq, "left") : -1;
      int hi_q = lo_q >= 0 ? nt_ref(nt, rq, "right") : -1;
      int str_ends = lo_q >= 0 && hi_q >= 0 &&
                     comp_ntype(c, lo_q) == TY_STRING && comp_ntype(c, hi_q) == TY_STRING;
      if (str_ends && nt_kind(nt, lo_q) == NK_StringNode && nt_kind(nt, hi_q) == NK_StringNode) {
        const char *lv2 = nt_str(nt, lo_q, "unescaped");
        if (!lv2) lv2 = nt_str(nt, lo_q, "content");
        const char *hv2 = nt_str(nt, hi_q, "unescaped");
        if (!hv2) hv2 = nt_str(nt, hi_q, "content");
        int plain = lv2 && hv2;
        for (const char *q2 = lv2; plain && q2 && *q2; q2++)
          if (!((*q2 >= 'a' && *q2 <= 'z') || (*q2 >= 'A' && *q2 <= 'Z') ||
                (*q2 >= '0' && *q2 <= '9') || *q2 == '_')) plain = 0;
        for (const char *q2 = hv2; plain && q2 && *q2; q2++)
          if (!((*q2 >= 'a' && *q2 <= 'z') || (*q2 >= 'A' && *q2 <= 'Z') ||
                (*q2 >= '0' && *q2 <= '9') || *q2 == '_')) plain = 0;
        if (plain) {
          int exq = (int)(nt_int(nt, rq, "flags", 0) & 4) ? 1 : 0;
          int quoted = sp_streq(name, "inspect");
          buf_puts(b, "SPL(\"");
          if (quoted) buf_puts(b, "\\\"");
          buf_puts(b, lv2);
          if (quoted) buf_puts(b, "\\\"");
          buf_puts(b, exq ? "..." : "..");
          if (quoted) buf_puts(b, "\\\"");
          buf_puts(b, hv2);
          if (quoted) buf_puts(b, "\\\"");
          buf_puts(b, "\")");
          return 1;
        }
      }
      /* string endpoints without a static rendering: leave to other arms
         (the int-backed sp_Range cannot render them) */
      /* `x..Float::INFINITY`: the int range records only "unbounded", so the
         rendering comes from the literal that named the bound (#3670) */
      int hi_is_inf = hi_q >= 0 && nt_kind(nt, hi_q) == NK_ConstantPathNode &&
                      nt_str(nt, hi_q, "name") && sp_streq(nt_str(nt, hi_q, "name"), "INFINITY");
      if (!str_ends && lo_q >= 0 && hi_is_inf) {
        int tq2 = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", tq2);
        emit_expr(c, recv, b);
        buf_printf(b, "; sp_sprintf(\"%%lld%%sInfinity\", (long long)_t%d.first,"
                      " _t%d.excl ? \"...\" : \"..\"); })", tq2, tq2);
        return 1;
      }
      if (!str_ends) {
        int tq = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", tq);
        emit_expr(c, recv, b);
        /* #inspect names the absent bounds of a fully-unbounded range
           ("nil..nil"), where #to_s prints only the dots (#3670) */
        buf_printf(b, "; %s(_t%d); })",
                   sp_streq(name, "inspect") ? "sp_range_inspect" : "sp_range_str", tq);
        return 1;
      }
    }
    /* min(n) / max(n): the n smallest or largest members, walked from the
       endpoint the count starts at, so a one-sided Range answers without
       materializing (and raises from the side it has no end on) (#3665). */
    if (argc == 1 && (is_minmax_query(name)) && block < 0) {
      int trr = ++g_tmp, tnn = ++g_tmp, too = ++g_tmp, thi = ++g_tmp, tii = ++g_tmp;
      int want_min = sp_streq(name, "min");
      buf_printf(b, "({ sp_Range _t%d = ", trr); emit_expr(c, recv, b);
      buf_printf(b, "; sp_int _t%d = ", tnn); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\");", tnn);
      int rq = unwrap_parens(c, recv);
      if (nt_kind(nt, rq) != NK_RangeNode) rq = local_sole_range_node(c, rq);
      if (!want_min && nt_kind(nt, rq) == NK_RangeNode && comp_ntype(c, nt_ref(nt, rq, "right")) == TY_FLOAT)
        buf_printf(b, " if (_t%d.nobeg) sp_raise_cls(\"TypeError\", \"can't iterate from NilClass\");", trr);
      if (want_min)
        buf_printf(b, " if (_t%d.nobeg) sp_raise_cls(\"RangeError\","
                      " \"cannot get the minimum of beginless range\");", trr);
      else
        buf_printf(b, " if (_t%d.noend) sp_raise_cls(\"RangeError\","
                      " \"cannot get the maximum of endless range\");", trr);
      buf_printf(b, " sp_int _t%d = _t%d.last - (_t%d.excl ? 1 : 0);", thi, trr, trr);
      buf_printf(b, " sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);", too, too);
      if (want_min)
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) {"
                      " sp_int _v = _t%d.first + _t%d;"
                      " if (!_t%d.noend && _v > _t%d) break;"
                      " sp_IntArray_push(_t%d, _v); }",
                   tii, tii, tnn, tii, trr, tii, trr, thi, too);
      else
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) {"
                      " sp_int _v = _t%d - _t%d;"
                      " if (!_t%d.nobeg && _v < _t%d.first) break;"
                      " sp_IntArray_push(_t%d, _v); }",
                   tii, tii, tnn, tii, thi, tii, trr, trr, too);
      buf_printf(b, " _t%d; })", too);
      return 1;
    }
    static const char *const rmeths[] = {
      "to_a", "entries", "include?", "member?", "cover?", "===", "sum", "min", "max",
      "first", "last", "size", "count", "begin", "end",
      "exclude_end?", "eql?", "equal?", "minmax", "overlap?", NULL };
    int known = 0;
    for (int i = 0; rmeths[i]; i++) if (sp_streq(name, rmeths[i])) known = 1;
    /* `count` with a block or argument is Enumerable#count, not Range#size:
       let it fall through to the int-array redispatch below. */
    if (sp_streq(name, "count") && (block >= 0 || argc >= 1)) known = 0;
    /* `sum` with a block is Enumerable#sum { }: the native Range sum ignored
       the block; let it fall through to the int-array redispatch below. */
    if (sp_streq(name, "sum") && block >= 0) known = 0;
    /* min(n)/max(n) return arrays of the smallest/largest n: Enumerable forms,
       served by the int-array redispatch below (the native arm is argless). */
    if ((is_minmax_query(name)) && argc >= 1) known = 0;
    /* min/max/minmax with a comparator block: the comparator emitter serves
       the lowerable shapes; anything else must reject rather than silently
       ignore the block. */
    if ((is_extrema_family(name)) && block >= 0) known = 0;
    if (known) {
      /* size/count on a string-literal range: no integer size -> nil, skip creating sp_Range */
      if ((is_size_or_count(name)) && argc == 0) {
        int rn = unwrap_parens(c, recv);
        if (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode")) {
          int lo = nt_ref(nt, rn, "left");
          if (lo >= 0 && comp_ntype(c, lo) == TY_STRING) {
            oint_open(c, id, TY_INT, b); buf_puts(b, "sp_oint_nil()"); oint_close(c, id, b); return 1;
          }
        }
      }
      int t = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Range _t%d = ", t);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      if (nt_int(nt, id, "to_set", 0))
        buf_printf(b, "(_t%d.noend ? (sp_raise_cls(\"RangeError\", \"cannot convert endless"
                      " range to a set\"), (sp_IntArray *)0) : sp_range_to_ia(_t%d))", t, t);
      else if (is_to_array_alias(name))
        buf_printf(b, "sp_range_to_ia(_t%d)", t);
      else if (is_range_membership(name)) {
        /* ===(range) / include?(range): CRuby compares endpoints against the
           value via <=>, and Integer <=> Range is nil, so these are always
           false. Only cover?(range) does endpoint containment. */
        if (argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE &&
            (sp_streq(name, "===") || sp_streq(name, "include?") ||
             sp_streq(name, "member?"))) {
          buf_puts(b, "0");
        }
        /* cover?(range) checks that both endpoints of the arg fit inside self */
        else if (sp_streq(name, "cover?") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
          int t2 = ++g_tmp;
          buf_printf(b, "({ sp_Range _t%d = ", t2); emit_expr(c, argv[0], b);
          buf_printf(b, "; sp_range_cover_rng(_t%d, _t%d); })", t, t2);
        }
        else {
          /* sp_range_include takes sp_int; a float arg (`(1..).include?(2.4)`)
             compares against the bounds as a Float instead, and so does a poly
             arg holding one (e.g. under --int-overflow=promote). */
          Repr at0r = repr_of(c, argv[0]);
          TyKind at0 = at0r.as_ty;
          int arg_is_float = at0 == TY_FLOAT;
          int arg_is_poly = at0 == TY_POLY;
          if (value_obj_compares(c, argv[0])) unsupported_feature(c, id, "Range#include? of a user object defining <=>");
          /* a Rational or a Bignum compares against the bounds (an end
             written as a Float as written): it answered false */
          if (at0 == TY_RATIONAL || at0r.big) {
            buf_printf(b, "sp_range_cover_poly(&_t%d, ", t);
            emit_boxed(c, argv[0], b);
            buf_puts(b, ")");
          }
          else if (value_kind_misses(c, argv[0], TY_INT)) {
            /* an Integer compares with nothing of this class: not covered */
            buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b); buf_puts(b, "); 0; })");
          }
          else {
            buf_printf(b, "sp_range_%s(&_t%d, ", arg_is_float ? "cover_f" : arg_is_poly ? "cover_poly" : "include", t);
            emit_expr(c, argv[0], b);
            buf_puts(b, ")");
          }
        }
      }
      else if (sp_streq(name, "min")) {  /* smallest enumerated element (direction-aware), nil when empty */
        oint_open(c, id, TY_INT, b); buf_printf(b, "sp_range_min_v(_t%d)", t); oint_close(c, id, b);
      }
      else if (sp_streq(name, "first") || sp_streq(name, "begin")) {
        /* #first enumerates, so a beginless range has none (#3668) */
        if (argc == 0 && sp_streq(name, "first")) {
          oint_lift_open(c, id, TY_INT, b);
          buf_printf(b, "({ if (_t%d.nobeg) sp_raise_cls(\"RangeError\","
                        " \"cannot get the first element of beginless range\"); _t%d.first; })", t, t);
          oint_lift_close(c, id, b);
        }
        else if (argc == 1) {
          /* first(n): the first n elements from `first`, walking by step. */
          int tf = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp, tc = ++g_tmp;
          buf_printf(b, "({ sp_IntArray *_t%d = sp_IntArray_new(); sp_int _t%d = ", tf, tn);
          emit_int_expr(c, argv[0], b);   /* first(nil) is CRuby's TypeError */
          buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\");", tn);
          buf_printf(b, " sp_int _t%d = sp_range_count(_t%d); sp_int _t%d = sp_range_step(_t%d);"
                        " for (sp_int _i%d = 0; _i%d < _t%d && _i%d < _t%d; _i%d++)"
                        " sp_IntArray_push(_t%d, _t%d.first + _i%d * _t%d); _t%d; })",
                     tc, t, ti, t, tf, tf, tn, tf, tc, tf, tf, t, tf, ti, tf);
        }
        else {
          /* #begin: nil for a beginless range, with its nil where the
             consumer takes the oint (node_is_oint) */
          oint_open(c, id, TY_INT, b); buf_printf(b, "sp_range_begin_o(_t%d)", t); oint_close(c, id, b);
        }
      }
      else if (sp_streq(name, "max")) {  /* largest enumerated element (direction-aware), nil when empty */
        oint_open(c, id, TY_INT, b); buf_printf(b, "sp_range_max_v(_t%d)", t); oint_close(c, id, b);
      }
      else if (sp_streq(name, "end") && argc == 0 && comp_ntype(c, id) == TY_FLOAT) {
        /* `x..Float::INFINITY`: the literal named the bound the int range can
           only record as "unbounded" -- answer the Float itself (#3670) */
        buf_puts(b, node_is_oint(c, id) ? "sp_ofloat_of(HUGE_VAL)" : "HUGE_VAL"); (void)t;   /* never nil */
      }
      else if (sp_streq(name, "end") && ({ int _rr = unwrap_parens(c, recv);
               nt_type(nt, _rr) && sp_streq(nt_type(nt, _rr), "RangeNode") &&
               nt_ref(nt, _rr, "right") < 0; })) {
        /* an ENDLESS literal range: #end is nil (#2413) */
        buf_puts(b, "sp_box_nil()"); (void)t;
      }
      else if (argc == 0 && sp_streq(name, "end")) {
        /* #end is nil for ANY endless range, however it was spelled: `1..nil`
           and a range held in a variable say so in their noend flag, where
           the literal-shape arm above sees no syntax to key on (#3670) */
        oint_open(c, id, TY_INT, b);
        buf_printf(b, "(_t%d.noend && !_t%d.fe ? sp_oint_nil() : sp_oint_of(sp_range_end_i(_t%d)))", t, t, t);
        oint_close(c, id, b);
      }
      else if (is_range_end_reader(name)) {
        /* #last enumerates, so an endless range has none (#3668) */
        if (argc == 0 && sp_streq(name, "last")) {
          oint_lift_open(c, id, TY_INT, b);
          buf_printf(b, "sp_range_last_i(_t%d)", t);
          oint_lift_close(c, id, b);
        }
        else if (argc == 1 && sp_streq(name, "last")) {
          /* last(n): collect up to n elements ending at last */
          int tf = ++g_tmp, tn = ++g_tmp, ts = ++g_tmp, te = ++g_tmp;
          buf_printf(b, "({ sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
          buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\");", tn);
          /* an endless range has no last n elements to walk back from: the
             loop counted down from INTPTR_MAX and allocated until the process
             died, where CRuby raises (#3861) */
          buf_printf(b, " if (_t%d.noend) sp_raise_cls(\"RangeError\","
                        " \"cannot get the last element of endless range\");", t);
          buf_printf(b, " sp_int _t%d = _t%d.last - _t%d.excl;", te, t, t);
          buf_printf(b, " sp_int _t%d = _t%d - _t%d + 1; if (_t%d < _t%d.first) _t%d = _t%d.first;", ts, te, tn, ts, t, ts, t);
          buf_printf(b, " sp_IntArray *_t%d = sp_IntArray_new(); for (sp_int _i%d = _t%d; _i%d <= _t%d; _i%d++)"
                        " sp_IntArray_push(_t%d, _i%d); _t%d; })",
                     tf, tf, ts, tf, te, tf, tf, tf, tf);
        }
        else buf_printf(b, "(_t%d.last)", t);
      }
      else if (is_size_or_count(name) && repr_of(c, id).kind == RK_BOXED)   /* promote: past the word */
        buf_printf(b, "sp_range_count_v(_t%d, %d)", t, sp_streq(name, "size"));
      else if (is_size_or_count(name))
        buf_printf(b, "sp_range_count_open(_t%d, %d)", t, sp_streq(name, "size"));
      else if (is_sum_name(name) && g_promote_mode) {
        if (repr_of(c, id).ty == TY_FLOAT) buf_puts(b, "sp_poly_Float(");
        buf_printf(b, "sp_range_sum_seed(_t%d, ", t);
        if (argc == 1) emit_boxed(c, argv[0], b);
        else buf_puts(b, "sp_box_int(0)");
        buf_puts(b, ")");
        if (repr_of(c, id).ty == TY_FLOAT) buf_puts(b, ")");
      }
      /* promote typed the sum poly: the total can leave the word */
      else if (sp_streq(name, "sum") && argc <= 1 && repr_of(c, id).kind == RK_BOXED &&
               (argc == 0 || comp_ntype(c, argv[0]) == TY_INT)) {
        buf_printf(b, "sp_IntArray_fold_v(sp_range_to_ia(_t%d), ", t);
        if (argc == 1) emit_boxed(c, argv[0], b); else buf_puts(b, "sp_box_int(0)");
        buf_puts(b, ", 0)");
      }
      else if (sp_streq(name, "sum") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        buf_puts(b, "(("); emit_expr(c, argv[0], b);
        buf_printf(b, ") + (double)sp_IntArray_sum(sp_range_to_ia(_t%d), 0))", t);
      }
      /* Range#sum takes an INTEGER seed exactly and runs every other class
         through Kernel#Float, answering a Float -- so a Bignum seed wrapped
         into the sp_int slot here, a Rational one did not compile, and nil and
         a String reported the wrong conversion. An empty range answers the
         seed untouched, which is why the helper decides at run time. */
      else if (sp_streq(name, "sum") && argc == 1 &&
               !fold_seed_typed(fold_seed_ntype(c, argv[0]), TY_INT)) {
        buf_printf(b, "sp_range_sum_seed(_t%d, ", t);
        emit_boxed(c, argv[0], b);
        buf_puts(b, ")");
      }
      else if (sp_streq(name, "sum") && argc == 1) {
        buf_printf(b, "sp_IntArray_sum(sp_range_to_ia(_t%d), ", t);
        emit_int_expr(c, argv[0], b);
        buf_puts(b, ")");
      }
      else if (sp_streq(name, "sum"))
        buf_printf(b, "sp_IntArray_sum(sp_range_to_ia(_t%d), 0)", t);
      else if (sp_streq(name, "exclude_end?"))
        buf_printf(b, "sp_range_excl_end(_t%d)", t);
      else if (is_eql_or_equal(name)) {
        /* the unboxed sp_Range has no object identity: equal? compares
           components, like the Complex/Rational value arms */
        if (argc == 1 && comp_ntype(c, argv[0]) != TY_RANGE && comp_ntype(c, argv[0]) != TY_POLY &&
            !repr_of(c, argv[0]).untyped) {
          /* a value of another class is never eql? to a Range */
          buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b); buf_puts(b, "); 0; })");
        }
        else { buf_printf(b, "sp_range_eql(_t%d, ", t); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      }
      else if (sp_streq(name, "overlap?")) {
        /* CRuby's range_overlap at run time: an empty Range overlaps
           nothing, and the argument may be a Float Range */
        buf_printf(b, "sp_range_overlap_v(sp_box_range(_t%d), ", t); emit_boxed(c, argv[0], b);
        buf_puts(b, ")");
      }
      else if (sp_streq(name, "minmax")) {
        /* a poly pair off the endpoints, max first: an empty (backwards)
           range yields [nil, nil] (#2412), and a Float end answers itself */
        buf_printf(b, "sp_range_minmax_poly(_t%d)", t);
      }
      return 1;
    }
  }
  /* Enumerable method on a Range that arrays support but Range does not handle
     natively (reduce(:sym), group_by, partition, flat_map, count(&block), ...):
     materialize the range into an int array once, then re-dispatch the call as
     an array by overriding the receiver's emission and type. Inference already
     typed the call as the array version (range_enum_redispatch). */
  /* A Hash Enumerable served by the pair-array redispatch (reduce/inject/
     each_with_index block forms): materialize the [k, v] pairs once and
     re-dispatch as a poly array, mirroring the range redispatch below. */
  if (recv >= 0 && ty_is_hash(rt) && hash_enum_redispatch(c, id) &&
      g_n_argov < MAX_ARG_OVERRIDE) {
    int ta = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_boxed(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_enum_items_from(%s); SP_GC_ROOT(_t%d);\n",
               ta, rb.p ? rb.p : "sp_box_nil()", ta);
    free(rb.p);
    view_bind(recv, "_t%d", ta);
    int v = view_push(c, recv, TY_POLY_ARRAY);
    /* find_all on the pair array is Enumerable select (a hash receiver only
       lands here through the redispatch, so the hash-returning Hash#select
       emitter is out of the picture) */
    const char *svn = nt_str(c->nt, id, "name");
    int fa = svn && sp_streq(svn, "find_all");
    /* the block form of each_with_index returns the RECEIVER hash in CRuby,
       not the pair array the redispatch iterates (#2417) */
    int ewi = svn && sp_streq(svn, "each_with_index") && nt_ref(c->nt, id, "block") >= 0;
    if (fa) nt_node_set_str((NodeTable *)c->nt, id, "name", "select");
    if (ewi) {
      Buf db; memset(&db, 0, sizeof db);
      emit_call(c, id, &db);
      buf_printf(b, "({ (void)(%s); ", db.p ? db.p : "0");
      free(db.p);
      view_pop(c, v);
      view_unbind(g_n_argov - 1);   /* re-emit the REAL receiver, not the override */
      emit_expr(c, recv, b);
      view_bind(recv, "_t%d", ta);
      v = view_push(c, recv, TY_POLY_ARRAY);
      buf_puts(b, "; })");
    }
    else emit_call(c, id, b);
    if (fa) nt_node_set_str((NodeTable *)c->nt, id, "name", "find_all");
    view_pop(c, v);
    view_unbind(g_n_argov - 1);
    return 1;
  }
  if (recv >= 0 && rt == TY_RANGE && range_enum_redispatch(c, id) &&
      g_n_argov < MAX_ARG_OVERRIDE) {
    int ta = ++g_tmp, tr = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_IntArray *_t%d = ({ sp_Range _t%d = %s; sp_range_to_ia(_t%d); }); SP_GC_ROOT(_t%d);\n",
               ta, tr, rb.p ? rb.p : "", tr, ta);
    free(rb.p);
    view_bind(recv, "_t%d", ta);
    int v = view_push(c, recv, TY_INT_ARRAY);
    emit_call(c, id, b);
    view_pop(c, v);
    view_unbind(g_n_argov - 1);
    return 1;
  }
  return 0;
}

/* If `recv` is an index expression `outer[oidx]` (a `[]` CallNode with a single
   int argument), set *outer/*oidx and return 1. Lets a `[]=`/splice on such a
   receiver write a promoted array back into outer's slot instead of dropping the
   write-back (which would lose a typed->poly promotion for a computed receiver). */
static int splice_recv_index_slot(Compiler *c, int recv, int *outer, int *oidx) {
  const NodeTable *nt = c->nt;
  const char *rty = nt_type(nt, recv);
  if (!rty || !sp_streq(rty, "CallNode")) return 0;
  const char *rn = nt_str(nt, recv, "name");
  if (!rn || !sp_streq(rn, "[]")) return 0;
  int ro = nt_ref(nt, recv, "receiver");
  if (ro < 0) return 0;
  int rargc; const int *rargv = call_args(nt, recv, &rargc);
  /* A boxed index is addressable too: emit_int_expr converts it. Requiring
     TY_INT here dropped `rows[r][c] = "*"` when `r` came from a destructured
     block parameter -- the store fell to the by-value form, which writes into
     a copy of the element and loses the assignment (#4078). */
  if (rargc != 1) return 0;
  { TyKind it = comp_ntype(c, rargv[0]);
    /* The slot helpers address the outer by INTEGER index, so a boxed index is
       only usable when the outer really is an array -- a boxed Hash KEY would
       be converted to an int and refused. Requiring TY_INT outright dropped
       `rows[r][c] = "*"` when `r` came from a destructured block parameter:
       the store fell to the by-value form, which writes into a copy of the
       element and loses the assignment (#4078). */
    if (it != TY_INT) {
      if (it != TY_POLY) return 0;
      TyKind ot = comp_ntype(c, ro);
      if (!ty_is_array(ot)) return 0;
    } }
  *outer = ro; *oidx = rargv[0];
  return 1;
}

/* `x[k] = v` on a boxed local or instance variable whose key or value
   rebinds it (`s[(s = t; 0)] = v`; aset_recv_rebinds_only). The arms below
   read the receiver after the key and value, so the store went to the new
   value, and a String splice or a widened Array came back over the new
   binding. CRuby stores into the receiver it read first and keeps the new
   binding. A value with to_ary for a Range key keeps its arm. */
static int poly_aset_rebound(Compiler *c, int recv, const int *argv) {
  if (comp_ntype(c, argv[0]) == TY_RANGE && splice_to_ary_mi(c, comp_ntype(c, argv[1])) >= 0) return 0;
  return aset_recv_rebinds_only(c, recv, argv[0], argv[1]);
}
/* Whether `id` reads the variable `nm` (a read of kind `rk`) or, for an
   instance variable, runs any call: something that may mutate it or move
   its String into a shared handle (`s << x`, `t = s`, `s = s`, a method
   that writes @s). */
static int aset_touches_recv(Compiler *c, int id, NodeKind rk, const char *nm) {
  const NodeTable *nt = c->nt;
  if (id < 0) return 0;
  NodeKind k = nt_kind(nt, id);
  if (k == rk) {
    const char *rn = nt_str(nt, id, "name");
    if (!rn || sp_streq(rn, nm)) return 1;
  }
  if (rk == NK_InstanceVariableReadNode &&
      ((k == NK_CallNode && !call_is_scalar_op(c, id)) || k == NK_YieldNode ||
       k == NK_SuperNode || k == NK_ForwardingSuperNode))
    return 1;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (aset_touches_recv(c, nt_ref_at(nt, id, i), rk, nm)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (aset_touches_recv(c, ids[j], rk, nm)) return 1;
  }
  return 0;
}
/* See codegen_internal.h. A plain String is a `const char *` that a mutator
   replaces with a new buffer, so the variable no longer holding the String
   read first does not tell a rebinding from a mutation: the store reads the
   receiver first only when nothing in the key or value can mutate it, and
   otherwise reads it after them, as before. */
int aset_recv_rebinds_only(Compiler *c, int recv, int key, int val) {
  NodeKind rk = nt_kind(c->nt, recv);
  if (rk != NK_LocalVariableReadNode && rk != NK_InstanceVariableReadNode) return 0;
  if (!read_rebound_by(c, recv, key) && !read_rebound_by(c, recv, val)) return 0;
  return !aset_recv_may_mutate(c, recv, key, val);
}
/* See codegen_internal.h. */
int aset_recv_may_mutate(Compiler *c, int recv, int key, int val) {
  const NodeTable *nt = c->nt;
  NodeKind rk = nt_kind(nt, recv);
  if (rk != NK_LocalVariableReadNode && rk != NK_InstanceVariableReadNode) return 0;
  const char *nm = nt_str(nt, recv, "name");
  if (!nm || aset_touches_recv(c, key, rk, nm) || aset_touches_recv(c, val, rk, nm)) return 1;
  /* a proc that captures the local may mutate it as well as rebind it */
  if (rk == NK_LocalVariableReadNode) {
    LocalVar *lv = scope_local(comp_scope_of(c, recv), nm);
    if (lv && lv->is_cell && (subtree_may_run_proc(c, key) || subtree_may_run_proc(c, val))) return 1;
  }
  return 0;
}
/* See codegen_internal.h. */
int emit_aset_recv_read(Compiler *c, int recv, Buf *b) {
  if (!g_pre) { emit_expr(c, recv, b); return 0; }
  int tn = ++g_tmp;
  Buf rb = expr_buf(c, recv);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_RbVal _t%d = %s; ", tn, rb.p ? rb.p : "sp_box_nil()");
  emit_gc_root_tmp(c, TY_POLY, tn, g_pre);
  buf_puts(g_pre, "\n");
  free(rb.p);
  buf_printf(b, "_t%d", tn);
  return 1;
}
/* The store for poly_aset_rebound: the receiver into a temp ahead of the
   key and the value, even of a prelude the key moves ahead of the statement
   (emit_aset_recv_read), then the key and the value, the store on the temp,
   and the answer (a spliced String, a widened Array) back only to a
   variable that still holds the receiver read. A Symbol or other key
   stores in place. */
static void emit_poly_aset_rebound(Compiler *c, int recv, const int *argv, Buf *b) {
  TyKind at = comp_ntype(c, argv[0]);
  /* a key that can be nil is held with its nil, and a nil one goes to the
     runtime boxed, as in the store below (poly_int_key_boxed) */
  int nkey = poly_int_key_boxed(c, argv[0]);
  int tr = ++g_tmp, tk = ++g_tmp, tv = ++g_tmp, to = ++g_tmp;
  buf_printf(b, "({ sp_RbVal _t%d = ", tr);
  if (emit_aset_recv_read(c, recv, b)) buf_puts(b, "; ");
  else buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", tr);
  if (nkey) { buf_printf(b, "sp_oint _t%d = ", tk); emit_oint_expr(c, argv[0], TY_INT, b); buf_puts(b, "; "); }
  else if (at == TY_INT) { buf_printf(b, "sp_int _t%d = ", tk); emit_int_expr(c, argv[0], b); buf_puts(b, "; "); }
  else if (at == TY_RANGE) { buf_printf(b, "sp_Range _t%d = ", tk); emit_expr(c, argv[0], b); buf_puts(b, "; "); }
  else if (at == TY_SYMBOL) { buf_printf(b, "sp_sym _t%d = ", tk); emit_expr(c, argv[0], b); buf_puts(b, "; "); }
  else {
    buf_printf(b, "sp_RbVal _t%d = ", tk); emit_boxed(c, argv[0], b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", tk);
  }
  buf_printf(b, "sp_RbVal _t%d = ", tv); emit_boxed(c, argv[1], b);
  buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", tv);
  const char *store = at == TY_INT ? "sp_poly_arr_widen_and_set"
                    : at == TY_RANGE ? "sp_poly_splice_range"
                    : at == TY_STRING || at == TY_REGEX || at == TY_POLY ? "sp_poly_str_aset_key" : NULL;
  if (!store) {
    buf_printf(b, at == TY_SYMBOL ? "sp_poly_set_sym(_t%d, _t%d, _t%d); _t%d; })"
                                  : "sp_poly_set_poly(_t%d, _t%d, _t%d); _t%d; })", tr, tk, tv, tv);
    return;
  }
  if (nkey)
    buf_printf(b, "sp_RbVal _t%d = SP_UNLIKELY(_t%d.nil) ? (sp_poly_set_poly(_t%d, sp_box_nil(), _t%d), _t%d) : %s(_t%d, _t%d.v, _t%d); if (",
               to, tk, tr, tv, tr, store, tr, tk, tv);
  else buf_printf(b, "sp_RbVal _t%d = %s(_t%d, _t%d, _t%d); if (", to, store, tr, tk, tv);
  emit_expr(c, recv, b); buf_printf(b, ".tag == _t%d.tag && ", tr);
  emit_expr(c, recv, b); buf_printf(b, ".v.p == _t%d.v.p) ", tr);
  emit_expr(c, recv, b); buf_printf(b, " = _t%d; _t%d; })", to, tv);
}

/* Is the receiver a variable? A String mutator's new contents go back into
   one; a receiver that is no variable -- an element read, a Hash value -- can
   take them only through a shared handle, and only from a mutator whose value
   is those contents (PF_VAL_SELF): the typed emitter leaves them in the
   receiver's temp for a variable alone. */
static int face_str_var_recv(const NodeTable *nt, int recv) {
  const char *rvt = nt_type(nt, recv);
  return rvt && (sp_streq(rvt, "LocalVariableReadNode") || sp_streq(rvt, "InstanceVariableReadNode"));
}
/* One owner's arm: unbox `box` (a temp holding the boxed receiver, or 0 to
   unbox the receiver expression itself) to `kind`'s representation in the
   statement prelude, override the receiver node with the temp, retype and
   pin it, and re-enter the same call so the typed emitter takes it from
   there. A receiver that is not of that kind at run time raises the
   NoMethodError the call would have raised, from the coercion. Answers the
   arm's value text in `val` and its type under the pin -- the node's settled
   type is the union over the inference passes and the owners, and may be
   poly where the arm answers a pointer. */
/* The class of upto's limit when it is a number by its type ("Integer",
   "Float"), else NULL */
static const char *face_upto_lim_cls(Compiler *c, int id) {
  int argc;
  const int *argv = call_args(c->nt, id, &argc);
  TyKind lt = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  return lt == TY_INT ? "Integer" : lt == TY_FLOAT ? "Float" : NULL;
}
static TyKind emit_face_arm(Compiler *c, int id, unsigned kind, unsigned flags, int box, Buf *val) {
  const NodeTable *nt = c->nt;
  /* The re-entered emitter may rename the node for its own re-entry (a
     String bang takes its plain form) and restore it into fresh storage, so
     the name is kept here, not borrowed from the node table. Only a table
     row's name arrives, so the buffer cannot truncate. */
  char name[128];
  snprintf(name, sizeof name, "%s", nt_str(nt, id, "name"));
  int recv = nt_ref(nt, id, "receiver");
  int has_blk = nt_ref(nt, id, "block") >= 0;
  int t = ++g_tmp;
  /* A String mutator on a receiver that is no variable -- an element read,
     a Hash value -- has only the shared handle behind the box to take its
     new contents. Its temp is named as the reader shim names a handle's
     shadow copy (sb_shadowed_reader), so the typed emitter writes the new
     contents back into it, as it does for a variable, and the write-back
     below hands them to the handle. Under a plain name, slice! answered the
     removed part and wrote nothing back. */
  const char *tp = (kind == PF_STRING && box && (flags & PF_MUT) && !face_str_var_recv(nt, recv)) ? "lv__sb" : "_t";
  char bx[32];
  Buf rb; memset(&rb, 0, sizeof rb);
  if (box) snprintf(bx, sizeof bx, "_t%d", box);
  else {
    /* the elements of a collection materialize from the box itself */
    if (kind == PF_ENUM) emit_boxed(c, recv, &rb);
    else emit_expr(c, recv, &rb);
  }
  const char *rs = box ? bx : rb.p ? rb.p : "sp_box_nil()";
  emit_indent(g_pre, g_indent);
  switch (kind) {
    /* A String mutator's receiver checks a shared handle's frozen flag
       first, as the Array and Hash coercions below do: the handle hands out
       its own bytes, and setbyte wrote them before the write-back raised. */
    case PF_STRING:
      buf_printf(g_pre, "const char *%s%d = sp_poly_recv_s%s(%s, \"%s\"); SP_GC_ROOT(%s%d);\n",
                 tp, t, (flags & PF_MUT) ? "_mut" : "", rs, name, tp, t);
      break;
    case PF_INT:
      /* The block iterators check: `"x".times { }` is a NoMethodError in
         CRuby, and coercing would silently run the loop zero times. The
         blockless names keep the plain coercion they have always used. */
      /* upto: a String receiver's own raises TypeError for the numeric limit */
      if (has_blk && is_upto_name(name) && face_upto_lim_cls(c, id))
        buf_printf(g_pre, "sp_int _t%d = sp_poly_upto_recv(%s, \"%s\");\n", t, rs, face_upto_lim_cls(c, id));
      else if (has_blk) buf_printf(g_pre, "sp_int _t%d = sp_poly_int_recv(%s, \"%s\");\n", t, rs, name);
      else buf_printf(g_pre, "sp_int _t%d = sp_poly_recv_i(\"%s\", %s);\n", t, name, rs);
      break;
    case PF_RANDOM:
      buf_printf(g_pre, "sp_Random *_t%d = sp_poly_random_recv(%s, \"%s\"); SP_GC_ROOT(_t%d);\n", t, rs, name, t);
      break;
    case PF_FLOAT:
      buf_printf(g_pre, "sp_float _t%d = sp_poly_float_recv(%s, \"%s\");\n", t, rs, name);
      break;
    /* a Range of the owner's kind, read by value out of its box */
    case PF_RANGE:
      buf_printf(g_pre, "sp_Range _t%d = sp_poly_range_recv(%s, \"%s\");\n", t, rs, name);
      break;
    case PF_FRANGE:
      buf_printf(g_pre, "sp_FloatRange _t%d = sp_poly_frange_recv(%s, \"%s\");\n", t, rs, name);
      break;
    case PF_SRANGE:
      buf_printf(g_pre, "sp_StrRange _t%d = sp_poly_srange_recv(%s, \"%s\"); SP_GC_ROOT_STR(_t%d.first); SP_GC_ROOT_STR(_t%d.last);\n",
                 t, rs, name, t, t);
      break;
    /* A mutator's coercion checks the original for frozenness first: the
       typed emitter would otherwise work on the copy, running a block over
       every element, and only the write-back would raise. */
    case PF_ARRAY:
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_array_recv(%s, \"%s\", %d); SP_GC_ROOT(_t%d);\n",
                 t, rs, name, (flags & PF_MUT) != 0, t);
      break;
    case PF_ENUM:
      /* a hash gives its [key, value] pairs; a receiver that is no collection
         raises the NoMethodError the call raised before */
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_enum_recv(%s, \"%s\"); SP_GC_ROOT(_t%d);\n", t, rs, name, t);
      break;
    case PF_HASH:
      buf_printf(g_pre, "sp_PolyPolyHash *_t%d = sp_poly_hash_recv(%s, \"%s\", %d); SP_GC_ROOT(_t%d);\n",
                 t, rs, name, (flags & PF_MUT) != 0, t);
      break;
  }
  free(rb.p);
  view_bind(recv, "%s%d", tp, t);
  TyKind as = ty_poly_face_kind(kind);
  int v = view_push(c, recv, as);
  int fv = view_push_face(recv, as);
  TyKind nat = infer_uncached(c, id);
  if (kind == PF_HASH && repr_of(c, id).demand && strbuf_hash_default_arg(c, id) >= 0) nat = TY_STRBUF;
  /* A numeric iterator with a block answers its receiver, and the arm's
     expression bridge renders exactly that, in the owner's own kind -- but
     a `break v` in the block makes the pinned inference say poly for the
     union, which emit_face_value reads as "already boxed" and hands the raw
     sp_int / sp_float to the poly slot (#4774). The value is the receiver. */
  if (nat == TY_POLY && has_blk &&
      (kind == PF_INT || kind == PF_FLOAT || kind == PF_RANGE || kind == PF_FRANGE || kind == PF_SRANGE) &&
      (is_integer_iteration(name)))
    nat = as;
  /* ...and so does a written-back mutator its row says always answers the
     receiver (delete_if, keep_if, sort!, merge!): inside the break wrapper
     (emit_brk_wrapped_call) a break's value goes to the wrapper's slot, so
     this text is the receiver in the face's kind, where the union said poly
     and it was bound into an sp_RbVal raw. One that answers nil when nothing
     changed (BOPF_SELF_OR_NIL) keeps the poly reading. */
  if (nat == TY_POLY && (flags & PF_MUT) && (flags & PF_VAL_SELF) && box && g_brk_skip_id == id) {
    int argc = 0;
    call_args(nt, id, &argc);
    if (bop_answers_self(as, name, argc, has_blk) == BOPF_SELF) nat = as;
  }
  Buf cb; memset(&cb, 0, sizeof cb);
  /* An Integer or Float answer that can be nil (bsearch finding nothing)
     comes back as its oint: a poly slot boxes the nil, a plain one unwraps
     it where it is read */
  int sfo = g_face_oint_id, sfg = g_face_oint_got;
  int keep_o = (nat == TY_INT || nat == TY_FLOAT) && !node_is_oint(c, id);
  if (keep_o) { g_face_oint_id = id; g_face_oint_got = 0; }
  emit_call(c, id, &cb);
  TyKind got_o = keep_o ? (TyKind)g_face_oint_got : TY_UNKNOWN;
  g_face_oint_id = sfo; g_face_oint_got = sfg;
  if (got_o) {
    Buf ob; memset(&ob, 0, sizeof ob);
    TyKind slot = repr_of(c, id).as_ty;
    if (slot == TY_POLY || slot == TY_UNKNOWN) {
      buf_printf(&ob, "%s(%s)", got_o == TY_FLOAT ? "sp_box_ofloat" : "sp_box_oint", cb.p ? cb.p : "");
      nat = TY_POLY;
    }
    else { buf_printf(&ob, "%s(%s)", oint_arg(got_o), cb.p ? cb.p : ""); nat = got_o; }
    free(cb.p); cb = ob;
  }
  view_pop(c, fv);
  view_pop(c, v);
  view_unbind(g_n_argov - 1);
  const char *call = cb.p ? cb.p : "0";
  /* A typed emitter that declines the call's argument shape answers the
     unresolved gate's raise token, a poly value that never returns: hand it
     on as poly, untouched, rather than bind it into a typed temp. */
  /* ...and the argument-validation token the numeric arms lower an
     operand of the wrong class to (`({ (void)(recv); sp_raise_cls(...);
     sp_box_nil(); })`): its value is the node's own default, boxed here since
     the node is poly, whatever kind the arm was pinned to (#4779) */
  size_t cl = strlen(call);
  int val_tok = strncmp(share_check_unmarked(call), "({ (void)(", 10) == 0 && strstr(call, "sp_raise_cls(") != NULL &&
                cl > 18 && strcmp(call + cl - 18, "; sp_box_nil(); })") == 0;
  if (strncmp(share_check_unmarked(call), "sp_raise_nomethod(", 18) == 0 || val_tok) {
    TyKind slot = repr_of(c, id).as_ty;
    if (slot == TY_POLY || slot == TY_UNKNOWN || slot == TY_VOID) { buf_puts(val, call); slot = TY_POLY; }
    else emit_unbox_text(c, slot, call, val);   /* the token is an sp_RbVal; the slot is not */
    free(cb.p);
    return slot;
  }
  /* A mutator worked on the unboxed representation -- the poly copy a typed
     array was normalized to, the text a string box stands for -- and the
     original has to take the result back once the value is taken. */
  if ((flags & PF_MUT) && box && (kind == PF_ARRAY || kind == PF_STRING || kind == PF_HASH)) {
    Buf wb; memset(&wb, 0, sizeof wb);
    int tr = ++g_tmp;
    int has_val = nat != TY_VOID && nat != TY_UNKNOWN;
    if (kind == PF_ARRAY) buf_printf(&wb, "sp_poly_arr_writeback(_t%d, _t%d)", box, t);
    else if (kind == PF_HASH) buf_printf(&wb, "sp_poly_hash_writeback(_t%d, _t%d)", box, t);
    else {
      /* The new contents -- the value itself when the mutator answers self,
         else the temp the typed emitter took the receiver's variable from and
         wrote to -- go back into the receiver's variable: a shared handle
         absorbs them, so a container the value came from observes the change;
         a plain string box is replaced, the way the typed path replaces its
         own. A receiver that is no variable (face_str_var_recv) has a handle
         to absorb them or nowhere to send them, and then raises what the call
         raised before the row existed, rather than a mutation that silently
         goes nowhere. Contents that are the receiver's own mean no write at
         all when the row says so (scrub!). */
      int var = face_str_var_recv(nt, recv);
      int nv = ((flags & PF_VAL_SELF) && has_val) ? tr : t;
      if (var) {
        emit_expr(c, recv, &wb); buf_puts(&wb, " = ");
        if (flags & PF_SAME_OK) buf_printf(&wb, "sp_poly_str_is_own(_t%d, _t%d) ? _t%d : ", box, nv, box);
        buf_printf(&wb, "sp_poly_str_become(_t%d, _t%d)", box, nv);
      }
      else {
        const char *np = nv == t ? tp : "_t";
        if (flags & PF_SAME_OK) buf_printf(&wb, "if (!sp_poly_str_is_own(_t%d, %s%d)) ", box, np, nv);
        buf_printf(&wb, "sp_poly_str_become_handle(_t%d, %s%d, \"%s\")", box, np, nv, name);
      }
    }
    if (!has_val) buf_printf(val, "({ (void)(%s); %s; })", call, wb.p);
    else if (nat == TY_NIL) {
      /* a literal nil argument answered by a setter (`box.default = nil`):
         nil has no C type of its own to bind the value to */
      buf_printf(val, "({ (void)(%s); %s; sp_box_nil(); })", call, wb.p);
      nat = TY_POLY;
    }
    else if (kind == PF_STRING && (flags & PF_VAL_SELF)) {
      /* The value is the receiver as the write left it: the shared handle, or
         the plain string box the variable now holds -- not a fresh box of the
         new contents, which no alias observes (s.concat("!").equal?(s)). */
      int tq = ++g_tmp;
      if (face_str_var_recv(nt, recv)) buf_printf(val, "({ %s _t%d = %s; sp_RbVal _t%d = (%s); _t%d; })", c_type_name(nat), tr, call, tq, wb.p, tq);
      else buf_printf(val, "({ %s _t%d = %s; %s; _t%d; })", c_type_name(nat), tr, call, wb.p, box);
      nat = TY_POLY;
    }
    else if (flags & PF_VAL_SELF) {
      /* The value is the receiver -- the box -- not the general copy the
         emitter worked on: a typed original has no general stand-in, and the
         copy is detached once written back, so a write through the value
         (h.merge!(a)[:k] = v, a.concat(b) << x) would go nowhere. compact!
         answers nil when it removed nothing, and the receiver else. */
      if (nat == TY_POLY) buf_printf(val, "({ sp_RbVal _t%d = %s; %s; sp_poly_nil_p(_t%d) ? _t%d : _t%d; })", tr, call, wb.p, tr, tr, box);
      else buf_printf(val, "({ (void)(%s); %s; _t%d; })", call, wb.p, box);
      nat = TY_POLY;
    }
    else buf_printf(val, "({ %s _t%d = %s; %s; _t%d; })", c_type_name(nat), tr, call, wb.p, tr);
    free(wb.p);
  }
  else buf_puts(val, call);
  free(cb.p);
  return nat;
}

/* The value of an arm in the call's result slot: as it is when the two
   types agree, boxed into a poly slot otherwise. */
static void emit_face_value(Compiler *c, TyKind slot, TyKind nat, const char *val, Buf *b) {
  /* a poly answer under the pin is not a boxed value: the Integer iterators
     answer their receiver's sp_int while the pinned inference says poly */
  if (slot == nat || nat == TY_POLY || nat == TY_UNKNOWN || nat == TY_VOID) buf_puts(b, val);
  else if (slot == TY_POLY) emit_boxed_text(c, nat, val, b);
  /* owners that answer arrays of different kinds (an Integer and a Float
     Range's minmax) share a poly-array slot: a typed array is copied into
     it element by element, where the plain unbox cast the Float array's
     memory to boxed values */
  else if (slot == TY_POLY_ARRAY && ty_is_array(nat)) {
    buf_puts(b, "sp_poly_to_poly_array(");
    emit_boxed_text(c, nat, val, b);
    buf_puts(b, ")");
  }
  else {
    Buf bx; memset(&bx, 0, sizeof bx);
    emit_boxed_text(c, nat, val, &bx);
    emit_unbox_text(c, slot, bx.p, b);
    free(bx.p);
  }
}

/* An arm under the silent emittability probe the dynamic-send dispatch
   uses: a typed emitter that declines the call longjmps out of emit, and
   the arm is dropped rather than the build. Everything the arm may have
   changed on the way out is put back -- the receiver's type view and face
   pin emit_face_arm pops only on its normal return (view_unwind pops them
   otherwise), the argument overrides, the conversion hold, the prelude,
   and the recovery point itself, which the driver armed for the whole
   unit. The arm's prelude is captured to `pre` and its value to `val`;
   answers 0 when the arm was dropped. */
static int face_probe_arm(Compiler *c, int id, unsigned kind, unsigned flags, int box,
                          Buf *pre, Buf *val, TyKind *nat) {
  Buf *sv_pre = g_pre;
  int sv_probe = g_unsup_probe;
  ConvHold *sv_hold = g_conv_hold;
  int sv_argov = g_n_argov;
  int sv_open_defaults = g_open_defaults;
  jmp_buf sv_jb; memcpy(sv_jb, g_unsup_recover, sizeof(jmp_buf));
  volatile int ok = 1;
  int sv_moves = comp_scope_move_depth(), sv_views = view_mark();
  g_pre = pre; g_unsup_probe = 1;
  if (setjmp(g_unsup_recover) == 0) *nat = emit_face_arm(c, id, kind, flags, box, val);
  else { ok = 0; comp_scope_move_unwind(sv_moves); view_unwind(sv_views); }
  memcpy(g_unsup_recover, sv_jb, sizeof(jmp_buf));
  view_unbind(sv_argov);
  g_open_defaults = sv_open_defaults;
  g_conv_hold = sv_hold;
  g_unsup_probe = sv_probe;
  g_pre = sv_pre;
  return ok;
}

/* One owner: exactly the re-entry, with the box kept only when a mutator
   has to write back through it. Answers 0 when the typed emitter declined
   the call, and the call goes on to the arms after this one. */
/* `poly.upto(lim [, exclusive])` that a String receiver may take
   (an_poly_str_upto): the receiver's tag picks String#upto, over the
   members sp_StrArray_from_string_range lists, or Integer#upto, counting
   (a Bignum boxed); any other value is NoMethodError. With a block one
   walk binds each member boxed to the parameter and the call answers the
   receiver; without one it answers the String's or the Bignum's members
   as an Enumerator, or the Integer's Range, boxed. The receiver and the
   arguments are held first, in order. */
static int emit_poly_str_upto(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  /* a block that breaks reaches here inside its break wrapper
     (emit_brk_wrapped_call), which delivers the break's value */
  if (!an_poly_str_upto(c, id) || (call_breaks(c, id) && id != g_brk_skip_id)) return 0;
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  int block = nt_ref(nt, id, "block");
  int tr = ++g_tmp, tl = ++g_tmp, tx = ++g_tmp, ts = ++g_tmp, th = ++g_tmp;
  tr = hold_operand_pre(c, recv, TY_POLY, 1, tr, 1);
  Buf lb; memset(&lb, 0, sizeof lb); emit_boxed(c, argv[0], &lb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_RbVal _t%d[2]; _t%d[0] = %s; SP_GC_ROOT_RBVAL(_t%d[0]);", tl, tl, lb.p ? lb.p : "sp_box_nil()", tl);
  free(lb.p);
  if (argc == 2) {
    Buf xb; memset(&xb, 0, sizeof xb); emit_boxed(c, argv[1], &xb);
    buf_printf(g_pre, " _t%d[1] = %s; SP_GC_ROOT_RBVAL(_t%d[1]);", tl, xb.p ? xb.p : "sp_box_nil()", tl);
    free(xb.p);
  }
  buf_puts(g_pre, "\n");
  /* the kind: 1 a String (its members in _t<ts>), 2 an Integer (counting
     to _t<th>), 3 a Bignum (counting boxed in _t<tc>); anything else
     raises */
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "int _t%d = 0; sp_StrArray *_t%d = NULL; SP_GC_ROOT(_t%d); sp_int _t%d = 0;\n", tx, ts, ts, th);
  char ex[48];
  if (argc == 2) snprintf(ex, sizeof ex, "sp_poly_truthy(_t%d[1])", tl);
  else snprintf(ex, sizeof ex, "0");
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "if (_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)) { _t%d = 1;"
                    " _t%d = sp_StrArray_from_string_range(sp_poly_recv_s(_t%d, \"upto\"), sp_poly_arg_str_chk(_t%d[0]), %s); }\n",
             tr, tr, tx, ts, tr, tl, ex);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "else if (_t%d.tag == SP_TAG_INT) { _t%d = 2;", tr, tx);
  if (argc == 2) buf_puts(g_pre, " sp_raise_cls(\"ArgumentError\", \"wrong number of arguments (given 2, expected 1)\");");
  buf_printf(g_pre, " if (_t%d[0].tag == SP_TAG_BIGINT) _t%d = 3;", tl, tx);
  buf_printf(g_pre, " else if (_t%d[0].tag == SP_TAG_INT) _t%d = _t%d[0].v.i;"
                    " else if (_t%d[0].tag == SP_TAG_FLT) _t%d = (sp_int)floor(_t%d[0].v.f);"
                    " else sp_raise_cls(\"ArgumentError\", sp_sprintf(\"comparison of Integer with %%s failed\", sp_poly_cmp_err_repr(_t%d[0]))); }\n",
             tl, th, tl, tl, th, tl, tl);
  /* a Bignum counts boxed, compared by value (a limit that is no number
     is the comparison's ArgumentError) */
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "else if (_t%d.tag == SP_TAG_BIGINT) { _t%d = 3;%s"
                    " if (!(_t%d[0].tag == SP_TAG_INT || _t%d[0].tag == SP_TAG_BIGINT || _t%d[0].tag == SP_TAG_FLT))"
                    " sp_raise_cls(\"ArgumentError\", sp_sprintf(\"comparison of Integer with %%s failed\", sp_poly_cmp_err_repr(_t%d[0]))); }\n",
             tr, tx, argc == 2 ? " sp_raise_cls(\"ArgumentError\", \"wrong number of arguments (given 2, expected 1)\");" : "",
             tl, tl, tl, tl);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "else sp_raise_nomethod(sp_nomethod_msg_args(\"upto\", _t%d, %d, _t%d));\n", tr, argc, tl);
  int tc = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_RbVal _t%d = _t%d; SP_GC_ROOT_RBVAL(_t%d);\n", tc, tr, tc);
  if (block < 0) {
    /* the String's members as its Enumerator, the Integer's Range, a
       Bignum's members as theirs */
    int tb = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = NULL; SP_GC_ROOT(_t%d);\n", tb, tb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "if (_t%d == 3) { _t%d = sp_PolyArray_new(); while (sp_poly_cmp_ck(_t%d, _t%d[0]) <= 0)"
                      " { sp_PolyArray_push(_t%d, _t%d); _t%d = sp_poly_add(_t%d, sp_box_int(1)); } }\n",
               tx, tb, tc, tl, tb, tc, tc, tc);
    buf_printf(b, "(_t%d == 1 ? sp_box_nullable_obj((void *)sp_Enumerator_new_from(sp_box_nullable_obj((void *)_t%d, SP_BUILTIN_STR_ARRAY)), SP_BUILTIN_ENUMERATOR)"
                  " : _t%d == 3 ? sp_box_nullable_obj((void *)sp_Enumerator_new_from(sp_box_poly_array(_t%d)), SP_BUILTIN_ENUMERATOR) : ",
               tx, ts, tx, tb);
    char rg[128];
    snprintf(rg, sizeof rg, "(sp_Range){ .first = _t%d.v.i, .last = _t%d, .excl = 0 }", tr, th);
    emit_boxed_text(c, TY_RANGE, rg, b);
    buf_puts(b, ")");
    return 1;
  }
  const char *p0o = block_param_name(c, block, 0);
  const char *p0 = p0o ? rename_local(p0o) : NULL;
  int body = nt_ref(nt, block, "body");
  int ti = ++g_tmp, te = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 0; ; _t%d++) {\n", ti, ti);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_RbVal _t%d;\n", te);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (_t%d == 1) { if (_t%d >= _t%d->len) break; _t%d = sp_box_str(_t%d->data[_t%d]); }\n",
             tx, ti, ts, te, ts, ti);
  emit_indent(g_pre, g_indent + 1);
  /* the count past the start, compared in unsigned: `start + i` itself
     overflows when the limit is near 2**63-1 */
  buf_printf(g_pre, "else if (_t%d == 2) { if (_t%d < _t%d.v.i || (uintptr_t)_t%d > (uintptr_t)_t%d - (uintptr_t)_t%d.v.i) break;"
                    " _t%d = sp_box_int(_t%d.v.i + _t%d); }\n",
             tx, th, tr, ti, th, tr, te, tr, ti);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "else { if (sp_poly_cmp_ck(_t%d, _t%d[0]) > 0) break; _t%d = _t%d; _t%d = sp_poly_add(_t%d, sp_box_int(1)); }\n",
             tc, tl, te, tc, tc, tc);
  if (p0) {
    char es[24]; snprintf(es, sizeof es, "_t%d", te);
    emit_iter_param_assign(c, block, p0o, p0, TY_POLY, es, g_pre, g_indent + 1);
  }
  if (body >= 0) emit_loop_body(c, body, g_pre, g_indent + 1);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tr);
  return 1;
}

static int emit_face_reentry(Compiler *c, int id, unsigned kind, unsigned flags, Buf *b) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  int box = 0;
  Buf pre = {0, 0, 0}, val = {0, 0, 0};
  TyKind nat = TY_UNKNOWN;
  /* A String mutator on a receiver that is no variable is taken too: the
     shadow temp emit_face_arm names takes its new contents, and only a
     shared handle behind the box can absorb them; a plain box raises the
     NoMethodError the call raised before. Declined here, setbyte raised
     that NoMethodError for the handle as well. */
  if ((flags & PF_MUT) && (kind == PF_ARRAY || kind == PF_STRING || kind == PF_HASH)) box = ++g_tmp;
  int pa_frame = g_plan_check ? pa_begin(id) : -1;
  int kept = face_probe_arm(c, id, kind, flags, box, &pre, &val, &nat);
  if (g_plan_check) {
    pa_resume(pa_frame);
    pa_observe(PA_TRIAL, PA_KEY_FACE + face_kind_index(kind), -1, TY_UNKNOWN, kept);
    pa_end(c, pa_frame, cplan_poly_face(c, id));
  }
  if (!kept) {
    free(pre.p); free(val.p);
    return 0;
  }
  if (box) {
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", box, rb.p ? rb.p : "sp_box_nil()", box);
    free(rb.p);
  }
  if (pre.p) buf_puts(g_pre, pre.p);
  TyKind slot = repr_of(c, id).as_ty;
  if (repr_of(c, id).demand && strbuf_hash_default_arg(c, id) >= 0) slot = TY_STRBUF;
  emit_face_value(c, slot, nat, val.p ? val.p : "0", b);
  free(pre.p); free(val.p);
  return 1;
}

/* The run-time test that the boxed value in temp `t` is of an owner's kind. */
static void emit_face_kind_test(unsigned kind, int t, Buf *b) {
  switch (kind) {
    case PF_STRING: buf_printf(b, "(_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d))", t, t); break;
    case PF_INT:    buf_printf(b, "(_t%d.tag == SP_TAG_INT)", t); break;
    case PF_RANDOM: buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_RANDOM)", t, t); break;
    case PF_FLOAT:  buf_printf(b, "(_t%d.tag == SP_TAG_FLT)", t); break;
    case PF_RANGE:  buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_RANGE)", t, t); break;
    case PF_FRANGE: buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_FLOAT_RANGE)", t, t); break;
    case PF_SRANGE: buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_STR_RANGE)", t, t); break;
    case PF_ARRAY:  buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_t%d.cls_id))", t, t); break;
    case PF_HASH:   buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(_t%d.cls_id))", t, t); break;
    case PF_ENUM:   buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && (sp_poly_is_array_kind(_t%d.cls_id) || sp_poly_is_hash_kind(_t%d.cls_id)))", t, t, t); break;
    default:        buf_puts(b, "(0)"); break;
  }
}

/* Has the inference typed the argument as some other kind than the owner's
   own? A poly or unknown one may still be of the owner's kind at run time;
   any other kind cannot be, whether or not it has a class name of its own (a
   Boolean is true or false only at run time), so the noun CRuby's TypeError
   names is spelled from the value when the arm runs. */
int face_arg_misfit(Compiler *c, unsigned kind, int arg) {
  TyKind at = comp_ntype(c, arg);
  if (at == TY_POLY || at == TY_UNKNOWN) return 0;
  if (kind == PF_STRING && (at == TY_STRING || at == TY_STRBUF || at == TY_INT)) return 0;  /* a codepoint concatenates too */
  if (kind == PF_ARRAY && (ty_is_array(at) || at == TY_POLY_ARRAY)) return 0;
  /* a Hash, or an object whose to_hash may answer one */
  if (kind == PF_HASH && (ty_is_hash(at) || ty_is_object(at))) return 0;
  return 1;
}

/* Does an argument rule the owner `kind` out of this call: the name takes
   arguments of the owner's own kind (PF_ARGS_OWN) and one is typed as
   another? The call is then the owner's TypeError, raised by
   emit_face_switch's misfit arm, for one owner as for several. */
int face_args_misfit(Compiler *c, int id, unsigned kind) {
  const NodeTable *nt = c->nt;
  int argc;
  const int *argv = call_args(nt, id, &argc);
  if (!nt_call_args_plain(nt, id)) return 0;
  unsigned fl = ty_poly_face_owner_flags(nt_str(nt, id, "name"), argc, nt_ref(nt, id, "block") >= 0, 1, kind);
  if (!(fl & PF_ARGS_OWN)) return 0;
  for (int i = 0; i < argc; i++)
    if (face_arg_misfit(c, kind, argv[i])) return 1;
  return 0;
}

/* Several owners: bind the box once and dispatch on its run-time kind, one
   re-entry per owner, each with its own coercion and prelude inside its own
   branch. An arm whose typed emitter declines the call is dropped, under the
   silent probe the dynamic-send dispatch uses; an arm ruled out by an
   argument's type raises CRuby's TypeError; a receiver of no owner's kind
   raises the NoMethodError the call raised before. Answers 0 when no arm
   survives, and the call falls through to the arms after this one. */
static int g_endless_step_node = -1;   /* the boxed step re-entering the face (endless Range check above) */
static int emit_face_switch(Compiler *c, int id, unsigned own, Buf *b) {
  const NodeTable *nt = c->nt;
  char name[128];   /* kept, not borrowed: an arm's re-entry may rename the node (see emit_face_arm) */
  snprintf(name, sizeof name, "%s", nt_str(nt, id, "name"));
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  int has_blk = nt_ref(nt, id, "block") >= 0;
  TyKind slot = repr_of(c, id).as_ty;
  if (slot == TY_UNKNOWN || slot == TY_VOID) slot = TY_POLY;
  int plain = nt_call_args_plain(nt, id);
  Buf arms; memset(&arms, 0, sizeof arms);
  int narm = 0;
  int box = ++g_tmp, tr = ++g_tmp;
  int pa_frame = g_plan_check ? pa_begin(id) : -1;
  for (unsigned kind = 1; kind & PF_OWNERS; kind <<= 1) {
    if (!(own & kind)) continue;
    unsigned fl = ty_poly_face_owner_flags(name, argc, has_blk, plain, kind);
    int misfit = -1;
    if ((fl & PF_ARGS_OWN) && plain)
      for (int i = 0; i < argc && misfit < 0; i++) if (face_arg_misfit(c, kind, argv[i])) misfit = i;
    Buf pre = {0, 0, 0}, val = {0, 0, 0};
    TyKind nat = TY_UNKNOWN;
    int ok = 1;
    if (misfit >= 0) {
      /* the arguments are evaluated for their effects and in order, as the
         typed arm would, and under the arm's own prelude, so an argument
         that needs one runs it in this branch alone; the one that cannot
         convert is kept to name itself (nil, true and false spell themselves,
         an object its class). Each argument is a statement of its own,
         after its own prelude: as one expression, an argument that needs a
         prelude (a literal built in place) ran ahead of the ones before it */
      Buf *sv_pre = g_pre; g_pre = &pre;
      int tm = ++g_tmp;
      buf_printf(&pre, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d); ", tm, tm);
      for (int i = 0; i < argc; i++) {
        Buf ab = {0, 0, 0};
        emit_boxed(c, argv[i], &ab);
        if (i == misfit) buf_printf(&pre, "_t%d = %s; ", tm, ab.p ? ab.p : "sp_box_nil()");
        else buf_printf(&pre, "(void)(%s); ", ab.p ? ab.p : "0");
        free(ab.p);
      }
      buf_printf(&val, "sp_raise_cls(\"TypeError\", sp_sprintf(\"no implicit conversion of %%s into %s\", sp_convert_src_name(_t%d)))",
                 kind == PF_STRING ? "String" : kind == PF_HASH ? "Hash" : "Array", tm);
      g_pre = sv_pre;
    }
    else ok = face_probe_arm(c, id, kind, fl, box, &pre, &val, &nat);
    if (g_plan_check) {
      pa_resume(pa_frame);
      pa_observe(misfit >= 0 ? PA_BUILTIN : PA_TRIAL, PA_KEY_FACE + face_kind_index(kind), -1, TY_UNKNOWN,
                 misfit >= 0 ? PC_SAME : ok);
    }
    if (!ok) { free(pre.p); free(val.p); continue; }
    if (narm) buf_puts(&arms, "}\nelse ");
    buf_puts(&arms, "if ");
    emit_face_kind_test(kind, box, &arms);
    buf_puts(&arms, " { ");
    if (pre.p) buf_puts(&arms, pre.p);
    if (misfit >= 0) buf_printf(&arms, "%s;", val.p);
    else {
      buf_printf(&arms, "_t%d = ", tr);
      emit_face_value(c, slot, nat, val.p ? val.p : "0", &arms);
      buf_puts(&arms, ";");
    }
    buf_puts(&arms, " ");
    narm++;
    free(pre.p); free(val.p);
  }
  if (g_plan_check) pa_end(c, pa_frame, cplan_poly_face(c, id));
  if (!narm) { free(arms.p); return 0; }
  /* The box's declaration goes to the prelude only now: the arms name it,
     but whether any survived to need it is known only after they are built. */
  Buf rb; memset(&rb, 0, sizeof rb); emit_boxed(c, recv, &rb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", box, rb.p ? rb.p : "sp_box_nil()", box);
  free(rb.p);
  buf_printf(b, "({ %s _t%d = %s; ", c_type_name(slot), tr, default_value_from_compiler(c, slot));
  buf_puts(b, arms.p);
  buf_printf(b, "}\nelse sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d)); _t%d; })", name, box, tr);
  free(arms.p);
  return 1;
}

/* A String value-form mutator on a boxed receiver: compute the non-bang
   transform against the unboxed contents, then write the result back
   through the box. The contents come from sp_poly_recv_s, as every other
   String method's receiver does (#4029): a receiver that is no string is
   CRuby's NoMethodError, where its #to_s rendering had the transform
   applied and the write-back took the box for a string: a Symbol or an
   Integer crashed there, and an Array answered its own rendering. */
static void emit_face_str_bang(Compiler *c, int id, unsigned own, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int nil_nc = !(own & PF_STR_SELF);
  /* The node's name is rewritten to the plain form for the re-entry, so
     both spellings live here, not in the node table. */
  char bang[64], plain[64];   /* a table row's bang name: never empty, never near the cap */
  snprintf(bang, sizeof bang, "%s", name);
  snprintf(plain, sizeof plain, "%.*s", (int)strlen(name) - 1, name);
  int tvb = ++g_tmp, tob = ++g_tmp, tnb = ++g_tmp;
  Buf rbb; memset(&rbb, 0, sizeof rbb); emit_expr(c, recv, &rbb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);"
                    " const char *_t%d = sp_poly_recv_s(_t%d, \"%s\"); SP_GC_ROOT(_t%d);\n",
             tvb, rbb.p ? rbb.p : "sp_box_nil()", tvb, tob, tvb, bang, tob);
  free(rbb.p);
  view_bind(recv, "_t%d", tob);
  int v = view_push(c, recv, TY_STRING);
  nt_node_set_str((NodeTable *)nt, id, "name", plain);
  Buf nbb; memset(&nbb, 0, sizeof nbb); emit_call(c, id, &nbb);
  nt_node_set_str((NodeTable *)nt, id, "name", bang);
  view_pop(c, v);
  view_unbind(g_n_argov - 1);
  buf_printf(b, "({ const char *_t%d = %s; ", tnb, nbb.p ? nbb.p : "\"\"");
  free(nbb.p);
  /* Decide "did it change?" BEFORE the mutation. The receiver's old text
     is the LIVE payload of a shared handle, so once become() has written
     the new contents into it the two compare equal and the bang method
     answered nil after a substitution that plainly happened (#4042). */
  int tchg = 0;
  if (nil_nc) {
    tchg = ++g_tmp;
    buf_printf(b, "int _t%d = !sp_str_eq(_t%d, _t%d); ", tchg, tob, tnb);
  }
  /* A shared handle absorbs the new contents; a plain string box cannot,
     so an lvalue receiver takes the value back the way the typed path
     does for the same case. */
  { const char *rvtb = nt_type(nt, recv);
    if (rvtb && (sp_streq(rvtb, "LocalVariableReadNode") ||
                 sp_streq(rvtb, "InstanceVariableReadNode"))) {
      emit_expr(c, recv, b);
      buf_printf(b, " = sp_poly_str_become(_t%d, _t%d); ", tvb, tnb);
    }
    else buf_printf(b, "sp_poly_str_become(_t%d, _t%d); ", tvb, tnb);
  }
  if (nil_nc) buf_printf(b, "_t%d ? _t%d : NULL; })", tchg, tnb);
  else buf_printf(b, "_t%d; })", tnb);
  if (g_plan_check) {   /* the face's one arm, held against the plan */
    int pa_frame = pa_begin(id);
    pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_FACE_STR_BANG, -1, TY_UNKNOWN, PC_SAME);
    pa_end(c, pa_frame, cplan_poly_face(c, id));
  }
}

/* The `[]` call this function is re-entering for a value that is not a
   Struct or Data class, or -1. */
static int g_aref_cls_skip = -1;

/* A literal reads the same wherever it is emitted, so it is left in place
   in emit_boxed_class_aref rather than bound to a temp. */
static int aref_arg_in_place(const NodeTable *nt, int a) {
  switch (nt_kind(nt, a)) {
    case NK_NilNode: case NK_TrueNode: case NK_FalseNode: case NK_IntegerNode:
    case NK_FloatNode: case NK_SymbolNode:
      return 1;
    default:
      return 0;
  }
}

/* For a class built by its generated constructor, the boxed `new` dispatch
   reads each argument as the type the compiler gave the member it fills.
   Answers 1 when some argument's type is another type, or is not known, so
   that class is left to the `[]` the value took before. */
static int aref_arg_mistyped(Compiler *c, int ci, const int *argv, int argc) {
  ClassInfo *k = &c->classes[ci];
  if (comp_method_in_chain(c, ci, "initialize", NULL) >= 0) return 0;
  for (int j = 0; j < argc && j < k->nreaders; j++) {
    char mvn[300]; snprintf(mvn, sizeof mvn, "@%s", k->readers[j]);
    int mvi = comp_ivar_index(k, mvn);
    TyKind pt = (mvi >= 0 && k->ivar_types[mvi] != TY_UNKNOWN) ? k->ivar_types[mvi] : TY_POLY;
    if (pt != TY_POLY && repr_of(c, argv[j]).as_ty != pt) return 1;
  }
  return 0;
}

/* `k[1, 2]` on a boxed receiver that holds a Struct or Data class: `[]` on
   such a class is `new`, and the boxed `[]` reads the receiver as an Array,
   a String or a user object, so a class value raised NoMethodError or
   TypeError, or answered nil. Branch on the value: a class box whose id is
   one of the program's Struct or Data classes takes the boxed `new`
   dispatch through a rename re-entry, and anything else the `[]` it always
   took, through a re-entry that reads the receiver and every argument that
   is not a literal from temps they are evaluated into once. Only a call
   whose value is boxed, with positional arguments that each have a C type,
   and not a `&.` call, takes this path. */
int emit_boxed_class_aref(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = recv >= 0 ? comp_recv_type(c, recv) : TY_VOID;
  const char *cop = nt_str(nt, id, "call_operator");
  if (cop && sp_streq(cop, "&.")) return 0;
  Repr ir = repr_of(c, id);
  if (recv >= 0 && rt == TY_POLY && name && sp_streq(name, "[]") && g_aref_cls_skip != id &&
      nt_ref(nt, id, "block") < 0 && (ir.kind == RK_BOXED || ir.untyped) &&
      g_n_argov + argc + 1 <= MAX_ARG_OVERRIDE) {
    for (int i = 0; i < argc; i++) {
      NodeKind ak = nt_kind(nt, argv[i]);
      if (ak == NK_KeywordHashNode || ak == NK_SplatNode || ak == NK_BlockArgumentNode) return 0;
      if (aref_arg_in_place(nt, argv[i])) continue;
      TyKind at = repr_of(c, argv[i]).as_ty;
      if (at == TY_UNKNOWN || at == TY_VOID || !c_type_name(at)) return 0;
    }
    int ncls = 0, tsv = 0;
    Buf ids; memset(&ids, 0, sizeof ids);
    for (int ci = 0; ci < c->nclasses; ci++) {
      ClassInfo *k = &c->classes[ci];
      if (!k->is_struct || is_builtin_reopen(k->name) || k->is_native_class) continue;
      if (comp_cmethod_in_chain(c, ci, "[]", NULL) >= 0 ||
          comp_cmethod_in_chain(c, ci, "new", NULL) >= 0) continue;
      if (aref_arg_mistyped(c, ci, argv, argc)) continue;
      if (!ncls) tsv = ++g_tmp;
      buf_printf(&ids, "%s_t%d.cls_id == %d", ncls++ ? " || " : "", tsv, ci);
    }
    if (!ncls) { free(ids.p); return 0; }
    /* the receiver, then each argument that is not a literal, once, into
       rooted temps both re-entries below read; an argument whose own parts
       are hoisted ahead of the statement (an Array or a Hash literal) has
       those parts evaluated there, as the `[]` did before */
    buf_printf(b, "({ sp_RbVal _t%d = ", tsv); emit_boxed(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", tsv);
    int nov0 = g_n_argov;
    for (int i = 0; i < argc; i++) {
      if (aref_arg_in_place(nt, argv[i])) continue;
      TyKind at = repr_of(c, argv[i]).as_ty;
      int ta = ++g_tmp;
      emit_ctype(c, at, b);
      buf_printf(b, " _t%d = ", ta); emit_expr(c, argv[i], b); buf_puts(b, "; ");
      if (ty_gc_rootable(c, at)) { emit_gc_root_tmp(c, at, ta, b); buf_puts(b, " "); }
      view_bind(argv[i], "_t%d", ta);
    }
    buf_printf(b, "(_t%d.tag == SP_TAG_CLASS && (%s)) ? ", tsv, ids.p);
    view_bind(recv, "_t%d", tsv);
    /* what either re-entry hoists (the arity guard of a builtin `[]`) reads
       the temps bound above, which live only inside this expression: it runs
       in its own arm, not ahead of the statement */
    Buf *sv_pre = g_pre;
    Buf npre; memset(&npre, 0, sizeof npre);
    g_pre = &npre;
    nt_node_set_str((NodeTable *)nt, id, "name", "new");
    Buf nb; memset(&nb, 0, sizeof nb); emit_call(c, id, &nb);
    nt_node_set_str((NodeTable *)nt, id, "name", "[]");
    int sv_skip = g_aref_cls_skip;
    g_aref_cls_skip = id;
    Buf apre; memset(&apre, 0, sizeof apre);
    g_pre = &apre;
    Buf ab; memset(&ab, 0, sizeof ab); emit_call(c, id, &ab);
    g_pre = sv_pre;
    g_aref_cls_skip = sv_skip;
    view_unbind(nov0);
    if (npre.p) buf_printf(b, "({\n%s(%s); })", npre.p, nb.p ? nb.p : "sp_box_nil()");
    else buf_printf(b, "(%s)", nb.p ? nb.p : "sp_box_nil()");
    if (apre.p) buf_printf(b, " : ({\n%s(%s); }); })", apre.p, ab.p ? ab.p : "sp_box_nil()");
    else buf_printf(b, " : (%s); })", ab.p ? ab.p : "sp_box_nil()");
    free(npre.p); free(apre.p); free(nb.p); free(ab.p); free(ids.p);
    return 1;
  }
  return 0;
}

/* A zero-argument call on a boxed receiver (emit_poly_call's arms, in their order) */
static int emit_poly_call0_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, int *out) {
  if (!(recv >= 0 && rt == TY_POLY && argc == 0)) return 0;
  /* Skip when a user class defines nil? so its method wins the dispatch --
     the same reason the to_a arm below gives. A Null Object answering true
     was folded to the tag test and its guard silently never fired. */
  if (sp_streq(name, "nil?") && !user_defines_or_reads(c, name)) {
    buf_puts(b, "sp_poly_nil_p("); emit_expr(c, recv, b); buf_puts(b, ")"); { *out = 1; return 1; }
  }
  /* Symbol#id2name: a Symbol's name, any other value's NoMethodError */
  if (sp_streq(name, "id2name") && !user_defines_or_reads(c, name) && comp_ntype(c, id) == TY_STRING) {
    buf_puts(b, "sp_poly_sym_id2name("); emit_expr(c, recv, b); buf_puts(b, ")"); { *out = 1; return 1; }
  }
  /* to_a on a runtime-tagged value: nil -> [], array -> itself, hash -> its
     pairs, anything else CRuby's NoMethodError. Skip when a user class
     defines to_a so its method wins the dispatch. */
  /* to_a / deconstruct on a poly value: for a Struct/Data both are the
     member values in order (sp_poly_to_a_arr derives them from the to_h
     hook); for an array/hash it is the elements/pairs. */
  if ((sp_streq(name, "to_a") || sp_streq(name, "deconstruct")) && argc == 0 &&
      nt_ref(nt, id, "block") < 0) {
    if (!poly_name_user_claimed(c, name, argc)) {
      /* to_a itself also answers a Time's fields (sp_poly_to_a_call) */
      buf_puts(b, sp_streq(name, "to_a") ? "sp_poly_to_a_call(" : "sp_poly_to_a_arr(");
      emit_expr(c, recv, b); buf_puts(b, ")");
      { *out = 1; return 1; }
    }
  }
  /* entries on a poly value the inference typed an Array: the elements,
     as a new Array, and nil's NoMethodError (sp_poly_entries). A user
     class with a method or a reader of the name wins the dispatch. */
  if (sp_streq(name, "entries") && nt_ref(nt, id, "block") < 0 &&
      repr_of(c, id).elem == TY_POLY) {
    if (!poly_name_user_claimed(c, name, argc)) {
      buf_puts(b, "sp_poly_entries("); emit_expr(c, recv, b); buf_puts(b, ")");
      { *out = 1; return 1; }
    }
  }
  /* Struct#members on a Struct/Data read out of a container. A class with
     a reader of the name (an attr_reader, a member named `members`) takes
     the dispatch instead, whose default still answers a Struct's names;
     answered here, `room.members` read out of a Hash was the names. */
  if (sp_streq(name, "members") && argc == 0 && nt_ref(nt, id, "block") < 0) {
    if (!poly_name_user_claimed(c, "members", argc)) {
      /* a Class read out of the slot answers the class-side members list
         through the generated sp_cls_members, when the program has it;
         anything else answers through the instance helper */
      if (g_gen_cls_answers) {
        int tm = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", tm);
        emit_expr(c, recv, b);
        buf_printf(b, "; _t%d.tag == SP_TAG_CLASS ? sp_cls_members(_t%d)"
                      " : sp_poly_struct_members(_t%d); })", tm, tm, tm);
        { *out = 1; return 1; }
      }
      buf_puts(b, "sp_poly_struct_members("); emit_expr(c, recv, b); buf_puts(b, ")");
      { *out = 1; return 1; }
    }
  }
  /* Hash#keys / #values on a poly value (e.g. an evidence-free empty `{}` that
     stayed poly). Skip when a user class defines keys/values so its method wins. */
  if (sp_streq(name, "keys") || sp_streq(name, "values")) {
    if (!poly_name_user_claimed(c, name, argc)) {
      buf_printf(b, "sp_poly_%s(", name); emit_expr(c, recv, b); buf_puts(b, ")"); { *out = 1; return 1; }
    }
  }
  if (sp_streq(name, "count")) {
    /* count / count(v) / count { |x| } on a boxed array (skip when any
       user class defines count -- same rule as length below) */
    int has_user_cnt = poly_name_user_claimed(c, "count", argc);
    int cblk = nt_ref(nt, id, "block");
    if (!has_user_cnt && argc == 0 && cblk >= 0) {
      int cbody = nt_ref(nt, cblk, "body");
      int cbn = 0; const int *cbb = cbody >= 0 ? nt_arr(nt, cbody, "body", &cbn) : NULL;
      const char *cp0 = block_param_name(c, cblk, 0);
      const char *cp0r = cp0 ? rename_local(cp0) : NULL;
      if (cbn >= 1) {
        int tr = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
        tr = hold_operand_pre(c, recv, TY_POLY, 0, tr, 1);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_int _t%d = 0;\n", tc);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_poly_length(_t%d); _t%d++) {\n", ti, ti, tr, ti);
        {
          /* sp_poly_each_elem, not a raw index: a boxed Hash renders each
             entry as its [key, value] pair, which a two-parameter block
             autosplats the way every sibling element loop does (#3448). */
          char csrc[64]; snprintf(csrc, sizeof csrc, "sp_poly_each_elem(_t%d, _t%d)", tr, ti);
          if (!emit_iter_autosplat(c, cblk, TY_POLY_ARRAY, csrc, g_indent + 1) && cp0r) {
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "lv_%s = %s;\n", cp0r, csrc);
          }
        }
        int svind = g_indent; g_indent++;
        for (int j = 0; j < cbn - 1; j++) emit_stmt(c, cbb[j], g_pre, g_indent);
        /* Render the condition into its own buffer first: anything it has to
           hoist (a rooted argument temp) is a STATEMENT, and appending it to
           g_pre after "if (" was written put the declaration in the middle of
           the expression. */
        { Buf ccv; memset(&ccv, 0, sizeof ccv);
          emit_boxed(c, cbb[cbn - 1], &ccv);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "if (sp_poly_truthy(%s)) _t%d++;\n",
                     ccv.p ? ccv.p : "sp_box_nil()", tc);
          free(ccv.p); }
        g_indent = svind;
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        buf_printf(b, "_t%d", tc);
        { *out = 1; return 1; }
      }
    }
    if (!has_user_cnt && argc == 0 && cblk < 0) {
      /* never nil, lifted where the slot holds its nil (a dispatch whose
         reader arm can answer nil) */
      int lo = node_is_oint(c, id);
      buf_puts(b, lo ? "sp_oint_of(sp_poly_count(" : "sp_poly_count("); emit_expr(c, recv, b);
      buf_puts(b, lo ? "))" : ")");
      { *out = 1; return 1; }
    }
  }
  if (sp_streq(name, "length") || sp_streq(name, "size") || sp_streq(name, "empty?")) {
    /* has_user_len must also consult comp_reader_in_chain: a user class's
       `.size`/`.length` is very often an attr_reader/attr_accessor -- or a
       Struct member, which registers the same way -- rather than a `def`
       method. comp_method_in_chain alone missed those, so this branch took
       the built-in-only sp_poly_length() path and silently returned 0 for
       any object whose class exposes the name only as a reader (e.g. a
       `Struct.new(:offset, :size, :name)` entry answering `.size`). */
    /* The question is about the name being CALLED. Asking about `length`
       for an `empty?` call sent every program that defines `length`
       anywhere down the dispatch path, where nothing answers `empty?` --
       so the call became an unconditional raise whatever the receiver was
       (#3805). Defining `length` does not define `empty?` in Ruby either. */
    int has_user_len = poly_name_user_claimed(c, name, argc);
    /* A Class value's own method takes the general dispatch too. */
    if (!has_user_len && !g_poly_builtin_arm) has_user_len = cplan_boxed_cmethod(c, id, name);
    if (!has_user_len) {
      if (sp_streq(name, "empty?")) {
        /* A user object has no #empty? of its own here, and sp_poly_length
           answers 0 for one, which would make every such object empty.
           Raise instead, as Ruby does. */
        buf_puts(b, "({ sp_RbVal _ep = "); emit_boxed(c, recv, b);
        /* nil, a number and a boolean have none either, and read as empty
           through the same 0 (#4485) */
        buf_puts(b, "; sp_poly_coll_chk(_ep, \"empty?\");"
                    " sp_poly_is_user_obj(_ep) ? (sp_raise_poly_nomethod(\"empty?\", _ep), 0)"
                    " : (sp_poly_length(_ep) == 0); })");
      }
      else if (sp_streq(name, "size")) {
        /* Integer#size is the byte width of the machine representation, not
           a length; sp_poly_length has no arm for it and answered 0. */
        oint_open(c, id, TY_INT, b); buf_puts(b, "sp_poly_size("); emit_boxed(c, recv, b); buf_puts(b, ")"); oint_close(c, id, b);
      }
      else {
        /* nil / a number / a user object has no #length: answering 0 turned a
           NoMethodError into a silent zero (#3974) */
        buf_puts(b, "sp_poly_length_m("); emit_boxed(c, recv, b); buf_puts(b, ")");
      }
      { *out = 1; return 1; }
    }
  }
  if (is_text_conversion(name)) {
    int has_user_method = 0;
    for (int k = 0; k < c->nclasses; k++)
      if (comp_poly_arm_defines_n(c, k, name, argc)) { has_user_method = 1; break; }
    if (!has_user_method) {
      buf_printf(b, "%s(", sp_streq(name, "to_s") ? "sp_poly_to_s" : "sp_poly_inspect");
      emit_expr(c, recv, b); buf_puts(b, ")"); { *out = 1; return 1; }
    }
  }
  /* Same guard as #to_s above: a user class defining the conversion wins
     through poly dispatch. sp_poly_to_i answers 0 for an object, so a
     wrapper's `value.to_i` silently read zero. */
  /* `to_int` is the same method by its other name (#2317): a boxed
     Rational answered NoMethodError for it while answering to_i fine. */
  if (sp_streq(name, "to_i") || sp_streq(name, "to_int") || sp_streq(name, "to_f")) {
    if (!poly_name_user_claimed(c, name, argc)) {
      /* sp_poly_to_i_meth / sp_poly_to_f_meth: this is the METHOD, named by
         the program, so an object without it is NoMethodError rather than
         the conversion protocol's TypeError, and nil.to_f is 0.0 */
      buf_printf(b, "%s(", !sp_streq(name, "to_f")
                            ? (repr_of(c, id).kind == RK_BOXED ? "sp_poly_to_i_meth_v" : "sp_poly_to_i_meth")
                            : "sp_poly_to_f_meth");
      emit_expr(c, recv, b); buf_puts(b, ")"); { *out = 1; return 1; }
    }
  }
  /* Complex#real / #imaginary on a poly value (a Complex read out of a
     container). A user class defining the same name wins via poly dispatch. */
  if ((sp_streq(name, "real") || sp_streq(name, "imaginary") || sp_streq(name, "imag") ||
       sp_streq(name, "conjugate") || sp_streq(name, "conj")) && argc == 0) {
    if (!poly_name_user_claimed(c, name, argc)) {
      const char *pfn = sp_streq(name, "real") ? "sp_poly_real"
                      : (sp_streq(name, "imaginary") || sp_streq(name, "imag")) ? "sp_poly_imaginary"
                      : "sp_poly_conjugate";
      buf_printf(b, "%s(", pfn);
      emit_expr(c, recv, b); buf_puts(b, ")"); { *out = 1; return 1; }
    }
  }
  /* Numeric#arg / #angle / #phase, #rect / #rectangular and #polar on a poly value,
     answered as the typed arms answer them. A user method, reader or class
     method of the same name wins via poly dispatch. */
  if ((sp_streq(name, "arg") || sp_streq(name, "angle") || sp_streq(name, "phase") ||
       sp_streq(name, "rect") || sp_streq(name, "rectangular") || sp_streq(name, "polar")) && argc == 0) {
    int has_user = 0;
    if (!g_poly_builtin_arm)
    for (int kk = 0; kk < c->nclasses && !has_user; kk++)
      if (comp_poly_arm_defines_n(c, kk, name, argc) ||
          (!c->classes[kk].is_native_class && comp_reader_in_chain(c, kk, name, NULL)) ||
          comp_cmethod_in_chain(c, kk, name, NULL) >= 0) has_user = 1;
    if (!has_user && sp_streq(name, "polar")) {
      buf_puts(b, "sp_poly_polar("); emit_expr(c, recv, b); buf_puts(b, ")"); { *out = 1; return 1; }
    }
    if (!has_user) {
      buf_printf(b, "%s(", is_rectangular_alias(name) ? "sp_poly_rect" : "sp_poly_arg");
      emit_expr(c, recv, b); buf_printf(b, ", \"%s\")", name); { *out = 1; return 1; }
    }
  }
  /* String#to_sym interns; Symbol#to_sym is identity; every other tag raises
     CRuby's NoMethodError. A user class defining to_sym wins via poly dispatch.
     `intern` answers as `to_sym` does on both, and stands aside, as the
     arm did not exist for it before, wherever the dispatch reads the name
     some other way: a user reader (an attr_reader, a Struct member) or an
     OpenStruct member when ostruct is loaded (#3197). */
  int intern_read = sp_streq(name, "intern") && (sp_feature_required("ostruct") || user_defines_or_reads(c, name));
  if (sp_streq(name, "to_sym") || (sp_streq(name, "intern") && !intern_read)) {
    if (!poly_name_user_claimed(c, name, argc)) {
      int t = ++g_tmp;
      /* The arm yields a raw sp_sym. When the call's own slot is poly (a
         case-result carrier, a boxed argument) it must be boxed HERE -- the
         generic boxed-value emitter passes a poly-typed node through
         untouched, so a raw scalar would land in an sp_RbVal slot (#3331). */
      int box_sym = repr_of(c, id).kind == RK_BOXED;
      if (box_sym) buf_puts(b, "sp_box_sym(");
      /* Root the boxed receiver: sp_sym_intern reads through the String's
         data pointer and allocates, so a GC mid-intern could otherwise free
         an unrooted temporary String out from under it. */
      /* a shared-string handle is a String: deref it into the immediate
         form the arm reads, or to_sym raised for it (#4279) */
      buf_printf(b, "({ sp_RbVal _t%d = sp_poly_strbuf_deref(", t); emit_expr(c, recv, b);
      buf_puts(b, ")");
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); _t%d.tag == SP_TAG_STR ? sp_sym_intern_n(_t%d.v.s, sp_str_byte_len(_t%d.v.s))"
                    " : (_t%d.tag == SP_TAG_SYM ? (sp_sym)_t%d.v.i"
                    " : (sp_raise_poly_nomethod(\"%s\", _t%d), (sp_sym)0)); })",
                 t, t, t, t, t, t, name, t);
      if (box_sym) buf_puts(b, ")");
      { *out = 1; return 1; }
    }
  }
  /* Numeric queries / rounding: dispatch on the runtime tag (a non-numeric
     tag raises CRuby's NoMethodError). A user method or attr reader with
     the same name wins -- the poly method dispatch handles it instead. */
  {
    const char *pfn =
      sp_streq(name, "nan?")      ? "sp_poly_nan_p" :
      sp_streq(name, "next_float") ? "sp_poly_next_float" :
      sp_streq(name, "prev_float") ? "sp_poly_prev_float" :
      sp_streq(name, "finite?")   ? "sp_poly_finite_p" :
      sp_streq(name, "infinite?") ? "sp_poly_infinite" :
      sp_streq(name, "zero?")     ? "sp_poly_zero_p" :
      sp_streq(name, "nonzero?")  ? "sp_poly_nonzero" :
      sp_streq(name, "positive?") ? "sp_poly_positive_p" :
      sp_streq(name, "negative?") ? "sp_poly_negative_p" :
      sp_streq(name, "real?")     ? "sp_poly_real_p" :
      sp_streq(name, "integer?")  ? "sp_poly_integer_p" :
      sp_streq(name, "abs") || sp_streq(name, "magnitude") ? "sp_poly_abs" :
      sp_streq(name, "abs2")      ? "sp_poly_abs2" :
      sp_streq(name, "floor")     ? "sp_poly_floor" :
      sp_streq(name, "ceil")      ? "sp_poly_ceil" :
      sp_streq(name, "round")     ? "sp_poly_round" :
      sp_streq(name, "truncate")  ? "sp_poly_truncate" :
      sp_streq(name, "bytesize")  ? "sp_poly_bytesize" :
      sp_streq(name, "ord")       ? "sp_poly_ord" :
      sp_streq(name, "bit_length") ? "sp_poly_bit_length" :
      sp_streq(name, "numerator")   ? "sp_poly_numerator" :
      sp_streq(name, "denominator") ? "sp_poly_denominator" :
      sp_streq(name, "begin")       ? "sp_poly_range_begin_v" :
      sp_streq(name, "end")         ? "sp_poly_range_end_v" :
      sp_streq(name, "exclude_end?") ? "sp_poly_range_exclude_end_p" : NULL;
    if (pfn) {
      int nf = sp_streq(name, "next_float") || sp_streq(name, "prev_float");
      int has_user = 0, has_cm = 0;
      if (!g_poly_builtin_arm)
      for (int kk = 0; kk < c->nclasses && !has_user; kk++) {
        if (comp_poly_arm_defines_n(c, kk, name, argc) ||
            (!c->classes[kk].is_native_class && comp_reader_in_chain(c, kk, name, NULL))) has_user = 1;
        if (nf && comp_cmethod_in_chain(c, kk, name, NULL) >= 0) has_cm = 1;
      }
      /* A class method of next_float / prev_float is a boxed Class's: the
         class-tag dispatch (#3215) takes that receiver, and its not-a-Class
         arm comes back here (g_cls_tag_skip) for the Float helper. A call
         typed Float takes that dispatch through a poly slot. */
      if (!has_user && has_cm && g_cls_tag_skip != id) {
        TyKind rty = comp_ntype(c, id);
        if (rty != TY_FLOAT) { *out = 0; return 1; }
        Buf pb2; memset(&pb2, 0, sizeof pb2);
        int v = view_push(c, id, TY_POLY);
        emit_expr(c, id, &pb2);
        view_pop(c, v);
        emit_unbox_text(c, TY_FLOAT, pb2.p ? pb2.p : "sp_box_nil()", b);
        free(pb2.p);
        { *out = 1; return 1; }
      }
      if (!has_user) {
        int boxf = nf && repr_of(c, id).kind == RK_BOXED;
        buf_printf(b, "%s%s(", boxf ? "sp_box_float(" : "", pfn); emit_expr(c, recv, b);
        buf_puts(b, boxf ? "))" : ")");
        { *out = 1; return 1; }
      }
    }
  }
  /* These stringify the receiver and apply a String method to the result, so
     a user class owning the name must win: a Struct member, Data field or
     attr_reader called `upcase` otherwise answers the UPCASED #inspect of
     the object holding it (#3380). The `bytes` / `chars` arms below have
     carried this guard since #2909 / #3364; this is the same list of names
     that return a String rather than an array, which is why it was missed.
     Declining falls through to the general poly dispatch, which reads the
     member -- and still serves a genuine String receiver in the same
     program. */
  int str_conv_owned = user_defines_or_reads(c, name);
  if (!str_conv_owned) {
  if ((is_succ_alias(name)) && argc == 0) {
    /* per kind, not per string: an Integer counts up and an Enumerator pulls
       its next value, where the string succ answered "" for both (#3843) */
    buf_puts(b, "sp_poly_succ_m("); emit_expr(c, recv, b);
    buf_printf(b, ", %d)", sp_streq(name, "next") ? 1 : 0);
    { *out = 1; return 1; }
  }
  /* an Enumerator's peek beside its next: a boxed Enumerator (a local the
     program also gives another class) answered NoMethodError for it, where
     next already pulled */
  if (sp_streq(name, "peek") && argc == 0) {
    int tv = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_expr(c, recv, b);
    buf_printf(b, "; if (!(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_ENUMERATOR && _t%d.v.p))"
                  " sp_raise_nomethod(sp_nomethod_msg(\"peek\", _t%d)); "
                  "sp_Enumerator_peek((sp_Enumerator *)_t%d.v.p); })", tv, tv, tv, tv, tv);
    { *out = 1; return 1; }
  }
  if (emit_builtin_op_stage(c, id, recv, rt, name, 1, b)) { *out = 1; return 1; }
  if (sp_streq(name, "strip"))      { buf_puts(b, "sp_box_str(sp_str_strip(sp_poly_recv_s("); emit_expr(c, recv, b); buf_printf(b, ", \"strip\")))"); { *out = 1; return 1; } }
  /* `strip` had an arm and its one-sided siblings did not, which is the
     shape of most of what follows: a String reaching the dispatch through a
     poly slot answered NoMethodError naming String, for a method String
     has. Each of these already works on a concrete receiver and the runtime
     function is the one that arm calls. */
  if (sp_streq(name, "lstrip"))     { buf_puts(b, "sp_box_str(sp_str_lstrip(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"lstrip\")))"); { *out = 1; return 1; } }
  if (sp_streq(name, "rstrip"))     { buf_puts(b, "sp_box_str(sp_str_rstrip(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"rstrip\")))"); { *out = 1; return 1; } }
  /* to_str is the implicit-conversion protocol, so a poly slot holding a
     String has to answer it: sp_poly_recv_s raises for anything else, which
     is what a non-String must do here. */
  if (sp_streq(name, "to_str") && argc == 0) {
    /* --share-strings: a String's to_str is the String itself, so a box
       holding one answers that box, as to_s does (sp_poly_to_s_box). */
    if (repr_share_rule(c)) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; const char *_s%d = sp_poly_recv_s(_t%d, \"to_str\"); "
                    "(_t%d.tag == SP_TAG_STR && _t%d.v.s) || sp_poly_is_strbuf(_t%d) ? _t%d : sp_box_str(_s%d); })",
                 t, t, t, t, t, t, t);
      { *out = 1; return 1; }
    }
    buf_puts(b, "sp_box_str(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"to_str\"))"); { *out = 1; return 1; }
  }
  if (sp_streq(name, "ascii_only?") && argc == 0) {
    buf_puts(b, "sp_box_bool(sp_str_ascii_only(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"ascii_only?\")))"); { *out = 1; return 1; }
  }
  /* the boxed Encoding a String hands out (cgi's escapeHTML guard) */
  if ((sp_streq(name, "ascii_compatible?") || sp_streq(name, "dummy?")) && argc == 0) {
    buf_printf(b, "sp_box_bool(sp_poly_enc_pred("); emit_expr(c, recv, b); buf_printf(b, ", \"%s\"))", name); { *out = 1; return 1; }
  }
  if (sp_streq(name, "valid_encoding?") && argc == 0) {
    buf_puts(b, "sp_box_bool(sp_str_valid_encoding(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"valid_encoding?\")))"); { *out = 1; return 1; }
  }
  /* encode is a no-op on the concrete arm -- every string here is UTF-8 --
     so the poly one only has to unbox and re-box, and raise for a
     non-String the way the others do. */
  if (sp_streq(name, "encode") && argc == 0) {
    buf_puts(b, "sp_box_str(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"encode\"))"); { *out = 1; return 1; }
  }
  if (sp_streq(name, "b") && argc == 0) {   /* a binary copy, as the String arm answers (#4441) */
    buf_puts(b, "sp_box_str(sp_str_b(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"b\")))"); { *out = 1; return 1; }
  }
  if (sp_streq(name, "scrub") && argc == 0) {
    buf_puts(b, "sp_box_str(sp_str_scrub(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"scrub\"), 0))"); { *out = 1; return 1; }
  }
  if (sp_streq(name, "dump") && argc == 0) {   /* the quoted form, as the String arm answers */
    buf_puts(b, "sp_box_str(sp_str_dump(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"dump\")))"); { *out = 1; return 1; }
  }
  if (sp_streq(name, "reverse"))    { buf_puts(b, "sp_poly_reverse("); emit_expr(c, recv, b); buf_puts(b, ")"); { *out = 1; return 1; } }
  /* `encoding` on a boxed String: the concrete arm has answered it since
     #723, and the poly dispatch had no entry -- so a String read out of a
     poly array raised NoMethodError naming its own class. A Symbol answers
     too (sp_poly_encoding). */
  if (sp_streq(name, "encoding") && argc == 0) {
    buf_puts(b, "sp_poly_encoding("); emit_expr(c, recv, b); buf_puts(b, ")");
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "chomp"))      { buf_puts(b, "sp_box_str(sp_str_chomp(sp_poly_recv_s("); emit_expr(c, recv, b); buf_printf(b, ", \"chomp\")))"); { *out = 1; return 1; } }
  if (sp_streq(name, "chop"))       { buf_puts(b, "sp_box_str(sp_str_chop(sp_poly_recv_s("); emit_expr(c, recv, b); buf_printf(b, ", \"chop\")))"); { *out = 1; return 1; } }
  /* The one-String-argument transforms, which the table above covers only for
     the zero-argument shapes. A String arriving through a poly slot -- a
     Fiber#resume value, a container read -- had no arm for these and raised
     NoMethodError naming String, which is what it was (#3436). */
  if ((sp_streq(name, "delete_prefix") || sp_streq(name, "delete_suffix")) && argc == 1) {
    buf_printf(b, "sp_box_str(sp_str_%s(sp_poly_recv_s(", name); emit_expr(c, recv, b);
    buf_printf(b, ", \"%s\"), ", name); emit_str_expr(c, argv[0], b); buf_puts(b, "))");
    { *out = 1; return 1; }
  }
  }
  /* no argument only: chr(Encoding::X) is resolved by emit_unresolved_call,
     and a byte chr here would drop the encoding */
  if (sp_streq(name, "chr") && argc == 0 && !str_conv_owned) {
    /* dispatch on the runtime tag: (48 + n).chr through a widened int
       must be Integer#chr -- stringifying first turned 61.chr into
       "61".chr == "6", corrupting percent-encoding digits (#3328) */
    int tvC = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = ", tvC); emit_boxed(c, recv, b);
    /* a String (or its shared handle) takes String#chr; nil and every
       other value have no chr (NoMethodError, args []), where read as a
       String nil answered "" and a Symbol or a Float its first character;
       a Bignum is past any character */
    buf_printf(b, "; _t%d.tag == SP_TAG_INT ? sp_box_str(sp_int_chr(_t%d.v.i))"
                  " : (_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)) ? sp_box_str(sp_str_chr(sp_poly_to_s(_t%d)))"
                  " : (_t%d.tag == SP_TAG_BIGINT ? sp_raise_cls(\"RangeError\", \"bignum out of char range\")"
                  " : sp_raise_nomethod(sp_nomethod_msg_args(\"chr\", _t%d, 0, NULL)), sp_box_nil()); })",
               tvC, tvC, tvC, tvC, tvC, tvC, tvC);
    { *out = 1; return 1; }
  }
  /* poly.bytes / poly.codepoints -> concrete TY_INT_ARRAY, no boxing (matches
     the inference rule). A String that widened to poly (a binary lump slice)
     reaches here; without this arm .bytes hit the generic poly method
     dispatch and raised "undefined method 'bytes' for poly". */
  if ((sp_streq(name, "bytes") || sp_streq(name, "codepoints")) && argc == 0 &&
      nt_ref(nt, id, "block") < 0) {
    /* Skip when a user class owns the name -- the runtime value may be one of
       those, and stringifying it would answer the bytes of its #inspect. A
       Struct member or attr_reader called `bytes` hit exactly that (#3364);
       the `chars` arm below has carried this guard since #2909. */
    if (!user_defines_or_reads(c, name)) {
      buf_printf(b, "sp_str_%s(sp_poly_recv_s(", sp_streq(name, "bytes") ? "bytes" : "codepoints");
      emit_expr(c, recv, b); buf_printf(b, ", \"%s\"))", name); { *out = 1; return 1; }
    }
  }
  /* poly.chars -> TY_STR_ARRAY: a String read out of a container or
     destructured from a pair (`|a, b|`) reaches here poly-typed (#2909). */
  if (sp_streq(name, "chars") && argc == 0 && nt_ref(nt, id, "block") < 0) {
    if (!user_defines_or_reads(c, "chars")) {
      buf_puts(b, "sp_str_chars(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"chars\"))"); { *out = 1; return 1; }
    }
  }
  /* poly.each_char { }: walk the same char array #chars answers. A String
     receiver has its own emitter that does not materialize one; a poly
     receiver only learns it is a String at run time, so it pays the array
     and yields out of it. Answers the receiver's string, as String#each_char
     answers self (#3402). each_line { } walks the array #lines answers the
     same way. */
  if ((sp_streq(name, "each_char") || sp_streq(name, "each_line")) && argc == 0 &&
      nt_ref(nt, id, "block") >= 0 && !user_defines_or_reads(c, name) &&
      !user_defines_or_reads(c, sp_streq(name, "each_char") ? "chars" : "lines")) {
    int eblk = nt_ref(nt, id, "block");
    const char *ebp = block_param_name(c, eblk, 0);
    const char *ebpn = ebp ? rename_local(ebp) : NULL;
    int ebody = nt_ref(nt, eblk, "body");
    int ebn = 0; const int *ebb = ebody >= 0 ? nt_arr(nt, ebody, "body", &ebn) : NULL;
    int ts = ++g_tmp, ta = ++g_tmp, ti = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = sp_poly_recv_s(", ts); emit_expr(c, recv, b);
    buf_printf(b, ", \"%s\"); SP_GC_ROOT(_t%d);", name, ts);
    buf_printf(b, " sp_StrArray *_t%d = %s(_t%d); SP_GC_ROOT(_t%d);",
               ta, sp_streq(name, "each_char") ? "sp_str_chars" : "sp_str_lines", ts, ta);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++) {", ti, ti, ta, ti);
    if (ebpn) emit_str_elem_param(c, eblk, ebp, ebpn, ta, ti, b);
    for (int k2 = 0; k2 < ebn; k2++) emit_stmt(c, ebb[k2], b, 0);
    buf_printf(b, " } _t%d; })", ts);
    { *out = 1; return 1; }
  }
  /* poly.each_byte { } / .each_codepoint { }: the same shape as each_char
     above, over the integer array #bytes / #codepoints answers. */
  if ((is_byte_codepoint_each(name)) && argc == 0 &&
      nt_ref(nt, id, "block") >= 0 && !user_defines_or_reads(c, name)) {
    int eblk = nt_ref(nt, id, "block");
    const char *ebp = block_param_name(c, eblk, 0);
    const char *ebpn = ebp ? rename_local(ebp) : NULL;
    int ebody = nt_ref(nt, eblk, "body");
    int ebn = 0; const int *ebb = ebody >= 0 ? nt_arr(nt, ebody, "body", &ebn) : NULL;
    const char *fn = sp_streq(name, "each_byte") ? "sp_str_bytes" : "sp_str_codepoints";
    int ts = ++g_tmp, ta = ++g_tmp, ti = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = sp_poly_recv_s(", ts); emit_expr(c, recv, b);
    buf_printf(b, ", \"%s\"); SP_GC_ROOT(_t%d);", name, ts);
    buf_printf(b, " sp_IntArray *_t%d = %s(_t%d); SP_GC_ROOT(_t%d);", ta, fn, ts, ta);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++) {", ti, ti, ta, ti);
    if (ebpn) {
      Scope *ebs = comp_scope_of(c, eblk);
      LocalVar *eblv = ebs ? scope_local(ebs, ebp) : NULL;
      if (eblv && eblv->type == TY_POLY)
        buf_printf(b, " sp_RbVal lv_%s = sp_box_int(sp_IntArray_get(_t%d, _t%d));", ebpn, ta, ti);
      else
        buf_printf(b, " sp_int lv_%s = sp_IntArray_get(_t%d, _t%d);", ebpn, ta, ti);
    }
    for (int k2 = 0; k2 < ebn; k2++) emit_stmt(c, ebb[k2], b, 0);
    buf_printf(b, " } _t%d; })", ts);
    { *out = 1; return 1; }
  }
  /* poly.lines -> TY_STR_ARRAY, the same shape as #chars above (#3403) */
  if (sp_streq(name, "lines") && argc == 0 && nt_ref(nt, id, "block") < 0) {
    if (!user_defines_or_reads(c, "lines")) {
      buf_puts(b, "sp_str_lines(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"lines\"))"); { *out = 1; return 1; }
    }
  }
  /* A blockless each_char / each_line / each_byte / each_codepoint is
     CRuby's Enumerator; materialize it into the array chars / lines / bytes
     answer, which is what the typed String path does too. */
  if (argc == 0 && nt_ref(nt, id, "block") < 0 && !user_defines_or_reads(c, name) &&
      is_str_each_iter(name)) {
    const char *fn = sp_streq(name, "each_char") ? "sp_str_chars"
                   : sp_streq(name, "each_line") ? "sp_str_lines"
                   : sp_streq(name, "each_byte") ? "sp_str_bytes" : "sp_str_codepoints";
    buf_printf(b, "%s(sp_poly_recv_s(", fn); emit_expr(c, recv, b); buf_printf(b, ", \"%s\"))", name);
    { *out = 1; return 1; }
  }
  if (sp_streq(name, "freeze"))     { buf_puts(b, "sp_poly_freeze("); emit_expr(c, recv, b); buf_puts(b, ")"); { *out = 1; return 1; } }
  /* the receiver of a boxed to_h { } rewritten onto map (analyze.c) */
  if (sp_streq(name, "__to_h_subject")) { buf_puts(b, "sp_poly_to_h_subject("); emit_boxed(c, recv, b); buf_puts(b, ")"); { *out = 1; return 1; } }
  return 0;
}

/* Element access on a boxed receiver: an index read, []= and [] with one or two arguments (emit_poly_call's arms, in their order) */
static int emit_poly_index_call(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, int *out) {
  /* poly receiver: arr[start, len] = src -- 3-arg splice assign
     Skip Fiber/Fiber.current storage receivers (handled later). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "[]=") && argc == 3 &&
      !sp_is_fiber_storage_recv(nt, recv)) {
    int tv = ++g_tmp;
    const char *rcvty = nt_type(nt, recv);
    int recv_is_lvalue = rcvty && (sp_streq(rcvty, "LocalVariableReadNode") ||
                                   sp_streq(rcvty, "InstanceVariableReadNode"));
    int outer, oidx;
    TyKind rty2 = comp_ntype(c, argv[2]);
    int tam = splice_to_ary_mi(c, rty2);
    buf_puts(b, "({ ");
    if (tam >= 0) {
      /* object RHS with to_ary: splice the coercion; the OBJECT is the value */
      Buf call; memset(&call, 0, sizeof call);
      TyKind cty = emit_splice_to_ary_src(c, argv[2], rty2, tam, tv, b, &call);
      buf_printf(b, "sp_RbVal _t%d = ", tv);
      emit_boxed_text(c, cty, call.p ? call.p : "", b);
      buf_puts(b, "; ");
      free(call.p);
    }
    else { buf_printf(b, "sp_RbVal _t%d = ", tv); emit_boxed(c, argv[2], b); buf_puts(b, "; "); }
    /* the value is held across the index operands, which may allocate */
    buf_printf(b, "SP_GC_ROOT_RBVAL(_t%d); ", tv);
    /* Store the possibly-promoted array back into the receiver so a typed->poly
       promotion survives: assign to a local/ivar lvalue, or write to outer's slot
       for a computed `outer[idx]` receiver; otherwise splice in place. */
    if (recv_is_lvalue) {
      emit_expr(c, recv, b); buf_puts(b, " = sp_poly_splice("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b);
    }
    else if (splice_recv_index_slot(c, recv, &outer, &oidx)) {
      buf_puts(b, "sp_poly_slot_splice("); emit_boxed(c, outer, b); buf_puts(b, ", "); emit_int_expr(c, oidx, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b);
    }
    else {
      buf_puts(b, "sp_poly_splice("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b);
    }
    if (tam >= 0)
      buf_printf(b, ", _t%d); sp_box_obj(_tq%d, %d); })", tv, tv, ty_object_class(rty2));
    else
      buf_printf(b, ", _t%d); _t%d; })", tv, tv);
    { *out = 1; return 1; }
  }
  /* `x = v` through a SYNTHESIZED writer (attr_writer / accessor, a Struct
     member) on a poly receiver, in value position: the statement form's
     cls_id switch over the writer arms, yielding the assigned value the way
     `[]=` below does. A name some class defines as a method instead takes
     the user-method dispatch, which yields the value itself. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && name_is_plain_setter(name) &&
      nt_ref(nt, id, "block") < 0 && !recv_user_defines(c, name)) {
    char base[256];
    int ncand = 0;
    if (setter_base_name(name, base, sizeof base))
      for (int k = 0; k < c->nclasses; k++)
        if (comp_is_writer(&c->classes[k], base)) ncand++;
    if (ncand > 0) {
      TyKind at = repr_of(c, argv[0]).as_ty;
      int nil_rhs = (at == TY_NIL || at == TY_VOID);
      TyKind at_eff = nil_rhs ? TY_POLY : at;
      int tv = ++g_tmp, tval = ++g_tmp;
      buf_puts(b, "({ "); tv = hold_operand(c, recv, TY_POLY, 0, tv, 1, " ", b);
      if (nil_rhs) buf_printf(b, "sp_RbVal _t%d = sp_box_nil();", tval);
      else { emit_ctype(c, at, b); buf_printf(b, " _t%d = ", tval); emit_one_arg(c, argv[0], 0, b); buf_puts(b, ";"); }
      buf_printf(b, " switch (_t%d.tag == SP_TAG_OBJ ? _t%d.cls_id : 0x7fffffff) {", tv, tv);
      char src[32]; snprintf(src, sizeof src, "_t%d", tval);
      char objp[32]; snprintf(objp, sizeof objp, "_t%d.v.p", tv);
      emit_boxed_writer_arms(c, base, name, objp, src, at_eff, argv[0], b);
      buf_printf(b, " default: sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d)); break;", name, tv);
      /* the call answers the value it stored; a shared handle stored where
         the call is read as a String answers the String it holds */
      if (at == TY_STRBUF && comp_ntype(c, id) != TY_STRBUF)
        buf_printf(b, " } (_t%d ? sp_String_cstr(_t%d) : NULL); })", tval, tval);
      else buf_printf(b, " } _t%d; })", tval);
      { *out = 1; return 1; }
    }
  }
  /* poly receiver: []= with symbol, string, int, or poly key -> runtime dispatch
     Skip Fiber/Fiber.current storage receivers (handled later). */
  /* A user class that takes `[]=` with two arguments owns the name: the call
     goes to the class dispatch, whose builtin arm re-enters here for a real
     Array or Hash. A Struct's builtin writer needs its class arm too.
     Taking it here stored into a boxed user object as if it
     were a hash and never ran the class's method (#4879). */
  int user_aset = 0;
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "[]=") && argc == 2 && !g_poly_builtin_arm)
    for (int kk = 0; kk < c->nclasses && !user_aset; kk++)
      if (comp_poly_arm_defines_n(c, kk, "[]=", 2) || cplan_struct_aset(c, kk, name, argc)) user_aset = 1;
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "[]=") && argc == 2 && !user_aset &&
      !sp_is_fiber_storage_recv(nt, recv)) {
    if (poly_aset_rebound(c, recv, argv)) {
      emit_poly_aset_rebound(c, recv, argv, b);
      { *out = 1; return 1; }
    }
    /* arr[range] = rhs on a poly receiver: a splice over the range's span. */
    if (comp_ntype(c, argv[0]) == TY_RANGE) {
      int tv = ++g_tmp;
      const char *rcvty = nt_type(nt, recv);
      int recv_is_lvalue = rcvty && (sp_streq(rcvty, "LocalVariableReadNode") ||
                                     sp_streq(rcvty, "InstanceVariableReadNode"));
      int outer, oidx;
      TyKind rty1 = comp_ntype(c, argv[1]);
      int tam = splice_to_ary_mi(c, rty1);
      buf_puts(b, "({ ");
      if (tam >= 0) {
        /* object RHS with to_ary: splice the coercion; the OBJECT is the value */
        Buf call; memset(&call, 0, sizeof call);
        TyKind cty = emit_splice_to_ary_src(c, argv[1], rty1, tam, tv, b, &call);
        buf_printf(b, "sp_RbVal _t%d = ", tv);
        emit_boxed_text(c, cty, call.p ? call.p : "", b);
        buf_puts(b, "; ");
        free(call.p);
      }
      else { buf_printf(b, "sp_RbVal _t%d = ", tv); emit_boxed(c, argv[1], b); buf_puts(b, "; "); }
      buf_printf(b, "SP_GC_ROOT_RBVAL(_t%d); ", tv);
      if (recv_is_lvalue) {
        emit_expr(c, recv, b); buf_puts(b, " = sp_poly_splice_range("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b);
      }
      else if (splice_recv_index_slot(c, recv, &outer, &oidx)) {
        buf_puts(b, "sp_poly_slot_splice_range("); emit_boxed(c, outer, b); buf_puts(b, ", "); emit_int_expr(c, oidx, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b);
      }
      else {
        buf_puts(b, "sp_poly_splice_range("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b);
      }
      if (tam >= 0)
        buf_printf(b, ", _t%d); sp_box_obj(_tq%d, %d); })", tv, tv, ty_object_class(rty1));
      else
        buf_printf(b, ", _t%d); _t%d; })", tv, tv);
      { *out = 1; return 1; }
    }
    TyKind at = comp_ntype(c, argv[0]);
    int tv = ++g_tmp;
    /* The value is boxed before the key runs, and a computed key allocates
       (`x[:"m#{j}"] = v` interns its Symbol): rooted, or a collection there
       reclaimed the value and the entry held garbage. An Integer index with
       no effect allocates nothing, and stays a plain temp. */
    buf_puts(b, "({ ");
    tv = hold_operand(c, argv[1], TY_POLY, 1, tv, at != TY_INT || subtree_has_side_effect(c, argv[0]), " ", b);
    int skey_outer, skey_oidx;
    NodeKind srk = nt_kind(nt, recv);
    /* a boxed key may be any of String#[]='s at run time (Integer, Range,
       String, Regexp): the String arm takes it as a Regexp's is taken */
    int pkey = at == TY_POLY && !splice_recv_index_slot(c, recv, &skey_outer, &skey_oidx);
    if ((at == TY_STRING || pkey || (at == TY_REGEX && !splice_recv_index_slot(c, recv, &skey_outer, &skey_oidx))) &&
        (srk == NK_LocalVariableReadNode || srk == NK_InstanceVariableReadNode)) {
      /* a String receiver replaces the key's first match: a plain one into
         a fresh buffer, stored back as the Integer index's widen_and_set
         is, a shared one in place; any other receiver stores as before.
         The key runs once, into a temp both stores read. */
      int tk = ++g_tmp;
      if (at == TY_STRING) { buf_printf(b, "const char *_t%d = ", tk); emit_expr(c, argv[0], b); }
      else { buf_printf(b, "sp_RbVal _t%d = ", tk); emit_boxed(c, argv[0], b); }
      buf_puts(b, "; if ("); emit_expr(c, recv, b); buf_puts(b, ".tag == SP_TAG_STR || sp_poly_is_strbuf(");
      emit_expr(c, recv, b); buf_puts(b, ")) ");
      emit_expr(c, recv, b);
      buf_puts(b, " = sp_poly_str_aset_key("); emit_expr(c, recv, b);
      buf_printf(b, at == TY_STRING ? ", sp_box_str(_t%d), _t%d);\nelse " : ", _t%d, _t%d);\nelse ", tk, tv);
      /* a boxed key that is an Integer at run time widens a typed Array
         as the Integer index does (sp_poly_aset_back), stored back */
      if (at == TY_STRING) buf_puts(b, "sp_poly_set_str(");
      else { emit_expr(c, recv, b); buf_puts(b, " = sp_poly_aset_back("); }
      emit_expr(c, recv, b);
      buf_printf(b, ", _t%d", tk);
    }
    else if (at == TY_STRING || pkey || (at == TY_REGEX && !splice_recv_index_slot(c, recv, &skey_outer, &skey_oidx))) {
      /* a shared String changes in place; anything else stores as before */
      buf_printf(b, "sp_poly_str_aset_key("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b);
    }
    else if (at == TY_SYMBOL) {
      buf_printf(b, "sp_poly_set_sym("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_expr(c, argv[0], b);
    }
    else if (at == TY_INT) {
      /* a key that can be nil (poly_int_key_boxed): a nil one goes to the
         runtime boxed, any other is the Integer index below, read through
         the held key -- the store back to the receiver's slot is kept */
      int nkey = poly_int_key_boxed(c, argv[0]), kmark = -1;
      if (nkey) {
        int tk = ++g_tmp;
        buf_printf(b, "sp_oint _t%d = ", tk); emit_oint_expr(c, argv[0], TY_INT, b);
        buf_printf(b, "; if (SP_UNLIKELY(_t%d.nil)) (void)sp_poly_set_poly(", tk); emit_expr(c, recv, b);
        buf_printf(b, ", sp_box_nil(), _t%d);\nelse ", tv);
        kmark = view_bind(argv[0], "_t%d.v", tk);
      }
      /* widen_and_set returns a *different* boxed value when a typed array is
         promoted to a PolyArray (element-kind mismatch); otherwise it mutates in
         place. Store the result back so promotion survives: assign to a
         local/ivar lvalue, or write to outer's slot for a computed `outer[idx]`
         receiver; a receiver we cannot address falls back to in-place mutation. */
      const char *rcvty = nt_type(nt, recv);
      int recv_is_lvalue = rcvty && (sp_streq(rcvty, "LocalVariableReadNode") ||
                                     sp_streq(rcvty, "InstanceVariableReadNode"));
      int outer, oidx;
      if (recv_is_lvalue) {
        emit_expr(c, recv, b);
        buf_puts(b, " = sp_poly_arr_widen_and_set("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_int_expr(c, argv[0], b);
      }
      else if (splice_recv_index_slot(c, recv, &outer, &oidx)) {
        buf_puts(b, "sp_poly_slot_set("); emit_boxed(c, outer, b); buf_puts(b, ", "); emit_int_expr(c, oidx, b);
        buf_puts(b, ", "); emit_int_expr(c, argv[0], b);
      }
      else {
        buf_puts(b, "sp_poly_arr_widen_and_set("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_int_expr(c, argv[0], b);
      }
      if (kmark >= 0) view_unbind(kmark);
    }
    else {
      /* A computed `outer[idx]` receiver needs the store-back form even for a
         boxed key: a String inner splices into a fresh buffer, and writing to
         the inner value alone dropped the assignment (#4067). */
      int outer_k, oidx_k;
      if (splice_recv_index_slot(c, recv, &outer_k, &oidx_k)) {
        buf_puts(b, "sp_poly_slot_set_key("); emit_boxed(c, outer_k, b);
        buf_puts(b, ", "); emit_int_expr(c, oidx_k, b);
        buf_puts(b, ", "); emit_boxed(c, argv[0], b);
      }
      else {
        buf_printf(b, "sp_poly_set_poly("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_boxed(c, argv[0], b);
      }
    }
    buf_printf(b, ", _t%d); _t%d; })", tv, tv);
    c->args_in_call = recv;
    { *out = 1; return 1; }
  }
  /* poly receiver: [] with symbol or string key -> runtime dispatch */
  /* poly receiver: arr[start, len] -> sp_poly_slice (string or typed array) */
  /* A user class of its own two-argument [] takes the per-class poly
     dispatch, which emits its arm beside the builtin ones, as the one-argument
     form below does: the slice read answered nil for a Grid (#5522). */
  int has_user_aref2 = 0;
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "[]") && argc == 2 && !g_poly_builtin_arm)
    for (int k = 0; k < c->nclasses; k++)
      if (comp_poly_arm_defines_n(c, k, "[]", argc) ||
          (g_cls_tag_skip != id && comp_cmethod_in_chain(c, k, "[]", NULL) >= 0)) { has_user_aref2 = 1; break; }
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "[]") && argc == 2 && !has_user_aref2) {
    /* The runtime dispatches on the receiver's tag: a string/array does a
       two-arg slice, a bound Method (optcarrot's poke handlers) is called with
       both int args. Both operands are raw integers. */
    /* ...but only a pair of statically Integer operands CAN be a slice. Proc#[]
       is #call, and its arguments are whatever the proc takes -- an Array here,
       which the two-integer reading rejected before any dispatch could happen
       (#4333). Anything else goes through the boxed dispatch; the int path is
       the hot one (optcarrot's poke tables) and keeps its raw operands. */
    if (!(comp_ntype(c, argv[0]) == TY_INT && comp_ntype(c, argv[1]) == TY_INT)) {
      buf_puts(b, "sp_poly_slice_or_call("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b);
      buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
      { *out = 1; return 1; }
    }
    buf_puts(b, "sp_poly_slice("); emit_expr(c, recv, b); buf_puts(b, ", ");
    emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
    { *out = 1; return 1; }
  }
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "[]") && argc == 1) {
    /* `@table[i][j]` dispatch table narrowed to int (poly_double_index_int):
       call the entry (bound method / int array) for an unboxed int result. */
    if (repr_of(c, id).as_ty == TY_INT) {
      /* a read that can miss answers its nil where the consumer takes the oint */
      buf_puts(b, node_is_oint(c, id) ? "sp_poly_index_oint(" : "sp_poly_index_int("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      { *out = 1; return 1; }
    }
    Repr ar = repr_of(c, argv[0]);
    TyKind at = ar.as_ty;
    /* Only use the fast single-call path when no user class defines [].
       If any user class has its own [] method, fall through to the per-class
       poly dispatch (line ~4640) which generates both user and builtin arms. */
    int has_user_aref = 0;
    /* a class's own `self.[]` counts too: a Class value in the slot is
       served by the class-method dispatch, which re-emits this read for the
       values that are not a Class (g_cls_tag_skip) (#5545) */
    for (int k = 0; k < c->nclasses; k++)
      if (comp_poly_arm_defines_n(c, k, "[]", argc) ||
          (g_cls_tag_skip != id && comp_cmethod_in_chain(c, k, "[]", NULL) >= 0)) { has_user_aref = 1; break; }
    if (!has_user_aref) {
      if (at == TY_SYMBOL) {
        buf_puts(b, "sp_poly_get_sym("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        { *out = 1; return 1; }
      }
      if (at == TY_STRING) {
        buf_puts(b, "sp_poly_get_str("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        { *out = 1; return 1; }
      }
      if (at == TY_INT && poly_int_key_boxed(c, argv[0])) {
        /* a key that can be nil: a nil one goes to the runtime boxed
           (poly_int_key_boxed); any other reads as the Integer index it is,
           on the same path a plain key takes */
        int tr = ++g_tmp, tk = ++g_tmp;
        const char *fn = expr_is_arr_or_nil(c, recv) && decide_node(c->nt, recv, "aon-get", NULL)
                           ? "sp_poly_arr_get_aon" : "sp_poly_arr_get_hash";
        buf_printf(b, "({ sp_RbVal _t%d = ", tr); emit_expr(c, recv, b);
        buf_puts(b, "; ");
        if (subtree_may_allocate(c->nt, argv[0])) buf_printf(b, "SP_GC_ROOT_RBVAL(_t%d); ", tr);
        buf_printf(b, "sp_oint _t%d = ", tk); emit_oint_expr(c, argv[0], TY_INT, b);
        buf_printf(b, "; SP_UNLIKELY(_t%d.nil) ? sp_poly_index_poly(_t%d, sp_box_nil()) : %s(_t%d, _t%d.v); })",
                   tk, tr, fn, tr, tk);
        { *out = 1; return 1; }
      }
      if (at == TY_INT || at == TY_POLY) {
        /* A receiver proved to hold only a poly array or nil reaches none of
           the hash, string or Struct arms, so the cls_id test and the cold
           call behind it are dead code on this read. analyze established the
           proof for the GC root elision; this is the same fact paying twice. */
        buf_puts(b, at != TY_INT ? "sp_poly_index_poly("
                    : expr_is_arr_or_nil(c, recv) && decide_node(c->nt, recv, "aon-get", NULL) ? "sp_poly_arr_get_aon("
                                                  : "sp_poly_arr_get_hash(");
        emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        { *out = 1; return 1; }
      }
      /* a non-poly key (e.g. a Method): box it, then index polymorphically */
      if (!ar.untyped) {
        buf_puts(b, "sp_poly_index_poly("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
        { *out = 1; return 1; }
      }
    }
  }
  return 0;
}

/* The builtin arm of a boxed ivar access, once the program can write an ivar
   on a builtin value (Compiler.bivar_table): a value of any builtin class
   (a negative cls_id) takes `stmt`, which asks the runtime's map. Nothing
   without the flag. */
static void emit_bivar_arm(Compiler *c, int tv, const char *stmt, Buf *b) {
  if (c->bivar_table) buf_printf(b, " default: if (_t%d.cls_id < 0) %s; break;", tv, stmt);
}

/* The slot a class value's ivar `sym` lives in, when the program declares
   one: the class-level static (civ_<Class>_<x>) its class methods read, or
   a `class << self` accessor's own (sg_<Class>_<x>). A builtin class's
   reopening holds none. Answers 0 when the class has no slot of the name. */
static int class_ivar_slot(Compiler *c, int k, const char *sym, char *out, size_t n, TyKind *t) {
  ClassInfo *ci = &c->classes[k];
  if (is_builtin_reopen(ci->name)) return 0;
  const char *base = sym + 1;
  if ((comp_is_sg_reader(ci, base) || comp_is_sg_writer(ci, base)) && !comp_is_sg_civ(ci, base)) {
    snprintf(out, n, "sg_%s_%s", ci->name, base);
    *t = TY_POLY;
    return 1;
  }
  int iv = comp_ivar_index(ci, sym);
  if (iv < 0) return 0;
  *t = ci->ivar_types[iv] == TY_UNKNOWN ? TY_INT : ci->ivar_types[iv];
  if (*t == TY_STRBUF) return 0;
  snprintf(out, n, "civ_%s_%s", ci->name, iv_c(base));
  return 1;
}

/* The class arm of a boxed ivar access, once the program can write an ivar
   on a builtin value: a class value whose class declares the slot reads or
   writes it, as the class's own methods do, and any other ivar of a class
   lives in the runtime's map (`dflt`, the map's statement). `op`: 's' sets
   from `_ivs<tv>`, 'g' reads into `_ivg<tv>`, 'd' asks into `_ivd<tv>`. A
   set records the name in the map too, so 'd' and the listing see it; a
   slot only a class method wrote counts when it holds a value.
   Static slots remain readable when no builtin-ivar map is needed.
   A boxed receiver only needs those of a class value that can escape. */
static void emit_class_ivar_arm(Compiler *c, int tv, const char *sym, char op, const char *dflt, int chain, Buf *b) {
  if (!c->bivar_table && op != 'g') return;
  int any = 0;
  for (int k = 0; k < c->nclasses; k++) {
    char slot[300]; TyKind t;
    if (!class_ivar_slot(c, k, sym, slot, sizeof slot, &t)) continue;
    if (!c->bivar_table && chain && !class_value_escapes(c, k)) continue;
    if (!any) buf_printf(b, "%sif (_t%d.tag == SP_TAG_CLASS) switch (_t%d.cls_id) {", chain ? "else " : "", tv, tv);
    any = 1;
    buf_printf(b, " case %d: ", k);
    /* a class-level number slot (civ_) holds its nil beside the value */
    int so = strncmp(slot, "civ_", 4) == 0 && oint_kind(t);
    if (op == 's') {
      char val[24]; snprintf(val, sizeof val, "_ivs%d", tv);
      buf_printf(b, "%s = ", slot);
      if (t == TY_POLY) buf_puts(b, val);
      else if (so) buf_printf(b, "%s(%s)", oint_unbox(t), val);
      else if (nil_value(t)) {
        buf_printf(b, "(%s.tag == SP_TAG_NIL ? %s : ", val, nil_value(t));
        emit_unbox_text(c, t, val, b);
        buf_puts(b, ")");
      }
      else emit_unbox_text(c, t, val, b);
      /* and the name in the map, holding nothing, for instance_variables'
         order and defined? */
      buf_printf(b, "; sp_bivar_set(_t%d, sp_sym_intern(\"%s\"), sp_box_nil());", tv, sym);
    }
    else {
      buf_printf(b, "%s%d = ", op == 'g' ? "_ivg" : "_ivd", tv);
      if (so && op == 'g') buf_printf(b, "%s(%s)", oint_box(t), slot);
      else if (so) buf_printf(b, "!(%s).nil || %s", slot, dflt);
      else if (op == 'g') emit_boxed_text(c, t, slot, b);
      else { buf_puts(b, "("); emit_boxed_text(c, t, slot, b); buf_printf(b, ").tag != SP_TAG_NIL || %s", dflt); }
      buf_puts(b, ";");
    }
    buf_puts(b, " break;");
  }
  if (!any) {
    if (!c->bivar_table) return;
    buf_printf(b, "%sif (_t%d.tag == SP_TAG_CLASS) switch (_t%d.cls_id) {", chain ? "else " : "", tv, tv);
  }
  if (op == 'd') buf_printf(b, " default: _ivd%d = %s; break; } ", tv, dflt);
  else if (c->bivar_table) buf_printf(b, " default: %s; break; } ", dflt);
  else buf_puts(b, " default: break; } ");
}

/* instance_variables of a class value: its map's names, then those of the
   slots its class declares that hold a value (a slot holding nil, or set
   by a class method, lists after the reflective sets) */
static void emit_class_ivar_list_arm(Compiler *c, int tv, Buf *b) {
  if (!c->bivar_table) return;
  buf_printf(b, " if (_t%d.tag == SP_TAG_CLASS) { _ivl%d = sp_bivar_list(_t%d); switch (_t%d.cls_id) {", tv, tv, tv, tv);
  for (int k = 0; k < c->nclasses; k++) {
    ClassInfo *ci = &c->classes[k];
    int any = 0;
    for (int j = 0; j < ci->nivars; j++) {
      char slot[300]; TyKind t;
      if (!class_ivar_slot(c, k, ci->ivars[j], slot, sizeof slot, &t)) continue;
      if (!any) { buf_printf(b, " case %d:", k); any = 1; }
      buf_puts(b, " if ((");
      /* a class-level number slot holds its nil beside the value */
      if (strncmp(slot, "civ_", 4) == 0 && oint_kind(t)) buf_printf(b, "%s(%s)", oint_box(t), slot);
      else emit_boxed_text(c, t, slot, b);
      buf_printf(b, ").tag != SP_TAG_NIL && !sp_bivar_defined(_t%d, sp_sym_intern(\"%s\"))) "
                    "sp_PolyArray_push(_ivl%d, sp_box_sym(sp_sym_intern(\"%s\")));", tv, ci->ivars[j], tv, ci->ivars[j]);
    }
    if (any) buf_puts(b, " break;");
  }
  buf_puts(b, " } }");
}

/* Instance-variable and field access on a boxed receiver: an ivar write, a field read dispatched over every class that has it, instance_variable_get and _set, instance_variables (emit_poly_call's arms, in their order) */

/* The class a boxed receiver's ivars are read by: its cls_id, or for an
   Array subclass instance, boxed as its Array, the class its scan names
   (sp_bsub_cls_of, #7449), whose struct holds them. */
static void emit_ivar_switch_key(Compiler *c, int tv, Buf *b) {
  if (!program_has_arysub(c)) { buf_printf(b, "_t%d.cls_id", tv); return; }
  buf_printf(b, "({ int _ik%d = sp_bsub_cls_of(_t%d); _ik%d >= 0 ? _ik%d : _t%d.cls_id; })", tv, tv, tv, tv, tv);
}

static int emit_poly_ivar_call(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, int *out) {
  /* A class value reads the same static ivar slots as the boxed class arm. */
  if (recv >= 0 && rt == TY_CLASS && is_ivar_access(name) && !is_ivar_set(name) &&
      argc == 1 && nt_ref(nt, id, "block") < 0 &&
      (nt_kind(nt, argv[0]) == NK_SymbolNode || nt_kind(nt, argv[0]) == NK_StringNode)) {
    const char *sym = nt_str(nt, argv[0], nt_kind(nt, argv[0]) == NK_SymbolNode ? "value" : "content");
    if (sym && sym[0] == '@') {
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_boxed(c, recv, b);
      buf_printf(b, "; sp_RbVal _ivg%d = sp_box_nil(); ", tv);
      char dflt[320];
      snprintf(dflt, sizeof dflt, "_ivg%d = sp_bivar_get(_t%d, sp_sym_intern(\"%s\"))", tv, tv, sym);
      emit_class_ivar_arm(c, tv, sym, 'g', dflt, 0, b);
      char val[24]; snprintf(val, sizeof val, "_ivg%d", tv);
      Repr rp = repr_of(c, id);
      if (rp.kind != RK_BOXED && rp.kind != RK_NONE) {
        if (rp.as_ty == TY_INT || rp.as_ty == TY_FLOAT) emit_unbox_nilable_text(c, rp.as_ty, val, b);
        else emit_unbox_text(c, rp.as_ty, val, b);
      }
      else buf_puts(b, val);
      buf_puts(b, "; })");
      *out = 1; return 1;
    }
  }
  /* instance_variable_set(:@x, v) on a POLY receiver with a literal name: the
     write twin of the dispatch below. The value is evaluated once, then
     stored into whichever instantiated class the receiver is, converted to
     that class's slot type; a class without the slot takes no write (the
     inference registered the value's type on every class that has one). The
     call answers the value. */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "instance_variable_set") &&
      argc == 2 && nt_ref(nt, id, "block") < 0 && nt_type(nt, argv[0]) &&
      (sp_streq(nt_type(nt, argv[0]), "SymbolNode") || sp_streq(nt_type(nt, argv[0]), "StringNode"))) {
    const char *a0ty = nt_type(nt, argv[0]);
    const char *sym = sp_streq(a0ty, "SymbolNode")
                        ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
    if (sym && sym[0] == '@') {
      /* a Struct member's name: CRuby keeps such an ivar beside the member,
         and Spinel has one slot for both, as the typed form refuses */
      for (int k = 0; k < c->nclasses; k++) {
        ClassInfo *sk = &c->classes[k];
        int miv = sk->instantiated && sk->is_struct && !sk->is_data ? comp_ivar_index(sk, sym) : -1;
        if (miv >= 0 && miv < sk->nmembers && poly_ivar_set_reaches(c, id, k))
          unsupported(c, id, "instance_variable_set to an ivar absent from the fixed object layout");
      }
      Repr rp = repr_of(c, id);
      TyKind res = rp.as_ty;
      int tv = ++g_tmp;
      buf_puts(b, "({ "); tv = hold_operand(c, recv, TY_POLY, 0, tv, 1, " ", b);
      buf_printf(b, "sp_RbVal _ivs%d = ", tv);
      emit_boxed(c, argv[1], b);
      buf_printf(b, "; if (_t%d.tag == SP_TAG_OBJ) switch (", tv);
      emit_ivar_switch_key(c, tv, b);
      buf_puts(b, ") {");
      for (int k = 0; k < c->nclasses; k++) {
        /* a Data instance is frozen: the write raises, as in CRuby */
        if (c->classes[k].instantiated && c->classes[k].is_data) {
          buf_printf(b, " case %d: sp_raise_frozen_obj(_t%d, (&(\"\\xff\" \"can't modify frozen %s\")[1])); break;",
                     k, tv, class_ruby_name(c, k) ? class_ruby_name(c, k) : c->classes[k].name);
          continue;
        }
        if (!c->classes[k].instantiated) continue;
        int iv = comp_ivar_index(&c->classes[k], sym);
        if (nt_int(nt, id, "builtin_only", 0) && !is_builtin_reopen(c->classes[k].name) &&
            (iv < 0 || comp_ty_value_obj(c, ty_object(k))) && poly_ivar_set_reaches(c, id, k))
          unsupported(c, id, "instance variable write to a receiver without a writable slot");
        if (comp_ty_value_obj(c, ty_object(k))) continue;   /* by value: no reference to write through */
        /* a Struct member is no ivar (#2849) */
        if (iv < 0 || (c->classes[k].is_struct && iv < c->classes[k].nmembers)) continue;
        TyKind t = c->classes[k].ivar_types[iv];
        if (t == TY_STRBUF && !repr_share_rule(c)) {
          if (nt_int(nt, id, "builtin_only", 0) && poly_ivar_set_reaches(c, id, k))
            unsupported(c, id, "instance variable write to a shared String slot through a boxed receiver");
          continue;
        }
        char val[48]; snprintf(val, sizeof val, "_ivs%d", tv);
        buf_printf(b, " case %d: ", k);
        size_t obn = strlen(c->classes[k].c_name) + 32;
        char *obj = (char *)malloc(obn);
        snprintf(obj, obn, "((sp_%s *)_t%d.v.p)", c->classes[k].c_name, tv);
        emit_frozen_obj_guard(c, k, obj, b);
        if (oint_kind(t) && ivar_has_nilbit(c, k, iv)) {
          /* the value with its nil into the field and its bit */
          char objp[96]; snprintf(objp, sizeof objp, "%s->", obj);
          char otext[80]; snprintf(otext, sizeof otext, "%s(%s)", oint_unbox(t), val);
          buf_printf(b, "%siv_%s = ", objp, iv_c(sym + 1));
          emit_ivar_text_nilbit(c, k, iv, objp, otext, b);
          buf_puts(b, ";");
        }
        else {
          buf_printf(b, "%s->iv_%s = ", obj, iv_c(sym + 1));
          if (t == TY_POLY) buf_puts(b, val);
          else if (nil_value(t)) {
            /* nil into a String slot is its NULL */
            buf_printf(b, "(%s.tag == SP_TAG_NIL ? %s : ", val, nil_value(t));
            emit_unbox_text(c, t, val, b);
            buf_puts(b, ")");
          }
          else emit_unbox_text(c, t, val, b);
          buf_puts(b, ";");
        }
        char mk[300];
        const char *mark = ivar_set_mark(c, k, sym, obj, "->", mk, sizeof mk);
        if (mark) buf_printf(b, " %s", mark);
        free(obj);
        buf_puts(b, " break;");
      }
      /* a bare Object keeps its ivars in a table of its own */
      buf_printf(b, " case SP_BUILTIN_OBJECT: sp_Object_ivar_set((sp_Object *)_t%d.v.p, sp_sym_intern(\"%s\"), _ivs%d); break;",
                 tv, sym, tv);
      /* a builtin value: the runtime's map, or FrozenError (emit_bivar_arm) */
      char bst[320];
      snprintf(bst, sizeof bst, "sp_bivar_set(_t%d, sp_sym_intern(\"%s\"), _ivs%d)", tv, sym, tv);
      emit_bivar_arm(c, tv, bst, b);
      buf_puts(b, " } ");
      emit_class_ivar_arm(c, tv, sym, 's', bst, 1, b);
      /* an immediate, a String, a Time: FrozenError, or refused when it runs */
      if (c->bivar_table) buf_printf(b, "else %s; ", bst);
      if (rp.kind != RK_BOXED && rp.kind != RK_NONE) {
        char ivn[24]; snprintf(ivn, sizeof ivn, "_ivs%d", tv);
        /* a number answers with its nil beside the value: the call is the
           value written, nil included */
        if (oint_kind(res)) { oint_open(c, id, res, b); buf_printf(b, "%s(%s)", oint_unbox(res), ivn); oint_close(c, id, b); }
        else emit_unbox_text(c, res, ivn, b);
        buf_puts(b, "; })");
      }
      else buf_printf(b, "_ivs%d; })", tv);
      { *out = 1; return 1; }
    }
  }

  /* instance_variable_get(:@x) on a POLY receiver with a literal symbol or
     string name: dispatch the field read over every instantiated class that
     has the slot, boxing per the slot's declared type (the poly twin of the
     concrete lowering; see the matching inference rule in analyze_infer.c).
     A receiver whose runtime class lacks the slot reads as nil, matching
     CRuby's unset-ivar behavior; the SP_TAG_OBJ guard keeps a boxed scalar
     (cls_id 0) from aliasing the user class at index 0 (cf. issue #1576). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "instance_variable_get") &&
      argc == 1 && nt_ref(nt, id, "block") < 0 && nt_type(nt, argv[0]) &&
      (sp_streq(nt_type(nt, argv[0]), "SymbolNode") || sp_streq(nt_type(nt, argv[0]), "StringNode"))) {
    const char *a0ty = nt_type(nt, argv[0]);
    const char *sym = sp_streq(a0ty, "SymbolNode")
                        ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
    if (sym && sym[0] == '@') {
      Repr rp = repr_of(c, id);
      TyKind res = rp.as_ty;
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", tv);
      emit_expr(c, recv, b);
      buf_printf(b, "; sp_RbVal _ivg%d = sp_box_nil(); if (_t%d.tag == SP_TAG_OBJ) switch (", tv, tv);
      emit_ivar_switch_key(c, tv, b);
      buf_puts(b, ") {");
      for (int k = 0; k < c->nclasses; k++) {
        if (!c->classes[k].instantiated) continue;
        int iv = comp_ivar_index(&c->classes[k], sym);
        /* a Struct member is no ivar: it reads nil (#2849) */
        if (iv < 0 || (c->classes[k].is_struct && iv < c->classes[k].nmembers)) continue;
        TyKind t = c->classes[k].ivar_types[iv];
        char fld[320];
        snprintf(fld, sizeof fld, "((sp_%s *)_t%d.v.p)->iv_%s", c->classes[k].c_name, tv, iv_c(sym + 1));
        buf_printf(b, " case %d: _ivg%d = ", k, tv);
        /* a field with a nil bit boxes as nil where the bit is set */
        if (oint_kind(t) && ivar_has_nilbit(c, k, iv)) {
          char objp[200]; snprintf(objp, sizeof objp, "((sp_%s *)_t%d.v.p)->", c->classes[k].c_name, tv);
          char bit[400]; ivar_nilbit_test(c, k, iv, objp, bit, sizeof bit);
          buf_printf(b, "(%s ? sp_box_nil() : %s(%s))", bit, t == TY_FLOAT ? "sp_box_float" : "sp_box_int", fld);
        }
        else emit_boxed_text(c, t, fld, b);
        buf_puts(b, "; break;");
      }
      buf_puts(b, " case SP_BUILTIN_OBJECT: ");
      /* main's ordinary ivars live in the same statics its top-level reads use. */
      int main_ci = comp_class_index(c, "Toplevel");
      int main_iv = main_ci >= 0 ? comp_ivar_index(&c->classes[main_ci], sym) : -1;
      if (main_iv >= 0) {
        char slot[300]; snprintf(slot, sizeof slot, "civ_Toplevel_%s", iv_c(sym + 1));
        buf_printf(b, "if (_t%d.v.p == sp_main_obj) _ivg%d = ", tv, tv);
        /* a static that holds its nil beside the value boxes with it */
        TyKind mt = c->classes[main_ci].ivar_types[main_iv];
        if (civ_is_oint(c, main_ci, main_iv)) buf_printf(b, "%s(%s)", oint_box(mt), slot);
        else emit_boxed_text(c, mt, slot, b);
        buf_puts(b, "; else ");
      }
      buf_printf(b, "_ivg%d = sp_Object_ivar_get((sp_Object *)_t%d.v.p, sp_sym_intern(\"%s\")); break;", tv, tv, sym);
      char bst[320];
      snprintf(bst, sizeof bst, "_ivg%d = sp_bivar_get(_t%d, sp_sym_intern(\"%s\"))", tv, tv, sym);
      emit_bivar_arm(c, tv, bst, b);
      buf_puts(b, " } ");
      emit_class_ivar_arm(c, tv, sym, 'g', bst, 1, b);
      if (rp.kind != RK_BOXED && rp.kind != RK_NONE) {
        /* a receiver whose class lacks the slot answers nil: an Integer or
           Float answer takes it as its nil flag */
        char ivn[24]; snprintf(ivn, sizeof ivn, "_ivg%d", tv);
        if (res == TY_INT || res == TY_FLOAT) emit_unbox_nilable_text(c, res, ivn, b);
        else emit_unbox_text(c, res, ivn, b);
        buf_puts(b, "; })");
      }
      else buf_printf(b, "_ivg%d; })", tv);
      { *out = 1; return 1; }
    }
  }

  /* instance_variable_defined?(:@x) and instance_variables on a POLY
     receiver: each instantiated class answers from its layout (as the typed
     forms do), a bare Object from its own table (sp_Object_ivar_defined,
     sp_Object_ivars), anything else has none. */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "instance_variable_defined?") && argc == 1 &&
      nt_ref(nt, id, "block") < 0 && nt_type(nt, argv[0]) &&
      (sp_streq(nt_type(nt, argv[0]), "SymbolNode") || sp_streq(nt_type(nt, argv[0]), "StringNode"))) {
    const char *sym = sp_streq(nt_type(nt, argv[0]), "SymbolNode")
                        ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
    if (sym && sym[0] == '@') {
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", tv);
      emit_expr(c, recv, b);
      buf_printf(b, "; sp_bool _ivd%d = FALSE; if (_t%d.tag == SP_TAG_OBJ) switch (", tv, tv);
      emit_ivar_switch_key(c, tv, b);
      buf_puts(b, ") {");
      for (int k = 0; k < c->nclasses; k++) {
        if (!c->classes[k].instantiated) continue;
        int iv = comp_ivar_index(&c->classes[k], sym);
        if (iv < 0 || (c->classes[k].is_struct && iv < c->classes[k].nmembers)) continue;
        if (nt_int(nt, id, "builtin_only", 0) && ivar_set_kind(c, k, sym) == 2)
          unsupported(c, id, "defined? of an instance variable with untracked presence on a boxed receiver");
        char ex[200], tb[300];
        snprintf(ex, sizeof ex, "((sp_%s *)_t%d.v.p)->iv_%s", c->classes[k].c_name, tv, iv_c(sym + 1));
        const char *set = ivar_set_test(c, k, sym, ex, tb, sizeof tb);
        buf_printf(b, " case %d: _ivd%d = %s; break;", k, tv, set ? set : "TRUE");
      }
      buf_printf(b, " case SP_BUILTIN_OBJECT: _ivd%d = sp_Object_ivar_defined((sp_Object *)_t%d.v.p, "
                    "sp_sym_intern(\"%s\")); break;", tv, tv, sym);
      char bst[320];
      snprintf(bst, sizeof bst, "_ivd%d = sp_bivar_defined(_t%d, sp_sym_intern(\"%s\"))", tv, tv, sym);
      emit_bivar_arm(c, tv, bst, b);
      buf_puts(b, " } ");
      char dex[300];
      snprintf(dex, sizeof dex, "sp_bivar_defined(_t%d, sp_sym_intern(\"%s\"))", tv, sym);
      emit_class_ivar_arm(c, tv, sym, 'd', dex, 1, b);
      buf_printf(b, "_ivd%d; })", tv);
      { *out = 1; return 1; }
    }
  }
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "instance_variables") && argc == 0 &&
      nt_ref(nt, id, "block") < 0) {
    int tv = ++g_tmp;
    buf_puts(b, "({ "); tv = hold_operand(c, recv, TY_POLY, 0, tv, 1, " ", b);
    buf_printf(b, "sp_PolyArray *_ivl%d = NULL; if (_t%d.tag == SP_TAG_OBJ) switch (", tv, tv);
    emit_ivar_switch_key(c, tv, b);
    buf_puts(b, ") {");
    for (int k = 0; k < c->nclasses; k++) {
      ClassInfo *ivc = &c->classes[k];
      if (!ivc->instantiated) continue;
      buf_printf(b, " case %d:", k);
      char ob[96], an[32];
      snprintf(ob, sizeof ob, "((sp_%s *)_t%d.v.p)", ivc->c_name, tv);
      snprintf(an, sizeof an, "_ivl%d", tv);
      emit_ivar_list_fill(c, k, ob, an, 1, b);
      buf_puts(b, " break;");
    }
    buf_printf(b, " case SP_BUILTIN_OBJECT: _ivl%d = sp_Object_ivars((sp_Object *)_t%d.v.p); break;", tv, tv);
    char bst[96];
    snprintf(bst, sizeof bst, "_ivl%d = sp_bivar_list(_t%d)", tv, tv);
    emit_bivar_arm(c, tv, bst, b);
    buf_puts(b, " }");
    emit_class_ivar_list_arm(c, tv, b);
    buf_printf(b, " if (!_ivl%d) _ivl%d = sp_PolyArray_new(); _ivl%d; })", tv, tv, tv);
    { *out = 1; return 1; }
  }
  return 0;
}

/* A numeric call on a boxed receiver: to_i with a base, count, round, floor and ceil with digits (emit_poly_call's arms, in their order) */
static int emit_poly_numeric_call(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, int *out) {
  /* poly receiver `.to_i(base)`: only String#to_i takes a radix. When the value
     is a String at runtime, parse it (mirroring String#to_i(base)); any other
     type -- Integer/Float/nil -- has a zero-arity to_i, so CRuby raises
     ArgumentError. Guard on the tag rather than blindly sp_poly_to_s'ing, which
     would silently parse "42".to_i(16) => 66 instead of raising. The no-arg
     conversions live in the argc == 0 block below, which this form would skip.
     Receiver then argument are bound in that order to keep CRuby's evaluation
     order (both are evaluated before the call raises). */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && sp_streq(name, "to_i") &&
      repr_of(c, id).kind != RK_BOXED) {
    int tr = ++g_tmp, tb = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = sp_poly_strbuf_deref(", tr); emit_expr(c, recv, b);
    buf_printf(b, "); sp_int _t%d = ", tb); emit_int_expr(c, argv[0], b);
    buf_printf(b, "; _t%d.tag == SP_TAG_STR ? sp_str_to_i_base(_t%d.v.s, _t%d)"
                  " : (sp_raise_cls(\"ArgumentError\", \"wrong number of arguments (given 1, expected 0)\"), (sp_int)0); })",
               tr, tr, tb);
    { *out = 1; return 1; }
  }

  /* poly receiver count(v): value-equality element count over a boxed Array,
     Hash, Range or Enumerator (sp_poly_count_val) */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && sp_streq(name, "count") &&
      nt_ref(nt, id, "block") < 0) {
    /* Only a user definition that can TAKE one positional argument blocks
       this arm: a `count(a, b)` or a reader cannot answer the call, and
       counting it steered a genuine String or Array receiver into the
       dispatch, whose arity filter then dropped every arm and raised
       (#4195). Same judgement as the dispatch's own candidate filter. */
    int has_user_cnt = 0;
    if (!g_poly_builtin_arm)
    for (int kk = 0; kk < c->nclasses && !has_user_cnt; kk++) {
      if (c->classes[kk].is_native_class) {   /* bindings only (#4504) */
        if (comp_poly_arm_defines_n(c, kk, "count", 1)) has_user_cnt = 1;
        continue;
      }
      int mi_k = comp_method_in_chain(c, kk, "count", NULL);
      if (mi_k >= 0) {
        Scope *cs_k = &c->scopes[mi_k];
        if (cs_k->rest_idx >= 0 || (1 >= cs_k->nrequired && 1 <= cs_k->nparams))
          has_user_cnt = 1;
      }
    }
    if (!has_user_cnt) {
      buf_puts(b, "sp_poly_count_val("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      { *out = 1; return 1; }
    }
  }
  /* Numeric#round(ndigits) on a poly: the digit-taking form the no-arg
     numeric path cannot express. A user `round` still wins (poly dispatch). */
  /* `round(half: :even)` -- with or without a digits argument -- on a boxed
     receiver. The keyword hash is not a positional argument: read as one it
     reached the digits slot and raised "no implicit conversion of Hash into
     Integer", where the typed Float and Integer paths have honoured the
     tie-break mode all along. Peel it off the positional view here, exactly
     as the typed arm does, so a value that went through a container answers
     what the same value answers when it did not. */
  if (recv >= 0 && rt == TY_POLY && (argc == 1 || argc == 2) &&
      is_round_family(name) &&
      nt_ref(nt, id, "block") < 0 &&
      nt_type(nt, argv[argc - 1]) &&
      sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
    int has_user_kw = poly_name_user_claimed(c, name, argc);
    /* Which keywords were written is a compile-time fact for a literal key;
       a `**splat` is read at run time, and a key spelled some other way is
       not read at all -- nothing may be called an unknown keyword on the
       strength of what cannot be read. The typed Float and Integer arms use
       the same reader, so a boxed receiver and a typed one cannot disagree
       about what the call said. */
    RoundKw kw; round_kw_read(c, argv[argc - 1], &kw);
    if (!has_user_kw) {
      /* CRuby evaluates the receiver, the positional argument and every
         keyword value before the call decides anything, so a call it then
         rejects has still run their side effects. Hold each in a temp here
         rather than emitting it inside the arm that may raise. */
      int tv = ++g_tmp;
      buf_puts(b, "({ "); tv = hold_operand(c, recv, TY_POLY, 0, tv, 1, " ", b);
      int tn = -1;
      if (argc == 2) {
        tn = ++g_tmp;
        buf_printf(b, "sp_int _t%d = ", tn);
        emit_int_expr(c, argv[0], b);
        buf_puts(b, "; ");
      }
      /* only #round takes a tie-break mode; the other three reject a keyword
         outright, with CRuby's words (the typed arm does the same, #3646).
         With a digit count as well the hash is a second argument, and the
         arity is what CRuby complains about first. The keyword values are
         still evaluated: the hash is built before the call rejects it. */
      if (!sp_streq(name, "round")) {
        emit_round_kw_effects(c, &kw, b);
        if (argc == 2)
          buf_printf(b, "(void)_t%d;"
                        " sp_raise_cls(\"ArgumentError\", \"wrong number of arguments"
                        " (given 2, expected 0..1)\");", tn);
        else
          /* CRuby's words for a Rational are its own, and which receiver
             this is only the run time knows */
          buf_printf(b, "sp_raise_cls(\"TypeError\", sp_poly_is_rational(_t%d)"
                        " ? \"not an integer\""
                        " : \"no implicit conversion of Hash into Integer\");", tv);
        buf_puts(b, " sp_box_nil(); })");
        { *out = 1; return 1; }
      }
      /* `round` takes `half:` and nothing else, so the binder raises for any
         other key -- a `**` source's keys included, which it reads at run
         time rather than leaving the mode silently defaulted. */
      int thalf = emit_round_kw_binds(c, &kw, b);
      /* the mode reaches the helper as the value it was written as: a
         Symbol, a String, nil for the default -- deciding which is the
         helper's job, since only it knows whether the receiver cares */
      buf_printf(b, "sp_poly_round_half(_t%d, ", tv);
      if (tn >= 0) buf_printf(b, "_t%d", tn); else buf_puts(b, "0");
      if (thalf >= 0) buf_printf(b, ", _t%d); })", thalf);
      else buf_puts(b, ", sp_box_nil()); })");
      { *out = 1; return 1; }
    }
  }
  if (recv >= 0 && rt == TY_POLY && argc == 1 &&
      is_round_family(name) &&
      nt_ref(nt, id, "block") < 0) {
    if (!poly_name_user_claimed(c, name, argc)) {
      /* ceil / floor / truncate with a precision had no arm at all and
         raised NoMethodError on a Float (#4532) */
      if (sp_streq(name, "round")) buf_puts(b, "sp_poly_round_n(");
      else buf_puts(b, "sp_poly_prec_n(");
      emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_int_expr(c, argv[0], b);
      if (!sp_streq(name, "round"))
        buf_printf(b, ", %s", name[0] == 'c' ? "SP_PREC_CEIL" : name[0] == 'f' ? "SP_PREC_FLOOR" : "SP_PREC_TRUNC");
      buf_puts(b, ")");
      { *out = 1; return 1; }
    }
  }
  return 0;
}

/* pow(n) on a boxed receiver. pow is Integer's alone: sp_poly_pow is `**`,
   which a Float, a Rational or a Complex answers too, so a boxed one
   answered pow where CRuby raises NoMethodError. The exponent is evaluated
   before the call, as CRuby evaluates arguments, and sp_poly_int_pow
   raises with it as the error's args. */
static void emit_poly_int_pow(Compiler *c, int recv, int arg, Buf *b) {
  int tv = ++g_tmp, te = ++g_tmp;
  buf_puts(b, "({ "); tv = hold_operand(c, recv, TY_POLY, 1, tv, 1, " ", b);
  buf_printf(b, "sp_RbVal _t%d = ", te);
  emit_boxed(c, arg, b);
  buf_printf(b, "; sp_poly_int_pow(_t%d, _t%d); })", tv, te);
}

static int emit_poly_scan_rows(Compiler *c, int id, Buf *b, const NodeTable *nt,
                               const char *name, int recv, int argc, const int *argv, int root) {
  int ts = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb);
  emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "const char *_t%d = sp_poly_recv_s(%s, \"scan\");", ts, rb.p ? rb.p : "sp_box_nil()");
  if (root) buf_printf(g_pre, " SP_GC_ROOT_STR(_t%d);", ts);
  buf_puts(g_pre, "\n");
  free(rb.p);
  char r[32]; snprintf(r, sizeof r, "_t%d", ts);
  return str_arms_pattern(c, id, b, nt, name, argc, argv, r);
}

static int emit_poly_scan_block(Compiler *c, int id, Buf *b, const NodeTable *nt,
                               const char *name, int recv, int argc, const int *argv) {
  /* poly.scan(pat) { }: the block form over a receiver only known to be a
     String at run time. Rows are precomputed exactly as the typed-String arm
     does, then the block runs per row; the value is the receiver string
     (CRuby answers self). A native class defining scan does not step this
     aside as it does the blockless forms: the dispatch has no String arm
     for a call with a block, so a genuine String would raise there. */
  if (is_scan_name(name) && argc == 1 && nt_ref(nt, id, "block") >= 0 &&
      !user_defines_or_reads(c, "scan")) {
    int sblk = nt_ref(nt, id, "block");
    const char *sp0 = block_param_name(c, sblk, 0);
    const char *sp0r = sp0 ? rename_local(sp0) : NULL;
    int sbody = nt_ref(nt, sblk, "body");
    int sbn = 0; const int *sbb = sbody >= 0 ? nt_arr(nt, sbody, "body", &sbn) : NULL;
    int re_i = re_lit_index(c, argv[0]);
    TyKind pat_t = comp_ntype(c, argv[0]);
    /* Capturing patterns use the typed String emitter's row binding.
       Hold the checked subject across the scan's allocations. */
    if (re_i >= 0 && an_re_has_captures(re_lit_src(c, argv[0]))) {
      return emit_poly_scan_rows(c, id, b, nt, name, recv, argc, argv, 1);
    }
    if (re_i < 0 && (pat_t == TY_REGEX || pat_t == TY_POLY))
      return emit_poly_scan_rows(c, id, b, nt, name, recv, argc, argv, 0);
    int ts = ++g_tmp, tm = ++g_tmp, ti = ++g_tmp;
    /* a body that reads `$~` or a capture global walks the subject for its
       own turn's match, as the typed-String arm does (#3601) */
    int tw = (re_i >= 0 || pat_t == TY_STRING) && subtree_reads_match_globals(c, sbody) ? ++g_tmp : -1;
    int tp = tw >= 0 && re_i < 0 ? ++g_tmp : -1;
    buf_printf(b, "({ const char *_t%d = sp_poly_recv_s(", ts); emit_expr(c, recv, b);
    buf_printf(b, ", \"%s\"); SP_GC_ROOT(_t%d);", name, ts);
    if (tp >= 0) {
      buf_printf(b, " const char *_t%d = ", tp); emit_expr(c, argv[0], b);
      buf_printf(b, "; SP_GC_ROOT_STR(_t%d);", tp);
    }
    if (tw >= 0) buf_printf(b, " sp_int _t%d = 0;", tw);
    buf_printf(b, " sp_StrArray *_t%d = ", tm);
    if (re_i >= 0) buf_printf(b, "sp_re_scan(sp_re_pat_%d, _t%d)", re_i, ts);
    else if (pat_t == TY_REGEX) { buf_puts(b, "sp_re_scan("); emit_expr(c, argv[0], b); buf_printf(b, ", _t%d)", ts); }
    else if (tp >= 0) buf_printf(b, "sp_str_scan(_t%d, _t%d)", ts, tp);
    else if (pat_t == TY_STRING) { buf_printf(b, "sp_str_scan(_t%d, ", ts); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else {
      /* the pattern arrived boxed (read out of a table): a Regexp or a
         String, told apart at run time */
      buf_printf(b, "sp_scan_boxed(_t%d, ", ts);
      emit_boxed(c, argv[0], b);
      buf_puts(b, ")");
    }
    buf_printf(b, "; SP_GC_ROOT(_t%d);", tm);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++) {", ti, ti, tm, ti);
    if (tp >= 0) buf_printf(b, " _t%d = sp_str_scan_at(_t%d, _t%d, _t%d);", tw, ts, tp, tw);
    else if (tw >= 0)
      buf_printf(b, " if (sp_re_match_at(sp_re_pat_%d, _t%d, _t%d) >= 0)"
                    " _t%d = sp_re_caps[1] > sp_re_caps[0] ? sp_re_caps[1] : sp_re_caps[1] + 1;",
                 re_i, ts, tw, tw);
    if (sp0r) {
      Scope *sbs = comp_scope_of(c, sblk);
      LocalVar *shlv = sbs ? scope_local(sbs, sp0) : NULL;
      if (shlv && shlv->type == TY_POLY) {
        buf_printf(b, " sp_RbVal lv_%s = sp_box_str(sp_StrArray_get(_t%d, _t%d));", sp0r, tm, ti);
        /* --share-strings: a boxed parameter of a shared class takes the match
           as a handle of its own, as the typed arms bind it */
        Buf lb; memset(&lb, 0, sizeof lb);
        emit_block_param_share(c, sbs, shlv, sp0r, &lb, 0);
        if (lb.p && lb.p[0]) buf_printf(b, "\n%s", lb.p);
        free(lb.p);
      }
      else
        buf_printf(b, " const char *lv_%s = sp_StrArray_get(_t%d, _t%d);", sp0r, tm, ti);
    }
    for (int k2 = 0; k2 < sbn; k2++) emit_stmt(c, sbb[k2], b, 0);
    buf_printf(b, " } _t%d; })", ts);
    return 1;
  }
  return 0;
}

/* A boxed String transform reads shared arguments as byte snapshots. Those
   copies are not kept by the handle's root, so two allocating operands must
   run in order. Hold their values first to retain String identity, then root
   the byte reads before another read can allocate. Integer slots are named
   by the caller, just as its runtime helper takes them. The sharing rule
   introduces these snapshots; default operands keep their existing holds.
   Hold even plain reads before later arguments can reassign their slots. */
static void emit_poly_str_transform(Compiler *c, int recv, int argc, const int *argv,
                                    const char *name, const char *fn, unsigned int_args, Buf *b) {
  int alloc = operand_may_allocate(c, recv);
  for (int i = 0; i < argc; i++) alloc += operand_may_allocate(c, argv[i]);
  int tr = -1, ta[2] = { -1, -1 }, views[2] = { -1, -1 }, mark = g_n_argov;
  if (repr_share_rule(c) && alloc > 1) {
    tr = hold_operand_pre(c, recv, TY_POLY, 1, ++g_tmp, 1);
    for (int i = 0; i < argc; i++) {
      Repr r = repr_of(c, argv[i]);
      int boxed = r.kind == RK_STRBUF;
      ta[i] = hold_operand_pre(c, argv[i], r.as_ty, boxed, ++g_tmp, 1);
      if (boxed) views[i] = view_push(c, argv[i], TY_POLY);
      view_bind(argv[i], "_t%d", ta[i]);
    }
    int ts = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "const char *_t%d = sp_poly_recv_s(_t%d, \"%s\"); SP_GC_ROOT(_t%d);\n", ts, tr, name, ts);
    tr = ts;
    for (int i = 0; i < argc; i++) if (ta[i] >= 0) {
      Buf sb; memset(&sb, 0, sizeof sb);
      int integer = (int_args & (1u << i)) != 0;
      if (integer) emit_int_expr(c, argv[i], &sb);
      else emit_str_expr(c, argv[i], &sb);
      ts = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "%s _t%d = %s;", integer ? "sp_int" : "const char *", ts, sb.p);
      if (!integer) buf_printf(g_pre, " SP_GC_ROOT(_t%d);", ts);
      buf_puts(g_pre, "\n");
      free(sb.p);
      ta[i] = ts;
    }
  }
  buf_printf(b, "sp_str_%s(", fn);
  if (tr >= 0) buf_printf(b, "_t%d", tr);
  else { buf_puts(b, "sp_poly_recv_s("); emit_expr(c, recv, b); buf_printf(b, ", \"%s\")", name); }
  for (int i = 0; i < argc; i++) {
    buf_puts(b, ", ");
    if (ta[i] >= 0) buf_printf(b, "_t%d", ta[i]);
    else if (int_args & (1u << i)) emit_int_expr(c, argv[i], b);
    else emit_str_expr(c, argv[i], b);
  }
  buf_puts(b, ")");
  for (int i = argc - 1; i >= 0; i--) if (views[i] >= 0) view_pop(c, views[i]);
  view_unbind(mark);
}

int emit_poly_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = comp_recv_type(c, recv);
  /* Hash#compare_by_identity switches a hash to identity (equal?/object_id)
     key comparison. Spinel's hash machinery compares keys by value, so the
     mutator can't take effect; emitting it as a no-op would silently diverge
     (subsequent lookups behave as a value-keyed hash). Reject loudly instead.
     The `compare_by_identity?` predicate is left to report false, which is
     correct for any hash this mutator never (successfully) ran on. Only a
     boxed receiver can be a Hash here (a typed one is refused in
     emit_hash_call), as every other arm of this function assumes: refused on
     the name alone, a user class's own compare_by_identity was rejected. */
  if (recv >= 0 && rt == TY_POLY &&
      sp_streq(name, "compare_by_identity"))  /* any arity: identity hashing is unsupported */
    unsupported(c, id, "Hash#compare_by_identity (identity-keyed hashing)");
  /* #slice on a boxed receiver is two different methods: Hash#slice(*keys)
     answers a sub-Hash, while String#slice / Array#slice is exactly #[]. Only
     the runtime value tells them apart, so branch on it and hand the non-hash
     side to the boxed `[]` dispatch through a rename re-entry, like the
     typed-array slice above (#3445, #3449). */
  if (recv >= 0 && rt == TY_POLY && argc >= 1 && sp_streq(name, "slice") &&
      nt_ref(nt, id, "block") < 0 && !user_defines_or_reads(c, "slice") &&
      g_n_argov + 3 < MAX_ARG_OVERRIDE) {
    int tsv = ++g_tmp;
    int has_splat = 0;
    for (int i = 0; i < argc; i++)
      if (nt_type(nt, argv[i]) && sp_streq(nt_type(nt, argv[i]), "SplatNode")) has_splat = 1;
    /* The key list below allocates -- a PolyArray per splat, plus the copies
       into it -- and the receiver is not read until after all of that. A
       receiver that is a temporary (`poly(1).slice(*keys)`) was collected in
       between and the slice answered from freed memory. */
    buf_puts(b, "({ ");
    tsv = hold_operand(c, recv, TY_POLY, 1, tsv, 1, " ", b);
    /* the arguments next, held and bound for every side below: CRuby runs
       them before the call, and the raise for a receiver with no slice
       names them as NoMethodError#args */
    int vmark = g_n_argov, held = !has_splat && argc <= 2;
    int tav[2] = {0, 0};
    for (int i = 0; held && i < argc; i++)
      tav[i] = hold_operand(c, argv[i], comp_ntype(c, argv[i]), 0, ++g_tmp, 1, " ", b);
    for (int i = 0; held && i < argc; i++) view_bind(argv[i], "_t%d", tav[i]);
    int tkeys = -1;
    if (has_splat) {
      /* A splat contributes all of its elements, so the key list has a length
         only the run time knows: build it as a PolyArray and hand the callee
         its buffer. The fixed `(sp_RbVal[]){...}` below cannot express that --
         it passed the whole array as ONE key, and as the wrong C type at that,
         since the splat expression is an unboxed sp_PolyArray * (#4164). */
      tkeys = ++g_tmp;
      buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tkeys, tkeys);
      for (int i = 0; i < argc; i++) {
        if (nt_type(nt, argv[i]) && sp_streq(nt_type(nt, argv[i]), "SplatNode")) {
          int sx = nt_ref(nt, argv[i], "expression");
          int tss = ++g_tmp, tsi = ++g_tmp;
          buf_printf(b, " sp_PolyArray *_t%d = sp_poly_to_poly_array(", tss);
          if (sx >= 0) emit_boxed(c, sx, b); else buf_puts(b, "sp_box_nil()");
          buf_printf(b, "); SP_GC_ROOT(_t%d);", tss);
          buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                        " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));",
                     tsi, tsi, tss, tsi, tkeys, tss, tsi);
          continue;
        }
        buf_printf(b, " sp_PolyArray_push(_t%d, ", tkeys); emit_boxed(c, argv[i], b);
        buf_puts(b, ");");
      }
      buf_puts(b, " ");
    }
    buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(_t%d.cls_id))", tsv, tsv);
    if (has_splat)
      buf_printf(b, " ? sp_poly_hash_slice(_t%d, (int)_t%d->len, _t%d->data) : ", tsv, tkeys, tkeys);
    else {
      char rb[32]; snprintf(rb, sizeof rb, "_t%d", tsv);
      buf_puts(b, " ? "); emit_rooted_key_call(c, "sp_poly_hash_slice", rb, argv, argc, b);
      buf_puts(b, " : ");
    }
    if (has_splat) {
      /* The non-hash side is String#slice / Array#slice, which is exactly #[]
         and takes one argument or two -- and with a splat only the run time
         knows which. Branch on the key list's length; any other length is the
         ArgumentError CRuby raises. */
      buf_printf(b, "(_t%d->len == 1 ? sp_poly_index_poly(_t%d, _t%d->data[0])"
                    " : _t%d->len == 2"
                    " ? sp_poly_slice(_t%d, sp_poly_arg_int_chk(_t%d->data[0]), sp_poly_arg_int_chk(_t%d->data[1]))"
                    " : (sp_raise_cls(\"ArgumentError\", \"wrong number of arguments\"), sp_box_nil()))",
                 tkeys, tsv, tkeys, tkeys, tsv, tkeys, tkeys);
    }
    else if (g_poly_builtin_arm && (argc == 1 || argc == 2)) {
      /* the builtin arm of a dispatch a user class owning `slice` opened: the
         value is a builtin here, but the `[]` re-entry below would ask that
         class's `[]` too and find no arm for an Array (#5114) */
      if (argc == 1) {
        buf_printf(b, "sp_poly_index_poly(_t%d, ", tsv); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      }
      else {
        buf_printf(b, "sp_poly_slice(_t%d, sp_poly_arg_int_chk(", tsv); emit_boxed(c, argv[0], b);
        buf_puts(b, "), sp_poly_arg_int_chk("); emit_boxed(c, argv[1], b); buf_puts(b, "))");
      }
    }
    else if (argc == 1 || argc == 2) {
      /* only a String, a Symbol and an Array have slice: through `[]` nil
         raised naming `[]` and an Integer answered a bit */
      buf_printf(b, "(_t%d.tag == SP_TAG_STR || _t%d.tag == SP_TAG_SYM || sp_poly_is_strbuf(_t%d) ||"
                    " (_t%d.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_t%d.cls_id))) ? ",
                 tsv, tsv, tsv, tsv, tsv);
      int rmark = g_n_argov;
      view_bind(recv, "_t%d", tsv);
      nt_node_set_str((NodeTable *)nt, id, "name", "[]");
      Buf ib; memset(&ib, 0, sizeof ib); emit_call(c, id, &ib);
      nt_node_set_str((NodeTable *)nt, id, "name", "slice");
      view_unbind(rmark);
      buf_puts(b, ib.p ? ib.p : "sp_box_nil()");
      free(ib.p);
      buf_printf(b, " : (sp_raise_nomethod(sp_nomethod_msg_args(\"slice\", _t%d, %d, (sp_RbVal[]){", tsv, argc);
      for (int i = 0; i < argc; i++) {
        char tn[24]; snprintf(tn, sizeof tn, "_t%d", tav[i]);
        if (i) buf_puts(b, ", ");
        emit_boxed_text(c, comp_ntype(c, argv[i]), tn, b);
      }
      buf_puts(b, "})), sp_box_nil())");
    }
    else buf_printf(b, "sp_raise_nomethod(sp_nomethod_msg(\"slice\", _t%d))", tsv);
    view_unbind(vmark);
    buf_puts(b, "; })");
    return 1;
  }
  /* The Integer surface on a boxed receiver that may hold a Bignum: the
     runtime helpers answer by the box's tag, where the face re-entry below
     narrowed the box to sp_int and computed on a truncated number (#4665).
     The inference answered these boxed (pred, pow, ceildiv, gcd, lcm) or as
     the fitting kind (digits, gcdlcm) in infer_poly_call. */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") < 0 && !user_defines_or_reads(c, name) &&
      ((argc == 0 && sp_streq(name, "pred")) ||
       ((argc == 1 || argc == 2) && sp_streq(name, "pow")) ||
       (argc == 1 && (sp_streq(name, "ceildiv") || sp_streq(name, "gcd") || sp_streq(name, "lcm") ||
                      sp_streq(name, "gcdlcm") || sp_streq(name, "allbits?") ||
                      sp_streq(name, "anybits?") || sp_streq(name, "nobits?"))) ||
       (argc <= 1 && sp_streq(name, "digits")))) {
    int has_user = 0;
    for (int kk = 0; kk < c->nclasses && !has_user; kk++)
      if (comp_poly_arm_defines_n(c, kk, name, argc)) has_user = 1;
    if (!has_user) {
      if (sp_streq(name, "pow") && argc == 1) emit_poly_int_pow(c, recv, argv[0], b);
      else if (sp_streq(name, "pow")) {
        buf_puts(b, "sp_poly_int_powmod_recv("); emit_boxed(c, recv, b);
        buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
      }
      else if (is_bits_query(name)) {
        buf_puts(b, "sp_poly_int_bits_test("); emit_boxed(c, recv, b); buf_puts(b, ", ");
        emit_boxed(c, argv[0], b);
        buf_printf(b, ", %d)", sp_streq(name, "allbits?") ? 0 : sp_streq(name, "anybits?") ? 1 : 2);
      }
      else if (sp_streq(name, "digits")) {
        buf_puts(b, "sp_poly_int_digits("); emit_boxed(c, recv, b); buf_puts(b, ", ");
        if (argc == 1) emit_to_int_expr(c, argv[0], b); else buf_puts(b, "10");
        buf_puts(b, ")");
      }
      else if (argc == 0) { buf_printf(b, "sp_poly_int_%s(", name); emit_boxed(c, recv, b); buf_puts(b, ")"); }
      else { buf_printf(b, "sp_poly_int_%s(", name); emit_boxed(c, recv, b);
             buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
      /* each of these but a lone pow's takes its operands as C arguments */
      if (!(is_pow_name(name) && argc == 1)) c->args_in_call = recv;
      return 1;
    }
  }
  /* Range#overlap? on a boxed receiver: an Integer or Float Range answers
     at run time (sp_range_overlap_v), anything else raises NoMethodError */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_kind(nt, argv[0]) != NK_SplatNode &&
      sp_streq(name, "overlap?") && !user_defines_or_reads(c, name)) {
    int res_boxed = repr_of(c, id).kind == RK_BOXED;
    if (res_boxed) buf_puts(b, "sp_box_bool(");
    buf_puts(b, "sp_range_overlap_v("); emit_boxed(c, recv, b);
    buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
    if (res_boxed) buf_puts(b, ")");
    return 1;
  }
  /* to_a / to_ary typed boxed where the answer is mutated
     (an_to_a_result_mutated): an Array answers itself, not the face's copy */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "to_a") || sp_streq(name, "to_ary")) && repr_of(c, id).kind == RK_BOXED) {
    if (!poly_name_user_claimed(c, name, argc)) {
      buf_printf(b, "sp_poly_to_a_self(\"%s\", ", name);
      emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
  }
  /* casecmp / casecmp?: a Symbol and a String each compare with their own
     kind, decided at run time (sp_poly_casecmp), where the String face below
     raised NoMethodError for a Symbol */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_kind(nt, argv[0]) != NK_SplatNode &&
      is_casecmp_family(name) && !user_defines_or_reads(c, name)) {
    buf_puts(b, "sp_poly_casecmp("); emit_boxed(c, recv, b);
    buf_puts(b, ", "); emit_boxed(c, argv[0], b);
    buf_printf(b, ", %d)", sp_streq(name, "casecmp?"));
    return 1;
  }
  if (recv >= 0 && rt == TY_POLY && emit_poly_str_upto(c, id, b)) return 1;
  /* The face table (types.h): unbox the receiver to the kind that owns the
     name, retype the receiver node and re-enter the same call, so the typed
     emitter IS the implementation and the inference, which answered under
     the same pin, has already sized the result slot to it. The last-resort
     rows are not taken here: the Hash face answers in emit_unresolved_call,
     once every poly-receiver emitter of its own has declined the name. */
  if (recv >= 0 && rt == TY_POLY && !user_defines_or_reads(c, name) &&
      g_n_argov < MAX_ARG_OVERRIDE) {
    int has_blk = nt_ref(nt, id, "block") >= 0;
    unsigned own = an_zero_arg_builtin_shadowed(c, name, argc) ? 0
                   : ty_poly_face_owners(name, argc, has_blk, nt_call_args_plain(nt, id), 0);
    unsigned kinds = own & PF_OWNERS;
    if (own & PF_STR_BANG) { emit_face_str_bang(c, id, own, b); return 1; }
    /* one owner an argument rules out takes the switch's misfit arm: the
       re-entry's typed emitter declines the argument, and the call fell
       to NoMethodError where CRuby raises the conversion's TypeError */
    int one = kinds && !(kinds & (kinds - 1)), misfit = one && face_args_misfit(c, id, kinds);
    if (one && !misfit && emit_face_reentry(c, id, kinds, own, b)) return 1;
    /* a blockless step(n) on a boxed ENDLESS Range walks as an Enumerator
       (sp_poly_range_endless_step), decided at run time ahead of the face,
       whose Range arm materializes and cannot fill an endless one; any other
       receiver takes the face as before. The argument is read in either
       branch, so only one without effects. */
    if (kinds && (kinds & (kinds - 1)) && sp_streq(name, "step") && argc == 1 && !has_blk &&
        g_endless_step_node != id && !subtree_has_side_effect(c, argv[0]) &&
        repr_of(c, id).kind == RK_BOXED) {
      int tv = ++g_tmp, tr = ++g_tmp;
      /* the receiver is read once, into the prelude: the face hoists its own
         read of it there too, which has to come after */
      tv = hold_operand_pre(c, recv, TY_POLY, 1, tv, 1);
      buf_printf(b, "({ sp_RbVal _t%d; if (sp_poly_range_is_endless(_t%d)) _t%d = sp_poly_range_endless_step(_t%d, ",
                 tr, tv, tr, tv);
      emit_boxed(c, argv[0], b);
      buf_printf(b, "); else _t%d = ", tr);
      int slot = view_bind(recv, "_t%d", tv);
      int sv = g_endless_step_node; g_endless_step_node = id;
      Buf fb; memset(&fb, 0, sizeof fb);
      int ok = emit_face_switch(c, id, kinds, &fb);
      g_endless_step_node = sv;
      view_unbind(slot);
      if (ok) {
        buf_puts(b, fb.p ? fb.p : "sp_box_nil()");
        buf_printf(b, "; _t%d; })", tr);
        free(fb.p);
        return 1;
      }
      free(fb.p);
    }
    if (kinds && (!one || misfit) && emit_face_switch(c, id, kinds, b)) return 1;
    /* a declined re-entry may have renamed the node and restored it into
       fresh storage (see emit_face_arm): the name is read again */
    name = nt_str(nt, id, "name");
  }
  /* The one/two-String-argument transforms on a boxed receiver: a String
     arriving through a poly slot (a Fiber#resume value, a container read) had
     no arm for these and raised NoMethodError naming String, which is what it
     was. A regexp pattern keeps the dedicated regexp emitters. The receiver
     is unboxed as a String, not converted to one: `42.tr("4", "x")` is
     NoMethodError, not "x2" (#4493). */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") < 0 &&
      !user_defines_or_reads(c, name)) {
    if (sp_streq(name, "squeeze") && argc == 1) {
      emit_poly_str_transform(c, recv, argc, argv, name, "squeeze_chars", 0, b);
      return 1;
    }
    if (sp_streq(name, "tr") && argc == 2) {
      emit_poly_str_transform(c, recv, argc, argv, name, "tr", 0, b);
      return 1;
    }
    if ((is_substitution(name)) && argc == 2 &&
        comp_ntype(c, argv[0]) == TY_STRING && comp_ntype(c, argv[1]) == TY_STRING) {
      emit_poly_str_transform(c, recv, argc, argv, name, name, 0, b);
      return 1;
    }
  }
  /* nil-aware conversions on a boxed receiver (a nil local widens to poly).
     The call's settled type may predate the widening (the receiver inferred
     TY_NIL on an early fixpoint pass and typed a captured local concretely);
     unbox the helper's boxed result to match it -- the receiver provably held
     nil there, so the payload really is the concrete kind. */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") < 0 &&
      ((argc == 0 && (sp_streq(name, "to_h") || sp_streq(name, "to_r") || sp_streq(name, "to_c"))) ||
       (argc <= 1 && sp_streq(name, "rationalize")))) {
    int has_user = 0;
    for (int k = 0; k < c->nclasses && !has_user; k++)
      if (comp_poly_arm_defines_n(c, k, name, argc)) has_user = 1;
    if (!has_user) {
      if (sp_streq(name, "to_r")) {
        buf_puts(b, "(*(sp_Rational *)sp_poly_to_r_m(");
        emit_expr(c, recv, b);
        buf_puts(b, ").v.p)");
      }
      /* rationalize is to_r for nil and an Integer (#2460), not for a Float
         (0.1.rationalize is (1/10), its to_r the exact binary value), and it
         takes an epsilon, which nil and an Integer ignore: the runtime picks
         the kind's own (sp_poly_rationalize_m), the epsilon boxed */
      else if (sp_streq(name, "rationalize")) {
        buf_puts(b, "(*(sp_Rational *)sp_poly_rationalize_m(");
        emit_expr(c, recv, b);
        buf_printf(b, ", %d, ", argc);
        if (argc == 1) emit_boxed(c, argv[0], b); else buf_puts(b, "sp_box_nil()");
        buf_puts(b, ").v.p)");
      }
      else if (sp_streq(name, "to_c")) {
        buf_puts(b, "(*(sp_Complex *)sp_poly_to_c_m(");
        emit_expr(c, recv, b);
        buf_puts(b, ").v.p)");
      }
      else {
        /* to_h: the helper passes a real hash through unchanged, so guard
           the variant before unboxing (a non-sym-keyed hash rejects loudly
           rather than reading through the wrong layout). When the call's own
           inferred type stayed poly (an OpenStruct|nil receiver whose to_h
           result feeds a poly dispatch), yield the boxed value instead of
           the concrete pointer -- the consumer switches on cls_id (#3282). */
        int th2 = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = sp_poly_to_h_m(", th2);
        emit_expr(c, recv, b);
        buf_puts(b, ");");
        /* A boxed slot takes any hash variant as-is. Only the concrete
           sym-keyed slot needs one: a hash built at runtime through the
           general merge path is a PolyPolyHash even when every key is a
           Symbol, and rejecting it outright was wrong (#3452). */
        if (repr_of(c, id).kind == RK_BOXED) buf_printf(b, " _t%d; })", th2);
        else buf_printf(b, " sp_poly_as_sym_hash(_t%d); })", th2);
      }
      return 1;
    }
  }
  /* The Regexp surface on a boxed receiver: the names Regexp alone owns unbox
     the pattern and re-dispatch through the typed emitter, and the match forms
     -- which String owns too -- go to the runtime pair dispatch, where either
     operand may carry the pattern. A Regexp arriving through a block parameter
     had no arm at all and raised NoMethodError naming Regexp (#3961). */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") < 0 &&
      !user_defines_or_reads(c, name) && g_n_argov < MAX_ARG_OVERRIDE) {
    static const char *const RXO[] = { "source", "options", "casefold?",
                                       "named_captures", "names", NULL };
    int want_rx = 0;
    for (int i = 0; RXO[i] && !want_rx; i++) if (sp_streq(name, RXO[i]) && argc == 0) want_rx = 1;
    if (want_rx) {
      int trx = ++g_tmp;
      Buf rbx; memset(&rbx, 0, sizeof rbx); emit_expr(c, recv, &rbx);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "mrb_regexp_pattern *_t%d = sp_poly_as_pattern(%s);\n",
                 trx, rbx.p ? rbx.p : "sp_box_nil()");
      free(rbx.p);
      view_bind(recv, "_t%d", trx);
      int v = view_push(c, recv, TY_REGEX);
      emit_call(c, id, b);
      view_pop(c, v);
      view_unbind(g_n_argov - 1);
      return 1;
    }
  }
  /* match?(pattern, pos) takes the same route, from the position */
  int match_at = argc == 2 && is_match_p_name(name) && nt_kind(nt, argv[0]) != NK_SplatNode &&
                 nt_kind(nt, argv[1]) != NK_SplatNode && nt_kind(nt, argv[1]) != NK_KeywordHashNode;
  if (recv >= 0 && (argc == 1 || match_at) && nt_ref(nt, id, "block") < 0 &&
      !user_defines_or_reads(c, name) &&
      (sp_streq(name, "match?") || sp_streq(name, "match") || sp_streq(name, "=~")) &&
      (rt == TY_POLY || ((rt == TY_STRING || rt == TY_STRBUF) &&
                         comp_ntype(c, argv[0]) == TY_POLY))) {
    /* `=~` answers the match offset or nil, so it rides boxed like the typed
       form does; the other two answer a bool and a MatchData. */
    if (sp_streq(name, "=~")) {
      int tmi = ++g_tmp;
      buf_printf(b, "({ sp_oint _t%d = sp_poly_match_index(", tmi);
      emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b);
      buf_printf(b, "); sp_box_oint(_t%d); })", tmi);
      return 1;
    }
    /* the operands are held and checked first (sp_poly_match_check): the
       helpers answered no match for a receiver without the method and for
       a pattern of the wrong kind */
    const char *fn = sp_streq(name, "match?") ? "sp_poly_match_p" : "sp_poly_match_data";
    int ta = ++g_tmp, tb = ++g_tmp;
    Buf mb; memset(&mb, 0, sizeof mb);
    buf_printf(&mb, "({ sp_RbVal _t%d = ", ta); emit_boxed(c, recv, &mb);
    buf_printf(&mb, "; SP_GC_ROOT_RBVAL(_t%d); sp_RbVal _t%d[2]; _t%d[0] = ", ta, tb, tb); emit_boxed(c, argv[0], &mb);
    buf_printf(&mb, "; SP_GC_ROOT_RBVAL(_t%d[0]); ", tb);
    if (argc == 2) {
      buf_printf(&mb, "_t%d[1] = ", tb); emit_boxed(c, argv[1], &mb);
      buf_printf(&mb, "; SP_GC_ROOT_RBVAL(_t%d[1]); ", tb);
    }
    buf_printf(&mb, "sp_poly_match_check(_t%d, \"%s\", %d, _t%d); ", ta, name, argc, tb);
    if (argc == 2) buf_printf(&mb, "sp_poly_match_p_at(_t%d, _t%d[0], sp_poly_arg_int_chk(_t%d[1])); })", ta, tb, tb);
    else buf_printf(&mb, "%s(_t%d, _t%d[0]); })", fn, ta, tb);
    /* This arm takes the builtin only when no REACHABLE class defines the
       name. The analyzer's twin counts every class that defines it, so a
       user `match` on a class the program never builds leaves the call typed
       poly there, and the bare MatchData pointer went into that poly slot:
       the C did not compile. Box it to the type the analyzer settled on. */
    if (sp_streq(name, "match") && repr_of(c, id).kind == RK_BOXED)
      emit_boxed_text(c, TY_MATCHDATA, mb.p, b);
    else buf_puts(b, mb.p);
    free(mb.p);
    return 1;
  }
  /* poly === arg dispatches on the RECEIVER's runtime class, the way CRuby's
     case-equality does: a Regexp matches, a Range covers, a Class tests
     membership. Answering plain equality made every one of them false (#3963). */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && sp_streq(name, "===")) {
    int has_user = 0;
    for (int k = 0; k < c->nclasses && !has_user; k++)
      if (comp_poly_arm_defines_n(c, k, name, argc)) has_user = 1;
    if (!has_user) {
      buf_puts(b, "sp_poly_case_eq_match(");
      emit_expr(c, recv, b);
      buf_puts(b, ", ");
      emit_boxed(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
  }
  /* encoding.name -> the encoding name string */
  if (sp_streq(name, "name") && argc == 0 && recv >= 0 && comp_ntype(c, recv) == TY_POLY) {
    const char *rty2 = nt_type(nt, recv);
    int is_enc = (rty2 && sp_streq(rty2, "SourceEncodingNode")) ||
                 (rty2 && sp_streq(rty2, "CallNode") &&
                  nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "encoding"));
    if (is_enc) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
  }

  { int r; if (emit_poly_ivar_call(c, id, b, nt, name, recv, argc, argv, rt, &r)) return r; }

  { int r; if (emit_poly_numeric_call(c, id, b, nt, name, recv, argc, argv, rt, &r)) return r; }
  /* poly receiver: nil? / conversions / a few type-agnostic queries */
  /* poly.scan(re) -- a String read out of a `{}`-then-filled Hash reaches
     here poly-typed, and without an arm it hit the NoMethodError gate
     (#3368). Mirrors the rt==TY_STRING arms: no captures -> string array,
     captures -> array of arrays. */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "scan") && argc == 1 &&
      nt_ref(nt, id, "block") < 0 && !user_defines_or_reads(c, name) &&
      !native_class_defines(c, name)) {
    /* Guard on the tag: only a String actually answers #scan, and a nil (or an
       Integer) receiver must raise NoMethodError as CRuby does rather than be
       stringified into an empty scan (test/issue_3147.rb pins that). */
    int rli = re_lit_index(c, argv[0]);
    int str_arg = comp_ntype(c, argv[0]) == TY_STRING;
    /* A pattern the compiler cannot resolve to a literal -- an inline
       `Regexp.new(s)`, a local holding one, an interpolated literal -- is
       still an mrb_regexp_pattern* at run time, and the String-receiver arm
       has taken it since #3389. Without it here the call fell past this
       handler to the unresolved-call gate and raised on the String (#3392).
       The tag guard stays: only a String answers #scan. */
    int re_arg = !str_arg && rli < 0 && comp_ntype(c, argv[0]) == TY_REGEX;
    if (rli >= 0 || str_arg || re_arg) {
      if (repr_share_rule(c) && str_arg && operand_may_allocate(c, recv) && operand_may_allocate(c, argv[0])) {
        emit_poly_str_transform(c, recv, argc, argv, name, "scan", 0, b);
        return 1;
      }
      /* follow the type analyze settled on, so emit and type stay in step for
         a run-time pattern (sp_re_scan_poly decides per match) */
      int poly_res = repr_of(c, id).elem == TY_POLY;
      int ts = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", ts); emit_boxed(c, recv, b);
      buf_printf(b, "; (_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)) ? ", ts, ts);
      if (rli >= 0)
        buf_printf(b, "%s(sp_re_pat_%d, sp_poly_to_s(_t%d))",
                   poly_res ? "sp_re_scan_poly" : "sp_re_scan", rli, ts);
      else if (re_arg) {
        buf_printf(b, "%s(", poly_res ? "sp_re_scan_poly" : "sp_re_scan");
        emit_expr(c, argv[0], b);
        buf_printf(b, ", sp_poly_to_s(_t%d))", ts);
      }
      else {
        buf_printf(b, "sp_str_scan(sp_poly_to_s(_t%d), ", ts);
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      buf_printf(b, " : (%s *)(sp_raise_nomethod(sp_nomethod_msg(\"scan\", _t%d)), (void *)0); })",
                 poly_res ? "sp_PolyArray" : "sp_StrArray", ts);
      return 1;
    }
  }
  /* The one-String-argument transforms. The block below covers the poly String
     surface only for the zero-argument shapes, so a String arriving through a
     poly slot -- a Fiber#resume value, a container read -- had no arm for
     these and fell through to the unresolved-call raise, naming String, which
     is what it was (#3436). */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "delete_prefix") || sp_streq(name, "delete_suffix")) &&
      !user_defines_or_reads(c, name)) {
    /* the inference rule answers TY_STRING, so hand back the raw const char * */
    emit_poly_str_transform(c, recv, argc, argv, name, name, 0, b);
    return 1;
  }
  /* `dig` on a receiver that stayed poly: the arms above are per container
     kind, and with none matching the call was coerced to a hash and raised
     NoMethodError on an Array that answers it. The runtime walk dispatches on
     the receiver's own kind at each step (#3509). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "dig") && argc >= 1 &&
      nt_ref(nt, id, "block") < 0) {
    int has_user_dig = poly_name_user_claimed(c, name, argc);
    /* the receiver is held across the arguments, which may allocate */
    if (!has_user_dig) {
      if (argc == 1 && nt_kind(nt, argv[0]) == NK_SplatNode)
        return emit_dig_splat(c, recv, argv[0], b);
      int any_splat = 0;
      for (int a = 0; a < argc; a++)
        if (nt_kind(nt, argv[a]) == NK_SplatNode) any_splat = 1;
      if (!any_splat) {
        Buf rb; int ch = hold_recv_open(c, recv, 1, "sp_RbVal", "SP_GC_ROOT_RBVAL", b, &rb);
        emit_rooted_key_call(c, "sp_poly_dig_n", rb.p, argv, argc, b); free(rb.p);
        if (ch) buf_puts(b, "; })");
        c->args_in_call = recv;
        return 1;
      }
      /* a splat beside other keys (`dig(*path, :k)`): the keys in order,
         each splat's elements in its place, walked as plain keys are */
      {
        Buf rb; int ch = hold_recv_open(c, recv, 1, "sp_RbVal", "SP_GC_ROOT_RBVAL", b, &rb);
        int tk = ++g_tmp, tr = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", tk, tk);
        for (int a = 0; a < argc; a++) {
          if (nt_kind(nt, argv[a]) == NK_SplatNode) {
            int ts = ++g_tmp;
            buf_printf(b, "{ sp_PolyArray *_t%d = sp_poly_to_poly_array(", ts); emit_boxed(c, argv[a], b);
            buf_printf(b, "); SP_GC_ROOT(_t%d); for (sp_int _i = 0; _i < _t%d->len; _i++)"
                          " sp_PolyArray_push(_t%d, _t%d->data[_i]); } ", ts, ts, tk, ts);
          }
          else {
            buf_printf(b, "sp_PolyArray_push(_t%d, ", tk); emit_boxed(c, argv[a], b); buf_puts(b, "); ");
          }
        }
        /* sp_poly_dig_n walks every receiver #dig walks (a Struct and a
           program object too) and raises the call's NoMethodError for the rest */
        buf_printf(b, "sp_RbVal _t%d = %s; sp_poly_dig_n(_t%d, _t%d->len, _t%d->data); })",
                   tr, rb.p, tr, tk, tk);
        free(rb.p);
        if (ch) buf_puts(b, "; })");
        return 1;
      }
    }
  }
  /* The one-argument numeric methods, the same rule the no-argument table
     below uses: dispatch on the runtime tag unless a user class owns the name.
     They were missing entirely, so an exact Rational reaching divmod / modulo
     / quo through a block parameter raised NoMethodError on methods it
     answers (#3512). */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0) {
    const char *pfn1 =
      sp_streq(name, "divmod")  ? "sp_poly_divmod" :
      sp_streq(name, "modulo")  ? "sp_poly_mod" :
      sp_streq(name, "div")     ? "sp_poly_div_m" :
      sp_streq(name, "remainder") ? "sp_poly_remainder" :
      sp_streq(name, "coerce")  ? "sp_poly_coerce" :
      sp_streq(name, "quo")     ? "sp_poly_quo" : NULL;
    if (pfn1) {
      if (!poly_name_user_claimed(c, name, argc)) {
        buf_printf(b, "%s(", pfn1); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
    }
  }
  /* poly.scan(pat) with no block: the rows themselves. The pattern may arrive
     boxed (read out of a table), a Regexp or a String (sp_scan_boxed). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "scan") && argc == 1 &&
      nt_ref(nt, id, "block") < 0 && !user_defines_or_reads(c, "scan") &&
      !native_class_defines(c, "scan")) {
    int sre = re_lit_index(c, argv[0]);
    TyKind spt = comp_ntype(c, argv[0]);
    int str_rows = repr_of(c, id).elem == TY_STRING;
    const char *sfn = str_rows ? "sp_re_scan" : "sp_re_scan_poly";
    if (spt == TY_STRING) { sfn = "sp_str_scan"; }
    /* a boxed pattern is a Regexp or a String, told apart at run time */
    if (sre < 0 && spt != TY_REGEX && spt != TY_STRING) {
      buf_printf(b, "%s(sp_poly_recv_s(", str_rows ? "sp_scan_boxed" : "sp_scan_boxed_poly");
      emit_expr(c, recv, b);
      buf_printf(b, ", \"%s\"), ", name);
      emit_boxed(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    buf_printf(b, "%s(", sfn);
    if (sre >= 0) buf_printf(b, "sp_re_pat_%d", sre);
    else if (spt == TY_REGEX) emit_expr(c, argv[0], b);
    else if (spt == TY_STRING) { buf_puts(b, "sp_poly_recv_s("); emit_expr(c, recv, b); buf_printf(b, ", \"%s\"), ", name); }
    if (spt == TY_STRING) { emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else { buf_puts(b, ", sp_poly_recv_s("); emit_expr(c, recv, b); buf_printf(b, ", \"%s\"))", name); }
    return 1;
  }
  if (recv >= 0 && rt == TY_POLY && emit_poly_scan_block(c, id, b, nt, name, recv, argc, argv)) return 1;
  if (rt == TY_POLY && emit_builtin_op(c, id, recv, rt, name, b)) return 1;
  { int r; if (emit_poly_call0_arms(c, id, b, nt, name, recv, argc, argv, rt, &r)) return r; }
  /* blockless cycle(n) on a poly value: the Enumerator over its items
     repeated n times that the typed arms build (sp_poly_cycle_n), the
     receiver held across the count, and a count the compiler types anything
     but Integer converted at run time (sp_poly_cycle_count). A class of the
     program's own with a method or a class method of the name wins via poly
     dispatch, and a cycle on Object, which answers for every receiver the
     dispatch does not, keeps its universal fallback. */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "cycle") && nt_ref(nt, id, "block") < 0 &&
      ((argc == 1 && nt_kind(nt, argv[0]) != NK_SplatNode) ||
       (argc == 0 && comp_ntype(c, id) == TY_ENUMERATOR))) {
    int oci = comp_class_index(c, "Object");
    int has_user = oci >= 0 && comp_method_in_chain(c, oci, name, NULL) >= 0;
    if (!g_poly_builtin_arm)
    for (int kk = 0; kk < c->nclasses && !has_user; kk++)
      if (comp_poly_arm_defines_n(c, kk, name, argc) ||
          (!c->classes[kk].is_native_class && comp_reader_in_chain(c, kk, name, NULL)) ||
          comp_cmethod_in_chain(c, kk, name, NULL) >= 0) has_user = 1;
    if (!has_user && argc == 0) {
      buf_puts(b, "sp_poly_cycle("); emit_boxed(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    if (!has_user) {
      Buf rcn;
      int ccn = hold_recv_open(c, recv, 1, "sp_RbVal", "SP_GC_ROOT_RBVAL", b, &rcn);
      buf_printf(b, "sp_poly_cycle_n(%s, ", rcn.p);
      if (comp_ntype(c, argv[0]) == TY_INT) emit_int_expr(c, argv[0], b);
      else { buf_printf(b, "sp_poly_cycle_count(%s, ", rcn.p); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
      buf_puts(b, ")");
      free(rcn.p);
      if (ccn) buf_puts(b, "; })");
      return 1;
    }
  }
  /* Hash#merge(other) { |key, old, new| }: the block decides the value for a
     key both hashes carry. Walk the other hash's pairs into a copy of the
     receiver, consulting the block on a collision -- sp_poly_hash_merge has no
     block form, and this arm handles boxed/cross-layout receivers. */
  if (recv >= 0 && (rt == TY_POLY || (ty_is_hash(rt) && rt != TY_POLY_POLY_HASH)) &&
      sp_streq(name, "merge") && argc == 1 &&
      nt_ref(nt, id, "block") >= 0 && (rt != TY_POLY || !user_defines_or_reads(c, "merge"))) {
    /* the merge is a general Hash; a call typed boxed -- a dispatch's
       builtin arm, whose slot it shares with a program class's merge --
       holds it boxed */
    Buf mb; memset(&mb, 0, sizeof mb);
    if (emit_merge_any_block_boxed(c, id, recv, argv[0], &mb)) {
      if (repr_of(c, id).kind == RK_BOXED) emit_boxed_text(c, TY_POLY_POLY_HASH, mb.p ? mb.p : "NULL", b);
      else buf_puts(b, mb.p ? mb.p : "NULL");
      free(mb.p);
      return 1;
    }
    free(mb.p);
  }
  /* poly.ljust/rjust/center(width[, pad]): a String read from a container
     widened to poly. Pad via sp_poly_to_s and re-box (#3222). Outside the
     argc==0 block above since these take a width (and optional pad) arg. Skip
     when a user class overrides the name (the runtime value may be it). */
  if (recv >= 0 && rt == TY_POLY &&
      (sp_streq(name, "ljust") || sp_streq(name, "rjust") || sp_streq(name, "center")) &&
      (argc == 1 || argc == 2)) {
    if (!poly_name_user_claimed(c, name, argc)) {
      const char *fn = sp_streq(name, "ljust") ? "sp_str_ljust"
                     : sp_streq(name, "rjust") ? "sp_str_rjust" : "sp_str_center";
      /* unboxed as a String: an Integer or nil in the slot is NoMethodError,
         not its to_s padded (#4493) */
      char op[32]; snprintf(op, sizeof op, "%s%s", fn + 7, argc == 2 ? "2" : "");
      buf_puts(b, "sp_box_str(");
      emit_poly_str_transform(c, recv, argc, argv, name, op, 1, b);
      buf_puts(b, ")");
      return 1;
    }
  }
  /* The argument-taking String methods on a poly receiver, outside the argc==0
     block for the same reason ljust is: that block is guarded on argc == 0, so
     an arm for these placed inside it can never be entered. Each reuses the
     runtime function the concrete arm calls, and declines to a user class
     owning the name, as the neighbours do. */
  /* The separator forms of the trimming methods. Their argc==0 spellings are
     in the table above; a line reader chomping with its OWN separator --
     `line.chomp(eol)` -- passes one, and that reached NoMethodError. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && !user_defines_or_reads(c, name) &&
      (sp_streq(name, "chomp") || sp_streq(name, "delete_prefix") ||
       sp_streq(name, "delete_suffix"))) {
    const char *fn = sp_streq(name, "chomp") ? "sp_str_chomp_sep"
                   : sp_streq(name, "delete_prefix") ? "sp_str_delete_prefix"
                   : "sp_str_delete_suffix";
    /* TY_STRING, not boxed: the analyze arm types these as the String they
       are, so the slot takes a const char * directly. */
    emit_poly_str_transform(c, recv, argc, argv, name, fn + 7, 0, b);
    return 1;
  }
  if (recv >= 0 && rt == TY_POLY && !user_defines_or_reads(c, name) &&
      ((sp_streq(name, "unpack") && argc == 1) ||
       (sp_streq(name, "byteslice") && (argc == 1 || argc == 2)) ||
       (sp_streq(name, "scrub") && argc == 1) ||
       (sp_streq(name, "encode") && argc >= 1 && argc <= 3) ||
       ((is_encoding_mutator(name)) && argc >= 1 && argc <= 2))) {
    if (sp_streq(name, "unpack")) {
      buf_puts(b, "sp_box_poly_array(");
      emit_poly_str_transform(c, recv, argc, argv, name, "unpack", 0, b);
      buf_puts(b, ")");
    }
    else if (sp_streq(name, "byteslice") && argc == 1 &&
             comp_ntype(c, argv[0]) == TY_RANGE) {
      /* byteslice(RANGE) on a poly receiver. The arm emitted the single-index
         helper whatever the argument was, so the Range reached emit_int_expr
         and became an unconditional "no implicit conversion of Range into
         Integer" -- the range was built, cast to void and thrown away, and
         the raise ran (#4308). Same endpoint resolution the String receiver
         uses. */
      int trp = ++g_tmp;
      buf_printf(b, "({ sp_Range _t%d = sp_range_ix(", trp); emit_expr(c, argv[0], b); buf_puts(b, ")");
      buf_puts(b, "; sp_box_nullable_str(sp_str_byteslice_range(sp_poly_recv_s(");
      emit_expr(c, recv, b);
      buf_printf(b, ", \"byteslice\"), _t%d.first, _t%d.last, _t%d.excl,"
                    " _t%d.nobeg, _t%d.noend)); })",
                 trp, trp, trp, trp, trp);
    }
    else if (sp_streq(name, "byteslice")) {
      buf_printf(b, "sp_box_nullable_str(sp_str_byteslice%s(sp_poly_recv_s(",
                 argc == 1 ? "1" : "");
      emit_expr(c, recv, b); buf_puts(b, ", \"byteslice\"), ");
      emit_int_expr(c, argv[0], b);
      if (argc == 2) { buf_puts(b, ", "); emit_int_expr(c, argv[1], b); }
      buf_puts(b, "))");
    }
    else if (sp_streq(name, "scrub")) {
      buf_puts(b, "sp_box_str(");
      emit_poly_str_transform(c, recv, argc, argv, name, "scrub", 0, b);
      buf_puts(b, ")");
    }
    else if (is_encoding_mutator(name)) {
      /* the String receiver's arm on the unboxed value: a `String | nil` slot
         holding a String answered NoMethodError for want of this (#4441) */
      /* a shared handle in the box takes the tag on the handle, and the call
         answers the box itself */
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_expr(c, recv, b);
      char hv[48]; snprintf(hv, sizeof hv, "((sp_String *)_t%d.v.p)", tv);
      buf_printf(b, "; sp_poly_is_strbuf(_t%d) ? (sp_String_force_encoding(%s, ", tv, hv);
      emit_strbuf_force_encoding_mode(c, name, hv, argv, argc, b);
      buf_printf(b, "), _t%d) : ", tv);
      char rv[64]; snprintf(rv, sizeof rv, "sp_poly_recv_s(_t%d, \"%s\")", tv, name);
      buf_puts(b, "sp_box_str(");
      emit_str_force_encoding(c, name, rv, argv, argc, b);
      buf_puts(b, "); })");
    }
    else {  /* encode: the same transcode the String receiver takes (#4439) */
      Buf rb; memset(&rb, 0, sizeof rb);
      buf_puts(&rb, "sp_poly_recv_s("); emit_expr(c, recv, &rb); buf_puts(&rb, ", \"encode\")");
      buf_puts(b, "sp_box_str(");
      emit_str_encode_call(c, rb.p ? rb.p : "", argv, argc, b);
      buf_puts(b, ")");
      free(rb.p);
    }
    return 1;
  }
  /* Data#with on a poly receiver (a Data read out of a container): build a
     symbol-keyed override hash from the keyword args, then dispatch by cls_id
     to a copy-update constructor (#2890). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "with") && argc == 1 &&
      nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) {
    if (!poly_name_user_claimed(c, "with", argc)) {
      int en = 0; const int *els = nt_arr(nt, argv[0], "elements", &en);
      int th = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_SymPolyHash *_t%d = sp_SymPolyHash_new(); SP_GC_ROOT(_t%d);\n", th, th);
      for (int e = 0; e < en; e++) {
        int key = nt_ref(nt, els[e], "key");
        const char *kty = key >= 0 ? nt_type(nt, key) : NULL;
        const char *kn = (kty && sp_streq(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
        int val = nt_ref(nt, els[e], "value");
        if (!kn || val < 0) continue;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_SymPolyHash_set(_t%d, sp_sym_intern(\"%s\"), ", th, kn);
        emit_boxed(c, val, g_pre); buf_puts(g_pre, ");\n");
      }
      buf_puts(b, "sp_poly_with_m("); emit_expr(c, recv, b);
      buf_printf(b, ", sp_box_obj(_t%d, SP_BUILTIN_SYM_POLY_HASH))", th);
      return 1;
    }
  }
  /* poly receiver: String#getbyte (a non-string tag raises NoMethodError).
     A user method or attr reader with the same name wins. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && sp_streq(name, "getbyte")) {
    if (!poly_name_user_claimed(c, name, argc)) {
      buf_puts(b, "sp_poly_getbyte("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
  }
  /* poly receiver: String#unpack1(fmt) -- unbox via sp_poly_to_s first. A
     String value that widened to poly (doom's binary WAD/texture parsing)
     was entirely unhandled here and hit the generic poly method dispatch,
     raising "undefined method 'unpack1' for poly". Mirrors the rt==TY_STRING
     codegen and its inference rule (a single-directive numeric format
     yields an unboxed int or float). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "unpack1") && argc == 1 &&
      !user_defines_or_reads(c, name)) {
    TyKind u1t = repr_of(c, id).as_ty;
    if (oint_kind(u1t)) { oint_open(c, id, u1t, b); buf_printf(b, "%s(", oint_unbox(u1t)); }
    /* the receiver and the format are held first: a value that is no
       String has no unpack1 (NoMethodError, the format as its args),
       where it was read as its #to_s */
    int tu = ++g_tmp, tf = ++g_tmp;
    buf_puts(b, "({ "); tu = hold_operand(c, recv, TY_POLY, 1, tu, 1, " ", b);
    tf = hold_operand(c, argv[0], TY_POLY, 1, tf, 1, " ", b);
    buf_printf(b, "if (SP_UNLIKELY(!(_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d))))"
                  " sp_raise_nomethod(sp_nomethod_msg_args(\"unpack1\", _t%d, 1, &_t%d));"
                  " sp_PolyArray_get(sp_str_unpack(sp_poly_to_s(_t%d), sp_poly_arg_str_chk(_t%d)), 0); })", tu, tu, tu, tf, tu, tf);
    if (oint_kind(u1t)) { buf_puts(b, ")"); oint_close(c, id, b); }
    return 1;
  }
  /* ...and unpack1(fmt, offset: n), as the typed receiver's arm takes it: with
     the keyword the boxed String had no arm and raised NoMethodError (#7317) */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "unpack1") && argc == 2 &&
      nt_kind(nt, argv[1]) == NK_KeywordHashNode && struct_kwarg_value(c, argv[1], "offset") >= 0 &&
      !user_defines_or_reads(c, name)) {
    int offv = struct_kwarg_value(c, argv[1], "offset");
    TyKind u1t = repr_of(c, id).as_ty;
    if (oint_kind(u1t)) { oint_open(c, id, u1t, b); buf_printf(b, "%s(", oint_unbox(u1t)); }
    const int args[2] = { argv[0], offv };
    buf_puts(b, "sp_PolyArray_get(");
    emit_poly_str_transform(c, recv, 2, args, name, "unpack_off", 2, b);
    buf_puts(b, ", 0)");
    if (oint_kind(u1t)) { buf_puts(b, ")"); oint_close(c, id, b); }
    return 1;
  }
  { int r; if (emit_poly_index_call(c, id, b, nt, name, recv, argc, argv, rt, &r)) return r; }
  /* poly receiver: join. Stands down for a user class that defines the name --
     the arm answered the receiver's #to_s where CRuby entered the method, and
     nothing was reported (#4071). The dispatch below builds the cls_id switch
     with an arm for the class and keeps a builtin one for the arrays. */
  /* ...but a NUMERIC argument cannot be a separator (Array#join(5) is a
     TypeError in CRuby), so it is Thread#join(limit) and answers the thread
     or nil rather than a string (#4287). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "join") && argc == 1 &&
      ty_is_numeric(comp_ntype(c, argv[0])) && !user_defines_or_reads(c, name)) {
    buf_puts(b, "sp_poly_join_timeout("); emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_float_expr(c, argv[0], b);
    { TyKind at0 = comp_ntype(c, argv[0]);
      buf_printf(b, ", \"%s\"", at0 == TY_FLOAT ? "Float" : at0 == TY_BIGINT ? "Integer" : "Integer"); }
    buf_puts(b, ")"); return 1;
  }
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "join") &&
      !user_defines_or_reads(c, name)) {
    /* the helper renders a non-container as its to_s, which is right for a
       nested element and wrong for the receiver: nil has no join (#4485),
       and neither has a Hash, a String or a Range */
    /* a program that spawns threads types this call poly (the receiver may
       be a Thread, whose join answers the thread): the boxed form waits on a
       Thread and joins anything else */
    const char *jfn = repr_of(c, id).kind == RK_BOXED ? "sp_poly_join_v" : "sp_poly_join";
    buf_printf(b, "%s(sp_poly_ary_chk(", jfn); emit_expr(c, recv, b);
    buf_puts(b, ", \"join\", 0), "); if (argc >= 1) emit_str_expr_nilable(c, argv[0], b); else buf_puts(b, "sp_str_empty");
    buf_puts(b, ")"); return 1;
  }
  /* poly receiver: clamp(lo, hi) tag-dispatches int/float at runtime; the range
     form clamp(a..b) routes through the same helper with boxed bounds. */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "clamp") && argc == 2) {
    buf_puts(b, "sp_poly_clamp("); emit_boxed(c, recv, b);
    buf_puts(b, ", "); emit_boxed(c, argv[0], b);
    buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
    return 1;
  }
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "clamp") && argc == 1 &&
      nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode")) {
    /* raises on an exclusive range with a real end; routes a user-object
       receiver through the `<=>` hook (sp_obj_clamp_range) */
    /* a Float Range (promote boxes an Integer receiver) clamps on its boxed
       bounds, an omitted side open, as the Range form does */
    if (comp_ntype(c, argv[0]) == TY_FLOAT_RANGE) {
      int tv = ++g_tmp, tf = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_boxed(c, recv, b);
      buf_printf(b, "; sp_FloatRange _t%d = ", tf); emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_poly_recv_ck(_t%d, \"clamp\");"
                    " if (_t%d.excl && !(_t%d.omitted & SP_FRANGE_NO_END))"
                    " sp_raise_cls(\"ArgumentError\", \"cannot clamp with an exclusive range\");"
                    " sp_num_clamp_open(_t%d, (_t%d.omitted & SP_FRANGE_NO_BEGIN) ? sp_box_nil() : sp_box_float(_t%d.first),"
                    " (_t%d.omitted & SP_FRANGE_NO_END) ? sp_box_nil() : sp_box_float(_t%d.last)); })",
                 tv, tf, tf, tv, tf, tf, tf, tf);
      return 1;
    }
    buf_puts(b, "sp_poly_clamp_range("); emit_boxed(c, recv, b);
    buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  /* poly receiver: replace(other) -> runtime dispatch (nullable array). The
     user-definition test its `pack` sibling below already carries: a user
     class owning the name means the runtime class decides, and taking the
     builtin here ran String#replace on an object -- answering the argument,
     silently (#4240). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "replace") && argc == 1 &&
      !user_defines_or_reads(c, name)) {
    /* A bang transform analyze.c lowered to `x.replace(x.transform_values { })`
       names the receiver node twice. Now that the replace reaches a hash, an
       `hs[(i += 1) % 2].transform_values! { }` would write one hash's
       transform into the other: bind the receiver once and let the inner
       call read the temp. A value that is no hash (a `break` out of the
       block) is the call's answer, and nothing is written. */
    if (nt_str(nt, id, "bang_splice") && g_n_argov < MAX_ARG_OVERRIDE) {
      int tv = ++g_tmp;
      tv = hold_operand_pre(c, recv, TY_POLY, 0, tv, 1);
      view_bind(recv, "_t%d", tv);
      buf_printf(b, "sp_poly_hash_splice(_t%d, ", tv); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      view_unbind(g_n_argov - 1);
      return 1;
    }
    /* a plain string box cannot hold the new contents, so an lvalue
       receiver takes the result back, as `insert` does (#3445), when the
       source can be a String; a container is replaced through its pointer
       and answers itself */
    const char *rvtr = nt_type(nt, recv);
    TyKind srt = comp_ntype(c, argv[0]);
    int wb = rvtr && (sp_streq(rvtr, "LocalVariableReadNode") || sp_streq(rvtr, "InstanceVariableReadNode")) &&
             (srt == TY_STRING || srt == TY_POLY);
    if (wb) { buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, " = "); }
    buf_puts(b, "sp_poly_replace_any("); emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
    if (wb) buf_puts(b, ")");
    return 1;
  }
  /* poly receiver: pack(fmt) -> runtime dispatch (nullable array). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "pack") && argc == 1 &&
      !user_defines_or_reads(c, name)) {
    buf_puts(b, "sp_poly_pack("); emit_expr(c, recv, b);
    buf_puts(b, ", ");
    /* a boxed format unboxes to the const char * slot, and a format of
       another class takes the String slot's conversion, as the typed
       receiver's arm does (a TypeError, or the refusal of a Rational) */
    TyKind fmt_t = comp_ntype(c, argv[0]);
    if (fmt_t != TY_STRING) emit_str_expr(c, argv[0], b);
    else emit_expr(c, argv[0], b);
    buf_puts(b, ")");
    c->args_in_call = recv;
    return 1;
  }
  /* poly receiver: delete(chars) -> String#delete on the unboxed payload.
     Mirrors the analyzer's poly rule (result is a concrete TY_STRING): a
     string that widened to poly -- `data[offset, 8].delete("\x00")` stripping
     NUL padding off a fixed-width WAD name field in doom's texture parser.
     Like the analyzer rule, skipped when a user class defines `delete` (the
     per-class poly dispatch below then generates the proper arms). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "delete") && argc == 1 &&
      comp_ntype(c, id) == TY_STRING) {
    int has_user_delete = 0;
    for (int k = 0; k < c->nclasses; k++)
      if (comp_poly_arm_defines_n(c, k, "delete", argc)) { has_user_delete = 1; break; }
    if (!has_user_delete) {
      buf_puts(b, "sp_str_delete(sp_poly_to_s("); emit_expr(c, recv, b);
      buf_puts(b, "), "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
  }

  /* poly receiver: gsub/sub with a regex literal -- extract the string
     payload (poly values reaching here are strings) and route to the
     engine, just like a TY_STRING receiver. */
  if (recv >= 0 && rt == TY_POLY && (is_substitution(name)) &&
      argc == 2 && re_lit_index(c, argv[0]) >= 0) {
    const char *suf = repr_hash_is(repr_of(c, argv[1]), TY_STRING, TY_STRING) ? "_str_str_hash" : "";
    /* A user method sharing the name can type the call poly (#8102):
       the String answer is boxed for that slot. */
    int boxed = comp_ntype(c, id) == TY_POLY;
    if (boxed) buf_puts(b, "sp_box_str(");
    buf_printf(b, "sp_re_%s%s(sp_re_pat_%d, sp_poly_to_s(", name, suf, re_lit_index(c, argv[0]));
    emit_expr(c, recv, b); buf_puts(b, "), ");
    emit_expr(c, argv[1], b); buf_puts(b, ")");
    if (boxed) buf_puts(b, ")");
    return 1;
  }
  return 0;
}

/* Object's universal protocol -- ===, ==, !=, equal?, eql?, frozen?, freeze,
   and on the IO family (a File/IO/File::Stat handle, a Dir handle) to_s and
   <=> as well -- on the native handle and value kinds that have no arm of
   their own. Reached from the tails of emit_call and emit_case_eq_call (and
   from the MatchData and OpenStruct arms and the case/when emitter, which
   delegate), after every typed arm and the user-method routing declined, so
   there it can never shadow a more specific answer: it turns a front-end
   rejection into the answer Ruby gives. The IO family is the exception: its
   site in emit_call runs early, ahead of the generic spaceship and to_s
   fallbacks that would otherwise claim the call, and like the rest of the IO
   surface it does not consult a user reopening of IO or Dir. WHICH calls it
   answers is decided once, in ty_object_protocol_answers (types.c), which
   infer_call types from as well.

   to_s is Object's #<Class:0xADDR> under the class the handle presents as
   (#inspect stays the handle's own render). <=> is Object's identity answer,
   0 or nil, boxed -- except two File::Stat handles, which Comparable orders by
   modification time, the same reading == takes for them (sp_io_cmp).

   Two semantics, per CRuby. A heap handle (Fiber, Thread, Queue, Mutex,
   ConditionVariable, Dir, Addrinfo, IO, Enumerator, Method, Exception,
   MatchData, a curried Proc) IS its pointer: the predicates are pointer
   identity and frozen? reads the bit freeze sets (the GC header, or the
   handle's own flag for an IO, whose standard streams are static storage).
   Random, OpenStruct, Exception, File::Stat, Time and Process::Tms answer ==
   and === structurally (state, members, class and message, modification
   time, instant, fields); eql? and equal? stay identity where the value has
   one, and OpenStruct's eql? is its table's. Time, Tms and a String range
   are by-value structs: equal? and (bar the Range, always frozen) frozen?
   are not answered for them, and `freeze` keeps the identity no-op it had.

   Evaluation order is Ruby's: the receiver first, into a rooted temp, then
   the operand, then the test -- an allocating operand must not collect a
   receiver held only in the expression. A poly operand is unwrapped in
   place: same tag, same builtin id, then the pointer or structural test. A
   kind with no builtin id (Random) has no boxed form at all, so a poly slot
   never holds one and the answer is false. */
static void emit_native_object_protocol_text(Compiler *c, const char *name, TyKind rt,
                                             const char *r, TyKind at, const char *a, Buf *b) {
  if (sp_streq(name, "to_s")) {
    buf_printf(b, "%s(%s)", rt == TY_IO ? "sp_io_to_s" : "sp_Dir_to_s", r);
    return;
  }
  Buf ct; memset(&ct, 0, sizeof ct); emit_ctype(c, rt, &ct);
  const char *cty = ct.p ? ct.p : "void *";
  if (sp_streq(name, "<=>")) {
    /* a value of another static kind is never the same object and cannot be
       a stat, and a NULL handle is nil, which is not it either: nil, with
       both sides evaluated for their effects and nothing boxed (a Random has
       no boxed form at all) */
    if (at != rt && at != TY_NIL && at != TY_POLY && at != TY_UNKNOWN) {
      buf_printf(b, "((void)(%s), (void)(%s), sp_box_nil())", r, a);
      free(ct.p);
      return;
    }
    /* receiver first, rooted across the operand, which may allocate; then one
       runtime reading of the operand as a boxed value: a same-kind operand
       and nil are boxed, a poly one already is */
    int t = ++g_tmp;
    Buf ab; memset(&ab, 0, sizeof ab);
    if (at == rt || at == TY_NIL) emit_boxed_text(c, at, a, &ab);
    else buf_puts(&ab, a);
    buf_printf(b, "({ %s _t%d = %s; SP_GC_ROOT(_t%d); sp_RbVal _u%d = %s; %s(_t%d, _u%d); })",
               cty, t, r, t, t, ab.p ? ab.p : a, rt == TY_IO ? "sp_io_cmp" : "sp_Dir_cmp", t, t);
    free(ab.p);
    free(ct.p);
    return;
  }
  int kind = ty_object_protocol_kind(rt);
  if (sp_streq(name, "frozen?")) {
    if (rt == TY_IO) buf_printf(b, "sp_io_frozen(%s)", r);
    else if (kind == 1) {
      int t = ++g_tmp;
      buf_printf(b, "({ %s _t%d = %s; _t%d ? sp_gc_is_frozen((void *)_t%d) : (sp_bool)1; })", cty, t, r, t, t);
    }
    else buf_printf(b, "((void)(%s), (sp_bool)1)", r);
    free(ct.p);
    return;
  }
  if (sp_streq(name, "freeze")) {
    if (rt == TY_IO) buf_printf(b, "sp_io_freeze(%s)", r);
    else buf_printf(b, "((%s)sp_gc_freeze((void *)(%s)))", cty, r);
    free(ct.p);
    return;
  }
  int is_ne = sp_streq(name, "!=");
  int is_eql = sp_streq(name, "eql?");
  int is_equal = sp_streq(name, "equal?");
  /* the cross-family tier: a Range, Array or Bignum against another family */
  if (kind == 0) {
    if (ty_is_array(rt) && ty_is_array(at)) {
      Buf rb; memset(&rb, 0, sizeof rb); emit_boxed_text(c, rt, r, &rb);
      Buf ab; memset(&ab, 0, sizeof ab); emit_boxed_text(c, at, a, &ab);
      buf_printf(b, "(%s%s(%s, %s))", is_ne ? "!" : "", is_eql ? "sp_poly_eql" : "sp_poly_eq",
                 rb.p ? rb.p : r, ab.p ? ab.p : a);
      free(rb.p); free(ab.p);
    }
    else buf_printf(b, "((void)(%s), (void)(%s), (sp_bool)%d)", r, a, is_ne ? 1 : 0);
    free(ct.p);
    return;
  }
  /* which comparisons look inside the value rather than at its address */
  const char *fn = NULL;
  if (rt == TY_RANDOM && !is_eql && !is_equal) fn = "sp_Random_eq";
  else if (rt == TY_OPENSTRUCT && !is_equal) fn = is_eql ? "sp_OpenStruct_eql" : "sp_OpenStruct_eq";
  else if (rt == TY_EXCEPTION && !is_eql && !is_equal) fn = "sp_exc_eq";
  else if (rt == TY_IO && !is_eql && !is_equal) fn = "sp_io_eq";
  else if (rt == TY_MATCHDATA && !is_eql && !is_equal) fn = "sp_MatchData_eq";
  int t = ++g_tmp;
  /* receiver, then operand, into temps; then the test (negated for !=) */
  buf_printf(b, "({ %s _t%d = %s; ", cty, t, r);
  if (kind == 1) buf_printf(b, "SP_GC_ROOT(_t%d); ", t);
  Buf test; memset(&test, 0, sizeof test);
  if (at == rt) {
    buf_printf(b, "%s _u%d = %s; ", cty, t, a);
    if (kind == 2 && rt == TY_TMS)
      buf_printf(&test, "(_t%d.utime == _u%d.utime && _t%d.stime == _u%d.stime && "
                        "_t%d.cutime == _u%d.cutime && _t%d.cstime == _u%d.cstime)", t, t, t, t, t, t, t, t);
    else if (kind == 2 && rt == TY_TIME) buf_printf(&test, "(sp_time_cmp(_t%d, _u%d) == 0)", t, t);
    else if (kind == 2) buf_printf(&test, "sp_srange_eq(_t%d, _u%d)", t, t);
    else if (fn) buf_printf(&test, "%s(_t%d, _u%d)", fn, t, t);
    else buf_printf(&test, "(_t%d == _u%d)", t, t);
  }
  else if (at == TY_POLY && kind == 2) {
    Buf rb; memset(&rb, 0, sizeof rb);
    char tref[24]; snprintf(tref, sizeof tref, "_t%d", t);
    emit_boxed_text(c, rt, tref, &rb);
    buf_printf(b, "sp_RbVal _u%d = %s; ", t, a);
    buf_printf(&test, "%s(%s, _u%d)", is_eql ? "sp_poly_eql" : "sp_poly_eq", rb.p ? rb.p : tref, t);
    free(rb.p);
  }
  else if (at == TY_POLY) {
    const char *bid = ty_nullable_builtin_id(rt);
    buf_printf(b, "sp_RbVal _u%d = %s; ", t, a);
    if (!bid) buf_printf(&test, "((void)_u%d, 0)", t);
    else {
      buf_printf(&test, "(_u%d.tag == SP_TAG_OBJ && _u%d.cls_id == %s && ", t, t, bid);
      if (fn) buf_printf(&test, "%s(_t%d, (%s)_u%d.v.p))", fn, t, cty, t);
      else buf_printf(&test, "_u%d.v.p == (void *)_t%d)", t, t);
    }
  }
  else if (at == TY_NIL && kind == 1) {
    /* A pointer-backed handle IS nil when it is NULL in this backend: that is
       what `nil?` and the dedicated `handle != nil` arm both answer. `== nil`
       reached the other-kind arm below and folded to false, so a slot holding
       nil answered false to `== nil` and false to `!= nil` at the same time
       (an empty pipe's `wait_readable(0)`). */
    buf_printf(b, "(void)(%s); ", a);
    buf_printf(&test, "(_t%d == NULL)", t);
  }
  else {
    /* a value of another static kind is never the same object, nor equal */
    buf_printf(b, "(void)(%s); ", a);
    buf_puts(&test, "0");
  }
  buf_printf(b, "(sp_bool)%s(%s); })", is_ne ? "!" : "", test.p ? test.p : "0");
  free(test.p);
  free(ct.p);
}

int emit_native_object_protocol(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !name || nt_ref(nt, id, "block") >= 0) return 0;
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = comp_recv_type(c, recv);
  TyKind at = argc == 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  if (!ty_object_protocol_answers(rt, at, name, argc)) return 0;
  /* a rescued value is typed TY_EXCEPTION, not as its user class: a user
     exception subclass defining the method keeps it */
  if (rt == TY_EXCEPTION && exc_subclass_defines(c, name)) return 0;
  Buf rs = expr_buf(c, recv);
  Buf as; memset(&as, 0, sizeof as);
  if (argc == 1) as = expr_buf(c, argv[0]);
  emit_native_object_protocol_text(c, name, rt, rs.p ? rs.p : "0", at, as.p ? as.p : "0", b);
  free(rs.p); free(as.p);
  return 1;
}

/* `case subj when cond`: Ruby asks `cond === subj`. The case/when emitter
   calls this before its raw C `==` fallback so a native kind answers the
   same way there as an explicit `===` does (subj_ref is the subject's temp). */
int emit_native_case_eq(Compiler *c, int cond, TyKind subj_t, const char *subj_ref, Buf *b) {
  TyKind ct = comp_ntype(c, cond);
  if (!ty_object_protocol_answers(ct, subj_t, "===", 1)) return 0;
  if (ct == TY_EXCEPTION && exc_subclass_defines(c, "===")) return 0;
  Buf cs = expr_buf(c, cond);
  emit_native_object_protocol_text(c, "===", ct, cs.p ? cs.p : "0", subj_t, subj_ref, b);
  free(cs.p);
  return 1;
}
