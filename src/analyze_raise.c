/* analyze_raise.c -- conservative whole-program may_raise / nothrow analysis.

   One place per decision (#7100, #8328): determines once, after types,
   representation, nil facts, class facts and reachability have settled, which
   scopes and subtrees provably cannot raise any Ruby exception or perform a
   non-local unwind (throw, proc return, sp_brk_throw, exit, signal/finalizer
   raise). Codegen reads the settled facts for both setjmp emission
   (emit_begin, RescueModifierNode) and volatile local qualification
   (is_setjmp_construct, scope_emits_setjmp, begin_volatile_names), so the two
   never disagree.

   Soundness invariant: false negatives are forbidden. Every unknown, dynamic,
   runtime, FFI, C-extension, stdlib, loop, cyclic-call, unchecked-op,
   checked-unbox, frozen-ivar-write, or non-local-control-transfer construct
   defaults to may_raise = 1. A scope or begin body is marked nothrow (0) ONLY
   when every expression, statement, implicit conversion, parameter binding,
   return coercion, and transitive callee (including caller blocks for yield)
   is provably non-raising. */

#include "analyze_internal.h"
#include "builtin_names.h"
#include "call_plan.h"
#include "implicit_calls.h"
#include "repr.h"
#include <stdlib.h>
#include <string.h>

#define RF_MAX_ENSURE_DEPTH 32

typedef struct {
  Compiler *c;
  const NodeTable *nt;
  const int *par;
  unsigned char *scope_mr;    /* [ns] 1 = may raise / non-locally unwind */
  int ns;
  int uses_finalizers;        /* program references Finalizers__ -> SP_FIN_POLL at method entry */
  ANameHash reopened;         /* method names defined in builtin reopenings, top-level, or undef'd */
  /* call graph edges: caller -> callees */
  int **tg;
  int *ntg, *ctg;
  /* yield call-site tracking per scope */
  unsigned char *yield_unk;   /* [ns] 1 = unknown/untracked/complex block caller */
  int **yblk;                 /* [ns] array of caller BlockNode ids */
  int *nyblk, *cyblk;
  /* direct subclass adjacency list: cls_head[parent] -> cls_next[child] */
  int *cls_head, *cls_next;
} RF;

static void *rf_xrealloc(void *p, size_t sz) {
  void *r = realloc(p, sz);
  if (!r) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  return r;
}

static void rf_add_edge(RF *f, int from, int to) {
  if (from < 0 || from >= f->ns || to < 0 || to >= f->ns) return;
  for (int i = 0; i < f->ntg[from]; i++)
    if (f->tg[from][i] == to) return;
  if (f->ntg[from] == f->ctg[from]) {
    f->ctg[from] = f->ctg[from] ? f->ctg[from] * 2 : 8;
    f->tg[from] = (int *)rf_xrealloc(f->tg[from], sizeof(int) * (size_t)f->ctg[from]);
  }
  f->tg[from][f->ntg[from]++] = to;
}

static void rf_add_yblk(RF *f, int mi, int blk) {
  if (mi < 0 || mi >= f->ns || blk < 0) return;
  for (int i = 0; i < f->nyblk[mi]; i++)
    if (f->yblk[mi][i] == blk) return;
  if (f->nyblk[mi] == f->cyblk[mi]) {
    f->cyblk[mi] = f->cyblk[mi] ? f->cyblk[mi] * 2 : 4;
    f->yblk[mi] = (int *)rf_xrealloc(f->yblk[mi], sizeof(int) * (size_t)f->cyblk[mi]);
  }
  f->yblk[mi][f->nyblk[mi]++] = blk;
}

/* Is a class a plain user class (not native, cext, Struct, Data, builtin,
   singleton subclass, or subclass of a builtin like Array/Hash/String/Time/Exception)? */
static int rf_plain_user_class(const Compiler *c, int k) {
  if (k < 0 || k >= c->nclasses) return 0;
  const ClassInfo *ci = &c->classes[k];
  if (ci->is_native_class || ci->cext_extension || ci->is_struct || ci->is_data ||
      ci->is_builtin_const || ci->is_singleton_of || ci->ary_root)
    return 0;
  if (ci->def_node < 0) return 0;
  if (ci->parent < 0 && class_def_super(c->nt, ci->def_node) >= 0) return 0;
  return 1;
}

/* Can node `id` with representation `r` be nil at runtime?
   Checks both Repr.may_nil and nil_fact_node (the latter also tracks TY_STRBUF
   handles whose null_is_nil trait is 0). */
static int rf_may_nil(const Compiler *c, int id, Repr r) {
  if (r.may_nil) return 1;
  if (id >= 0 && (r.kind == RK_PTR || r.kind == RK_STRBUF || r.kind == RK_VOBJ) &&
      nil_fact_node(c, id))
    return 1;
  return 0;
}

/* Can storing a value with representation `src` into a slot with representation
   `dst` perform a checked unboxing or nil-unwrap that raises TypeError? */
static int rf_store_may_raise(Compiler *c, Repr dst, Repr src, int src_node) {
  int src_nil = rf_may_nil(c, src_node, src);
  if (dst.kind == RK_NONE)
    return !(dst.as_ty == TY_VOID || dst.as_ty == TY_NIL);
  if (src.kind == RK_NONE) return 1;
  if (dst.kind == RK_BOXED) return 0;
  if (src.kind == RK_BOXED) return 1;
  if (dst.kind == RK_SCALAR) {
    if (src_nil) return 1;
    if (dst.as_ty == src.as_ty) return 0;
    if (dst.as_ty == TY_FLOAT && src.as_ty == TY_INT) return 0;
    return 1;
  }
  if (dst.kind == RK_OPT) {
    if (src.as_ty == TY_NIL) return 0;
    if (src.as_ty == dst.as_ty) return 0;
    if (dst.as_ty == TY_FLOAT && src.as_ty == TY_INT) return 0;
    return 1;
  }
  if (dst.kind == RK_PTR || dst.kind == RK_STRBUF) {
    if (src.as_ty == TY_NIL) return 0;
    if ((dst.as_ty == TY_STRING || dst.as_ty == TY_STRBUF) &&
        (src.as_ty == TY_STRING || src.as_ty == TY_STRBUF))
      return 0;
    if (dst.as_ty == src.as_ty) return 0;
    if (ty_is_object(dst.as_ty) && ty_is_object(src.as_ty) &&
        is_descendant(c, ty_object_class(src.as_ty), ty_object_class(dst.as_ty)))
      return 0;
    return 1;
  }
  if (dst.kind == RK_VOBJ || dst.kind == RK_STRUCT)
    return !(dst.as_ty == src.as_ty && !src_nil);
  return 1;
}

/* Does a Scope have complex parameter forms (rest, kw, kwrest, posts, ..., &blk)
   whose call-site binding we treat conservatively? */
