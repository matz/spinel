/* analyze_share.c -- the questions the String rule asks of the program
   (#6765): whether a slot's type can hold a String (sh_may_hold), whether
   a call stores the block it is given (share_call_stores_block) and whether
   a block hands control back to its caller (share_block_returns). See
   share.h.

   The tables the facts keep (ShareFacts) are read from the calls when the
   rule is applied (share_facts_build): the blocks each method can be
   given, the methods a method hands its own block on to and the names a
   send or a Method may reach. */

#include <stdlib.h>
#include <string.h>
#include "analyze_internal.h"
#include "builtin_ops.h"
#include "builtin_names.h"
#include "call_plan.h"
#include "method_table_check.h"
#include "share.h"
#include "repr.h"
#include "codegen_internal.h"
#include "value_pos.h"

typedef struct ShareFacts {
  int nnodes;
  /* the literal blocks each method can be given (sh_mb_add): pairs, then
     once built, per method scope its run (mb_start..mb_start+1) of mb_blk */
  int *mb_m, *mb_b, nmb, cmb;
  int *mb_start, *mb_blk;
  /* the blocks a method can run: a method's `&blk` handed on (`m2(&blk)`,
     an anonymous `&`, a zsuper) as an edge from the method to each it
     reaches (fw_from, fw_to), and blk_unk per method scope: a block that is
     neither a literal nor such a forward reaches it */
  int *fw_from, *fw_to, nfw, cfw;
  unsigned char *blk_unk;
  /* the method names a Method, `send` or `&:name` can reach; dyn_all when
     one is computed */
  const char **dyn;
  int ndyn, cdyn, dyn_all;
  int ostruct;         /* the program names OpenStruct, whose fields a poly
                          receiver's call may read */
} ShareFacts;

/* Can a value of type t hold a String, or a container of them? An object's
   Strings sit in its ivars, which are holders of their own. */
int sh_may_hold(const Compiler *c, TyKind t) {
  /* an object holds its Strings in ivars, which are holders of their own;
     a native class's object keeps the String its binding declares, and so
     does a subclass's, which is built by the class's constructor */
  if (ty_is_object(t)) {
    int k = ty_object_class(t);
    if (k < c->nclasses && c->classes[k].nsub_root > 0) k = c->classes[k].nsub_root - 1;
    return k < c->nclasses && c->classes[k].native_share_keeps;
  }
  if (ty_is_obj_array(t)) return 0;
  switch (t) {
  case TY_VOID: case TY_NIL: case TY_INT: case TY_BIGINT: case TY_FLOAT: case TY_SYMBOL:
  case TY_BOOL: case TY_RANGE: case TY_FLOAT_RANGE: case TY_TIME: case TY_COMPLEX:
  case TY_RATIONAL: case TY_MATCHDATA: case TY_REGEX: case TY_EXCEPTION:
  case TY_INT_ARRAY: case TY_FLOAT_ARRAY: case TY_INT_ARRAY_ARRAY: case TY_FLOAT_ARRAY_ARRAY:
  case TY_STR_INT_HASH: case TY_INT_INT_HASH: case TY_PROC: case TY_CURRY: case TY_FIBER:
  case TY_THREAD: case TY_QUEUE: case TY_MUTEX: case TY_CONDVAR: case TY_RANDOM: case TY_DIR:
  case TY_ADDRINFO: case TY_SOCKOPT: case TY_TMS: case TY_PROCESS_STATUS:
  case TY_METHOD: case TY_IO: case TY_ARGF: case TY_CLASS: case TY_TIME_RANGE: case TY_OBJ_RANGE:
    return 0;
  default:
    return 1;
  }
}

static int sh_method_index(Compiler *c, int node) {
  Scope *s = comp_scope_of(c, node);
  return s && s->def_node >= 0 ? (int)(s - c->scopes) : -1;
}

