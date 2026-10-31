/* repr.c -- a value's C representation, read off the analysis's flags
   (see repr.h). Pure: it reads the types and the flags and changes
   nothing. */

#include <string.h>
#include <limits.h>
#include "repr.h"
#include "value_pos.h"
#include "analyze_internal.h"
#include "codegen_internal.h"
#include "share.h"
#include "holder.h"
#include "call_plan.h"
#include "builtin_ops.h"

/* The reader method `nm` resolved on class cid: the ivar it answers, when
   that ivar carries a nil bit (its read is an sp_oint). */
static int reader_ivar_has_nilbit(Compiler *c, int cid, const char *nm) {
  int defc = -1;
  if (cid < 0 || cid >= c->nclasses || !nm || !comp_reader_in_chain(c, cid, nm, &defc)) return 0;
  char ivb[300];
  snprintf(ivb, sizeof ivb, "@%s", comp_resolve_alias(c, cid, nm));
  int k = defc >= 0 ? defc : cid;
  int iv = comp_ivar_index(&c->classes[k], ivb);
  return iv >= 0 && ivar_has_nilbit(c, k, iv);
}

/* the number of parent hops from class cid up to class k (a large value
   when k is not an ancestor) */
static int class_chain_depth(Compiler *c, int cid, int k) {
  int d = 0;
  for (int x = cid; x >= 0 && x < c->nclasses; x = c->classes[x].parent, d++) if (x == k) return d;
  return 1 << 20;
}

/* Call `call` on an object of class `cid` dispatches to `mi` or to a
   subclass's own `nm`: can one of them answer nil -- its answer is an oint,
   or it answers nil itself (`def nop = nil` beside an override answering
   7)? The dispatch switch then holds the oint (g_disp_ro). mi is -1 for an
   attr reader, whose own answer is its field's (reader_ivar_has_nilbit). */
static int dispatch_answers_nil(Compiler *c, int call, int cid, const char *nm, int mi) {
  if (mi >= 0 && method_ret_is_oint(&c->scopes[mi])) return 1;
  int any_nil = mi >= 0 && (c->scopes[mi].ret == TY_NIL || c->scopes[mi].ret == TY_VOID), overridden = 0;
  int nd = 0; const int *ds = comp_descendants(c, cid, &nd);
  for (int k = 0; k < nd; k++) {
    int odef = -1, omi = call_target(c, call, ds[k], nm, 0, &odef);
    if (omi < 0 || omi == mi || odef != ds[k]) continue;
    overridden = 1;
    if (method_ret_is_oint(&c->scopes[omi])) return 1;
    if (c->scopes[omi].ret == TY_NIL || c->scopes[omi].ret == TY_VOID) any_nil = 1;
  }
  return overridden && any_nil;
}

Repr repr_array_operand(Compiler *c, int node) {
  Repr r = repr_of(c, node);
  if (nt_kind(c->nt, node) == NK_LocalVariableReadNode) {
    Scope *sc = comp_scope_of(c, node);
    LocalVar *lv = sc ? scope_local(sc, nt_str(c->nt, node, "name")) : NULL;
    if (lv && lv->type != TY_UNKNOWN && comp_ty_ary_root(c, lv->type) < 0) r = repr_of_slot(c, lv);
  }
  return r;
}

int file_stat_nil_call(Compiler *c, int v);   /* analyze.c */
static int node_is_oint_asked(Compiler *c, int node);

/* Codegen asks this while a view overrides a node's type (a re-entered
   call on its receiver boxed), and the analysis it asks (nullable_int_value)
   re-derives a receiver's type: asked as a pure read, as repr_of asks
   (an_pure_read_begin), so the derived type is not recorded over the
   view's. */