static int rf_scope_has_complex_params(const Compiler *c, const Scope *sc) {
  if (sc->rest_idx >= 0 || sc->kwrest_idx >= 0 || sc->npost_rest > 0 ||
      sc->params_later || (sc->blk_param && sc->blk_param[0]))
    return 1;
  if (sc->def_node < 0) return 1;
  int pn = nt_ref(c->nt, sc->def_node, "parameters");
  if (pn < 0) return 0;
  int nkw = 0, nposts = 0;
  nt_arr(c->nt, pn, "keywords", &nkw);
  nt_arr(c->nt, pn, "posts", &nposts);
  if (nkw > 0 || nposts > 0) return 1;
  if (nt_ref(c->nt, pn, "keyword_rest") >= 0) return 1;
  if (nt_ref(c->nt, pn, "rest") >= 0) return 1;
  if (nt_ref(c->nt, pn, "block") >= 0) return 1;
  return 0;
}

/* Does a BlockNode have keyword/destructuring/splat parameters that can raise
   ArgumentError or TypeError (to_ary) on yield? */
static int rf_block_has_complex_params(const NodeTable *nt, int blk) {
  int bps = nt_ref(nt, blk, "parameters");
  if (bps < 0) return 0;
  NodeKind bk = nt_kind(nt, bps);
  if (bk == NK_NumberedParametersNode || bk == NK_ItParametersNode) return 0;
  if (bk != NK_BlockParametersNode) return 1;
  int pn = nt_ref(nt, bps, "parameters");
  if (pn < 0) return 0;
  int nkw = 0, nreq = 0, nopt = 0, nposts = 0;
  const int *reqs = nt_arr(nt, pn, "requireds", &nreq);
  nt_arr(nt, pn, "optionals", &nopt);
  nt_arr(nt, pn, "keywords", &nkw);
  nt_arr(nt, pn, "posts", &nposts);
  if (nkw > 0 || nopt > 0 || nposts > 0) return 1;
  if (nt_ref(nt, pn, "keyword_rest") >= 0 || nt_ref(nt, pn, "rest") >= 0 ||
      nt_ref(nt, pn, "block") >= 0)
    return 1;
  for (int i = 0; i < nreq; i++)
    if (nt_kind(nt, reqs[i]) != NK_RequiredParameterNode) return 1;
  return 0;
}

/* Extract a small signed 64-bit integer literal value (no bigval). */
static int rf_small_int_lit(const NodeTable *nt, int id, long long *out) {
  if (id < 0 || nt_kind(nt, id) != NK_IntegerNode) return 0;
  if (nt_str(nt, id, "bigval")) return 0;
  *out = nt_int(nt, id, "value", 0);
  return 1;
}

/* Extract a float literal value. */
static int rf_float_lit(const NodeTable *nt, int id, double *out) {
  if (id < 0 || nt_kind(nt, id) != NK_FloatNode) return 0;
  const char *s = nt_content(nt, id);
  *out = strtod(s ? s : "0", NULL);
  return 1;
}

/* Is a builtin integer binary op (+, -, *, /, %, >>, etc.) provably non-raising
   on two non-nil TY_INT operands, accounting for --int-overflow=raise|wrap? */
static int rf_int_binop_may_raise(const NodeTable *nt, const char *nm, int recv, int arg) {
  if (sp_streq(nm, "==") || sp_streq(nm, "!=") || sp_streq(nm, "<") || sp_streq(nm, ">") ||
      sp_streq(nm, "<=") || sp_streq(nm, ">=") || sp_streq(nm, "<=>") ||
      sp_streq(nm, "&") || sp_streq(nm, "|") || sp_streq(nm, "^"))
    return 0;
  if (sp_streq(nm, "+") || sp_streq(nm, "-") || sp_streq(nm, "*")) {
    if (g_wrap_mode) return 0;
    long long a = 0, b = 0;
    if (!rf_small_int_lit(nt, recv, &a) || !rf_small_int_lit(nt, arg, &b)) return 1;
    long long lim = sp_streq(nm, "*") ? 46340LL : 1000000000LL;
    return !(a >= -lim && a <= lim && b >= -lim && b <= lim);
  }
  if (sp_streq(nm, "/")) {
    long long b = 0;
    if (!rf_small_int_lit(nt, arg, &b) || b == 0) return 1;
    if (b == -1 && !g_wrap_mode) {
      long long a = 0;
      if (!rf_small_int_lit(nt, recv, &a) || a < -1000000000LL || a > 1000000000LL) return 1;
    }
    return 0;
  }
  if (sp_streq(nm, "%")) {
    long long b = 0;
    return !(rf_small_int_lit(nt, arg, &b) && b != 0);
  }
  if (sp_streq(nm, ">>")) {
    long long b = 0;
    return !(rf_small_int_lit(nt, arg, &b) && b >= 0 && b <= 63);
  }
  return 1;
}

/* Is a builtin float (or mixed int/float) binary op provably non-raising
   on two non-nil numeric operands? */
static int rf_float_binop_may_raise(const NodeTable *nt, const char *nm, int arg) {
  if (sp_streq(nm, "+") || sp_streq(nm, "-") || sp_streq(nm, "*") || sp_streq(nm, "/") ||
      sp_streq(nm, "<") || sp_streq(nm, ">") || sp_streq(nm, "<=") || sp_streq(nm, ">=") ||
      sp_streq(nm, "==") || sp_streq(nm, "!="))
    return 0;
  if (sp_streq(nm, "%")) {
    long long bi = 0; double bf = 0.0;
    if (rf_small_int_lit(nt, arg, &bi)) return bi == 0;
    if (rf_float_lit(nt, arg, &bf)) return bf == 0.0;
    return 1;
  }
  return 1;
}

/* Is a builtin call (pl.dispatch == CP_NONE, no block, no implicit user callbacks,
   name not reopened on any builtin class) provably non-raising? */