static void sh_dyn_name(ShareFacts *F, const char *name) {
  if (!name) { F->dyn_all = 1; return; }
  if (F->ndyn >= F->cdyn) {
    F->cdyn = F->cdyn ? F->cdyn * 2 : 16;
    F->dyn = realloc(F->dyn, sizeof(char *) * (size_t)F->cdyn);
  }
  F->dyn[F->ndyn++] = name;
}

static const char *sh_lit_name(const NodeTable *nt, int n);
/* the literal name a `send`, `method` or `instance_variable_*` names */
static const char *sh_lit_name(const NodeTable *nt, int a) {
  if (a < 0) return NULL;
  NodeKind k = nt_kind(nt, a);
  if (k == NK_SymbolNode) return nt_str(nt, a, "value") ? nt_str(nt, a, "value") : nt_str(nt, a, "unescaped");
  if (k == NK_StringNode) return nt_str(nt, a, "content");
  return NULL;
}

/* Method from hands the block it was given on to method to */
static void sh_fwd_edge(ShareFacts *F, int from, int to) {
  if (from < 0 || to < 0) return;
  if (F->nfw >= F->cfw) {
    F->cfw = F->cfw ? F->cfw * 2 : 16;
    F->fw_from = realloc(F->fw_from, sizeof(int) * (size_t)F->cfw);
    F->fw_to = realloc(F->fw_to, sizeof(int) * (size_t)F->cfw);
    if (!F->fw_from || !F->fw_to) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  F->fw_from[F->nfw] = from;
  F->fw_to[F->nfw++] = to;
}
/* Call n hands method mi a block as a value (blk, a BlockArgumentNode): the
   enclosing method's own &blk (or an anonymous `&`) is a forward edge; any
   other block (`&proc`, `&:sym`, `&method(:m)`) is one the facts do not
   list (blk_unk). */
static void sh_block_value_to(ShareFacts *F, Compiler *c, int n, int blk, int mi) {
  const NodeTable *nt = c->nt;
  int x = nt_ref(nt, blk, "expression");
  int cur = sh_method_index(c, n);
  const char *bp = cur >= 0 ? c->scopes[cur].blk_param : NULL;
  if (cur >= 0 && bp &&
      (x < 0 ? !bp[0] : nt_kind(nt, x) == NK_LocalVariableReadNode && nt_int(nt, x, "depth", 0) == 0 &&
                        nt_str(nt, x, "name") && bp[0] && sp_streq(nt_str(nt, x, "name"), bp)))
    sh_fwd_edge(F, cur, mi);
  else F->blk_unk[mi] = 1;
}

int share_call_stores_block(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  int blk = nt_ref(nt, n, "block");
  const char *nm = nt_str(nt, n, "name");
  if (!nm || blk < 0 || nt_kind(nt, blk) != NK_BlockNode || cplan_has_targets(c, n)) return 0;
  int recv = nt_ref(nt, n, "receiver");
  NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_NONE;
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) {
    const char *cn = key_name(c, recv);
    return cn && sp_streq(nm, "new") && (sp_streq(cn, "Array") || sp_streq(cn, "Hash"));
  }
  static const TyKind fams[] = { BOP_ANY_ARRAY, BOP_ANY_HASH, BOP_ANY_RECV };
  int keep = 0, any = 0;
  for (size_t i = 0; i < sizeof fams / sizeof *fams; i++) {
    int s = bop_share_named(fams[i], nm);
    if (s == BSH_ITER_MAP_BANG || s == BSH_FILL) keep = any = 1;
    else if (s == BSH_ITER_MAP || s == BSH_MERGE) any = 1;
  }
  return any && (keep || !an_node_dropped(c, n));
}

static void sh_free(ShareFacts *F) {
  if (!F) return;
  free(F->mb_m); free(F->mb_b); free(F->mb_start); free(F->mb_blk);
  free(F->fw_from); free(F->fw_to); free(F->blk_unk);
  free(F->dyn);
  free(F);
}