int node_is_oint(Compiler *c, int node) {
  an_pure_read_begin();
  int r = node_is_oint_asked(c, node);
  an_pure_read_end();
  return r;
}
static int node_is_oint_asked(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return 0;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_NilNode) return 1;
  TyKind t = comp_ntype(c, node);
  if (!oint_kind(t)) return 0;
  switch (k) {
  case NK_LocalVariableReadNode: {
    /* the slot's own oint, unless a nil guard narrowed this read */
    const char *ln = nt_str(nt, node, "name");
    Scope *s = ln ? comp_scope_of(c, node) : NULL;
    LocalVar *lv = s ? scope_local(s, ln) : NULL;
    return lv && slot_is_oint(lv) && nullable_int_value(c, node);
  }
  case NK_InstanceVariableReadNode: {
    /* inside an instance_eval / instance_exec splice whose receiver's class
       has no such ivar (or a receiver that is no object): the read is nil
       there, whatever the analysis says of the name elsewhere */
    if (oint_kind(comp_ntype(c, node))) {
      if (g_ie_nil_ivars) return 1;
      Scope *ics = comp_scope_of(c, node);
      int iec = ie_class_of(c, node);
      if (iec < 0 && ics && ics->class_id < 0 && !ics->is_cmethod) iec = g_ie_class_id;
      int icid, iiv;
      if (iec >= 0 && !ivar_node_slot(c, node, &icid, &iiv)) return 1;
    }
    return nullable_int_value(c, node);
  }
  case NK_ClassVariableReadNode:
  case NK_GlobalVariableReadNode:
    return nullable_int_value(c, node);
  case NK_LocalVariableAndWriteNode: {
    /* `x &&= v` answers the slot: its oint where the slot holds its nil
       (the untaken arm answers the nil the slot held) */
    const char *ln = nt_str(nt, node, "name");
    Scope *s = ln ? comp_scope_of(c, node) : NULL;
    LocalVar *lv = s ? scope_local(s, ln) : NULL;
    return lv && slot_is_oint(lv);
  }
  case NK_InstanceVariableWriteNode: {
    /* `@x = v` as an expression answers the slot it wrote: its oint where
       the field carries a nil bit (or the static is an oint), unless v is
       a plain value, which clears the bit (emit_ivar_value_nilbit) and is
       the answer; the `||=` / `&&=` / `op=` value forms answer by the
       analysis (default) */
    if (!ivar_read_slot_is_oint(c, node)) return 0;
    int wcid, wiv, v = nt_ref(nt, node, "value");
    if (ivar_node_slot(c, node, &wcid, &wiv) == 1 && v >= 0 && nt_kind(nt, v) != NK_NilNode &&
        !ivar_value_carries_nil(c, c->classes[wcid].ivar_types[wiv], v)) return 0;
    return 1;
  }
  case NK_GlobalVariableWriteNode:
  case NK_GlobalVariableOrWriteNode:
  case NK_GlobalVariableAndWriteNode:
  case NK_GlobalVariableOperatorWriteNode: {
    /* a global's write answers its static: the oint where that is one */
    const char *gn = nt_str(nt, node, "name");
    const char *grn = gn && gn[0] == '$' ? comp_resolve_gvar(c, gn + 1) : gn;
    LocalVar *g = grn ? comp_gvar(c, grn) : NULL;
    return g && gvar_is_oint(c, g);
  }
  case NK_ClassVariableWriteNode:
  case NK_ClassVariableOrWriteNode:
  case NK_ClassVariableAndWriteNode:
  case NK_ClassVariableOperatorWriteNode: {
    HolderRef h;
    if (!holder_of_node_in(c, node, g_class_body_id, &h)) return 0;
    return cvar_is_oint(c, h.cid, h.idx);
  }
  case NK_IndexOrWriteNode:
  case NK_IndexAndWriteNode:
    /* `h[k] ||= v`: the slot read with its nil (the `&&=` arm keeps it) */
    return nullable_int_value(c, node);
  case NK_ParenthesesNode: {
    /* `(expr)` is its expression's form */
    int u = nt_paren_single(c->nt, node);
    return u != node && u >= 0 ? node_is_oint(c, u) : nullable_int_value(c, node);
  }
  case NK_CallOrWriteNode:
  case NK_CallAndWriteNode: {
    /* `o.x ||= v` / `o.x &&= v` answers what the reader answers on the arm
       that does not assign: the reader's oint when the reader has one (a
       method written out answering its nil, an attr reader over a nil bit) */
    int r = nt_ref(nt, node, "receiver");
    const char *nm = nt_str(nt, node, "name");
    TyKind rt = r >= 0 ? comp_ntype(c, r) : TY_UNKNOWN;
    if (r < 0 || !nm || !ty_is_object(rt)) return nullable_int_value(c, node);
    int cid = ty_object_class(rt), rmi = -1;
    int rk = comp_resolve_member(c, cid, nm, 0, NULL, &rmi);
    if (rk == SP_MEMBER_METHOD && rmi >= 0) return method_ret_is_oint(&c->scopes[rmi]);
    /* an attr's `||=` stores a nil field's replacement and answers it: nil
       only where the value can be */
    if (rk == SP_MEMBER_ATTR && k == NK_CallOrWriteNode) {
      int v = nt_ref(nt, node, "value");
      return v >= 0 && (nt_kind(nt, v) == NK_NilNode || node_has_oint_form(c, v));
    }
    if (rk == SP_MEMBER_ATTR) return reader_ivar_has_nilbit(c, cid, nm);
    return nullable_int_value(c, node);
  }
  case NK_CallNode: {
    /* `r&.m`: nil when r is -- except on the safe-navigation emitter's
       re-entry for the same node (g_sn_skip), which emits the plain call on
       the guarded receiver and wraps it itself */
    if (nt_safe_nav(nt, node) && g_sn_skip != node) {
      /* nil when the receiver is -- only where a guard stands (a receiver
         that cannot be nil, `3&.fdiv(2)`, has neither guard nor nil) */
      if (sn_guard_pending(c, node)) return 1;
      /* (--share-strings: so is one on a handle, which the guard holds,
         `id(s)&.setbyte(0, 90)`, as emit_call_safe_nav_arms guards it) */
      int sr = nt_ref(nt, node, "receiver");
      if (sr >= 0 && repr_of(c, sr).as_ty == TY_STRBUF && repr_of(c, sr).kind != RK_BOXED) return 1;
    }
    const char *nm = view_call_name(c, node);
    if (!nm) return 0;
    /* a File::Stat field and File.size?: the runtime answers the oint */
    if (file_stat_nil_call(c, node)) return 1;
    /* a call on true or false where TrueClass or FalseClass defines the
       name: the oint where a side's method answers one (emit_call_bool_sides) */
    { int br = nt_ref(nt, node, "receiver"), btm, bfm;
      if (br >= 0 && comp_ntype(c, br) == TY_BOOL && cplan_bool_sides(c, node, &btm, &bfm) &&
          ((btm >= 0 && method_ret_is_oint(&c->scopes[btm])) || (bfm >= 0 && method_ret_is_oint(&c->scopes[bfm]))))
        return 1; }
    /* `o.x = v` as a value answers v, whatever the writer returns */
    if (call_is_setter_assign(nt, node)) {
      /* ...except a native class's binding (StringScanner#pos=), which the
         call emitter answers with the binding's own return, typed by its
         spec (analyze_infer_recv): its Integer argument is converted
         strictly, so a nil one raises before the binding answers */
      int sr = nt_ref(nt, node, "receiver");
      TyKind srt = sr >= 0 ? comp_ntype(c, sr) : TY_UNKNOWN;
      int snk = ty_is_object(srt) ? comp_native_answer_class(c, ty_object_class(srt), nm) : -1;
      if (snk >= 0 && comp_native_method_find(c, snk, nm, 1, 0) >= 0) return 0;
      int sa = nt_ref(nt, node, "arguments"), san = 0;
      const int *sav = sa >= 0 ? nt_arr(nt, sa, "arguments", &san) : NULL;
      if (sav && san >= 1) return nt_kind(nt, sav[san - 1]) == NK_NilNode || nullable_int_value(c, sav[san - 1]);
    }
    int blk = nt_ref(nt, node, "block");
    int r = nt_ref(nt, node, "receiver");
    TyKind rt = r >= 0 ? comp_ntype(c, r) : TY_UNKNOWN;
    int a2 = nt_ref(nt, node, "arguments"), an2 = 0;
    if (a2 >= 0) nt_arr(nt, a2, "arguments", &an2);
    /* `x.then { ... }` answers its block: a nil tail or a `next` that can
       hand nil makes it nil */
    if ((sp_streq(nm, "then") || sp_streq(nm, "yield_self")) && blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) {
      int bb = nt_ref(nt, blk, "body");
      int bn = 0; const int *bs = bb >= 0 ? nt_arr(nt, bb, "body", &bn) : NULL;
      if (bn > 0 && (node_has_oint_form(c, bs[bn - 1]) || nt_kind(nt, bs[bn - 1]) == NK_NilNode)) return 1;
      return bb >= 0 && block_next_may_be_nil(c, bb, 0);
    }
    /* `a[i] = v` on an Integer / Float array answers v as it was stored:
       with its nil (a nil, a boxed value, an oint) -- emit_array_call's
       store; so does `h[k] = v` on an Integer-valued typed Hash, which
       stores it through _oset (D3b-ii) */
    if (sp_streq(nm, "[]=") && r >= 0 && an2 >= 2 &&
        (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY || rt == TY_STR_INT_HASH || rt == TY_INT_INT_HASH)) {
      int sa = nt_ref(nt, node, "arguments"), san = 0;
      const int *sav = nt_arr(nt, sa, "arguments", &san);
      int lv = sav[san - 1];
      return nt_kind(nt, lv) == NK_NilNode || repr_of(c, lv).kind == RK_BOXED || node_has_oint_form(c, lv);
    }
    /* `x.tap { ... }` answers its receiver, in the receiver's form */
    if (sp_streq(nm, "tap") && blk >= 0 && nt_kind(nt, blk) == NK_BlockNode && r >= 0 && oint_kind(rt)) return node_is_oint(c, r);
    /* unary `+` hands an Integer / Float its operand: the operand's form */
    if (sp_streq(nm, "+@") && an2 == 0 && r >= 0 && oint_kind(rt)) return node_is_oint(c, r);
    /* a proc's result comes back boxed and is unboxed with its nil */
    if (is_call_or_yield(nm) && r >= 0 && rt == TY_PROC) return 1;
    /* `o.instance_eval { ... }` spliced over an object answers its body's
       last expression in that expression's form */
    TyKind iert = rt;
    if (r < 0) { Scope *ies = comp_scope_of(c, node); if (ies && ies->class_id >= 0 && !ies->is_cmethod) iert = ty_object(ies->class_id); }
    /* over a boxed receiver the splice runs per class, and a receiver with no
       such ivar (nil, a builtin, Object.new) reads it nil: an Integer / Float
       tail can be nil -- so too over a typed receiver that is no object of
       the program's (nil, a number, a String) */
    if ((sp_streq(nm, "instance_eval") || sp_streq(nm, "instance_exec")) && blk >= 0 && r >= 0 &&
        (rt == TY_POLY || (rt != TY_UNKNOWN && !ty_is_object(rt))) && nt_kind(nt, blk) == NK_BlockNode) {
      int bb = nt_ref(nt, blk, "body");
      int bn = 0; const int *bs = bb >= 0 ? nt_arr(nt, bb, "body", &bn) : NULL;
      return bn > 0 && oint_kind(comp_ntype(c, bs[bn - 1]));
    }
    if ((sp_streq(nm, "instance_eval") || sp_streq(nm, "instance_exec")) && blk >= 0 &&
        ty_is_object(iert) && call_target(c, node, ty_object_class(iert), nm, 0, NULL) < 0 &&
        nt_kind(nt, blk) == NK_BlockNode) {
      int bb = nt_ref(nt, blk, "body");
      int bn = 0; const int *bs = bb >= 0 ? nt_arr(nt, bb, "body", &bn) : NULL;
      return (bn > 0 && node_is_oint(c, bs[bn - 1])) || (bb >= 0 && block_next_may_be_nil(c, bb, 0));
    }
    /* Array's, Hash's or String's own method on an instance of a subclass
       of one (comp_arysub_call, a `super` into it too): the rows below for
       the container the instance holds, as the call's emitter reads it
       (emit_arysub_call) */
    { TyKind ak = TY_UNKNOWN;
      if (r >= 0 && comp_arysub_call(c, node, rt, &ak) && (ty_is_array(ak) || ty_is_hash(ak) || ak == TY_STRING))
        rt = ak; }
    /* a method the program defines, resolved as the call emitter resolves
       it: its C function answers an oint iff method_ret_is_oint */
    int mi = -1;
    /* the builtin a reopened IO class overrides, emitted as that builtin
       (emit_io_builtin_call): the builtin's own rules below */
    int io_bi = g_io_skip_reopen && g_io_skip_node == node;
    int src = r < 0 && !io_bi && an2 == 0 && blk < 0 ? implicit_self_reader_cid(c, node) : -1;
    if (io_bi) ;
    /* a bare call of the class's own attr reader: the oint where the ivar
       carries a nil bit or a subclass's override can answer nil (the read
       dispatches, emit_reader_override_dispatch), the plain field
       otherwise, as with an explicit receiver below */
    else if (src >= 0) return reader_ivar_has_nilbit(c, src, nm) || dispatch_answers_nil(c, node, src, nm, -1);
    else if (r < 0) mi = comp_self_call_mi(c, node, nm);
    else if (ty_is_object(rt)) {
      int cid = ty_object_class(rt);
      int mdef = -1, rdef = -1;
      mi = call_target(c, node, cid, nm, 0, &mdef);
      /* an attr reader is its ivar's read: the ivar's oint when the ivar
         carries a nil bit, the plain field otherwise (whatever the reader's
         own return mark says: the field has no other form) -- unless a
         method written out nearer in the chain overrides the reader */
      if (an2 == 0 && blk < 0 && comp_reader_in_chain(c, cid, nm, &rdef) &&
          (mi < 0 || (mdef != cid &&
                      class_chain_depth(c, cid, rdef < 0 ? cid : rdef) <= class_chain_depth(c, cid, mdef < 0 ? cid : mdef))))
        return reader_ivar_has_nilbit(c, cid, nm) || dispatch_answers_nil(c, node, cid, nm, -1);
    }
    else if (nt_kind(nt, r) == NK_ConstantReadNode || nt_kind(nt, r) == NK_ConstantPathNode) {
      int rci = const_node_class_index(c, r);
      if (rci >= 0) mi = call_target(c, node, rci, nm, CT_CMETHOD, NULL);
    }
    /* `obj.class.m`: the switch over obj's class and the classes below it
       (emit_call_class_method_arms) */
    else if (nt_kind(nt, r) == NK_CallNode && sp_streq(nt_str(nt, r, "name"), "class") &&
             nt_ref(nt, r, "arguments") < 0) {
      int x = nt_ref(nt, r, "receiver");
      TyKind xt = x >= 0 ? comp_ntype(c, x) : TY_UNKNOWN;
      if (nullable_int_value(c, node)) return 1;
      if (ty_is_object(xt) && call_target(c, node, ty_object_class(xt), nm, CT_CMETHOD, NULL) >= 0)
        return class_switch_arm_oint(c, node, ty_object_class(xt), nm);
    }
    /* a class or module value only the run time knows: the switch over the
       classes with a class method of the name (#8985) */
    else if (rt == TY_CLASS && nullable_int_value(c, node)) return 1;
    /* a builtin value's method the program reopened its class with
       (`class Symbol; def mark = ...`) */
    if (mi < 0 && !io_bi && r >= 0 && rt != TY_POLY && rt != TY_UNKNOWN && !ty_is_object(rt))
      mi = comp_builtin_kind_reopen_mi(c, rt, nm);
    /* a method whose return widened past the call's (a yielding method
       answering its block, typed per call site): the call's own analysis */
    if (mi >= 0 && (c->scopes[mi].ret == TY_POLY || c->scopes[mi].ret == TY_UNKNOWN)) return nullable_int_value(c, node);
    /* the dispatch to a subclass's override, on an object receiver or on
       an instance method's self */
    if (mi >= 0 && oint_kind(comp_ntype(c, node))) {
      int dcid = -1;
      if (r >= 0 && ty_is_object(rt)) dcid = ty_object_class(rt);
      else if (r < 0) { Scope *self = comp_scope_of(c, node); if (self && !self->is_cmethod) dcid = self->class_id; }
      if (dcid >= 0 && dispatch_answers_nil(c, node, dcid, nm, mi)) return 1;
    }
    /* ...and a class method's call of a sibling: an inherited body runs for
       its class and each subclass, each calling its own class's method of the
       name, and one of them answering nil (`def self.dp = nil` in Base, 80 in
       Sub) makes the site's Integer nil there */
    if (r < 0 && oint_kind(comp_ntype(c, node))) {
      Scope *self = comp_scope_of(c, node);
      if (self && self->is_cmethod && self->class_id >= 0 && cmethod_sibling_answers_nil(c, node, self->class_id, nm)) return 1;
    }
    if (mi >= 0) return method_ret_is_oint(&c->scopes[mi]);
    /* `<=>` answers nil for an incomparable operand: the analysis's answer
       where both sides are of one comparable kind (numbers, Strings,
       Symbols); any other pairing can answer nil at run time */
    /* a native class's method declared to answer an Integer or a Float */
    if (native_call_ret_plain_num(c, node)) return 0;
    if (sp_streq(nm, "<=>") && an2 == 1) {
      int a1 = nt_ref(nt, node, "arguments"), a1n = 0;
      const int *a1a = a1 >= 0 ? nt_arr(nt, a1, "arguments", &a1n) : NULL;
      TyKind at = a1a && a1n == 1 ? comp_ntype(c, a1a[0]) : TY_UNKNOWN;
      /* a builtin receiver with no `<=>` of its own (a Regexp, a Proc, a
         Queue, an exception) takes a reopened Object's: its answer's form */
      if (r >= 0 && rt != TY_POLY && rt != TY_UNKNOWN && !ty_is_object(rt) && !oint_kind(rt) &&
          rt != TY_BIGINT && rt != TY_RATIONAL && rt != TY_STRING && rt != TY_SYMBOL && rt != TY_TIME &&
          !ty_is_array(rt)) {
        int oc = comp_core_class(c, CORE_OBJECT);
        int omi = oc >= 0 ? call_target(c, node, oc, nm, 0, NULL) : -1;
        if (omi >= 0) return method_ret_is_oint(&c->scopes[omi]);
      }
      int num_l = rt == TY_INT || rt == TY_FLOAT || rt == TY_BIGINT || rt == TY_RATIONAL;
      int num_r = at == TY_INT || at == TY_FLOAT || at == TY_BIGINT || at == TY_RATIONAL;
      /* a Float side can be NaN; a non-literal String side can be nil */
      int same = (num_l && num_r && rt != TY_FLOAT && at != TY_FLOAT) ||
                 (rt == TY_STRING && at == TY_STRING) || (rt == TY_SYMBOL && at == TY_SYMBOL) ||
                 (rt == TY_TIME && at == TY_TIME);
      return same ? nullable_int_value(c, node) : 1;
    }
    /* `s[0]` / `s[:a]` / `s["a"]` / `s.dig(:a)` on a Struct or Data: the
       member's read, with its nil where the member carries a nil bit */
    if ((sp_streq(nm, "[]") || sp_streq(nm, "dig")) && an2 == 1 && r >= 0 && ty_is_object(rt)) {
      int scid = ty_object_class(rt);
      ClassInfo *sci = &c->classes[scid];
      if (sci->is_struct || sci->is_data) {
        int a1 = nt_ref(nt, node, "arguments"), a1n = 0;
        const int *a1a = a1 >= 0 ? nt_arr(nt, a1, "arguments", &a1n) : NULL;
        int mix = -1;
        if (a1a && a1n == 1) {
          NodeKind ak = nt_kind(nt, a1a[0]);
          if (ak == NK_IntegerNode && !sp_streq(nm, "dig")) {
            long long ix = nt_int(nt, a1a[0], "value", 0);
            if (ix < 0) ix += sci->nmembers;
            if (ix >= 0 && ix < sci->nmembers) mix = (int)ix;
          }
          else if (ak == NK_SymbolNode || ak == NK_StringNode) {
            const char *mn = ak == NK_SymbolNode ? nt_str(nt, a1a[0], "value") : nt_str(nt, a1a[0], "content");
            char ivb[300]; snprintf(ivb, sizeof ivb, "@%s", mn ? mn : "");
            int iv = mn ? comp_ivar_index(sci, ivb) : -1;
            if (iv >= 0 && iv < sci->nmembers) mix = iv;
          }
        }
        if (mix >= 0) return ivar_has_nilbit(c, scid, mix);
      }
    }
    /* `o.instance_variable_get(:@x)` on a typed object: the field's read,
       with its nil where the field carries a nil bit (as an attr reader);
       `o.remove_instance_variable(:@x)` answers the same read */
    if ((sp_streq(nm, "instance_variable_get") || sp_streq(nm, "remove_instance_variable")) &&
        an2 == 1 && r >= 0 && ty_is_object(rt)) {
      int a1 = nt_ref(nt, node, "arguments"), a1n = 0;
      const int *a1a = a1 >= 0 ? nt_arr(nt, a1, "arguments", &a1n) : NULL;
      if (a1a && a1n == 1) {
        NodeKind ak = nt_kind(nt, a1a[0]);
        const char *sn = ak == NK_SymbolNode ? nt_str(nt, a1a[0], "value")
                       : ak == NK_StringNode ? nt_str(nt, a1a[0], "content") : NULL;
        if (sn) {
          char ivb[300]; snprintf(ivb, sizeof ivb, "%s%s", sn[0] == '@' ? "" : "@", sn);
          int cid = ty_object_class(rt);
          int iv = comp_ivar_index(&c->classes[cid], ivb);
          if (iv >= 0) return ivar_has_nilbit(c, cid, iv);
        }
      }
    }
    /* `o.instance_variable_set(:@x, v)` answers v: its form */
    if (sp_streq(nm, "instance_variable_set") && an2 == 2 && r >= 0) {
      int a1 = nt_ref(nt, node, "arguments"), a1n = 0;
      const int *a1a = a1 >= 0 ? nt_arr(nt, a1, "arguments", &a1n) : NULL;
      return a1a && a1n == 2 && (nt_kind(nt, a1a[1]) == NK_NilNode || node_has_oint_form(c, a1a[1]));
    }
    /* a receiver that stayed poly: the dispatch answers an oint when a
       target can answer nil (the analysis's dispatch set) -- except on the
       safe-navigation re-entry (g_sn_skip), where the analysis mark belongs
       to the `&.` as a whole and the inner emitter's own form decides */
    if ((rt == TY_POLY || (r < 0 && !ty_is_object(rt))) && g_sn_skip != node) {
      /* in the call's own builtin arm (a dispatch re-entering it with
         g_poly_builtin_arm) the program's methods of the name are no
         targets: the builtin's own answer, which the arm lifts to the
         dispatch's oint (emit_oint_expr) when it is plain */
      int sv_bc = g_nn_builtin_call;
      if (g_poly_builtin_arm && g_pd_skip == node) g_nn_builtin_call = node;
      int nn = nullable_int_value(c, node);
      g_nn_builtin_call = sv_bc;
      if (nn) return 1;
    }
    /* `Integer(x, exception: false)` / `Float(x, exception: false)`: nil on
       a failed conversion */
    if (r < 0 && (sp_streq(nm, "Integer") || sp_streq(nm, "Float")) && an2 >= 2) {
      int a2v = nt_ref(nt, node, "arguments"), a2n = 0;
      const int *a2a = a2v >= 0 ? nt_arr(nt, a2v, "arguments", &a2n) : NULL;
      if (a2n >= 2 && nt_argv_kwh(nt, a2a, a2n) >= 0) return 1;
    }
    /* a class-level attribute reader over an oint static (`Cfg.level`, or
       `level` in the class's own class method) answers the static */
    {
      int sgc = -1;
      if (r >= 0 && (nt_kind(nt, r) == NK_ConstantReadNode || nt_kind(nt, r) == NK_ConstantPathNode))
        sgc = const_node_class_index(c, r);
      else if (r < 0 || nt_kind(nt, r) == NK_SelfNode) {
        Scope *ss = comp_scope_of(c, node);
        if (ss && ss->is_cmethod) sgc = ss->class_id;
      }
      if (sgc >= 0 && an2 == 0 && call_target(c, node, sgc, nm, 0, NULL) < 0) {
        ClassInfo *sgi = &c->classes[sgc];
        const char *rn = comp_resolve_cm_alias_at(c, sgc, nm, NULL);
        if (!rn) rn = nm;
        if (comp_is_sg_reader(sgi, rn) && comp_is_sg_civ(sgi, rn)) {
          char ivb[300]; snprintf(ivb, sizeof ivb, "@%s", rn);
          int iv = comp_ivar_index(sgi, ivb);
          return iv >= 0 && civ_is_oint(c, sgc, iv);
        }
      }
    }
    /* a class's own methods (File.delete, IO::Buffer.size_of) are no container's */
    if (r >= 0 && (nt_kind(nt, r) == NK_ConstantReadNode || nt_kind(nt, r) == NK_ConstantPathNode) &&
        !oint_kind(rt) && rt != TY_COMPLEX && rt != TY_RATIONAL && rt != TY_BIGINT &&
        !ty_is_array(rt) && !ty_is_hash(rt) && rt != TY_RANGE && rt != TY_STRING &&   /* a constant holding a number or a container (Float::INFINITY, FREE = []) is a value */
        !sp_streq(nm, "world_readable?") && !sp_streq(nm, "world_writable?"))   /* File.world_readable?(f): nil when it is not */
      return 0;
    /* the runtime functions that answer an sp_oint (RUNTIME-API.md) */
    if ((sp_streq(nm, "bsearch") || sp_streq(nm, "bsearch_index")) && blk >= 0) return 1;
    if ((sp_streq(nm, "nonzero?") && oint_kind(rt)) ||
        (sp_streq(nm, "infinite?") && (rt == TY_FLOAT || rt == TY_INT || rt == TY_COMPLEX || rt == TY_RATIONAL || rt == TY_BIGINT)))
      return 1;
    /* a Method object's call runs its target: the target's own answer */
    if (r >= 0 && rt == TY_METHOD && is_method_invoke(nm)) {
      int mn = method_recv_node(c, r);
      int target = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
      /* the direct arms (a self-less target called by name, a bound target
         called through emit_bound_method_call) answer the target's own
         form; the fn-cast arm answers the plain value */
      /* every arm (a self-less target by name, emit_bound_method_call, the
         fn cast) answers the target's own form */
      if (target >= 0) return method_ret_is_oint(&c->scopes[target]);
      return 0;
    }
    /* a search with its needle; without one the call is an arity error, no nil */
    if (rt == TY_STRING && an2 >= 1 && (sp_streq(nm, "index") || sp_streq(nm, "rindex") ||
                                        sp_streq(nm, "byteindex") || sp_streq(nm, "byterindex"))) return 1;
    /* String#getbyte past the end, IO#getbyte at end of file: nil */
    if (sp_streq(nm, "getbyte") && r >= 0 && !oint_kind(rt)) return 1;
    if (sp_streq(nm, "exitstatus") || sp_streq(nm, "termsig") ||
        sp_streq(nm, "world_readable?") || sp_streq(nm, "world_writable?")) return 1;
    if (r >= 0 && ty_is_array(rt) &&
        (sp_streq(nm, "index") || sp_streq(nm, "rindex") || sp_streq(nm, "find_index") || sp_streq(nm, "delete_at") || sp_streq(nm, "delete") ||
         ((sp_streq(nm, "pop") || sp_streq(nm, "shift")) && an2 == 0))) return 1;
    if (r >= 0 && ty_is_hash(rt) && (sp_streq(nm, "delete") || sp_streq(nm, "find_index"))) return 1;
    if (r >= 0 && rt == TY_MATCHDATA && (sp_streq(nm, "bytebegin") || sp_streq(nm, "byteend"))) return 1;
    if (is_range_bound_reader(nm) && r >= 0 &&
        (rt == TY_MATCHDATA || rt == TY_RANGE || rt == TY_FLOAT_RANGE)) return 1;
    if (r >= 0 && (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY)) {
      /* an Integer / Float array's element answers: nil when empty or missing */
      if (an2 == 0 && blk < 0 &&
          (sp_streq(nm, "first") || sp_streq(nm, "last") || sp_streq(nm, "sample"))) return 1;
      /* ... and so does its extreme, except on a literal that is never
         empty, where only an element can be nil */
      if (an2 == 0 && blk < 0 && is_minmax_query(nm))
        return !array_literal_filled(nt, r) || nullable_int_value(c, node);
      if (sp_streq(nm, "slice!") && an2 == 1) return 1;
      if (sp_streq(nm, "find_index")) return 1;
      /* a seedless fold, a comparator min / max: nil over an empty receiver */
      if (blk >= 0 && an2 == 0 && (is_reduce_alias(nm) || sp_streq(nm, "min") || sp_streq(nm, "max"))) return 1;
    }
    if (r >= 0 && (rt == TY_RANGE || rt == TY_FLOAT_RANGE) && an2 == 0 && blk < 0 &&
        (sp_streq(nm, "min") || sp_streq(nm, "max"))) return 1;
    /* `s.unpack1("q")` past the input's end is nil */
    if (r >= 0 && sp_streq(nm, "unpack1")) return 1;
    /* a String range has no size (nil); an Enumerator's find_index / index
       answers nil on a miss */
    if (r >= 0 && rt == TY_STR_RANGE && an2 == 0 && (sp_streq(nm, "size"))) return 1;
    if (r >= 0 && rt == TY_ENUMERATOR && (sp_streq(nm, "find_index") || sp_streq(nm, "index"))) return 1;
    if (r >= 0 && ty_is_hash(rt) && sp_streq(nm, "dig") && an2 >= 1) return 1;
    /* fetch with a nil default answers that nil */
    if (r >= 0 && (ty_is_array(rt) || ty_is_hash(rt)) && sp_streq(nm, "fetch") && an2 == 2) {
      int fv = nt_ref(nt, node, "arguments"), fn = 0;
      const int *fa = fv >= 0 ? nt_arr(nt, fv, "arguments", &fn) : NULL;
      if (fa && fn == 2 && (nt_kind(nt, fa[1]) == NK_NilNode || node_is_oint(c, fa[1]))) return 1;
    }
    /* an element read that can miss, a fold or a search over a container
       that can hold nil: the analysis's answer, for a container receiver */
    if (r >= 0 && (ty_is_array(rt) || ty_is_hash(rt) || rt == TY_RANGE) &&
        (sp_streq(nm, "[]") || (sp_streq(nm, "slice") && an2 == 1) || sp_streq(nm, "at") ||
         sp_streq(nm, "fetch") || sp_streq(nm, "dig") ||
         sp_streq(nm, "first") || sp_streq(nm, "last") || sp_streq(nm, "min") || sp_streq(nm, "max") ||
         sp_streq(nm, "sum") || sp_streq(nm, "sample") || sp_streq(nm, "find_index") ||
         is_find_alias(nm) || is_reduce_alias(nm)))
      return nullable_int_value(c, node);
    return 0;
  }
  default:
    return nullable_int_value(c, node);
  }
}