static int rf_builtin_call_may_raise(RF *f, int id, const char *nm, int recv, int argc, const int *argv) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  if (recv < 0) return 1;
  if (anh_find(&f->reopened, nm) >= 0) return 1;
  Repr rr = repr_of(c, recv);
  Repr vr = repr_of(c, id);
  if (rr.kind == RK_NONE || rr.kind == RK_BOXED || rr.big) return 1;
  if (vr.kind == RK_NONE || vr.big) return 1;
  TyKind rt = rr.as_ty;

  /* 0-arg pure predicates / unary ops */
  if (argc == 0) {
    if (sp_streq(nm, "!")) return 0;
    if (sp_streq(nm, "nil?")) return 0;
    if (rf_may_nil(c, recv, rr)) return 1;
    if (rt == TY_INT) {
      if (sp_streq(nm, "+@") || sp_streq(nm, "~") || sp_streq(nm, "even?") ||
          sp_streq(nm, "odd?") || sp_streq(nm, "zero?") || sp_streq(nm, "positive?") ||
          sp_streq(nm, "negative?") || sp_streq(nm, "to_i") || sp_streq(nm, "to_f"))
        return 0;
      if (sp_streq(nm, "-@") || sp_streq(nm, "abs") || sp_streq(nm, "succ") ||
          sp_streq(nm, "next") || sp_streq(nm, "pred")) {
        if (g_wrap_mode) return 0;
        long long a = 0;
        return !(rf_small_int_lit(nt, recv, &a) && a >= -1000000000LL && a <= 1000000000LL);
      }
      return 1;
    }
    if (rt == TY_FLOAT) {
      if (sp_streq(nm, "-@") || sp_streq(nm, "+@") || sp_streq(nm, "abs") ||
          sp_streq(nm, "zero?") || sp_streq(nm, "positive?") || sp_streq(nm, "negative?") ||
          sp_streq(nm, "nan?") || sp_streq(nm, "infinite?") || sp_streq(nm, "finite?") ||
          sp_streq(nm, "to_f"))
        return 0;
      return 1;
    }
    if ((rt == TY_STRING || rt == TY_STRBUF) &&
        nt_kind(nt, recv) == NK_StringNode &&
        (sp_streq(nm, "length") || sp_streq(nm, "size") || sp_streq(nm, "bytesize") || sp_streq(nm, "empty?")))
      return 0;
    if ((ty_is_array(rt) || ty_is_obj_array(rt)) &&
        nt_kind(nt, recv) == NK_ArrayNode &&
        (sp_streq(nm, "length") || sp_streq(nm, "size") || sp_streq(nm, "empty?")))
      return 0;
    return 1;
  }

  /* 1-arg pure scalar binary ops */
  if (argc == 1) {
    Repr ar = repr_of(c, argv[0]);
    if (ar.kind == RK_NONE || ar.kind == RK_BOXED || ar.big) return 1;
    TyKind at = ar.as_ty;
    if ((sp_streq(nm, "==") || sp_streq(nm, "!=")) &&
        (rt == TY_INT || rt == TY_FLOAT || rt == TY_BOOL || rt == TY_SYMBOL || rt == TY_NIL) &&
        (at == TY_INT || at == TY_FLOAT || at == TY_BOOL || at == TY_SYMBOL || at == TY_NIL))
      return 0;
    if (rf_may_nil(c, recv, rr) || rf_may_nil(c, argv[0], ar)) return 1;
    if (rt == TY_INT && at == TY_INT)
      return rf_int_binop_may_raise(nt, nm, recv, argv[0]);
    if ((rt == TY_FLOAT || rt == TY_INT) && (at == TY_FLOAT || at == TY_INT))
      return rf_float_binop_may_raise(nt, nm, argv[0]);
    return 1;
  }

  return 1;
}

/* Return 1 if class `k` or any descendant of `k` undefs `nm` (or makes `nm`
   non-public at `id` when `check_vis` is set). Uses the call plan so no direct
   method-table primitive is read outside call_plan.c, and walks the precomputed
   direct-subclass tree so leaf classes pay O(1). */
static int rf_class_or_sub_undef_or_closed(RF *f, int id, int k, const char *nm, int check_vis, int depth) {
  Compiler *c = f->c;
  if (k < 0 || k >= c->nclasses || !nm || depth > 128) return 1;
  if (cplan_undefined(c, k, 0, nm)) return 1;
  if (check_vis && cplan_method_vis(c, id, k, nm, 0, NULL) != SP_VIS_PUBLIC) return 1;
  if (f->cls_head) {
    for (int d = f->cls_head[k]; d >= 0; d = f->cls_next[d])
      if (rf_class_or_sub_undef_or_closed(f, id, d, nm, check_vis, depth + 1)) return 1;
    return 0;
  }
  for (int d = 0; d < c->nclasses; d++) {
    if (d == k || !is_descendant(c, d, k)) continue;
    if (cplan_undefined(c, d, 0, nm)) return 1;
    if (check_vis && cplan_method_vis(c, id, d, nm, 0, NULL) != SP_VIS_PUBLIC) return 1;
  }
  return 0;
}

/* Return 1 if class `k` or any descendant of `k` has freeze_observed set
   (codegen emits sp_check_frozen on ivar writes when any descendant can be frozen). */
static int rf_class_or_sub_frozen(RF *f, int k, int depth) {
  Compiler *c = f->c;
  if (k < 0 || k >= c->nclasses || depth > 128) return 1;
  if (c->classes[k].freeze_observed) return 1;
  if (f->cls_head) {
    for (int d = f->cls_head[k]; d >= 0; d = f->cls_next[d])
      if (rf_class_or_sub_frozen(f, d, depth + 1)) return 1;
    return 0;
  }
  for (int d = 0; d < c->nclasses; d++)
    if (d != k && is_descendant(c, d, k) && c->classes[d].freeze_observed) return 1;
  return 0;
}

/* Check one user method target `mi` for call `id`: arity, visibility,
   parameter binding coercions, and (if from_scope < 0) settled scope_mr. */
static int rf_check_user_target(RF *f, int from_scope, int id, int mi,
                                int recv, int argc, const int *argv) {
  Compiler *c = f->c;
  if (mi < 0 || mi >= f->ns) return 1;
  if (f->uses_finalizers) return 1;
  const Scope *m = &c->scopes[mi];
  if (!m->name || m->def_node < 0 || m->body < 0 || m->entry != ENTRY_NONE ||
      m->versioned || m->is_proc_form || m->is_lowered_yield ||
      m->is_ext_entry || m->is_ext_method || m->ret_rbs_cont ||
      rf_scope_has_complex_params(c, m))
    return 1;
  int check_vis = (nt_str(f->nt, id, "vis_enforce") != NULL) ||
                  (recv >= 0 && !nt_ignore_visibility(f->nt, id));
  if (!m->is_cmethod && (m->class_id < 0 || m->class_id == c->toplevel_class)) {
    if (check_vis && cplan_top_def_vis(c, id, m->name) != SP_VIS_PUBLIC) return 1;
    if (m->class_id >= 0 && cplan_undefined(c, m->class_id, 0, m->name)) return 1;
  }
  else if (!m->is_cmethod && m->class_id >= 0 && m->class_id < c->nclasses) {
    if (rf_class_or_sub_undef_or_closed(f, id, m->class_id, m->name, check_vis, 0))
      return 1;
  }
  else if (check_vis && m->is_cmethod && m->class_id >= 0 && m->class_id < c->nclasses &&
           (c->classes[m->class_id].ncm_vis > 0 ||
            !cplan_cmethod_visible(c, id, m->class_id, m->name) ||
            cplan_method_vis(c, id, m->class_id, m->name, CPV_CMETHOD, NULL) != SP_VIS_PUBLIC)) {
    return 1;
  }
  /* arity check (requireds + optionals, no rest/kw/posts) */
  if (argc < m->nrequired || argc > m->nparams) return 1;
  /* parameter binding / unboxing check */
  for (int i = 0; i < argc; i++) {
    const LocalVar *p = m->pnames[i] ? scope_local((Scope *)m, m->pnames[i]) : NULL;
    if (!p) return 1;
    Repr dst = repr_of_slot(c, p);
    Repr src = repr_of(c, argv[i]);
    if (rf_store_may_raise(c, dst, src, argv[i])) return 1;
  }
  if (from_scope >= 0) {
    if (f->scope_mr[mi]) return 1;
    rf_add_edge(f, from_scope, mi);
    return 0;
  }
  return f->scope_mr[mi] ? 1 : 0;
}

static int rf_node(RF *f, int from_scope, int id, int in_block, int depth);

