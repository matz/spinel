/* analyze_arm.c -- per-arm facts (#7483).

   A block given to an iteration on a value of more than one kind is
   emitted inside each arm of the receiver's dispatch, with its first
   parameter typed by that arm (the parameter of `xs.each_slice(2) { |s| }`
   is the slice of whichever Array the arm holds; a `map` over a boxed
   receiver pins its parameter to the box). The analysis records one type
   per node, so codegen typed the body again under each arm's assumption
   (arm_body_infer, codegen_view.c), and the types it settled on there were
   not the ones recorded.

   This file types those bodies once, after the analysis has settled: for
   each block a site of that kind can ask about, and each parameter type it
   can ask with, the body's node types under that parameter type, keyed by
   (block, kind). The node types and the parameter's slot are put back as
   they were, so nothing the analysis recorded changes. --arm-check holds
   these facts against the typing codegen runs (arm_body_infer), with no
   reader yet: a missing fact or a type that differs is reported. */

#include "analyze_internal.h"
#include "repr.h"

/* the iterations whose block codegen retypes per arm (codegen_iter.c,
   codegen_fold.c, codegen_call_object.c), and the parameter type each
   asks with: the box, the receiver's own kind, or a String */
enum { AK_POLY = 1, AK_RECV = 2, AK_STRING = 4, AK_THEN = 8 };
static int arm_kinds_of(const char *nm) {
  static const struct { const char *name; int kinds; } tab[] = {
    { "map", AK_POLY }, { "collect", AK_POLY }, { "sum", AK_POLY },
    { "tap", AK_RECV }, { "then", AK_THEN }, { "yield_self", AK_THEN },
    { "each_slice", AK_RECV }, { "each_cons", AK_RECV },
    { "combination", AK_RECV }, { "permutation", AK_RECV },
    { "repeated_combination", AK_RECV }, { "repeated_permutation", AK_RECV },
    { "each", AK_STRING }, { "split", AK_STRING }, { "scan", AK_STRING }, { "upto", AK_STRING },
    { NULL, 0 } };
  for (int i = 0; tab[i].name; i++) if (sp_streq(nm, tab[i].name)) return tab[i].kinds;
  return 0;
}

static void arm_collect(const NodeTable *nt, int n, int **out, int *len, int *cap, int depth) {
  if (n < 0 || depth > 4096) return;
  if (*len >= *cap) {
    *cap = *cap ? *cap * 2 : 64;
    *out = realloc(*out, sizeof **out * (size_t)*cap);
    if (!*out) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  (*out)[(*len)++] = n;
  if (nt_kind(nt, n) == NK_DefNode) return;
  for (int i = 0; i < nt_num_refs(nt, n); i++) arm_collect(nt, nt_ref_at(nt, n, i), out, len, cap, depth + 1);
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int an = 0; const int *av = nt_arr_at(nt, n, i, &an);
    for (int k = 0; k < an; k++) arm_collect(nt, av[k], out, len, cap, depth + 1);
  }
}

const ArmFact *arm_fact_find(const Compiler *c, int block, TyKind kind) {
  for (int i = 0; i < c->narm_facts; i++)
    if (c->arm_facts[i].block == block && c->arm_facts[i].kind == kind) return &c->arm_facts[i];
  return NULL;
}

/* the fact for (block, kind), or with `extra` >= 0 the one for that node
   typed after the body (a `map(&:sym)` pair's call), keyed by -2 - extra */