static int repr_sealed_flag;

/* the kind a value of type t is held in, before any flag refines it */
static ReprKind repr_kind_of_type(const Compiler *c, TyKind t) {
  switch (t) {
  case TY_UNKNOWN: case TY_VOID:
    return RK_NONE;
  case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_SYMBOL: case TY_NIL:
    return RK_SCALAR;
  case TY_RANGE: case TY_FLOAT_RANGE: case TY_STR_RANGE: case TY_TIME:
  case TY_TMS: case TY_COMPLEX: case TY_RATIONAL: case TY_CLASS:
    return RK_STRUCT;
  case TY_STRBUF:
    return RK_STRBUF;
  case TY_POLY:
    return RK_BOXED;
  default:
    break;
  }
  if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    if (cid >= 0 && cid < c->nclasses && c->classes[cid].is_value_type) return RK_VOBJ;
  }
  return RK_PTR;
}

/* Is the node recv held boxed (repr_of(recv).kind == RK_BOXED)? Only a
   Poly-typed node is, or a local read whose slot settled poly
   (RS_SLOT_POLY); any other node answers no without repr_of's walk, which
   asks the node's own receiver in turn, so asking it of every link of a
   chain (`s << a << b << ...`) re-walked the chain below each link. */
static int repr_recv_boxed(const Compiler *c, int recv) {
  if (comp_ntype(c, recv) != TY_POLY && nt_kind(c->nt, recv) != NK_LocalVariableReadNode) return 0;
  return repr_of(c, recv).kind == RK_BOXED;
}

/* The layout of a value stored as t that its type names and its kind does
   not: every Array and Hash is a pointer, and which container it points to
   -- what an Array holds its elements as (ty_array_elem), what a Hash
   holds its keys and its values as (its variant's row: ty_hash_key,
   ty_hash_val) -- decides the helpers a call on it takes and the C type
   they name; an Integer is an sp_int scalar, and one held big an
   sp_Bigint * pointer, which takes the Bignum helpers. Every Range is a
   by-value struct, and which one -- an sp_Range of Integer bounds, an
   sp_FloatRange of Float ones, an sp_StrRange of Strings -- decides the
   same. A value with no type at all is RK_NONE, as a void one is; untyped
   tells them apart. */
static void repr_layout(Repr *r, TyKind t) {
  /* (a String Array of handles holds each String as its handle) */
  r->elem = ty_is_array(t) || ty_is_obj_array(t) ? ty_array_elem(t) : TY_UNKNOWN;
  r->key = ty_hash_key(t);
  r->val = ty_hash_val(t);
  r->range = t == TY_RANGE ? TY_INT : t == TY_FLOAT_RANGE ? TY_FLOAT
           : t == TY_STR_RANGE ? TY_STRING : TY_UNKNOWN;
  r->big = t == TY_BIGINT;
  r->untyped = t == TY_UNKNOWN;
}

/* What a local's slot says about where its value lives and what it holds,
   beside its type: its cell, a String kept volatile across a setjmp, a box
   proven to hold only a PolyArray or nil. A read of the local says the
   same. */
static void repr_slot_storage(Repr *r, const LocalVar *lv) {
  r->cell = (unsigned char)(lv->byref_out ? RC_BYREF : lv->inline_alias ? RC_ALIAS
                            : lv->is_cell ? RC_HEAP : RC_NONE);
  r->volatile_str = lv->borrowed_volatile != 0;
  r->arr_or_nil = lv->arr_or_nil == 1 && lv->type == TY_POLY;
}

/* An Array the analysis saw a nil stored into: an Integer or Float one's
   mark (`marked`, nullable_int_elem), a pointer one's the nil fact's
   (`pmarked`, obj_elem_may_nil: a nil stored, or a gap left). */
static int repr_elem_nil(TyKind elem, int marked, int pmarked) {
  if (elem == TY_INT || elem == TY_FLOAT) return marked != 0;
  return nil_fact_tracked(elem) && pmarked;
}

int repr_hash_is(Repr r, TyKind key, TyKind val) {
  return r.key != TY_UNKNOWN && r.key == key && r.val == val;
}

/* Two facts of a stored type that depend on its class, found together so the
   class's parent walk (class_is_exc_subclass) runs once. *exc: the type is
   the builtin Exception or a user object of a class below an exception, an
   object laid out as sp_Exception (Repr.exc). The return value: an object
   whose class some other class inherits from, so its static type is only the
   base and its box reads the class from the object (Repr.dyn_cls). */
static int repr_class_facts(const Compiler *c, TyKind t, int *exc) {
  *exc = t == TY_EXCEPTION;
  if (!ty_is_object(t)) return 0;
  int cid = ty_object_class(t);
  if (cid < 0 || cid >= c->nclasses) return 0;
  *exc = class_is_exc_subclass((Compiler *)c, cid);
  /* an exception's object starts with its class name, not a class id, so
     it is boxed with the static id (emit_boxed) */
  if (c->classes[cid].is_value_type || *exc) return 0;
  /* an Array subclass instance is boxed as its Array (arysub_box_id) */
  if (c->classes[cid].ary_root > 0) return 0;
  /* (once the analysis is done, from a table of the classes some class
     inherits from: asked of every stored value, the scan of every class
     grew with the square of the program) */
  if (g_scopes_settled) {
    static unsigned char *inherited = NULL;
    static int inh_n = -1;
    static unsigned inh_shape;
    if (inh_n != c->nclasses || inh_shape != comp_class_shape_gen()) {
      free(inherited);
      inh_n = c->nclasses; inh_shape = comp_class_shape_gen();
      inherited = calloc((size_t)(inh_n > 0 ? inh_n : 1), 1);
      if (!inherited) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      for (int k = 0; k < inh_n; k++) {
        int p = c->classes[k].parent;
        if (p >= 0 && p < inh_n && p != k) inherited[p] = 1;
      }
    }
    return inherited[cid];
  }
  for (int k = 0; k < c->nclasses; k++)
    if (k != cid && c->classes[k].parent == cid) return 1;
  return 0;
}

int repr_dyn_cls(const Compiler *c, TyKind t) {
  int exc;
  return repr_class_facts(c, t, &exc);
}

/* sets Repr.dyn_cls and Repr.exc for a value stored as type t */
static void repr_set_class_facts(const Compiler *c, Repr *r, TyKind t) {
  int exc;
  r->dyn_cls = repr_class_facts(c, t, &exc);
  r->exc = exc;
}

/* The slot a leaf read names, when that slot is an oint: the local, the
   instance ivar with a nil bit, the oint static. */