/* Walk a NK_CallNode. Returns 1 if it may raise or non-locally unwind. */
static int rf_call(RF *f, int from_scope, int id, int in_block, int depth) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm) return 1;
  int recv = nt_ref(nt, id, "receiver");
  int blk = nt_ref(nt, id, "block");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;

  /* Reject splats, keyword hashes, forwarding args, and block-arg (&b) */
  for (int i = 0; i < argc; i++) {
    NodeKind ak = nt_kind(nt, argv[i]);
    if (ak == NK_SplatNode || ak == NK_KeywordHashNode ||
        ak == NK_ForwardingArgumentsNode || ak == NK_BlockArgumentNode)
      return 1;
    if (rf_node(f, from_scope, argv[i], in_block, depth + 1)) return 1;
  }
  if (blk >= 0 && nt_kind(nt, blk) != NK_BlockNode) return 1;
  if (recv >= 0 && rf_node(f, from_scope, recv, in_block, depth + 1)) return 1;
  if (ic_node_may_call_program(c, id)) return 1;
  if (cplan_nil(c, id) != CN_NONE) return 1;
  if (cplan_missing(c, id, NULL, 0, NULL) != CM_NONE) return 1;

  CallPlan pl = cplan_user(c, id);
  if (pl.dispatch == CP_REFUSE || pl.vis_cell || pl.closed || pl.absent ||
      pl.delegate_undef || pl.method_call || pl.by_name ||
      pl.via == UC_SEND_BLIND || pl.via == UC_POLY || pl.via == UC_IE)
    return 1;

  if (pl.dispatch == CP_DIRECT || pl.dispatch == CP_SWITCH || pl.dispatch == CP_PER_ARM) {
    int check_vis = (nt_str(nt, id, "vis_enforce") != NULL) ||
                    (recv >= 0 && !nt_ignore_visibility(nt, id));
    /* Verify receiver cannot hit a runtime NoMethodError/NameError (nil, boxed,
       partial builtin reopening like TrueClass/FalseClass, private class method,
       or descendant-only dispatch miss) */
    if (recv >= 0) {
      Repr rr = repr_of(c, recv);
      if (rr.kind == RK_NONE || rr.kind == RK_BOXED) return 1;
      if (ty_is_object(rr.as_ty)) {
        if (rf_may_nil(c, recv, rr) && !nt_safe_nav(nt, id)) return 1;
        int k = ty_object_class(rr.as_ty);
        if (k < 0 || k >= c->nclasses || c->classes[k].is_singleton_of) return 1;
        if (cplan_dispatch_miss(c, id, k) != CM_NONE) return 1;
        if (call_target(c, id, k, nm, 0, NULL) < 0) return 1;
        if (rf_class_or_sub_undef_or_closed(f, id, k, nm, check_vis, 0)) return 1;
      }
      else if (rr.as_ty == TY_CLASS) {
        NodeKind rk = nt_kind(nt, recv);
        if (rk != NK_ConstantReadNode && rk != NK_ConstantPathNode) return 1;
        if (pl.dispatch != CP_DIRECT) return 1;
        int rk_ci = const_node_class_index(c, recv);
        if (rk_ci < 0 || rk_ci >= c->nclasses) return 1;
        if (check_vis &&
            (!cplan_cmethod_visible(c, id, rk_ci, nm) ||
             cplan_method_vis(c, id, rk_ci, nm, CPV_CMETHOD, NULL) != SP_VIS_PUBLIC))
          return 1;
      }
      else {
        /* Reopened builtin class (e.g., TrueClass/FalseClass, String, Integer):
           may dispatch partially or raise on the other side / nil */
        return 1;
      }
    }
    else {
      Scope *sc = comp_scope_of(c, id);
      if (sc && sc->class_id >= 0 && !sc->is_cmethod && sc->class_id != c->toplevel_class) {
        int is_top = (pl.dispatch == CP_DIRECT && pl.mi >= 0 &&
                      (c->scopes[pl.mi].class_id < 0 || c->scopes[pl.mi].class_id == c->toplevel_class));
        if (!is_top) {
          if (cplan_dispatch_miss(c, id, sc->class_id) != CM_NONE) return 1;
          if (call_target(c, id, sc->class_id, nm, 0, NULL) < 0) return 1;
          if (rf_class_or_sub_undef_or_closed(f, id, sc->class_id, nm, check_vis, 0)) return 1;
        }
      }
    }
    if (blk >= 0) {
      /* Only allow literal block on a direct user method whose block is non-raising */
      if (pl.dispatch != CP_DIRECT || pl.mi < 0) return 1;
      if (rf_block_has_complex_params(nt, blk)) return 1;
      int bbody = nt_ref(nt, blk, "body");
      if (bbody >= 0 && rf_node(f, from_scope, bbody, 1, depth + 1)) return 1;
    }
    if (pl.dispatch == CP_DIRECT) {
      if (pl.mi < 0) return 1;
      return rf_check_user_target(f, from_scope, id, pl.mi, recv, argc, argv);
    }
    int tg[CPT_MAX];
    int ntg = cplan_targets(c, id, tg, CPT_MAX);
    if (ntg <= 0 || ntg == CPT_UNKNOWN) return 1;
    for (int i = 0; i < ntg; i++)
      if (rf_check_user_target(f, from_scope, id, tg[i], recv, argc, argv)) return 1;
    return 0;
  }

  /* pl.dispatch == CP_NONE: check Foo.new, plain attr_reader, or pure builtin op */
  if (blk >= 0) return 1;

  /* Plain user class `Foo.new(...)` */
  if (recv >= 0 && sp_streq(nm, "new") &&
      (nt_kind(nt, recv) == NK_ConstantReadNode || nt_kind(nt, recv) == NK_ConstantPathNode)) {
    int k = const_node_class_index(c, recv);
    if (rf_plain_user_class(c, k) && !comp_class_is_module(c, &c->classes[k])) {
      if (c->classes[k].ncm_vis > 0 ||
          !cplan_cmethod_visible(c, id, k, "new") ||
          cplan_method_vis(c, id, k, "new", CPV_CMETHOD, NULL) != SP_VIS_PUBLIC)
        return 1;
      int init_mi = cplan_hook_target(c, id, k, "initialize", NULL);
      if (init_mi < 0) return argc == 0 ? 0 : 1;
      return rf_check_user_target(f, from_scope, id, init_mi, -1, argc, argv);
    }
    return 1;
  }

  /* Plain attr_reader on a non-nil user object (`obj.x`, 0 args) */
  if (recv >= 0 && argc == 0) {
    Repr rr = repr_of(c, recv);
    if (ty_is_object(rr.as_ty) && (!rf_may_nil(c, recv, rr) || nt_safe_nav(nt, id))) {
      int k = ty_object_class(rr.as_ty);
      if (rf_plain_user_class(c, k)) {
        MethodLookup ml = { .def_class = -1 };
        call_target(c, id, k, nm, CT_ATTR, &ml);
        if (ml.kind == ML_ATTR && !rf_class_or_sub_undef_or_closed(f, id, k, nm, 1, 0))
          return 0;
      }
    }
  }

  return rf_builtin_call_may_raise(f, id, nm, recv, argc, argv);
}

/* Check a local variable write/or-write/and-write for checked unboxing. */
static int rf_local_write(RF *f, int from_scope, int id, int in_block, int depth) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  int val = nt_ref(nt, id, "value");
  if (val < 0 || rf_node(f, from_scope, val, in_block, depth + 1)) return 1;
  Scope *sc = comp_scope_of(c, id);
  const char *nm = nt_str(nt, id, "name");
  LocalVar *lv = sc && nm ? scope_local(sc, nm) : NULL;
  if (!lv) return 1;
  return rf_store_may_raise(c, repr_of_slot(c, lv), repr_of(c, val), val);
}