/* The literal blocks by method (mb_start/mb_blk), a counting sort of the
   pairs sh_mb_add recorded. */
static void sh_index_blocks(ShareFacts *F, Compiler *c) {
  size_t ns = (size_t)(c->nscopes > 0 ? c->nscopes : 1);
  F->mb_start = calloc(ns + 1, sizeof(int));
  F->mb_blk = malloc(sizeof(int) * (size_t)(F->nmb > 0 ? F->nmb : 1));
  if (!F->mb_start || !F->mb_blk) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < F->nmb; i++) F->mb_start[F->mb_m[i] + 1]++;
  for (int m = 0; m < c->nscopes; m++) F->mb_start[m + 1] += F->mb_start[m];
  int *at = malloc(sizeof(int) * ns);
  if (!at) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int m = 0; m < c->nscopes; m++) at[m] = F->mb_start[m];
  for (int i = 0; i < F->nmb; i++) F->mb_blk[at[F->mb_m[i]]++] = F->mb_b[i];
  free(at);
}

/* The nodes of the methods the program never names. Such a method runs
   nothing, so its body's reads and writes are no facts about the Strings the
   program holds, and the walk leaves it out: `def run1(f, x) = q { f.call(x) }`
   beside `def run(f) = q { f.call }` joined q's value (one holder per method,
   not per call site) with what any proc may answer, and
   `w = run(proc { +"new" }); w << "!"` was refused (#9258, cluster 9).
   Scope.reachable is the census an earlier phase took, before the rewrites the
   fixpoint adds calls through, so the walk takes its own census of the table
   as it stands (compute_reachable), keyed by the table's version, and puts the
   scopes' flags back as they were. A method the census reaches by name alone
   (a send, a Symbol, an alias) counts as named. */
static void sh_mark_subtree(const NodeTable *nt, int id, unsigned char *dead) {
  if (id < 0 || id >= nt->count || dead[id]) return;
  dead[id] = 1;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) sh_mark_subtree(nt, nt_ref_at(nt, id, i), dead);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++) sh_mark_subtree(nt, ids[j], dead);
  }
}
static const NodeTable *g_dead_nt;
static unsigned g_dead_version;
static int g_dead_count, g_dead_nscopes;
static unsigned char *g_dead;
static unsigned char *sh_dead_nodes(Compiler *c) {
  const NodeTable *nt = c->nt;
  size_t n = (size_t)(nt->count > 0 ? nt->count : 1);
  if (!g_dead || g_dead_nt != nt || g_dead_version != nt->version || g_dead_count != nt->count ||
      g_dead_nscopes != c->nscopes) {
    unsigned char *was = malloc((size_t)(c->nscopes > 0 ? c->nscopes : 1));
    free(g_dead);
    g_dead = calloc(n, 1);
    if (!was || !g_dead) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    /* a node two scopes own (a clone of a method the dispatch splices keeps
       its source's def) is dead only when no scope that owns it is reached */
    unsigned char *live = calloc(n, 1);
    if (!live) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int s = 0; s < c->nscopes; s++) was[s] = (unsigned char)c->scopes[s].reachable;
    compute_reachable(c, 1);
    for (int s = 0; s < c->nscopes; s++) {
      Scope *m = &c->scopes[s];
      if ((m->reachable || was[s]) && m->def_node >= 0) { sh_mark_subtree(nt, m->def_node, live); sh_mark_subtree(nt, m->body, live); }
    }
    for (int s = 0; s < c->nscopes; s++) {
      Scope *m = &c->scopes[s];
      /* (dead only where the earlier census agrees: a scope a rewrite made
         is reached by a mark of its own, not by a name, as the proc form of
         a reopening's method is) */
      if (!m->reachable && !was[s] && m->def_node >= 0) { sh_mark_subtree(nt, m->def_node, g_dead); sh_mark_subtree(nt, m->body, g_dead); }
      { m->reachable = was[s]; comp_touch_scope(m->idx); }
    }
    for (size_t i = 0; i < n; i++) if (live[i]) g_dead[i] = 0;
    free(live); free(was);
    g_dead_nt = nt; g_dead_version = nt->version; g_dead_count = nt->count; g_dead_nscopes = c->nscopes;
  }
  unsigned char *d = malloc(n);
  if (!d) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(d, g_dead, n);
  return d;
}