int repr_leaf_slot_is_opt(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_LocalVariableReadNode) {
    const char *ln = nt_str(nt, node, "name");
    Scope *s = ln ? comp_scope_of(c, node) : NULL;
    return slot_is_oint(s ? scope_local(s, ln) : NULL);
  }
  if (k == NK_InstanceVariableReadNode) return ivar_read_slot_is_oint(c, node);
  if (k == NK_ClassVariableReadNode) {
    HolderRef h;
    return holder_of_node_in(c, node, g_class_body_id, &h) && cvar_is_oint(c, h.cid, h.idx);
  }
  if (k == NK_GlobalVariableReadNode) {
    const char *gn = nt_str(nt, node, "name");
    const char *rn = gn ? comp_resolve_gvar(c, gn + 1) : NULL;
    return gvar_is_oint(c, rn ? comp_gvar(c, rn) : NULL);
  }
  return 0;
}

/* An Integer or Float node whose box has to test for nil, as emit_boxed
   decides it: the node has an oint form (node_has_oint_form), the value
   carrying its nil beside it.
   The analysis re-derives a receiver's type (infer_type) to answer, so the
   question is asked as a pure read (an_pure_read_begin): nothing derived is
   recorded, and asking changes nothing codegen reads next.
   Read the producer and slot facts directly here: the public form query
   reads Repr, so calling it would ask this same question again. */

static int repr_nil_scalar_form(const Compiler *c, int node, TyKind t, int *producer) {
  Compiler *mc = (Compiler *)c;
  int r = 0;
  an_pure_read_begin();
  if (node >= 0 && (t == TY_INT || t == TY_FLOAT)) {
    int p = node_is_oint(mc, node);
    if (producer) *producer = p;
    r = p || repr_leaf_slot_is_opt(mc, node);
  }
  an_pure_read_end();
  return r;
}

int repr_nil_scalar(const Compiler *c, int node, TyKind t) {
  return repr_nil_scalar_form(c, node, t, NULL);
}

TyKind repr_opt_kind(const Compiler *c, int node) {
  Repr r = repr_of(c, node);
  return r.kind == RK_OPT ? r.as_ty : TY_UNKNOWN;
}

/* repr.h: the write's slot holds the rule's handle */
int repr_write_share(const Compiler *c, int node) {
  if (node < 0) return 0;
  Compiler *mc = (Compiler *)c;
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_LocalVariableWriteNode || k == NK_LocalVariableOrWriteNode || k == NK_LocalVariableAndWriteNode ||
      k == NK_LocalVariableOperatorWriteNode) {
    const char *ln = nt_str(nt, node, "name");
    Scope *s = ln ? comp_scope_of(mc, node) : NULL;
    return s && repr_of_slot(c, scope_local(s, ln)).share;
  }
  if (k == NK_InstanceVariableWriteNode || k == NK_InstanceVariableOrWriteNode ||
      k == NK_InstanceVariableAndWriteNode) {
    const char *nm = nt_str(nt, node, "name");
    int cid = nm ? strbuf_ivar_owner(mc, node) : -1;
    int iv = cid >= 0 ? comp_ivar_index(&c->classes[cid], nm) : -1;
    return iv >= 0 && repr_of_ivar(c, cid, iv).share;
  }
  if (k == NK_GlobalVariableWriteNode || k == NK_GlobalVariableOrWriteNode || k == NK_GlobalVariableAndWriteNode ||
      k == NK_ClassVariableWriteNode || k == NK_ClassVariableOrWriteNode || k == NK_ClassVariableAndWriteNode)
    return repr_static_share(c, node);
  /* an attribute assignment (`obj.x = v`): its value is v, the handle an
     attr writer stored in its field, an explicit writer was handed or a
     native binding keeps */
  if (k == NK_CallNode) {
    int cid, iv;
    return attr_assign_slot(mc, node, &cid, &iv) ? repr_of_ivar(c, cid, iv).share
                                                 : user_assign_handle(mc, node) || native_assign_keeps(mc, node) != NULL;
  }
  return 0;
}

/* --share-strings: the binding a boxed receiver's call v takes for native
   class k, when it answers the String its object keeps (`native_share ...
   "answers"`), has that String's handle form and the rule shares the
   receiver's Strings; else NULL. The dispatch's arm for k publishes that
   handle (emit_poly_user_arm0), as a method arm's tail read does. */
const NativeMethod *repr_native_arm_handle(Compiler *c, int v, int k) {
  const NodeTable *nt = c->nt;
  int recv = v >= 0 && nt_kind(nt, v) == NK_CallNode ? nt_ref(nt, v, "receiver") : -1;
  if (!c->share || recv < 0 || k < 0 || k >= c->nclasses || !c->classes[k].is_native_class ||
      nt_ref(nt, v, "arguments") >= 0 || nt_ref(nt, v, "block") >= 0)
    return NULL;
  int nm = comp_native_method_find(c, k, nt_str(nt, v, "name"), 0, 0);
  const NativeMethod *m = nm >= 0 ? &c->native_methods[nm] : NULL;
  return m && (m->share & NSH_ANSWERS) && m->share_csym ? m : NULL;
}
/* Is v a boxed receiver's call whose arm, for each class the receiver can
   hold, is such a native binding (`o.string` where o holds a StringIO or a
   class without the method, which raises)? */
static int repr_boxed_native_handle(Compiler *c, int v) {
  int recv = nt_ref(c->nt, v, "receiver");
  const char *nm = nt_str(c->nt, v, "name");
  if (recv < 0 || !nm || !repr_recv_boxed(c, recv)) return 0;
  const PolyPlan *p = cplan_poly_arms(c, v);
  int nk = 0, any = 0;
  const int *ks = poly_recv_classes(c, v, &nk);
  if (p->n <= 0 || !ks || nk <= 0) return 0;
  for (int j = 0; j < nk; j++) {
    int arm = -1;
    for (int i = 0; i < p->n && arm < 0; i++)
      if (p->arm[i].key == ks[j]) arm = i;
    /* (a class with no arm and no method_missing raises NoMethodError,
       which answers no String: strbuf_poly_user_boxes) */
    if (arm < 0) {
      if (ks[j] < 0 || ks[j] >= c->nclasses || comp_method_in_chain(c, ks[j], "method_missing", NULL) >= 0 ||
          comp_resolve_member(c, ks[j], nm, 0, NULL, NULL) != SP_MEMBER_NONE)
        return 0;
      continue;
    }
    if (p->arm[arm].kind != PA_NATIVE || !repr_native_arm_handle(c, v, ks[j])) return 0;
    any = 1;
  }
  return any;
}

/* --share-strings: a call whose every target method answers a shared
   String's handle (Scope.ret_handle): the value is that handle, which the
   callee's tail read publishes. (A call marked to be stored as the handle
   is read with its mark lifted, as a String, and picked up here.) */
/* --share-strings: `super(v)` in a method of a subclass of a native class
   whose object keeps a String, reaching the class's binding of the name
   that keeps the String it is handed (`native_share "string=" ...
   "keeps"`), where the instance's own String is shared (#9258): the
   binding's handle form answers the handle it kept. The binding, or NULL. */
const NativeMethod *repr_super_native_keeps(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  if (!c->share || n < 0 || nt_kind(nt, n) != NK_SuperNode) return NULL;
  Scope *ms = comp_scope_of(c, n);
  if (!ms || ms->is_cmethod || ms->class_id < 0 || ms->class_id >= c->nclasses || ms->def_node < 0 ||
      !ms->name || is_initialize_family(ms->name) || c->classes[ms->class_id].nsub_root <= 0)
    return NULL;
  int root = c->classes[ms->class_id].nsub_root - 1;
  if (root >= c->nclasses || !c->classes[root].native_share_keeps) return NULL;
  int argc = 0; const int *argv = call_args(nt, n, &argc);
  if (argc != 1 || nt_ref(nt, n, "block") >= 0) return NULL;
  TyKind at = comp_ntype(c, argv[0]);
  int nm = comp_native_method_find_typed(c, root, ms->name, 1, 0, &at);
  const NativeMethod *m = nm >= 0 ? &c->native_methods[nm] : NULL;
  if (!m || m->kind != 0 || !(m->share & NSH_KEEPS) || !m->share_csym || m->rest || m->nargs != 1) return NULL;
  return m;
}

/* repr.h: does method mi answer no value -- every path of its body ends in
   a raise (a receiverless raise or fail the program does not redefine), with
   no `return` leaving it? A base class's NotImplementedError stub, which a
   subclass overrides: its type is the family's, so its return type says
   nothing of it. */
static int repr_leaf_not_raise(const VpLeaf *lf, void *ud) {
  Compiler *c = ud;
  if (lf->kind != VPK_EXPR) return 1;
  int n = lf->node;
  const char *nm = nt_kind(c->nt, n) == NK_CallNode && nt_ref(c->nt, n, "receiver") < 0 ? nt_str(c->nt, n, "name") : NULL;
  return !(nm && is_raise_alias(nm) && !bare_call_class_owned(c, n));
}
int scope_always_raises(Compiler *c, int mi) {
  const Scope *s = mi > 0 && mi < c->nscopes ? &c->scopes[mi] : NULL;
  if (!s || s->body < 0) return 0;
  for (int r = comp_ret_first(c, mi); r >= 0; r = comp_ret_next(c, r))
    if (nt_kind(c->nt, r) == NK_ReturnNode && comp_scope_of(c, r) == s) return 0;
  return !for_each_value_position(c->nt, s->body, VPS_ALL, repr_leaf_not_raise, c);
}

/* Do the n targets mis of call v publish its handle -- each answers no
   value (scope_always_raises), publishes it, or answers a String literal
   where nothing between the pickup's reset and its return can publish
   another handle (share_plan_literal_pickup_clean), and one publishes?
   With n -1, the targets are every method the call can run past the
   plan's list (share_name_targets, #9258). */
static int repr_targets_return_handle(Compiler *c, int v, const int *mis, int n) {
  int *own = NULL;
  if (n < 0) {
    own = malloc(sizeof(int) * (size_t)(c->nscopes + 1));
    if (!own) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    n = share_name_targets(c, v, own, c->nscopes + 1);
    mis = own;
  }
  int npub = 0, ok = n > 0;
  for (int i = 0; ok && i < n; i++) {
    if (scope_always_raises(c, mis[i])) continue;
    if (scope_ret_handle(c, mis[i])) { npub++; continue; }
    if (!share_plan_literal_pickup_clean(c, v, mis[i])) ok = 0;
  }
  free(own);
  return ok && npub > 0;
}
int repr_call_returns_handle(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  v = nt_paren_single(c->nt, v);
  if (repr_super_native_keeps(c, v)) return 1;
  if (v >= 0 && (nt_kind(nt, v) == NK_SuperNode || nt_kind(nt, v) == NK_ForwardingSuperNode)) {
    const CallPlan *p = cplan_user(c, v);
    return p->dispatch == CP_DIRECT && repr_self_handle(c, p->mi) && scope_ret_handle(c, p->mi);
  }
  if (v < 0 || nt_kind(nt, v) != NK_CallNode) return 0;
  /* a String's value, or one the pickup marks to be stored as the handle */
  TyKind t = c->ntype[v];
  if (t != TY_STRING && !(t == TY_STRBUF && (c->strbuf_box[v] || c->n_cext_extensions))) return 0;
  if (strbuf_io_outbuf(c, v) >= 0) return 1;
  /* a Method's call: the method `method(:m)` names */
  int recv = nt_ref(nt, v, "receiver");
  const char *nm = nt_str(nt, v, "name");
  /* A missing ENV key answers the default handle; a present one clears
     the return channel before making its own String. delete's
     missing-key block answers its value the same way. */
  if (recv >= 0 && nt_kind(nt, recv) == NK_ConstantReadNode &&
      is_env_const(nt_str(nt, recv, "name")) && !comp_const(c, "ENV") &&
      bop_share_named(BOP_ENV, nm) == BSH_FETCH) {
    int argc = 0; const int *argv = call_args(nt, v, &argc);
    int tail = env_delete_block_value(c, v);
    if (nt_ref(nt, v, "block") < 0 ? argc == 2 && repr_of(c, argv[1]).kind == RK_STRBUF
                                   : tail >= 0 && (repr_of(c, tail).kind == RK_STRBUF || strbuf_value_carries(c, tail)))
      return 1;
  }
  if (recv >= 0 && nm && comp_ntype(c, recv) == TY_METHOD && is_call_alias(nm)) {
    int mn = method_recv_node(c, recv);
    int mi = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    return mi > 0 && scope_ret_handle(c, mi);
  }
  int mis[CPT_MAX];
  int n;
  /* Kernel#String calls to_str, or to_s where the class has no to_str.
     A fresh-answering target must keep the ordinary conversion's copy.
     A nil to_str falls through to to_s; a nil to_s falls to fresh object
     text in the bridge, so neither may pick up an earlier publication. */
  if (recv < 0 && is_string_class_name(nm) && call_target(c, v, -1, nm, CT_TOP, NULL) < 0 && !bare_call_class_owned(c, v)) {
    int ac = 0;
    const int *av = call_args(nt, v, &ac);
    if (ac != 1 || nt_ref(nt, v, "block") >= 0) return 0;
    /* a boxed argument's classes, or an object's own and its subclasses'
       (the class alone for a `K.new`, which builds exactly K) */
    TyKind at = comp_ntype(c, av[0]);
    int ocid = ty_is_object(at) ? ty_object_class(at) : -1;
    if (ocid < 0 && repr_of(c, av[0]).kind != RK_BOXED) return 0;
    int own[CPT_MAX];
    const int *ks = ocid >= 0 ? own : poly_recv_classes(c, v, &n);
    if (ocid >= 0) {
      n = 0;
      for (int k = 0; k < c->nclasses; k++)
        if (k == ocid || (share_exact_new_class(c, av[0]) != ocid && is_descendant(c, k, ocid))) {
          if (n == CPT_MAX) return 0;
          own[n++] = k;
        }
    }
    if (!ks || n <= 0 || n > CPT_MAX) return 0;
    for (int i = 0; i < n; i++) {
      /* (ret_nil_pickup is decided with ret_handle) */
      int mi = comp_method_in_chain(c, ks[i], "to_str", NULL);
      if (mi >= 0 && (scope_ret_handle(c, mi), c->scopes[mi].ret_nil_pickup)) {
        int fallback = comp_method_in_chain(c, ks[i], "to_s", NULL);
        if (fallback < 0 || !scope_ret_handle(c, fallback) || c->scopes[fallback].ret_nil_pickup) return 0;
      }
      if (mi < 0) {
        mi = comp_method_in_chain(c, ks[i], "to_s", NULL);
        if (mi >= 0 && (scope_ret_handle(c, mi), c->scopes[mi].ret_nil_pickup)) return 0;
      }
      if (mi < 0) return 0;
      mis[i] = mi;
    }
  }
  else n = cplan_targets(c, v, mis, CPT_MAX);
  /* past the plan's list: every method the call can run (#9258) */
  if (n == CPT_UNKNOWN) return repr_targets_return_handle(c, v, NULL, -1);
  /* a boxed receiver's call that reaches no method but native bindings
     answering their objects' handles, which their arms publish */
  if (n == 0 && repr_boxed_native_handle(c, v)) return 1;
  if (n <= 0) return 0;
  /* a target that answers no value (every path raises: a base class's
     NotImplementedError stub a subclass overrides) hands on no String; a
     target answering a String literal publishes none, and the pickup's new
     handle over its bytes is the literal's own, which keeps one identity,
     where nothing between the pickup's reset and the target's return can
     publish another handle (share_plan_literal_pickup_clean); the others
     each publish their handle, and one has to */
  return repr_targets_return_handle(c, v, mis, n);
}
/* A boxed receiver's reader arms already box their shared fields as
   handles. A String demand can take that boxed result without a copy.
   Names with a builtin face keep their own route. */