/* Check `x op= val` on a local variable. */
static int rf_local_op_write(RF *f, int from_scope, int id, int in_block, int depth) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  const char *op = nt_str(nt, id, "binary_operator");
  int val = nt_ref(nt, id, "value");
  if (!op || val < 0 || rf_node(f, from_scope, val, in_block, depth + 1)) return 1;
  if (anh_find(&f->reopened, op) >= 0) return 1;
  Scope *sc = comp_scope_of(c, id);
  const char *nm = nt_str(nt, id, "name");
  LocalVar *lv = sc && nm ? scope_local(sc, nm) : NULL;
  if (!lv) return 1;
  Repr lr = repr_of_slot(c, lv);
  Repr vr = repr_of(c, val);
  if (lr.kind != RK_SCALAR || lr.may_nil || lr.big) return 1;
  if (vr.kind != RK_SCALAR || vr.may_nil || vr.big) return 1;
  if (lr.as_ty == TY_INT && vr.as_ty == TY_INT)
    return rf_int_binop_may_raise(nt, op, -1, val);
  if (lr.as_ty == TY_FLOAT && (vr.as_ty == TY_FLOAT || vr.as_ty == TY_INT))
    return rf_float_binop_may_raise(nt, op, val);
  return 1;
}

/* Core conservative AST walker. Returns 1 if node `id` may raise or perform a
   non-local unwind, 0 ONLY when provably nothrow. */
static int rf_node(RF *f, int from_scope, int id, int in_block, int depth) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  if (id < 0 || id >= nt->count) return 0;
  if (depth > 500) return 1;

  switch (nt_kind(nt, id)) {
    case NK_IntegerNode:
      return nt_str(nt, id, "bigval") != NULL;
    case NK_FloatNode:
    case NK_TrueNode:
    case NK_FalseNode:
    case NK_NilNode:
    case NK_SymbolNode:
    case NK_StringNode:
    case NK_SelfNode:
    case NK_SourceFileNode:
    case NK_SourceLineNode:
    case NK_SourceEncodingNode:
    case NK_LocalVariableReadNode:
    case NK_ItLocalVariableReadNode:
      return 0;

    case NK_InstanceVariableReadNode: {
      Scope *sc = comp_scope_of(c, id);
      if (!sc || sc->is_cmethod || !rf_plain_user_class(c, sc->class_id)) return 1;
      return 0;
    }

    case NK_InstanceVariableWriteNode: {
      int val = nt_ref(nt, id, "value");
      if (val < 0 || rf_node(f, from_scope, val, in_block, depth + 1)) return 1;
      Scope *sc = comp_scope_of(c, id);
      const char *nm = nt_str(nt, id, "name");
      if (!sc || sc->is_cmethod || !nm || !rf_plain_user_class(c, sc->class_id)) return 1;
      ClassInfo *ci = &c->classes[sc->class_id];
      if (rf_class_or_sub_frozen(f, sc->class_id, 0)) return 1;
      int iv = comp_ivar_index(ci, nm);
      if (iv < 0) return 1;
      return rf_store_may_raise(c, repr_of_ivar(c, sc->class_id, iv), repr_of(c, val), val);
    }

    case NK_ConstantReadNode: {
      if (!an_const_effect_runs_nothing(an_const_read_effect(c, id))) return 1;
      if (const_path_is_private(c, id)) return 1;
      const char *nm = nt_str(nt, id, "name");
      if (!nm) return 1;
      if (comp_const_at(c, id) != NULL) return 0;
      if (comp_class_index(c, nm) >= 0 || is_builtin_class_name(nm) || is_builtin_module_name(nm))
        return 0;
      return 1;
    }

    case NK_LocalVariableWriteNode:
    case NK_LocalVariableOrWriteNode:
    case NK_LocalVariableAndWriteNode:
      return rf_local_write(f, from_scope, id, in_block, depth);

    case NK_LocalVariableOperatorWriteNode:
      return rf_local_op_write(f, from_scope, id, in_block, depth);

    case NK_StatementsNode: {
      int n = 0;
      const int *body = nt_arr(nt, id, "body", &n);
      for (int i = 0; i < n; i++)
        if (rf_node(f, from_scope, body[i], in_block, depth + 1)) return 1;
      return 0;
    }

    case NK_ParenthesesNode:
      return rf_node(f, from_scope, nt_ref(nt, id, "body"), in_block, depth + 1);

    case NK_ElseNode:
      return rf_node(f, from_scope, nt_ref(nt, id, "statements"), in_block, depth + 1);

    case NK_IfNode:
    case NK_UnlessNode: {
      if (rf_node(f, from_scope, nt_ref(nt, id, "predicate"), in_block, depth + 1)) return 1;
      if (rf_node(f, from_scope, nt_ref(nt, id, "statements"), in_block, depth + 1)) return 1;
      int sub = nt_ref(nt, id, nt_kind(nt, id) == NK_IfNode ? "subsequent" : "else_clause");
      return rf_node(f, from_scope, sub, in_block, depth + 1);
    }

    case NK_AndNode:
    case NK_OrNode:
      return rf_node(f, from_scope, nt_ref(nt, id, "left"), in_block, depth + 1) ||
             rf_node(f, from_scope, nt_ref(nt, id, "right"), in_block, depth + 1);

    case NK_CaseNode: {
      /* Only conditionless `case; when cond; ...; end` avoids `===` dispatch */
      if (nt_ref(nt, id, "predicate") >= 0) return 1;
      int nw = 0;
      const int *whens = nt_arr(nt, id, "conditions", &nw);
      for (int i = 0; i < nw; i++) {
        if (nt_kind(nt, whens[i]) != NK_WhenNode) return 1;
        int nc = 0;
        const int *conds = nt_arr(nt, whens[i], "conditions", &nc);
        for (int j = 0; j < nc; j++)
          if (rf_node(f, from_scope, conds[j], in_block, depth + 1)) return 1;
        if (rf_node(f, from_scope, nt_ref(nt, whens[i], "statements"), in_block, depth + 1))
          return 1;
      }
      return rf_node(f, from_scope, nt_ref(nt, id, "else_clause"), in_block, depth + 1);
    }

    case NK_ArrayNode: {
      Repr ar = repr_of(c, id);
      if (ar.kind == RK_NONE) return 1;
      int ne = 0;
      const int *els = nt_arr(nt, id, "elements", &ne);
      for (int i = 0; i < ne; i++) {
        if (nt_kind(nt, els[i]) == NK_SplatNode) return 1;
        if (rf_node(f, from_scope, els[i], in_block, depth + 1)) return 1;
        if (ar.elem != TY_UNKNOWN && ar.elem != TY_POLY) {
          Repr er = repr_of(c, els[i]);
          if (er.kind == RK_NONE || er.kind == RK_BOXED) return 1;
          if (er.as_ty != ar.elem && er.as_ty != TY_NIL &&
              !((ar.elem == TY_STRING || ar.elem == TY_STRBUF) &&
                (er.as_ty == TY_STRING || er.as_ty == TY_STRBUF)) &&
              !(ty_is_object(ar.elem) && ty_is_object(er.as_ty) &&
                is_descendant(c, ty_object_class(er.as_ty), ty_object_class(ar.elem))))
            return 1;
        }
      }
      return 0;
    }

    case NK_BeginNode: {
      int body = nt_ref(nt, id, "statements");
      int rc = nt_ref(nt, id, "rescue_clause");
      int ec = nt_ref(nt, id, "else_clause");
      int ens = nt_ref(nt, id, "ensure_clause");
      int body_mr = rf_node(f, from_scope, body, in_block, depth + 1);
      /* If body may raise and there is a rescue, conservatively treat whole begin as may_raise */
      if (body_mr) return 1;
      /* Body cannot raise: rescue is dead; check else and ensure */
      (void)rc;
      if (ec >= 0 && rf_node(f, from_scope, nt_ref(nt, ec, "statements"), in_block, depth + 1))
        return 1;
      if (ens >= 0 && rf_node(f, from_scope, nt_ref(nt, ens, "statements"), in_block, depth + 1))
        return 1;
      return 0;
    }

    case NK_RescueModifierNode:
      return rf_node(f, from_scope, nt_ref(nt, id, "expression"), in_block, depth + 1);

    case NK_ReturnNode: {
      /* A return inside a block/proc may unwind non-locally via sp_proc_return / SP_UNWIND_EXIT */
      if (in_block) return 1;
      int args = nt_ref(nt, id, "arguments"), na = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &na) : NULL;
      for (int i = 0; i < na; i++)
        if (rf_node(f, from_scope, av[i], in_block, depth + 1)) return 1;
      Scope *sc = comp_scope_of(c, id);
      if (!sc || !sc->name) return 1; /* top-level return */
      if (na == 1 && sc->ret != TY_POLY && sc->ret != TY_VOID && sc->ret != TY_NIL) {
        Repr sr = repr_of(c, av[0]);
        if (sr.kind == RK_NONE || sr.kind == RK_BOXED) return 1;
        if ((sc->ret == TY_INT || sc->ret == TY_FLOAT) && !sc->ret_nullable_int && sr.may_nil) return 1;
        if (sr.as_ty != sc->ret && !(sc->ret == TY_FLOAT && sr.as_ty == TY_INT) &&
            !((sc->ret == TY_STRING || sc->ret == TY_STRBUF) &&
              (sr.as_ty == TY_STRING || sr.as_ty == TY_STRBUF)))
          return 1;
      }
      return 0;
    }

    case NK_BreakNode:
    case NK_NextNode:
    case NK_RedoNode: {
      /* Inside a block, break/next can trigger sp_brk_throw / proc unwind */
      if (in_block) return 1;
      int args = nt_ref(nt, id, "arguments"), na = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &na) : NULL;
      for (int i = 0; i < na; i++)
        if (rf_node(f, from_scope, av[i], in_block, depth + 1)) return 1;
      return 0;
    }

    case NK_YieldNode: {
      int args = nt_ref(nt, id, "arguments"), na = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &na) : NULL;
      for (int i = 0; i < na; i++) {
        NodeKind ak = nt_kind(nt, av[i]);
        if (ak == NK_SplatNode || ak == NK_KeywordHashNode ||
            ak == NK_ForwardingArgumentsNode || ak == NK_BlockArgumentNode)
          return 1;
        if (rf_node(f, from_scope, av[i], in_block, depth + 1)) return 1;
      }
      Scope *sc = comp_scope_of(c, id);
      int si = sc ? (int)(sc - c->scopes) : -1;
      if (si < 0 || si >= f->ns || f->yield_unk[si] || f->nyblk[si] == 0) return 1;
      for (int i = 0; i < f->nyblk[si]; i++) {
        int bbody = nt_ref(nt, f->yblk[si][i], "body");
        if (bbody >= 0 && rf_node(f, from_scope, bbody, 1, depth + 1)) return 1;
      }
      return 0;
    }

    case NK_CallNode:
      return rf_call(f, from_scope, id, in_block, depth);

    default:
      /* Loops (signal/finalizer polls), raise/throw, super, eval, hashes,
         interpolated strings, ranges, pattern matching, multi-write,
         class/module/def, globals, cvars, retry, etc.: conservatively 1. */
      return 1;
  }
}