/* ---- the block tables, read from the calls ----

   The literal blocks each method can be given (mb), the methods a method
   hands its own block on to (fw), the methods a block the table does not
   list reaches (blk_unk). */
static void sh_mb_add(ShareFacts *F, Compiler *c, int blk, int mi) {
  const Scope *m = &c->scopes[mi];
  if (!m->yields && !m->is_lowered_yield && !m->blk_param) return;
  if (F->nmb >= F->cmb) {
    F->cmb = F->cmb ? F->cmb * 2 : 64;
    F->mb_m = realloc(F->mb_m, sizeof(int) * (size_t)F->cmb);
    F->mb_b = realloc(F->mb_b, sizeof(int) * (size_t)F->cmb);
    if (!F->mb_m || !F->mb_b) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  F->mb_m[F->nmb] = mi;
  F->mb_b[F->nmb++] = blk;
}
/* call n hands method mi its block blk (none, a literal, or a value) */
static void sh_mb_block(ShareFacts *F, Compiler *c, int n, int blk, int mi) {
  if (blk < 0 || mi < 0) return;
  if (nt_kind(c->nt, blk) == NK_BlockNode) { sh_mb_add(F, c, blk, mi); return; }
  sh_block_value_to(F, c, n, blk, mi);
}
/* A name a send, a Method or `&:name` reaches a method by; NULL for one
   computed, which may be any. */
static void sh_mb_dyn(ShareFacts *F, Compiler *c, int n, const unsigned char *dead) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, n, "name");
  int args = nt_ref(nt, n, "arguments"), argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int blk = nt_ref(nt, n, "block");
  if (blk >= 0 && nt_kind(nt, blk) == NK_BlockArgumentNode) {
    int bx = nt_ref(nt, blk, "expression");
    if (bx >= 0 && nt_kind(nt, bx) == NK_SymbolNode) sh_dyn_name(F, nt_str(nt, bx, "value"));
  }
  if (!name || dead[n]) return;
  const char *lit = argc >= 1 ? sh_lit_name(nt, argv[0]) : NULL;
  if (is_send_family(name)) {
    if (lit || !nt_int(nt, n, "dyn_send_complete", 0)) sh_dyn_name(F, lit);
    return;
  }
  if (bop_share_named(BOP_ANY_RECV, name) == BSH_METHOD_REF || bop_share_named(TY_CLASS, name) == BSH_METHOD_REF) {
    if (lit) sh_dyn_name(F, lit);
    else if (!(is_define_method_name(name) && argc >= 1)) F->dyn_all = 1;
  }
}
static void sh_mb_build(ShareFacts *F, Compiler *c) {
  const NodeTable *nt = c->nt;
  const unsigned char *dead = sh_dead_nodes(c);
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    if (dead[n]) continue;
    sh_mb_dyn(F, c, n, dead);
    int blk = nt_ref(nt, n, "block");
    if (blk < 0) continue;
    int tg[64], ntg = cplan_live_targets(c, n, tg, 64, 1);
    /* methods the plan cannot list: its block may reach each method of the
       names the plan bounds them by (cplan_entries_bound_names) */
    if (ntg < 0) {
      const char *bn[2];
      int nb = cplan_entries_bound_names(c, n, bn);
      for (int j = 0; j < nb; j++)
        for (int m = an_scopes_named_first(c, bn[j]); m >= 1; m = an_scopes_named_next(c, bn[j], m))
          if (c->scopes[m].def_node >= 0) sh_mb_block(F, c, n, blk, m);
      continue;
    }
    int nbind = 0;
    for (int i = 0; i < ntg; i++) nbind += cplan_target_binds(c, n, tg[i]);
    if (ntg > 0 && nbind > 0)
      for (int i = 0; i < ntg; i++) sh_mb_block(F, c, n, blk, tg[i]);
  }
  static const NodeKind sup[] = { NK_SuperNode, NK_ForwardingSuperNode };
  for (int k = 0; k < 2; k++)
    NT_FOREACH_KIND(nt, sup[k], n) {
      if (dead[n]) continue;
      int blk = nt_ref(nt, n, "block");
      /* (the methods it enters: a program `new`'s super into the builtin
         Class#new enters the initialize it runs, which answers no value) */
      int tg[64], ntg = cplan_live_targets(c, n, tg, 64, 1), cur = sh_method_index(c, n);
      for (int i = 0; i < ntg; i++) {
        if (sup[k] == NK_ForwardingSuperNode && cur >= 0) sh_fwd_edge(F, cur, tg[i]);
        if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) sh_mb_add(F, c, blk, tg[i]);
        else if (blk >= 0) sh_block_value_to(F, c, n, blk, tg[i]);
        else if (sup[k] == NK_SuperNode) sh_fwd_edge(F, cur, tg[i]);
      }
    }
  /* what a Method, a `send` or define_method reaches is given a block the
     table does not see; so is a method_missing */
  for (int mi = 0; mi < c->nscopes; mi++) {
    Scope *m = &c->scopes[mi];
    if (m->def_node < 0 || !m->name) continue;
    int reach = F->dyn_all || sp_streq(m->name, "method_missing");
    for (int k = 0; k < F->ndyn && !reach; k++) reach = sp_streq(F->dyn[k], m->name);
    if (!reach) continue;
    F->blk_unk[mi] = 1;
  }
}
/* The tables the questions below read, without the walk. */
static ShareFacts *sh_memo_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  /* (the round's types are settled: the call targets held from earlier
     rounds are asked again) */
  cplan_targets_drop();
  ShareFacts *F = calloc(1, sizeof *F);
  size_t ns = (size_t)(c->nscopes > 0 ? c->nscopes : 1);
  if (!F) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  F->nnodes = nt->count;
  F->blk_unk = calloc(ns, 1);
  if (!F->blk_unk) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  NT_FOREACH_KIND(nt, NK_ConstantReadNode, cr)
    if (!F->ostruct && key_name(c, cr) && sp_streq(key_name(c, cr), "OpenStruct")) F->ostruct = 1;
  sh_mb_build(F, c);
  sh_index_blocks(F, c);
  return F;
}

void share_facts_build(Compiler *c) {
  share_facts_free(c);
  c->share = sh_memo_build(c);
  comp_touch_global();   /* the facts inference reads under --share-strings are new */
}

void share_facts_free(Compiler *c) {
  sh_free(c->share);
  c->share = NULL;
}

/* Does the subtree at n hold a `next` that leaves it (not one in a nested
   block, lambda, method or loop)? */
static int sh_has_next(const NodeTable *nt, int n) {
  if (n < 0) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_NextNode) return 1;
  if (k == NK_BlockNode || k == NK_LambdaNode || k == NK_DefNode || k == NK_WhileNode || k == NK_UntilNode ||
      k == NK_ForNode)
    return 0;
  for (int i = 0; i < nt_num_refs(nt, n); i++) if (sh_has_next(nt, nt_ref_at(nt, n, i))) return 1;
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int m = 0; const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) if (sh_has_next(nt, ids[j])) return 1;
  }
  return 0;
}
/* share.h */
int share_block_returns(const Compiler *c, int blk) {
  const NodeTable *nt = c->nt;
  int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1, n = 0;
  const int *st = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &n) : NULL;
  return n == 0 || nt_kind(nt, st[n - 1]) != NK_BreakNode || sh_has_next(nt, body);
}