int repr_boxed_reader_handle(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (v < 0 || nt_kind(nt, v) != NK_CallNode ||
      (c->ntype[v] != TY_STRING && !(c->ntype[v] == TY_STRBUF && c->strbuf_box[v])) ||
      nt_ref(nt, v, "block") >= 0 ||
      nt_ref(nt, v, "arguments") >= 0) return 0;
  int recv = nt_ref(nt, v, "receiver");
  if (recv < 0 || !repr_recv_boxed(c, recv) ||
      ty_poly_face_owners(nt_str(nt, v, "name"), 0, 0, 1, 1)) return 0;
  const PolyPlan *p = cplan_poly_arms(c, v);
  if (p->n == 0) return 0;
  for (int i = 0; i < p->n; i++)
    if (p->arm[i].kind != PA_READER || p->arm[i].vty != TY_STRBUF) return 0;
  /* (an exception in the box answers #message through the exception's
     route, whose fallback takes these readers' boxes: exc_message_boxed_call) */
  return !exc_message_boxed_call(c, v);
}

/* A boxed to_s can answer the String in the box itself. The route takes a
   block-less, argument-less to_s on a boxed receiver, typed as a String or
   shared buffer, under --share-strings. Every user method the call can
   dispatch to must return the handle or a new String; a target answering
   anything else, or one the call cannot resolve, declines. Builtin arms
   are not among those targets, nor is the method the call is in (`def
   to_s = @v.to_s`, whose box may hold another of its objects): that arm
   answers what the method's other tails answer, which its own return
   decides. */
int repr_boxed_to_s_operand(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (v < 0 || nt_kind(nt, v) != NK_CallNode) return -1;
  const char *nm = nt_str(nt, v, "name");
  int recv = nt_ref(nt, v, "receiver");
  if (!is_to_s_name(nm) || recv < 0 || call_plain_argc(c, v) != 0 ||
      nt_ref(nt, v, "block") >= 0 || repr_of(c, recv).kind != RK_BOXED ||
      (c->ntype[v] != TY_STRING && c->ntype[v] != TY_STRBUF)) return -1;
  int targets[CPT_MAX], n = cplan_targets(c, v, targets, CPT_MAX);
  if (n < 0) return -1;
  for (int i = 0; i < n; i++) {
    if (c->scopes + targets[i] == comp_scope_of(c, v)) continue;
    if (c->scopes[targets[i]].ret != TY_STRING ||
        (!scope_ret_handle(c, targets[i]) && !scope_ret_fresh(c, targets[i]))) return -1;
  }
  return recv;
}

/* The self holder of a String reopening's method stands for the String
   self is; a method of a subclass of a native class has one too, for the
   String its instance keeps (#9258), and self there is the object, never a
   String slot. */
static int scope_self_is_string(const Compiler *c, int scope) {
  if (scope < 0) return 0;
  const Scope *s = &c->scopes[scope];
  return !s->is_cmethod && s->class_id >= 0 && s->class_id == comp_class_index((Compiler *)c, "String");
}

int repr_self_handle(const Compiler *c, int scope) {
  return scope_self_is_string(c, scope);
}

int repr_self_shared(const Compiler *c, int node) {
  if (node < 0 || nt_kind(c->nt, node) != NK_SelfNode) return 0;
  Scope *s = comp_scope_of((Compiler *)c, node);
  return s && repr_self_handle(c, (int)(s - c->scopes));
}

/* Is every method call v reaches a String reopening's `def m = self` (no
   parameter, and the self read its whole body)? Such a method answers its
   receiver untouched, as the builtin conversion of its name does. */
static int repr_user_answers_self(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  int tg[CPT_MAX], n = cplan_targets(c, v, tg, CPT_MAX);
  if (n <= 0) return 0;
  for (int i = 0; i < n; i++) {
    const Scope *m = &c->scopes[tg[i]];
    int body = m->body, nb = 1;
    if (body >= 0 && nt_kind(nt, body) == NK_StatementsNode) nt_arr(nt, body, "body", &nb);
    int last = scope_body_last(c, tg[i]);
    if (m->def_node < 0 || m->nparams != 0 || nb != 1 || last < 0 || nt_kind(nt, last) != NK_SelfNode ||
        !scope_self_is_string(c, tg[i]))
      return 0;
  }
  return 1;
}
/* A builtin receiver conversion on a String keeps its handle, and so does
   a reopening's conversion that answers self. The seal, the route emitter
   and boxed-form prediction use the same fact. */
int repr_string_conversion_operand(Compiler *c, int v) {
  v = nt_paren_single(c->nt, v);
  if (v < 0 || nt_kind(c->nt, v) != NK_CallNode) return -1;
  int r = nt_ref(c->nt, v, "receiver");
  return r >= 0 && is_receiver_conversion(nt_str(c->nt, v, "name")) &&
         call_plain_argc(c, v) == 0 && (nt_ref(c->nt, v, "block") < 0 || nt_int(c->nt, v, "dyn_arm", 0)) &&
         comp_recv_type(c, r) == TY_STRING &&
         (cplan_user(c, v)->dispatch == CP_NONE || repr_user_answers_self(c, v)) ? r : -1;
}
/* ENV copies its stored bytes, but a store answers its value operand.
   A boxed operand can still hold plain bytes: an observable alias stays
   refused. A mutable key held across value evaluation that can change it
   must carry its handle too. */
int repr_env_store_operand(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (v < 0 || nt_kind(nt, v) != NK_CallNode) return -1;
  int r = nt_ref(nt, v, "receiver"), argc = 0;
  const int *argv = call_args(nt, v, &argc);
  return r >= 0 && nt_kind(nt, r) == NK_ConstantReadNode && is_env_const(nt_str(nt, r, "name")) &&
         !comp_const(c, "ENV") && argc == 2 && bop_share_named(BOP_ENV, nt_str(nt, v, "name")) == BSH_LAST &&
         (repr_of(c, argv[1]).kind != RK_BOXED || share_value_unobserved(c, argv[1])) &&
         (repr_of(c, argv[0]).handle || share_value_fresh(c, argv[0], 0) || !subtree_has_side_effect(c, argv[1]))
         ? argv[1] : -1;
}
/* Does string conversion `node` hand on a handle: the operand it ends in,
   through further conversions, is a boxed value or a route that carries
   one (strbuf_route_kind, the arms emit_strbuf_value takes the conversion
   by)? */
static int repr_conversion_hands_handle(Compiler *c, int node) {
  int x = repr_string_conversion_operand(c, node), op, more;
  while ((more = x >= 0 ? repr_string_conversion_operand(c, x) : -1) >= 0) x = more;
  return x >= 0 && (repr_of(c, x).kind == RK_BOXED || strbuf_route_kind(c, x, &op) != SPK_NONE ||
                    strbuf_cond_has_handle_leaf(c, x));
}
/* Where the boxed form of a shared-mutable String comes from, as emit_boxed
   decides it for a node stored as (or holding) the handle. */
static int repr_strbuf_src(const Compiler *c, int node, TyKind t) {
  Compiler *mc = (Compiler *)c;
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, node);
  if (repr_self_shared(c, node)) return RS_HANDLE;
  /* an element a String Array of handles reads or removes is the handle
     the Array keeps, as a String Range's endpoint is (#8328 step 5) */
  if ((t == TY_STRING || t == TY_STRBUF) && strh_handle_call(mc, node)) return RS_HANDLE;
  if (t == TY_STRING) {
    /* a global holding the handle (--share-strings): its read boxes it */
    if (repr_static_read_kind(k)) return repr_static_share(c, node) ? RS_HANDLE : RS_NONE;
    /* An ivar promoted after the node types settled still reads as
       String here. Its box carries the shared slot's handle too. */
    if (k == NK_InstanceVariableReadNode) {
      const char *nm = nt_str(nt, node, "name");
      int cid = nm ? strbuf_ivar_owner(mc, node) : -1;
      int iv = cid >= 0 ? comp_ivar_index(&c->classes[cid], nm) : -1;
      return iv >= 0 && repr_of_ivar(c, cid, iv).share ? RS_HANDLE : RS_NONE;
    }
    /* a write in value position whose slot holds the handle the rule
       assigned: its value is that slot, as the slot's read is */
    if (repr_write_share(c, node)) return RS_HANDLE;
    /* a local promoted to the handle after the node types were final */
    if (k != NK_LocalVariableReadNode) return RS_NONE;
    const char *ln = nt_str(nt, node, "name");
    Scope *s = ln ? comp_scope_of(mc, node) : NULL;
    LocalVar *lv = s ? scope_local(s, ln) : NULL;
    if (lv && lv->type == TY_POLY && c->nilnarrow[node] == TY_STRBUF) return RS_SLOT_POLY;
    return lv && lv->type == TY_STRBUF && lv->str_shared ? RS_HANDLE : RS_NONE;
  }
  if (t != TY_STRBUF) return RS_NONE;
  if (k == NK_LocalVariableReadNode) {
    /* the mark can outlive the slot's type: a slot that settled poly holds
       the box already */
    const char *ln = nt_str(nt, node, "name");
    Scope *s = ln ? comp_scope_of(mc, node) : NULL;
    LocalVar *lv = s ? scope_local(s, ln) : NULL;
    return lv && lv->type == TY_POLY ? RS_SLOT_POLY : RS_HANDLE;
  }
  if (k == NK_InstanceVariableReadNode) {
    const char *nm = nt_str(nt, node, "name");
    int cid = nm ? strbuf_ivar_owner(mc, node) : -1;
    int iv = cid >= 0 ? comp_ivar_index(&c->classes[cid], nm) : -1;
    if (iv >= 0 && c->classes[cid].ivar_types[iv] == TY_STRBUF) return RS_HANDLE;
  }
  /* a global holding the handle (--share-strings) */
  if ((repr_static_read_kind(k) || k == NK_GlobalVariableWriteNode) && repr_static_share(c, node))
    return RS_HANDLE;
  /* an ivar write's value is the slot when the slot is the handle; a
     plain String slot's value is a String, wrapped fresh below (a raise
     arm beside it boxed that `const char *` as the handle, and the C did
     not build) */
  if (k == NK_InstanceVariableWriteNode) {
    const char *nm = nt_str(nt, node, "name");
    int cid = nm ? strbuf_ivar_owner(mc, node) : -1;
    int iv = cid >= 0 ? comp_ivar_index(&c->classes[cid], nm) : -1;
    if (iv < 0 || c->classes[cid].ivar_types[iv] == TY_STRBUF) return RS_HANDLE;
  }
  /* so is any write's whose slot holds the rule's handle */
  if (repr_write_share(c, node)) return RS_HANDLE;
  /* an element a boxed container hands out is a boxed handle already */
  if (strbuf_boxed_elem_read(mc, node)) return RS_ELEM;
  /* a String Range's endpoint (begin, end, first, last, min, max), also
     as the value of parentheses (strbuf_srange_tail), is the handle the
     Range keeps for it, as a slot's read is the slot's handle */
  if (strbuf_srange_tail(mc, node) >= 0) return RS_HANDLE;
  /* a reader call (or a call answering its receiver) that renders the
     handle itself */
  if (k == NK_CallNode) {
    /* A demanded `s.to_s` over a local holding the handle boxes that
       handle, as emit_boxed_strbuf does (an_str_conversion_local). */
    if (c->strbuf_box[node]) {
      int cr = an_str_conversion_local(mc, node);
      if (cr >= 0 && comp_ntype(c, cr) == TY_STRBUF) return RS_HANDLE;
    }
    /* A boxed receiver route keeps its handle beside fresh user answers. */
    if (repr_boxed_to_s_operand(mc, node) >= 0) return RS_HANDLE;
    /* A conversion keeps its operand's handle only where the operand has
       one to hand on; over bytes (`split(".").last.to_s`,
       `File.basename(p).to_s`) it is a String value, wrapped fresh where a
       handle is demanded, as the operand alone would be. */
    if (repr_string_conversion_operand(mc, node) >= 0)
      return repr_conversion_hands_handle(mc, node) ? RS_HANDLE : RS_FRESH;
    int ops[3];
    if (strbuf_route_clamp(mc, node, ops)) return RS_HANDLE;
    if (repr_env_store_operand(mc, node) >= 0) return RS_HANDLE;
    /* A demanded call whose return route carries a handle is already that
       handle, including when operand ordering holds it in a temp. */
    if (c->strbuf_handle_demand[node] && repr_call_returns_handle(mc, node)) return RS_DEMANDED;
    /* A container store can mark the reader itself as the handle too. */
    if (repr_boxed_reader_handle(mc, node)) return RS_DEMANDED;
    /* The implicit-self reader boxes its slot too, not a fresh String. */
    if ((c->strbuf_box[node] || c->strbuf_handle_demand[node]) &&
        strbuf_self_reader_slot(mc, node)) return RS_DEMANDED;
    int r = nt_ref(nt, node, "receiver");
    if (r >= 0 && ty_is_object(comp_ntype(c, r)) &&
        (strbuf_marked_yields_handle(mc, node) || c->strbuf_handle_demand[node]))
      return RS_DEMANDED;
    /* A boxed return pickup keeps its published handle too. A demanded
       reader above already emits the handle, including from a held temp. */
    if (c->strbuf_box[node] &&
        (repr_call_returns_handle(mc, node) || strbuf_route_inline_call(mc, node))) return RS_HANDLE;
  }
  /* a String value stored where a handle is demanded: a fresh one */
  return RS_FRESH;
}