/* Mark every scope in a non-trivial SCC (size > 1 or self-loop) as may_raise = 1.
   Cyclic methods emit signal polls (poll_scope_on_cycle) and may overflow stack. */
typedef struct {
  int *idx, *low, *stk;
  unsigned char *onstk;
  int timer, sp;
} Tarjan;

static void rf_tarjan_dfs(RF *f, Tarjan *t, int u) {
  t->idx[u] = t->low[u] = ++t->timer;
  t->stk[t->sp++] = u;
  t->onstk[u] = 1;
  for (int i = 0; i < f->ntg[u]; i++) {
    int v = f->tg[u][i];
    if (v == u) f->scope_mr[u] = 1;
    if (!t->idx[v]) {
      rf_tarjan_dfs(f, t, v);
      if (t->low[v] < t->low[u]) t->low[u] = t->low[v];
    }
    else if (t->onstk[v]) {
      if (t->idx[v] < t->low[u]) t->low[u] = t->idx[v];
    }
  }
  if (t->low[u] == t->idx[u]) {
    int start = t->sp;
    while (start > 0 && t->stk[start - 1] != u) start--;
    int scc_size = t->sp - start + 1;
    while (t->sp > 0) {
      int w = t->stk[--t->sp];
      t->onstk[w] = 0;
      if (scc_size > 1) f->scope_mr[w] = 1;
      if (w == u) break;
    }
  }
}

static void rf_mark_cycles_and_propagate(RF *f) {
  int ns = f->ns;
  Tarjan t;
  t.idx = (int *)calloc((size_t)ns + 1, sizeof(int));
  t.low = (int *)calloc((size_t)ns + 1, sizeof(int));
  t.stk = (int *)malloc(sizeof(int) * ((size_t)ns + 1));
  t.onstk = (unsigned char *)calloc((size_t)ns + 1, 1);
  t.timer = 0; t.sp = 0;
  if (!t.idx || !t.low || !t.stk || !t.onstk) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int s = 0; s < ns; s++)
    if (!t.idx[s]) rf_tarjan_dfs(f, &t, s);
  free(t.idx); free(t.low); free(t.stk); free(t.onstk);

  /* Reverse-edge worklist BFS: any caller of a may_raise scope also may_raise */
  int *rhead = (int *)malloc(sizeof(int) * ((size_t)ns + 1));
  int *work = (int *)malloc(sizeof(int) * ((size_t)ns + 1));
  int nedge = 0, nw = 0;
  for (int s = 0; s < ns; s++) {
    rhead[s] = -1;
    nedge += f->ntg[s];
    if (f->scope_mr[s]) work[nw++] = s;
  }
  int *rfrom = (int *)malloc(sizeof(int) * ((size_t)nedge + 1));
  int *rnext = (int *)malloc(sizeof(int) * ((size_t)nedge + 1));
  if (!rhead || !work || !rfrom || !rnext) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int s = 0, x = 0; s < ns; s++)
    for (int i = 0; i < f->ntg[s]; i++, x++) {
      int v = f->tg[s][i];
      rfrom[x] = s; rnext[x] = rhead[v]; rhead[v] = x;
    }
  while (nw > 0) {
    int v = work[--nw];
    for (int x = rhead[v]; x >= 0; x = rnext[x]) {
      int u = rfrom[x];
      if (!f->scope_mr[u]) { f->scope_mr[u] = 1; work[nw++] = u; }
    }
  }
  free(rhead); free(work); free(rfrom); free(rnext);
}