static void arm_record_x(Compiler *c, int block, LocalVar *lv, TyKind kind, int extra) {
  const NodeTable *nt = c->nt;
  int key = extra >= 0 ? -2 - extra : block;
  if (arm_fact_find(c, key, kind)) return;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  int *nodes = NULL, len = 0, cap = 0;
  /* the extra node's fact holds its own subtree; the body is typed first,
     as the site types it, and put back with the rest */
  int *bnodes = NULL, blen = 0, bcap = 0;
  if (extra >= 0) {
    arm_collect(nt, extra, &nodes, &len, &cap, 0);
    for (int j = 0; j < bn; j++) arm_collect(nt, bb[j], &bnodes, &blen, &bcap, 0);
  }
  else for (int j = 0; j < bn; j++) arm_collect(nt, bb[j], &nodes, &len, &cap, 0);
  TyKind *bsaved = malloc(sizeof *bsaved * (size_t)(blen ? blen : 1));
  if (!bsaved) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < blen; i++) bsaved[i] = comp_ntype_settled(c, bnodes[i]);
  TyKind *saved = malloc(sizeof *saved * (size_t)(len ? len : 1));
  TyKind *types = malloc(sizeof *types * (size_t)(len ? len : 1));
  if (!saved || !types) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < len; i++) saved[i] = comp_ntype_settled(c, nodes[i]);
  /* a block whose first parameter is no plain one (`|*r|`): the sites
     retype the body with no slot set, and so does this */
  TyKind lsaved = lv ? lv->type : TY_UNKNOWN;
  /* the calls the body makes keep what the analysis bound them to: typed
     under one arm, a call on the parameter binds that arm's method alone
     (--plan-check) */
  UCallInf *usaved = c->ucall_inf ? malloc(sizeof *usaved * (size_t)(len + blen + 1)) : NULL;
  if (c->ucall_inf && !usaved) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; usaved && i < len + blen; i++) {
    int n = i < len ? nodes[i] : bnodes[i - len];
    if (n < c->node_cap) usaved[i] = c->ucall_inf[n];
  }
  /* ...and so do the rows they were answered from (--plan-check's bop_inf) */
  const struct BuiltinOp **rsaved = malloc(sizeof *rsaved * (size_t)(len + blen + 1));
  if (!rsaved) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < len + blen; i++) {
    int n = i < len ? nodes[i] : bnodes[i - len];
    rsaved[i] = n < c->node_cap ? c->bop_inf[n] : NULL;
  }
  if (lv) lv_set_type(lv, kind);
  for (int j = 0; j < bn; j++) infer_type(c, bb[j]);
  if (extra >= 0) infer_type(c, extra);
  for (int i = 0; i < len; i++) types[i] = comp_ntype_settled(c, nodes[i]);
  if (lv) lv_set_type(lv, lsaved);
  for (int i = 0; i < len; i++) if (nodes[i] < c->node_cap) comp_set_ntype(c, nodes[i], saved[i]);
  for (int i = 0; i < blen; i++) if (bnodes[i] < c->node_cap) comp_set_ntype(c, bnodes[i], bsaved[i]);
  for (int i = 0; usaved && i < len + blen; i++) {
    int n = i < len ? nodes[i] : bnodes[i - len];
    if (n < c->node_cap) c->ucall_inf[n] = usaved[i];
  }
  for (int i = 0; i < len + blen; i++) {
    int n = i < len ? nodes[i] : bnodes[i - len];
    if (n < c->node_cap) c->bop_inf[n] = rsaved[i];
  }
  free(rsaved);
  free(usaved);
  free(saved); free(bsaved); free(bnodes);
  if (c->narm_facts >= c->carm_facts) {
    c->carm_facts = c->carm_facts ? c->carm_facts * 2 : 16;
    c->arm_facts = realloc(c->arm_facts, sizeof *c->arm_facts * (size_t)c->carm_facts);
    if (!c->arm_facts) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  c->arm_facts[c->narm_facts++] = (ArmFact){ key, kind, len, nodes, types };
}
static void arm_record(Compiler *c, int block, LocalVar *lv, TyKind kind) {
  arm_record_x(c, block, lv, kind, -1);
}

void an_arm_facts(Compiler *c) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    int block = nt_ref(nt, id, "block");
    int r = nt_ref(nt, id, "receiver");
    if (!nm || block < 0 || r < 0 || nt_kind(nt, block) != NK_BlockNode) continue;
    int kinds = arm_kinds_of(nm);
    if (!kinds) continue;
    /* then / yield_self (codegen_call_object.c): the parameter typed by the
       receiver's representation where its slot says otherwise */
    if (kinds & AK_THEN) {
      const char *q0 = block_param_name(c, block, 0);
      Scope *qs = q0 ? comp_scope_of(c, block) : NULL;
      LocalVar *qv = qs ? scope_local(qs, q0) : NULL;
      TyKind rk = repr_of(c, r).as_ty;
      if (qv && qv->type != rk && rk != TY_UNKNOWN) arm_record(c, block, qv, rk);
      continue;
    }
    /* the sites run in the arms of a dispatch over a boxed value, or over
       a boxed Array's elements */
    TyKind rt = comp_ntype(c, r);
    if (rt != TY_POLY && rt != TY_POLY_ARRAY) continue;
    const char *p0 = block_param_name(c, block, 0);
    Scope *bs = p0 ? comp_scope_of(c, block) : NULL;
    LocalVar *lv = bs ? scope_local(bs, p0) : NULL;
    if (kinds & AK_POLY) {
      arm_record(c, block, lv, TY_POLY);
      int pair = nt_ref(nt, block, "sym_proc_pair");
      if (pair >= 0) arm_record_x(c, block, lv, TY_POLY, pair);
    }
    /* the receiver's own kind; a boxed one's arm is the boxed Array's */
    if ((kinds & AK_RECV) && rt != TY_UNKNOWN) arm_record(c, block, lv, rt == TY_POLY ? TY_POLY_ARRAY : rt);
    if (kinds & AK_STRING) arm_record(c, block, lv, TY_STRING);
  }
}