/* See repr.h. */
int repr_handle_apart(Compiler *c, int v, Repr r) {
  return r.as_ty == TY_STRBUF && (r.strbuf_src == RS_FRESH || strbuf_srange_tail(c, v) >= 0);
}

/* The type a node is STORED as, given its cached type t (comp_ntype's
   String-handle refinement). TY_STRBUF is a codegen-only storage refinement
   (a mutable sp_String for a `<<`-appended local): all type-directed logic
   treats it as a String, and only a read marked strbuf_box yields the live
   HANDLE, so the mutation is observable through the container it is stored
   in (#3227). A node under a handle demand STORES as the handle -- a temp
   spilled from it has to be an sp_String *, not a const char * -- while
   still dispatching as a String, which comp_recv_type answers for. That
   split is the whole point of the second array (#4363). */
TyKind repr_stored_type(const Compiler *c, int id, TyKind t) {
  /* (--share-strings: a value the analysis typed as the handle that is
     demanded as one is stored as one, as any other demanded value is:
     `{ |t| u = t }` spliced under a demand, the write of a handle local) */
  if (t == TY_STRBUF) return c->strbuf_box[id] || (c->strbuf_handle_demand[id]) ? TY_STRBUF : TY_STRING;
  /* (a boxed value is held in its box whatever is asked of it: a reader
     whose ivar widened past a String answers the box, which a handle's
     slot did not take, and the C did not build) */
  if (c->strbuf_handle_demand[id] && t != TY_POLY) return TY_STRBUF;
  return t;
}

/* 1 iff t is a user-object type whose class is represented by value (sp_X,
   not a heap pointer). See detect_value_types. */
int repr_value_obj(const Compiler *c, TyKind t) {
  if (!ty_is_object(t)) return 0;
  int cid = ty_object_class(t);
  return cid >= 0 && cid < c->nclasses && c->classes[cid].is_value_type;
}

/* An argument whose boxing must allow nil though its typed reads need no
   nil check: an int ivar read nothing has to assign first, or a parameter
   already bound from one (box_nullable_arg). */
int repr_box_nullable_arg(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (v < 0) return 0;
  if (nt_kind(nt, v) == NK_InstanceVariableReadNode) {
    Scope *s = comp_scope_of(c, v);
    int cid = s ? s->class_id : -1;
    if (cid < 0) cid = c->toplevel_class;
    if (cid < 0 || cid >= c->nclasses) return 0;
    ClassInfo *ci = &c->classes[cid];
    const char *ivn = nt_str(nt, v, "name");
    int iv = comp_ivar_index(ci, ivn);
    if (iv < 0 || (ci->ivar_types[iv] != TY_INT && ci->ivar_types[iv] != TY_FLOAT)) return 0;
    return !ivar_assigned_in_initialize(c, cid, ivn);
  }
  if (nt_kind(nt, v) == NK_LocalVariableReadNode) {
    Scope *s = comp_scope_of(c, v);
    const char *ln = nt_str(nt, v, "name");
    LocalVar *lv = s && ln ? scope_local(s, ln) : NULL;
    return lv && lv->is_param && lv->box_nullable;
  }
  return 0;
}


/* Can a value held as t be nil? A user object and a builtin pointer the
   analysis follows (nil_fact_tracked: a String, an Array, a Hash, an IO)
   answer by their nil fact, `fact` (analyze_nil.c, #7444). Any other kind
   whose NULL is its nil (a Bignum, a String buffer, a Proc, a Method, a
   MatchData, ...) has no fact, so it can hold one. */
static int repr_may_nil(TyKind t, int fact) {
  return nil_fact_tracked(t) ? fact : ty_null_is_nil(t);
}

Repr repr_of(const Compiler *c, int node) {
  Repr r;
  memset(&r, 0, sizeof r);
  r.ty = r.as_ty = r.narrowed = r.elem = r.key = r.val = r.range = TY_UNKNOWN;
  r.kind = RK_NONE;
  r.untyped = 1;
  if (node < 0 || node >= c->nt->count) return r;
  r.ty = c->ntype[node];
  r.as_ty = comp_ntype(c, node);
  /* the layout first, from the type it is stored as, then the flags:
     GCC 13's store merging at -O2 drops the nil_cold store when
     repr_layout's bitfield stores come after it; setting the layout first
     avoids it */
  repr_layout(&r, r.as_ty);
  r.narrowed = c->nilnarrow ? c->nilnarrow[node] : TY_UNKNOWN;
  r.handle = c->strbuf_box[node] != 0;
  r.demand = c->strbuf_handle_demand[node] != 0;
  r.read_raw = c->strbuf_read_raw ? c->strbuf_read_raw[node] != 0 : 0;
  r.poly_lift = c->poly_strbuf_lift ? c->poly_strbuf_lift[node] != 0 : 0;
  r.nil_tested = c->nil_tested ? c->nil_tested[node] != 0 : 0;
  r.nil_cold = c->nil_tested ? c->nil_tested[node] == 2 : 0;
  r.head_held = c->head_held ? c->head_held[node] != 0 : 0;
  /* a node is boxed as the type it is stored as; a nil-guard narrowing is
     read where the value is used, not where it is boxed */
  TyKind kt = r.as_ty;
  r.kind = (unsigned char)repr_kind_of_type(c, kt);
  repr_set_class_facts(c, &r, kt);
  int opt_producer = 0;
  if (repr_nil_scalar_form(c, node, kt, &opt_producer)) {
    r.kind = RK_OPT;
    r.may_nil = r.nil_scalar = 1;
  }
  r.opt_producer = (unsigned)opt_producer;
  r.strbuf_src = (unsigned char)repr_strbuf_src(c, node, kt);
  r.share = (unsigned)repr_static_share(c, node);
  if (r.strbuf_src == RS_SLOT_POLY && !(r.narrowed == TY_STRBUF)) r.kind = RK_BOXED;
  else if (r.strbuf_src != RS_NONE) r.kind = RK_STRBUF;
  /* a pointer that can be nil (repr_may_nil); a by-value user object the
     nil fact says may be nil too, whose layout has no nil to hold it in */
  if ((r.kind == RK_PTR || r.kind == RK_VOBJ || r.kind == RK_STRBUF) &&
      repr_may_nil(kt, nil_fact_tracked(kt) && nil_fact_node(c, node)))
    r.may_nil = 1;
  /* an Integer or Float Array the analysis marked (nullable_int_elem_array
     asks the node's source, as a pure read) */
  if (r.elem == TY_INT || r.elem == TY_FLOAT) {
    an_pure_read_begin();
    r.elem_nil_marked = (unsigned)repr_elem_nil(r.elem, nullable_int_elem_array((Compiler *)c, node), 0);
    r.elem_nil_reach = (unsigned)(r.elem_nil_marked || elem_nil_reach_expr((Compiler *)c, node));
    an_pure_read_end();
  }
  else r.elem_nil_marked = (unsigned)repr_elem_nil(r.elem, 0, nil_elem_fact_node(c, node));
  /* a local read says what its slot does about where the value lives */
  if (nt_kind(c->nt, node) == NK_LocalVariableReadNode) {
    const char *ln = nt_str(c->nt, node, "name");
    Scope *s = ln ? comp_scope_of((Compiler *)c, node) : NULL;
    LocalVar *lv = s ? scope_local(s, ln) : NULL;
    if (lv) repr_slot_storage(&r, lv);
  }
  return r;
}

ReprForm repr_box_form(const Compiler *c, Repr r) {
  TyKind t = r.as_ty;
  switch ((ReprKind)r.kind) {
  case RK_NONE:     return RF_NIL_EFFECT;
  case RK_BOXED:    return RF_PASS;
  case RK_OPT: return t == TY_FLOAT ? RF_FLT_NIL : RF_INT_NIL;
  case RK_STRUCT:   return RF_STRUCT;
  case RK_VOBJ:     return RF_VOBJ;
  case RK_STRBUF:
    return r.strbuf_src == RS_ELEM ? RF_STRBUF_ELEM
         : r.strbuf_src == RS_FRESH ? RF_STRBUF_FRESH : RF_STRBUF_HANDLE;
  case RK_SCALAR:
    switch (t) {
    case TY_INT:    return RF_INT;
    case TY_FLOAT:  return RF_FLT;
    case TY_BOOL:   return RF_BOOL;
    case TY_SYMBOL: return RF_SYM;
    default:        return RF_NIL_EFFECT;   /* nil */
    }
  case RK_PTR:
  default:
    break;
  }
  (void)c;
  if (t == TY_STRING) return RF_STR;
  if (r.big) return RF_BIGINT;
  /* an Array of pointers (objects, nested Arrays, a String Array's handles)
     is stamped with what its elements are */
  if (ty_is_object(r.elem) || ty_is_array(r.elem)) return RF_PTR_ARRAY;
  if (ty_is_object(t)) return r.dyn_cls ? RF_NULLABLE_DYN : RF_NULLABLE;
  return RF_NULLABLE;
}

const char *repr_form_name(int form) {
  static const char *const names[RF__COUNT] = {
    "PASS", "NIL_EFFECT", "INT", "INT_NIL", "FLT", "FLT_NIL", "BIGINT", "STR",
    "BOOL", "SYM", "STRUCT", "NULLABLE", "NULLABLE_DYN", "VOBJ",
    "STRBUF_HANDLE", "STRBUF_FRESH", "STRBUF_ELEM", "PTR_ARRAY", "YIELD", "SPECIAL",
  };
  return form >= 0 && form < RF__COUNT ? names[form] : "?";
}

/* The C value class of a kind: what C allows between two of them. */
int repr_store_class(const Compiler *c, TyKind t) {
  switch (t) {
    case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_SYMBOL: return SC_ARITH;
    case TY_POLY: return SC_BOXED;
    case TY_UNKNOWN: case TY_VOID: case TY_NIL: return SC_NONE;
    default: break;
  }
  if (ty_is_object(t)) return comp_ty_value_obj(c, t) ? SC_STRUCT : SC_PTR;
  if (ty_is_struct_valued(t)) return SC_STRUCT;
  return c_type_name(t) ? SC_PTR : SC_NONE;
}

/* Does a value of kind `from`, written as it is, keep its value in a slot of
   kind `to`? The same C type does; so does an exact arithmetic widening
   (an Integer into a Float slot, a boolean into an Integer one), a nil
   literal's 0 in a pointer slot, which is NULL, and a subclass instance in
   its ancestor's pointer slot. A nil fits as it is only where it is a
   literal (repr_store_nil_fits). An untyped value's C type is whatever its
   emitter chose (a boxed result, the gate's token, a super call's String),
   which the kind does not say, so it is not checked. A void one fits
   nothing. */
/* A nil literal renders as 0, which is a pointer slot's NULL and a boolean's
   false: it is written as it is there, and into an operand a builtin
   converts itself (CO_CONVERT), whose nilable forms read the 0 as they
   always have. Any other nil value -- a call that answers nil, kept for its
   effect -- and a nil into a variable whose nil is not a 0 (an Integer or
   a Float, its nil beside the value; a Symbol) takes the slot's nil. */
int repr_store_nil_fits(Compiler *c, int node, TyKind slot, int how) {
  return node >= 0 && nt_kind(c->nt, node) == NK_NilNode &&
         (repr_store_class(c, slot) == SC_PTR || slot == TY_BOOL ||
          (how == CO_CONVERT && repr_store_class(c, slot) == SC_ARITH));
}

int repr_store_fits(Compiler *c, TyKind from, TyKind to) {
  if (from == to || to == TY_UNKNOWN || to == TY_VOID || from == TY_UNKNOWN) return 1;
  /* Under the one-String mode a String slot of either face takes the
     handle: a typed container's element is the handle (sp_strelem), and a
     bytes slot reads it through SP_STR_B, which the C compiler holds the
     emitter to (string-one-repr-design.md). */
  if (from == TY_STRBUF && to == TY_STRING) return 1;
  int fc = repr_store_class(c, from), tc = repr_store_class(c, to);
  if (from == TY_NIL) return 0;   /* see store_nil_fits */
  if (fc == SC_NONE) return 0;
  if (fc == SC_ARITH && tc == SC_ARITH) return from != TY_FLOAT || to == TY_FLOAT;
  if (ty_is_object(from) && ty_is_object(to) && fc == SC_PTR && tc == SC_PTR)
    return is_descendant(c, ty_object_class(from), ty_object_class(to));
  Buf fb, tb;
  memset(&fb, 0, sizeof fb); memset(&tb, 0, sizeof tb);
  emit_ctype(c, from, &fb); emit_ctype(c, to, &tb);
  int same = fb.p && tb.p && sp_streq(fb.p, tb.p);
  free(fb.p); free(tb.p);
  return same;
}

/* Would emit_empty_literal_as build `v` at the slot's kind? An empty
   literal, a bare Array.new or Hash.new, for an Array or Hash slot it has
   a constructor for. */