/* Populate reopened builtin/top-level/undef names and Finalizers__ flag. */
static void rf_collect_env(RF *f) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  for (int s = 0; s < f->ns; s++) {
    Scope *sc = &c->scopes[s];
    if (!sc->name) continue;
    int add = (sc->class_id < 0 || sc->class_id == c->toplevel_class);
    if (!add && sc->class_id >= 0 && sc->class_id < c->nclasses) {
      const char *cn = c->classes[sc->class_id].name;
      if (cn && (c->classes[sc->class_id].is_builtin_const ||
                 is_builtin_class_name(cn) || is_builtin_module_name(cn)))
        add = 1;
    }
    if (add && anh_find(&f->reopened, sc->name) < 0) anh_add(&f->reopened, sc->name);
  }
  for (int id = 0; id < nt->count; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k == NK_UndefNode) {
      int nn = 0;
      const int *names = nt_arr(nt, id, "names", &nn);
      for (int i = 0; i < nn; i++) {
        const char *nm = nt_str(nt, names[i], "value");
        if (nm && anh_find(&f->reopened, nm) < 0) anh_add(&f->reopened, nm);
      }
    }
    else if (k == NK_ConstantReadNode || k == NK_ConstantPathNode) {
      const char *nm = nt_str(nt, id, "name");
      if (nm && sp_streq(nm, "Finalizers__")) f->uses_finalizers = 1;
    }
  }
}

/* Record call-site blocks for every yielding scope. */
static void rf_collect_yield_sites(RF *f) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  int dyn_all = 0;
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (nm && (sp_streq(nm, "send") || sp_streq(nm, "__send__") || sp_streq(nm, "public_send") ||
               sp_streq(nm, "method") || sp_streq(nm, "public_method") ||
               sp_streq(nm, "instance_method") || sp_streq(nm, "eval")))
      dyn_all = 1;
    CallPlan pl = cplan_user(c, id);
    int blk = nt_ref(nt, id, "block");
    if (pl.dispatch == CP_DIRECT && pl.mi >= 0 && pl.mi < f->ns) {
      if (!c->scopes[pl.mi].yields) continue;
      if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode || rf_block_has_complex_params(nt, blk))
        f->yield_unk[pl.mi] = 1;
      else
        rf_add_yblk(f, pl.mi, blk);
    }
    else if (pl.dispatch == CP_SWITCH || pl.dispatch == CP_PER_ARM) {
      int tg[CPT_MAX];
      int ntg = cplan_targets(c, id, tg, CPT_MAX);
      if (ntg == CPT_UNKNOWN) { dyn_all = 1; continue; }
      for (int i = 0; i < ntg; i++) {
        int t = tg[i];
        if (t < 0 || t >= f->ns || !c->scopes[t].yields) continue;
        if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode || rf_block_has_complex_params(nt, blk))
          f->yield_unk[t] = 1;
        else
          rf_add_yblk(f, t, blk);
      }
    }
  }
  if (dyn_all)
    for (int s = 0; s < f->ns; s++)
      if (c->scopes[s].yields) f->yield_unk[s] = 1;
}

/* Is node `id` lexically inside a BlockNode or LambdaNode (within its enclosing
   method/class/program)? Used to classify return/break/next as potentially non-local. */
static int rf_node_in_block(const NodeTable *nt, const int *par, int id) {
  if (!par) return 1;
  for (int p = par[id]; p >= 0; p = par[p]) {
    NodeKind k = nt_kind(nt, p);
    if (k == NK_BlockNode || k == NK_LambdaNode) return 1;
    if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode ||
        k == NK_SingletonClassNode || k == NK_ProgramNode)
      return 0;
  }
  return 0;
}

/* Count enclosing BeginNodes with an ensure body inside the same function. */
static int rf_enclosing_ensures(const NodeTable *nt, const int *par, int id) {
  if (!par) return RF_MAX_ENSURE_DEPTH;
  int d = 0;
  for (int p = par[id]; p >= 0; p = par[p]) {
    NodeKind k = nt_kind(nt, p);
    if (k == NK_DefNode || k == NK_BlockNode || k == NK_LambdaNode ||
        k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode ||
        k == NK_ProgramNode)
      break;
    if (k == NK_BeginNode) {
      int ec = nt_ref(nt, p, "ensure_clause");
      if (ec >= 0 && nt_ref(nt, ec, "statements") >= 0) d++;
    }
  }
  return d;
}

/* Does subtree `id` contain any local jump (return/break/next/redo) outside
   nested defs/classes? Used to pick straight-line vs goto-ensure lowering. */
int an_subtree_has_local_jump(const NodeTable *nt, int id) {
  if (id < 0 || id >= nt->count) return 0;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_ReturnNode || k == NK_BreakNode || k == NK_NextNode || k == NK_RedoNode) return 1;
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode ||
      k == NK_SingletonClassNode || k == NK_LambdaNode)
    return 0;
  const SpNode *nd = &nt->nodes[id];
  for (int i = 0; i < nd->nr; i++)
    if (an_subtree_has_local_jump(nt, nd->r[i].ref)) return 1;
  for (int i = 0; i < nd->na; i++)
    for (int j = 0; j < nd->a[i].n; j++)
      if (an_subtree_has_local_jump(nt, nd->a[i].ids[j])) return 1;
  return 0;
}

static void rf_free(RF *f) {
  for (int s = 0; s < f->ns; s++) {
    free(f->tg ? f->tg[s] : NULL);
    free(f->yblk ? f->yblk[s] : NULL);
  }
  free(f->tg); free(f->ntg); free(f->ctg);
  free(f->yield_unk); free(f->yblk); free(f->nyblk); free(f->cyblk);
  free(f->cls_head); free(f->cls_next);
  anh_free(&f->reopened);
}