static int repr_empty_lit_as(Compiler *c, int v, TyKind slot) {
  const NodeTable *nt = c->nt;
  v = nt_paren_single(c->nt, v);
  const char *vty = v >= 0 ? nt_type(nt, v) : NULL;
  NodeKind vk = nt_kind(nt, v);
  if (!vty) return 0;
  int n = 0;
  if (vk == NK_ArrayNode) {
    nt_arr(nt, v, "elements", &n);
    return !n && (ty_is_ptr_array(slot) || slot == TY_POLY_ARRAY || array_kind(slot));
  }
  if (vk == NK_HashNode || vk == NK_KeywordHashNode) {
    nt_arr(nt, v, "elements", &n);
    return !n && ty_is_hash(slot) && ty_hash_cname(slot);
  }
  if (vk == NK_CallNode && node_is_empty_container(c, v)) {
    const char *rn = nt_str(nt, nt_ref(nt, v, "receiver"), "name");
    if (rn && sp_streq(rn, "Hash") && ty_is_hash(slot) && ty_hash_cname(slot)) return 1;
    if (rn && sp_streq(rn, "Array"))
      return ty_is_ptr_array(slot) || slot == TY_POLY_ARRAY || array_kind(slot);
  }
  return 0;
}

/* Does emit_poly_rhs_coerced take a boxed value into a `slot` slot? An
   object through the checked unbox, a scalar or a String through its
   conversion. */
static int repr_poly_rhs_ok(TyKind slot) {
  return ty_is_object(slot) || slot == TY_INT || slot == TY_BOOL || slot == TY_FLOAT ||
         slot == TY_SYMBOL || slot == TY_STRING;
}

int repr_coerce_text_form(Compiler *c, int node, TyKind from, TyKind slot, int how) {
  if (repr_store_fits(c, from, slot) || (from == TY_NIL && repr_store_nil_fits(c, node, slot, how)))
    return CF_FIT;
  if (slot == TY_POLY) return CF_BOX;
  if (from == TY_VOID || from == TY_NIL) return CF_NIL_SENT;
  if (slot == TY_BIGINT && from == TY_INT) return CF_INT2BIG;
  if (how == CO_CONVERT && slot == TY_FLOAT && (from == TY_BIGINT || from == TY_RATIONAL))
    return CF_CONVERT;
  return CF_REFUSE;
}

int repr_coerce_plan(Compiler *c, int node, TyKind slot, int how, TyKind *from_out) {
  if (from_out) *from_out = TY_UNKNOWN;
  if (how == CO_CONVERT && slot == TY_BOOL) return CF_CONVERT;
  TyKind from = store_value_kind(c, node);
  if (from_out) *from_out = from;
  int container = ty_is_array(slot) || ty_is_hash(slot);
  if (from == TY_UNKNOWN && container && repr_empty_lit_as(c, node, slot)) return CF_EMPTY_LIT;
  if (repr_store_fits(c, from, slot) || (from == TY_NIL && repr_store_nil_fits(c, node, slot, how)))
    return CF_FIT;
  if (slot == TY_POLY) return CF_BOX;
  if (container && repr_empty_lit_as(c, node, slot)) return CF_EMPTY_LIT;
  if (from == TY_NIL && nt_kind(c->nt, node) == NK_NilNode) return CF_NIL_SENT;
  if (slot == TY_BIGINT && from == TY_INT) return CF_INT2BIG;
  /* a String into the shared handle slot an ivar holds under the share
     rule: the handle the value is, or a fresh one around it, as a plain
     `@x = v` stores it. A frozen literal is the handle of its own site
     (SP_SHARE_STRING_LITERALS), so it keeps one identity and stays frozen */
  if (slot == TY_STRBUF && how == CO_HOLD && from == TY_STRING) return CF_STRBUF_HANDLE;
  if (from == TY_POLY && how == CO_HOLD) {
    if (repr_poly_rhs_ok(slot)) return CF_POLY_RHS;
    if (ty_is_array(slot) || ty_is_ptr_array(slot) || ty_is_hash(slot) || slot == TY_BIGINT ||
        slot == TY_STRBUF || slot == TY_CLASS || slot == TY_TIME || (ty_is_object(slot) && !repr_value_obj(c, slot)))
      return CF_CHECKED_UNBOX;
  }
  return repr_coerce_text_form(c, node, from, slot, how);
}

int repr_coerce_form(Compiler *c, int node, TyKind slot, int how) {
  return repr_coerce_plan(c, node, slot, how, NULL);
}

const char *repr_coerce_form_name(int form) {
  static const char *const names[CF__COUNT] = {
    "FIT", "BOX", "EMPTY_LIT", "NIL_SENT", "INT2BIG", "POLY_RHS", "CHECKED_UNBOX", "STRBUF_HANDLE", "CONVERT", "REFUSE",
  };
  return form >= 0 && form < CF__COUNT ? names[form] : "?";
}

int g_repr_check = 0;

/* repr_of is a pure read: asked anywhere, it changes nothing. Under
   --repr-check codegen asks it of every node it is about to emit, which an
   ordinary compile does not, and repr_check.sh fails when the C then
   differs from the C without the flag: a type recorded while a view was
   open re-materialized a Range from its own temp. */
void repr_check_ask(const Compiler *c, int node) {
  if (node >= 0 && node < c->nt->count) (void)repr_of(c, node);
}

/* Like the boxing shadow, this observes emission without changing it. The
   existing settled return walk records leaves, not a second return walk.
   Frames distinguish repeated emissions of a node; an enclosing frame
   drops abandoned trial frames just as pa_end does. */
typedef struct { int mi, active; unsigned char want, seen; } ChannelLeaf;
typedef struct { int node, form, ensure, clearing; } ChannelFrame;
struct ReprChannelCheck {
  ChannelLeaf *leaf;
  ChannelFrame *frame;
  int count, n, cap, clearing;
  unsigned long checked, conflicts, pickups;
};

static void channel_grow(struct ReprChannelCheck *q) {
  if (q->n < q->cap) return;
  if (q->cap > INT_MAX / 2) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  q->cap = q->cap ? q->cap * 2 : 16;
  q->frame = realloc(q->frame, (size_t)q->cap * sizeof *q->frame);
  if (!q->frame) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
}

static void channel_drop(struct ReprChannelCheck *q, int frame) {
  q->clearing = q->frame[frame].clearing;
  while (q->n > frame) {
    ChannelFrame *d = &q->frame[--q->n];
    if (!d->ensure) q->leaf[d->node].active = 0;
  }
}

void repr_channel_predict(Compiler *c, int node, int mi, int form) {
  if (node < 0) return;
  struct ReprChannelCheck *q = c->repr_channel_check;
  if (!q) {
    q = calloc(1, sizeof *q);
    if (!q) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    q->count = c->nt->count;
    q->leaf = calloc((size_t)q->count, sizeof *q->leaf);
    if (!q->leaf) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    c->repr_channel_check = q;
  }
  if (node >= q->count) {
    int count = c->nt->count;
    q->leaf = realloc(q->leaf, (size_t)count * sizeof *q->leaf);
    if (!q->leaf) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memset(q->leaf + q->count, 0, (size_t)(count - q->count) * sizeof *q->leaf);
    q->count = count;
  }
  q->leaf[node].mi = mi;
  q->leaf[node].want = (unsigned char)form;
}

int repr_channel_begin(Compiler *c, int node) {
  struct ReprChannelCheck *q = c->repr_channel_check;
  if (!q || node < 0 || node >= q->count) return -1;
  ChannelLeaf *l = &q->leaf[node];
  if (!l->want || l->active || l->mi <= 0 || l->mi >= c->nscopes) return -1;
  Scope *m = &c->scopes[l->mi];
  if (!m->ret_handle && !m->ret_channel_check) return -1;
  channel_grow(q);
  int f = q->n++;
  q->frame[f] = (ChannelFrame){ node, RCH_NONE, 0, q->clearing };
  l->active = f + 1;
  return f;
}


int repr_channel_ensure(Compiler *c, int node) {
  struct ReprChannelCheck *q = c->repr_channel_check;
  Scope *m = q ? comp_scope_of(c, node) : NULL;
  if (!m || (!m->ret_handle && !m->ret_channel_check)) return -1;
  channel_grow(q);
  int f = q->n++;
  q->frame[f] = (ChannelFrame){ node, RCH_NONE, 1, q->clearing };
  return f;
}

void repr_channel_note(Compiler *c, int node, int form) {
  struct ReprChannelCheck *q = c->repr_channel_check;
  if (!q || node < 0 || node >= q->count) return;
  /* An ensure can overwrite a saved return's channel even when its own
     value is dropped. Report the unproved preservation separately: an
     ensure that returns instead can deliberately supersede that value. */
  if (form == RCH_PUBLISH || form == RCH_CLEAR)
    for (int i = 0; i < q->n; i++) if (q->frame[i].ensure) q->frame[i].form = form;
  int f = q->leaf[node].active - 1;
  if (f >= 0 && f < q->n) q->frame[f].form = form;
}

static const char *channel_form(int form) {
  const char *const names[] = { "unobserved", "publish", "clear", "nil", "handle", "bytes", "boxed" };
  return form >= RCH_NONE && form <= RCH_BOXED ? names[form] : "?";
}

void repr_channel_end(Compiler *c, int frame) {
  struct ReprChannelCheck *q = c->repr_channel_check;
  if (!q || frame < 0 || frame >= q->n) return;
  ChannelFrame f = q->frame[frame];
  if (f.ensure) {
    if (f.form != RCH_NONE)
      fprintf(stderr, "repr-check: channel-gap: node %d ensure: emitted %s after saved value; preservation unproved\n",
              f.node, channel_form(f.form));
    channel_drop(q, frame);
    return;
  }
  ChannelLeaf *l = &q->leaf[f.node];
  if (q->clearing) f.form = RCH_CLEAR;
  int want = l->want;
  l->seen = 1;
  q->checked++;
  /* nil is decided by the returned bytes, before any pickup reads the
     channel. A handle-valued view/boxed return carries the value itself. */
  if (want != RCH_NIL && f.form != RCH_HANDLE && f.form != RCH_BOXED && f.form != want) {
    int conflict = want == RCH_CLEAR || f.form != RCH_NONE;
    const char *cls = conflict ? "channel-conflict" : "channel-unobserved";
    if (conflict) q->conflicts++;
    fprintf(stderr, "repr-check: %s: node %d %s method %s: emitted %s, predicted %s\n",
            cls, f.node, nt_type(c->nt, f.node), c->scopes[l->mi].name,
            channel_form(f.form), channel_form(want));
  }
  channel_drop(q, frame);
}

void repr_channel_call(Compiler *c, int node, int mi) {
  if (mi > 0 && mi < c->nscopes && c->scopes[mi].ret_handle)
    repr_channel_note(c, node, RCH_PUBLISH);
}

void repr_channel_boxed(Compiler *c, int node, int frame) {
  repr_channel_note(c, node, RCH_BOXED);
  repr_channel_end(c, frame);
}

void repr_channel_pickup(Compiler *c, int node, int nil_guard) {
  int mis[CPT_MAX], n = cplan_targets(c, node, mis, CPT_MAX);
  if (c->repr_channel_check) c->repr_channel_check->pickups++;
  for (int i = 0; i < n; i++) {
    Scope *m = &c->scopes[mis[i]];
    /* The earlier pickup proof also admits owned handles and mixed fresh
       tails. ret_handle is the later forwarding proof, not its negation. */
    if ((m->ret_handle || m->ret_channel_check) && (!m->ret_nil_pickup || nil_guard)) continue;
    fprintf(stderr, "repr-check: channel-conflict: node %d pickup method %s: "
            "ret_handle %d ret_fresh %d ret_nil_pickup %d nil_guard %d\n",
            node, m->name, m->ret_handle, m->ret_fresh, m->ret_nil_pickup, nil_guard);
  }
  if (n <= 0)
    fprintf(stderr, "repr-check: channel-unobserved: node %d pickup: targets %d\n", node, n);
}

void repr_channel_report(Compiler *c) {
  struct ReprChannelCheck *q = c->repr_channel_check;
  if (!q) return;
  int unseen = 0;
  for (int i = 0; i < q->count; i++) {
    ChannelLeaf *l = &q->leaf[i];
    if (l->want && !l->seen && l->mi > 0 && c->scopes[l->mi].ret_handle) unseen++;
  }
  fprintf(stderr, "repr-check: channel: %lu tails, %lu conflicts, %lu pickups, %d unemitted leaves\n",
          q->checked, q->conflicts, q->pickups, unseen);
}

void repr_channel_free(Compiler *c) {
  struct ReprChannelCheck *q = c->repr_channel_check;
  if (!q) return;
  free(q->leaf); free(q->frame); free(q);
  c->repr_channel_check = NULL;
}

ReprKind repr_slot_kind(const Compiler *c, const LocalVar *lv) {
  if (!lv) return RK_NONE;
  /* an Integer or Float slot that holds its nil beside the value */
  if (slot_is_oint(lv)) return RK_OPT;
  return repr_kind_of_type(c, lv->type);
}
Repr repr_of_slot(const Compiler *c, const LocalVar *lv) {
  Repr r;
  memset(&r, 0, sizeof r);
  r.ty = r.as_ty = r.narrowed = r.elem = r.key = r.val = r.range = TY_UNKNOWN;
  r.kind = RK_NONE;
  r.untyped = 1;
  if (!lv) return r;
  r.ty = r.as_ty = lv->type;
  repr_layout(&r, lv->type);
  ReprKind k = repr_slot_kind(c, lv);
  if (k == RK_OPT) r.may_nil = r.nil_scalar = 1;
  /* a `||=` can read the slot before any write: nil until then */
  if (lv->or_written) r.may_nil = 1;
  /* a pointer slot that can hold nil (repr_may_nil) */
  if (repr_may_nil(lv->type, lv->obj_may_nil)) r.may_nil = 1;
  /* str_shared refines TY_STRBUF; it can outlive that storage type */
  if (lv->type == TY_STRBUF && lv->str_shared) r.handle = 1;
  /* under the rule, that handle is the one it assigned */
  r.share = r.handle;
  r.elems_handle = lv->elems_shared && lv->type == TY_POLY_ARRAY;
  r.elem_nil_marked = (unsigned)repr_elem_nil(r.elem, lv->nullable_int_elem & NIE_MARKED, lv->obj_elem_may_nil);
  r.elem_nil_reach = (unsigned)repr_elem_nil(r.elem, lv->nullable_int_elem & (NIE_MARKED | NIE_REACH), 0);
  repr_slot_storage(&r, lv);
  r.kind = (unsigned char)k;
  repr_set_class_facts(c, &r, lv->type);
  return r;
}


/* Method and constructor signatures pass a kept &block as sp_Proc *.
   Check the slot as well as expression boxing: a read can infer Proc even
   when a copied parameter's local was left untyped and widened to POLY. */
static void repr_check_block_params(Compiler *c) {
  for (int si = 1; si < c->nscopes; si++) {
    Scope *sc = &c->scopes[si];
    if (!sc->blk_param || !sc->blk_param[0] || sc->yields) continue;
    LocalVar *lv = scope_local(sc, sc->blk_param);
    Repr r = repr_of_slot(c, lv);
    if (!lv || !lv->is_param || r.ty != TY_PROC || r.kind != RK_PTR)
      fprintf(stderr, "repr-check: conflict: method %s block parameter %s: "
              "slot %s, signature sp_Proc *\n", sc->name ? sc->name : "?",
              sc->blk_param, ty_name(r.ty));
  }
}

void repr_seal(Compiler *c) {
  if (g_repr_check) repr_check_block_params(c);
  repr_sealed_flag = 1;
  /* the String plan (share_plan.c), from the final facts and flags, asked
     as codegen will ask them: sealed */
  share_plan_build(c);
}


/* ---- --share-strings (#6765) ---- */

int repr_static_read_kind(NodeKind k) {
  return k == NK_GlobalVariableReadNode || k == NK_ConstantReadNode || k == NK_ConstantPathNode ||
         k == NK_ClassVariableReadNode;
}
/* repr_of's `share` for a read or write node of a global, a constant or a
   class variable: its slot's (only the rule makes one the handle) */
int repr_static_share(const Compiler *c, int node) {
  HolderRef h;
  if (node < 0 || !holder_static_node(nt_kind(c->nt, node))) return 0;
  return holder_of_node(c, node, &h) && (h.r.share || (c->n_cext_extensions && h.r.handle));
}


/* Does local lv of scope s hold a String the rule shares: one whose type
   can hold a String, but the loop variable a define_method unroll
   substitutes (collect_dm_each_unroll), which is no slot? */
int repr_local_shares(const Compiler *c, const Scope *s, const LocalVar *lv) {
  if (!s || !lv) return 0;
  if (s->dm_subst_name && lv->name && sp_streq(lv->name, s->dm_subst_name)) return 0;
  return sh_may_hold(c, lv->type);
}




/* repr.h: a container type whose C form holds its Strings as `const char *`.
   Under the one-String mode there is none: the typed containers hold the
   handle (sp_strelem, string-one-repr-design.md). */
int repr_typed_str_container(TyKind t) {
  (void)t;
  return 0;
}

int repr_str_literal_shares(Compiler *c, int node) {
  if (node < 0) return 0;
  NodeKind k = nt_kind(c->nt, node);
  return (k == NK_ArrayNode || k == NK_HashNode) &&
         repr_typed_str_container(c->ntype[node]) && comp_scope_of(c, node)->reachable;
}










/* ---- --dump-repr (#7501) ----
   One line per slot with the representation chosen for it, sorted, so the
   dumps two compilers give for one program can be diffed. A slot that
   became a shared handle or a box, or left a by-value layout, costs at run
   time without changing any output (#7482), and this is where it shows.
   Locals, parameters, globals and constants are LocalVars and read through
   repr_of_slot. An ivar, a class variable and a method's value are not:
   repr_of_ivar, repr_of_cvar and repr_of_ret read their own flags by the
   same rules. The dump only reads. It is taken once the
   analysis is final and printed once the compile has passed, so a program
   codegen refuses fails as a compile does; no C is written. */
int g_dump_repr = 0;

static const char *repr_kind_name(int k) {
  static const char *const names[] = {
    "none", "scalar", "opt", "struct", "vobj", "ptr", "strbuf", "boxed",
  };
  return k >= 0 && k <= RK_BOXED ? names[k] : "?";
}

/* an ivar's slot: an Integer or Float one keeps its nil in its nil byte
   (ivar_has_nilbit) when some write can leave nil in it, or when it can be
   read before any write (initialize does not write it, Class#allocate
   makes the object), which its reads box nil-aware (box_nullable_arg) */
Repr repr_of_ivar(const Compiler *c, int cid, int iv) {
  const ClassInfo *ci = &c->classes[cid];
  Repr r;
  memset(&r, 0, sizeof r);
  r.ty = r.as_ty = ci->ivar_types[iv];
  r.narrowed = TY_UNKNOWN;
  repr_layout(&r, r.ty);
  r.kind = (unsigned char)repr_kind_of_type(c, r.ty);
  if (ivar_has_nilbit((Compiler *)c, cid, iv)) {
    r.kind = RK_OPT;
    r.may_nil = r.nil_scalar = 1;
  }
  if (repr_may_nil(r.ty, nil_fact_ivar(c, cid, ci->ivars[iv]))) r.may_nil = 1;
  if (r.ty == TY_STRBUF && ci->ivar_str_shared[iv]) r.handle = 1;
  r.share = r.handle;
  r.elems_handle = (ci->ivar_elems_shared[iv] & ELEMS_POLY) && r.ty == TY_POLY_ARRAY;
  /* the element marking sits on the family's topmost class that has the
     ivar, where every subclass reads it (nullable_elem_ivar_in) */
  int ec = cid, ek = iv;
  for (int p = ci->parent; p >= 0 && p < c->nclasses; p = c->classes[p].parent) {
    int k = comp_ivar_index((ClassInfo *)&c->classes[p], ci->ivars[iv]);
    if (k < 0) break;
    ec = p; ek = k;
  }
  r.elem_nil_marked = (unsigned)repr_elem_nil(r.elem, c->classes[ec].ivar_nullable_int_elem[ek] & NIE_MARKED,
                                               iv < ci->n_ivar_obj_may_nil && ci->ivar_elem_may_nil &&
                                               ci->ivar_elem_may_nil[iv]);
  r.elem_nil_reach = (unsigned)repr_elem_nil(r.elem, c->classes[ec].ivar_nullable_int_elem[ek] & (NIE_MARKED | NIE_REACH), 0);
  repr_set_class_facts(c, &r, r.ty);
  return r;
}

/* a class variable's slot, as an ivar's: an Integer or Float one some
   write leaves nil in is an oint (cvar_is_oint) */
ReprKind repr_cvar_kind(const Compiler *c, int cid, int idx) {
  const ClassInfo *ci = &c->classes[cid];
  TyKind t = ci->cvar_types[idx];
  if (cvar_is_oint((Compiler *)c, cid, idx)) return RK_OPT;
  return repr_kind_of_type(c, t);
}
/* Only the rule makes a class variable the shared handle
   (cvar_str_shared). A class variable has no nil fact (analyze_nil.c keeps
   one per ivar): a pointer one may be nil (repr_may_nil), as nil_fact_ivar
   answers for an ivar it has none for. */
Repr repr_of_cvar(const Compiler *c, int cid, int idx) {
  const ClassInfo *ci = &c->classes[cid];
  Repr r;
  memset(&r, 0, sizeof r);
  r.ty = r.as_ty = ci->cvar_types[idx];
  r.narrowed = r.elem = r.key = r.val = TY_UNKNOWN;
  repr_layout(&r, r.ty);
  r.kind = (unsigned char)repr_cvar_kind(c, cid, idx);
  if (r.kind == RK_OPT) r.may_nil = r.nil_scalar = 1;
  if (repr_may_nil(r.ty, 1)) r.may_nil = 1;
  if (r.ty == TY_STRBUF && ci->cvar_str_shared[idx]) r.handle = 1;
  r.share = r.handle;
  r.elems_handle = (ci->cvar_elems_shared[idx] & ELEMS_POLY) && r.ty == TY_POLY_ARRAY;
  repr_set_class_facts(c, &r, r.ty);
  return r;
}

/* a method's value: its nilable scalar (ret_nullable_int, or an RBS
   signature's `?`) is an oint */
Repr repr_of_ret(const Compiler *c, const Scope *sc) {
  Repr r;
  memset(&r, 0, sizeof r);
  r.ty = r.as_ty = sc->ret;
  r.narrowed = TY_UNKNOWN;
  repr_layout(&r, r.ty);
  r.kind = (unsigned char)repr_kind_of_type(c, r.ty);
  if ((r.ty == TY_INT || r.ty == TY_FLOAT) && (sc->ret_nullable_int || sc->ret_rbs_nilable)) {
    r.kind = RK_OPT;
    r.may_nil = r.nil_scalar = 1;
  }
  if (repr_may_nil(r.ty, sc->ret_obj_may_nil)) r.may_nil = 1;
  repr_set_class_facts(c, &r, r.ty);
  return r;
}

/* a type's name, with the class of a user object or of an object array */
static void repr_dump_ty(const Compiler *c, TyKind t, char *out, size_t n) {
  TyKind e = ty_is_obj_array(t) ? ty_array_elem(t) : t;
  if (ty_is_object(e) && ty_object_class(e) < c->nclasses)
    snprintf(out, n, "%s%s", ty_is_obj_array(t) ? "obj_array:" : "obj:",
             c->classes[ty_object_class(e)].name);
  else snprintf(out, n, "%s", ty_name(t));
}

typedef struct { char **v; int n, cap; } ReprLines;

/* where a local lives, as the dump prints it */
static const char *repr_cell_name(int cell) {
  switch ((ReprCell)cell) {
  case RC_NONE:  return "";
  case RC_HEAP:  return " cell=heap";
  case RC_BYREF: return " cell=byref";
  case RC_ALIAS: return " cell=alias";
  }
  return " cell=?";
}

static void repr_dump_line(const Compiler *c, ReprLines *ls, const char *where, Repr r) {
  char ty[256], line[1024];
  repr_dump_ty(c, r.ty, ty, sizeof ty);
  snprintf(line, sizeof line, "%s: %s ty=%s%s%s%s%s%s%s%s", where, repr_kind_name(r.kind), ty,
           r.handle ? " handle" : "", r.may_nil ? " may_nil" : "", r.dyn_cls ? " dyn_cls" : "",
           r.elem_nil_marked ? " elem_nil" : "", r.arr_or_nil ? " arr_or_nil" : "",
           repr_cell_name(r.cell), r.volatile_str ? " volatile" : "");
  if (ls->n == ls->cap) {
    ls->cap = ls->cap ? ls->cap * 2 : 64;
    ls->v = realloc(ls->v, (size_t)ls->cap * sizeof *ls->v);
  }
  ls->v[ls->n++] = strdup(line);
}

static int repr_line_cmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

/* A method's name as the dump prints it: Class#m, Class.m for a class
   method, <main> for the top level, and a mark for each copy of a method
   another scope also emits. */
static void repr_scope_label(const Compiler *c, const Scope *sc, char *out, size_t n) {
  const char *cls = sc->class_id >= 0 && sc->class_id < c->nclasses ? c->classes[sc->class_id].name : NULL;
  snprintf(out, n, "%s%s%s%s%s%s", cls ? cls : "", cls ? (sc->is_cmethod ? "." : "#") : "",
           sc->name ? sc->name : "<main>", sc->is_proc_form ? "(proc)" : "",
           sc->is_include_copy ? "(include)" : "", sc->is_extend_copy ? "(extend)" : "");
}

char *repr_dump(const Compiler *c) {
  ReprLines ls = {0};
  char where[1024], label[512];
  for (int si = 0; si < c->nscopes; si++) {
    const Scope *sc = &c->scopes[si];
    /* a method nothing calls is not run */
    if (sc->def_node >= 0 && !sc->reachable) continue;
    repr_scope_label(c, sc, label, sizeof label);
    for (int k = 0; k < sc->nlocals; k++) {
      const LocalVar *lv = &sc->locals[k];
      if (!lv->name) continue;
      snprintf(where, sizeof where, "%s %s %s", lv->is_param ? "param" : "local", label, lv->name);
      repr_dump_line(c, &ls, where, repr_of_slot(c, lv));
    }
    if (sc->def_node >= 0) {
      snprintf(where, sizeof where, "ret %s", label);
      repr_dump_line(c, &ls, where, repr_of_ret(c, sc));
    }
  }
  for (int cid = 0; cid < c->nclasses; cid++)
    for (int iv = 0; iv < c->classes[cid].nivars; iv++) {
      snprintf(where, sizeof where, "ivar %s %s", c->classes[cid].name, c->classes[cid].ivars[iv]);
      repr_dump_line(c, &ls, where, repr_of_ivar(c, cid, iv));
    }
  for (int cid = 0; cid < c->nclasses; cid++)
    for (int cv = 0; cv < c->classes[cid].ncvars; cv++) {
      snprintf(where, sizeof where, "cvar %s %s", c->classes[cid].name, c->classes[cid].cvars[cv]);
      repr_dump_line(c, &ls, where, repr_of_cvar(c, cid, cv));
    }
  for (int k = 0; k < c->ngvars; k++) {
    snprintf(where, sizeof where, "gvar $%s", c->gvars[k].name);
    repr_dump_line(c, &ls, where, repr_of_slot(c, &c->gvars[k]));
  }
  for (int k = 0; k < c->nconsts; k++) {
    snprintf(where, sizeof where, "const %s", c->consts[k].name);
    repr_dump_line(c, &ls, where, repr_of_slot(c, &c->consts[k]));
  }
  qsort(ls.v, (size_t)ls.n, sizeof *ls.v, repr_line_cmp);
  size_t len = 1;
  for (int k = 0; k < ls.n; k++) len += strlen(ls.v[k]) + 1;
  char *out = malloc(len), *o = out;
  for (int k = 0; k < ls.n; k++) {
    size_t n = strlen(ls.v[k]);
    memcpy(o, ls.v[k], n);
    o[n] = '\n';
    o += n + 1;
    free(ls.v[k]);
  }
  *o = 0;
  free(ls.v);
  return out;
}