void an_raise_facts(Compiler *c) {
  const NodeTable *nt = c->nt;
  int ns = c->nscopes, nn = nt->count;
  free(c->scope_may_raise);
  free(c->node_elide_setjmp);
  c->scope_may_raise = (unsigned char *)calloc((size_t)ns + 1, 1);
  c->scope_may_raise_n = ns;
  c->node_elide_setjmp = (unsigned char *)calloc((size_t)nn + 1, 1);
  c->node_elide_setjmp_n = nn;
  if (!c->scope_may_raise || !c->node_elide_setjmp) {
    fprintf(stderr, "spinel: out of memory\n");
    exit(1);
  }
  /* Under --exceptions=abort, keep conservative defaults so abort-mode
     refusals remain untouched. */
  if (g_exc_abort) {
    memset(c->scope_may_raise, 1, (size_t)ns);
    return;
  }

  RF f;
  memset(&f, 0, sizeof f);
  f.c = c;
  f.nt = nt;
  f.par = comp_parent_map(c);
  f.scope_mr = c->scope_may_raise;
  f.ns = ns;
  f.tg = (int **)calloc((size_t)ns + 1, sizeof(int *));
  f.ntg = (int *)calloc((size_t)ns + 1, sizeof(int));
  f.ctg = (int *)calloc((size_t)ns + 1, sizeof(int));
  f.yield_unk = (unsigned char *)calloc((size_t)ns + 1, 1);
  f.yblk = (int **)calloc((size_t)ns + 1, sizeof(int *));
  f.nyblk = (int *)calloc((size_t)ns + 1, sizeof(int));
  f.cyblk = (int *)calloc((size_t)ns + 1, sizeof(int));
  f.cls_head = (int *)malloc(sizeof(int) * ((size_t)c->nclasses + 1));
  f.cls_next = (int *)malloc(sizeof(int) * ((size_t)c->nclasses + 1));
  if (!f.tg || !f.ntg || !f.ctg || !f.yield_unk || !f.yblk || !f.nyblk || !f.cyblk ||
      !f.cls_head || !f.cls_next) {
    fprintf(stderr, "spinel: out of memory\n");
    exit(1);
  }
  for (int k = 0; k < c->nclasses; k++) { f.cls_head[k] = -1; f.cls_next[k] = -1; }
  for (int k = 0; k < c->nclasses; k++) {
    int p = c->classes[k].parent;
    if (p >= 0 && p < c->nclasses && p != k) {
      f.cls_next[k] = f.cls_head[p];
      f.cls_head[p] = k;
    }
  }

  rf_collect_env(&f);
  rf_collect_yield_sites(&f);

  /* Pass 1: per-scope direct eligibility, default expressions, and body walk */
  for (int s = 0; s < ns; s++) {
    Scope *sc = &c->scopes[s];
    if (f.uses_finalizers || !sc->name || sc->def_node < 0 || sc->body < 0 ||
        sc->entry != ENTRY_NONE || sc->is_proc_form || sc->is_lowered_yield ||
        sc->is_ext_entry || sc->is_ext_method || sc->ret_rbs_cont ||
        rf_scope_has_complex_params(c, sc)) {
      f.scope_mr[s] = 1;
      continue;
    }
    if (sc->class_id >= 0 && sc->class_id < c->nclasses &&
        (c->classes[sc->class_id].cext_extension || c->classes[sc->class_id].is_native_class)) {
      f.scope_mr[s] = 1;
      continue;
    }
    /* RBS-seeded non-poly return may emit a checked unbox on the tail */
    if (sc->ret_rbs_seeded && sc->ret != TY_POLY && sc->ret != TY_VOID && sc->ret != TY_NIL) {
      Repr br = repr_of(c, sc->body);
      if (br.kind == RK_NONE || br.kind == RK_BOXED || br.as_ty != sc->ret ||
          ((sc->ret == TY_INT || sc->ret == TY_FLOAT) && !sc->ret_nullable_int && br.may_nil)) {
        f.scope_mr[s] = 1;
        continue;
      }
    }
    int mr = 0;
    for (int i = 0; i < sc->nparams && !mr; i++)
      if (sc->pdefault && sc->pdefault[i] >= 0 && rf_node(&f, s, sc->pdefault[i], 0, 0))
        mr = 1;
    if (!mr && rf_node(&f, s, sc->body, 0, 0)) mr = 1;
    if (mr) f.scope_mr[s] = 1;
  }

  /* Fixpoint: mark call cycles and propagate may_raise backward along call edges */
  rf_mark_cycles_and_propagate(&f);

  /* Pass 2: decide setjmp elision for every BeginNode and RescueModifierNode */
  for (int id = 0; id < nn; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k == NK_BeginNode) {
      int body = nt_ref(nt, id, "statements");
      int rescue = nt_ref(nt, id, "rescue_clause");
      int else_c = nt_ref(nt, id, "else_clause");
      int ensure_c = nt_ref(nt, id, "ensure_clause");
      int else_stmts = else_c >= 0 ? nt_ref(nt, else_c, "statements") : -1;
      int ensure_stmts = ensure_c >= 0 ? nt_ref(nt, ensure_c, "statements") : -1;
      if (rescue < 0 && else_stmts < 0 && ensure_stmts < 0) continue;
      int in_blk = rf_node_in_block(nt, f.par, id);
      if (body >= 0 && rf_node(&f, -1, body, in_blk, 0)) continue;
      if (ensure_stmts >= 0) {
        if (else_stmts >= 0 && rf_node(&f, -1, else_stmts, in_blk, 0)) continue;
        int has_jump = an_subtree_has_local_jump(nt, body) || an_subtree_has_local_jump(nt, else_stmts);
        if (has_jump && rf_enclosing_ensures(nt, f.par, id) >= RF_MAX_ENSURE_DEPTH) continue;
      }
      c->node_elide_setjmp[id] = 1;
      for (int rc = rescue; rc >= 0 && rc < nn; rc = nt_ref(nt, rc, "subsequent"))
        c->node_elide_setjmp[rc] = 1;
    }
    else if (k == NK_RescueModifierNode) {
      int expr = nt_ref(nt, id, "expression");
      int in_blk = rf_node_in_block(nt, f.par, id);
      if (expr < 0 || !rf_node(&f, -1, expr, in_blk, 0))
        c->node_elide_setjmp[id] = 1;
    }
  }

  rf_free(&f);
}

int an_scope_may_raise(const Compiler *c, int si) {
  if (!c || !c->scope_may_raise || si < 0 || si >= c->scope_may_raise_n) return 1;
  return c->scope_may_raise[si] != 0;
}

int an_begin_elides_setjmp(const Compiler *c, int id) {
  if (!c || !c->node_elide_setjmp || id < 0 || id >= c->node_elide_setjmp_n) return 0;
  return nt_kind(c->nt, id) == NK_BeginNode && c->node_elide_setjmp[id] != 0;
}

int an_rescue_mod_elides_setjmp(const Compiler *c, int id) {
  if (!c || !c->node_elide_setjmp || id < 0 || id >= c->node_elide_setjmp_n) return 0;
  return nt_kind(c->nt, id) == NK_RescueModifierNode && c->node_elide_setjmp[id] != 0;
}

int an_rescue_elided(const Compiler *c, int id) {
  if (!c || !c->node_elide_setjmp || id < 0 || id >= c->node_elide_setjmp_n) return 0;
  return nt_kind(c->nt, id) == NK_RescueNode && c->node_elide_setjmp[id] != 0;
}

int an_expr_may_raise(Compiler *c, int id) {
  if (!c || !c->scope_may_raise || id < 0 || id >= c->nt->count) return 1;
  if (g_exc_abort) return 1;
  RF f;
  memset(&f, 0, sizeof f);
  f.c = c;
  f.nt = c->nt;
  f.par = comp_parent_map(c);
  f.scope_mr = c->scope_may_raise;
  f.ns = c->scope_may_raise_n;
  f.yield_unk = (unsigned char *)calloc((size_t)f.ns + 1, 1);
  f.nyblk = (int *)calloc((size_t)f.ns + 1, sizeof(int));
  if (!f.yield_unk || !f.nyblk) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memset(f.yield_unk, 1, (size_t)f.ns);
  rf_collect_env(&f);
  int mr = rf_node(&f, -1, id, rf_node_in_block(c->nt, f.par, id), 0);
  rf_free(&f);
  return mr;
}

int an_stmt_may_raise(Compiler *c, int id) {
  return an_expr_may_raise(c, id);
}
