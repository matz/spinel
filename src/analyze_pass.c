#include "analyze_internal.h"
#include <stdio.h>
#include <stdlib.h>

/* True when `recv` reads an ivar/local that has at least one direct write of a
   numeric scalar (an Integer/Float literal, or a numeric-inferred value). Used
   to disambiguate `x << y`: a numeric-assigned slot means Integer#<< (a bit
   shift), not Array#push, so the slot must not be promoted to an array. The
   Integer/Float *literal* test is syntactic and therefore stable across
   fixpoint iterations, which keeps an early push pass from corrupting the slot
   to an array before the numeric assignment has been folded in (after which the
   array would unify with the int writes to poly and break every later shift). */
/* Name-keyed index of ivar/local write nodes, cached per node table. Several
   inference helpers look up "writes to the same name as this receiver"; during
   the fixpoint that runs many times per node, so a full rescan each call is
   O(recvs * nodes * iterations). The index is built once and reused across all
   fixpoint iterations (rebuilt if the table changes). */
static unsigned wrn_hash(const char *s) {
  unsigned h = 2166136261u;
  for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
  return h;
}
/* Bucket key. Local writes are keyed by (name, scope) so a common local name
   (`s`, `result`) written across thousands of methods does not collapse into
   one giant chain that every query must walk; ivar writes are keyed by name
   alone, matching the lookup semantics (an ivar `@x` write anywhere counts). */
static unsigned wrn_key(const char *nm, int scopeidx) {
  unsigned h = wrn_hash(nm);
  if (scopeidx >= 0) h = h * 31u + (unsigned)scopeidx * 2654435761u;
  return h;
}
static const NodeTable *wrn_nt = NULL;
static int wrn_ntc = -1, wrn_buckets = 0;
static int *wrn_next = NULL, *wrn_head = NULL;
static void wrn_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  int n = nt->count;
  free(wrn_next); free(wrn_head);
  wrn_buckets = n > 0 ? n : 1;
  wrn_next = malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
  wrn_head = malloc((size_t)wrn_buckets * sizeof(int));
  wrn_nt = nt; wrn_ntc = n;
  if (!wrn_next || !wrn_head) { wrn_buckets = 0; return; }
  for (int i = 0; i < wrn_buckets; i++) wrn_head[i] = -1;
  for (int id = 0; id < n; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    int is_local = sp_streq(ty, "LocalVariableWriteNode");
    if (!is_local && !sp_streq(ty, "InstanceVariableWriteNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int scopeidx = is_local ? (int)(comp_scope_of(c, id) - c->scopes) : -1;
    unsigned b = wrn_key(nm, scopeidx) % (unsigned)wrn_buckets;
    wrn_next[id] = wrn_head[b]; wrn_head[b] = id;
  }
}
static int recv_has_scalar_numeric_write(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  const char *rty = recv >= 0 ? nt_type(nt, recv) : NULL;
  if (!rty) return 0;
  int is_ivar = sp_streq(rty, "InstanceVariableReadNode");
  int is_local = sp_streq(rty, "LocalVariableReadNode");
  if (!is_ivar && !is_local) return 0;
  const char *rnm = nt_str(nt, recv, "name");
  if (!rnm) return 0;
  Scope *rscope = is_local ? comp_scope_of(c, recv) : NULL;
  const char *wkind = is_ivar ? "InstanceVariableWriteNode" : "LocalVariableWriteNode";
  if (wrn_nt != nt || wrn_ntc != nt->count) wrn_build(c);
  if (!wrn_buckets) return 0;
  int qscope = is_local ? (int)(rscope - c->scopes) : -1;
  unsigned b = wrn_key(rnm, qscope) % (unsigned)wrn_buckets;
  for (int id = wrn_head[b]; id >= 0; id = wrn_next[id]) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, wkind)) continue;
    const char *wnm = nt_str(nt, id, "name");
    if (!wnm || !sp_streq(wnm, rnm)) continue;
    if (is_local && comp_scope_of(c, id) != rscope) continue;
    int v = nt_ref(nt, id, "value");
    if (v < 0) continue;
    const char *vty = nt_type(nt, v);
    if (vty && (sp_streq(vty, "IntegerNode") || sp_streq(vty, "FloatNode"))) return 1;
    TyKind vt = infer_type(c, v);
    if (vt == TY_INT || vt == TY_FLOAT || vt == TY_BIGINT) return 1;
  }
  return 0;
}

/* Name-keyed index of `recv[k] = v` (`[]=`) call sites whose receiver is an
   ivar/local read, cached per node table. aset_value_type and
   infer_param_hash_value both look these up by receiver name; during the
   fixpoint that is O(recvs * nodes * iterations) without the index. The shape
   is stable across the pass (only inferred types change), so it is built once
   and reused; per-call filters (exact name, receiver kind, scope) run fresh. */
static const NodeTable *aw_nt = NULL;
static int aw_ntc = -1, aw_buckets = 0;
static int *aw_next = NULL, *aw_head = NULL;
static void aw_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  int n = nt->count;
  free(aw_next); free(aw_head);
  aw_buckets = n > 0 ? n : 1;
  aw_next = malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
  aw_head = malloc((size_t)aw_buckets * sizeof(int));
  aw_nt = nt; aw_ntc = n;
  if (!aw_next || !aw_head) { aw_buckets = 0; return; }
  for (int i = 0; i < aw_buckets; i++) aw_head[i] = -1;
  for (int id = 0; id < n; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "[]=")) continue;
    int wr = nt_ref(nt, id, "receiver");
    if (wr < 0) continue;
    int wk = nt_kind(nt, wr);
    if (wk != NK_InstanceVariableReadNode && wk != NK_LocalVariableReadNode) continue;
    const char *wn = nt_str(nt, wr, "name");
    if (!wn) continue;
    unsigned b = wrn_hash(wn) % (unsigned)aw_buckets;
    aw_next[id] = aw_head[b]; aw_head[b] = id;
  }
}
/* First `[]=` call id chained for receiver name `rnm`, or -1; walk via aw_next.
   Caller must still verify the exact name (hash collisions) and receiver kind. */
static int aw_first(Compiler *c, const char *rnm) {
  const NodeTable *nt = c->nt;
  if (aw_nt != nt || aw_ntc != nt->count) aw_build(c);
  if (!aw_buckets) return -1;
  return aw_head[wrn_hash(rnm) % (unsigned)aw_buckets];
}

/* Unified value type of `recv[k] = v` writes that target the same ivar/local
   as `recv`. Lets a hash promoted via a string-key READ inherit the value type
   its `[]=` writes establish (e.g. `@h[s] = int` -> str_int_hash) instead of
   defaulting to a str_poly slot that can never narrow. TY_UNKNOWN if there is
   no such write (caller falls back to poly). */
/* Unified value type of every `recv[k] = v` write to this slot, and (through
   nwrites) whether there are any. "No writes" and "writes whose value type has
   not been derived yet" both answer TY_UNKNOWN, and a caller deciding a hash
   variant has to tell them apart: the first means poly is the right answer,
   the second means wait. */
TyKind aset_value_type_ex(Compiler *c, int recv, int *nwrites) {
  const NodeTable *nt = c->nt;
  const char *rty = nt_type(nt, recv);
  if (!rty) return TY_UNKNOWN;
  int is_ivar = sp_streq(rty, "InstanceVariableReadNode");
  int is_local = sp_streq(rty, "LocalVariableReadNode");
  if (!is_ivar && !is_local) return TY_UNKNOWN;
  const char *rnm = nt_str(nt, recv, "name");
  if (!rnm) return TY_UNKNOWN;
  Scope *rsc = comp_scope_of(c, recv);
  int rcls = rsc ? rsc->class_id : -1;
  TyKind acc = TY_UNKNOWN;
  for (int id = aw_first(c, rnm); id >= 0; id = aw_next[id]) {
    int wrecv = nt_ref(nt, id, "receiver");
    if (wrecv < 0) continue;
    const char *wn = nt_str(nt, wrecv, "name");
    if (!wn || !sp_streq(wn, rnm)) continue;
    if (is_ivar) {
      if (nt_kind(nt, wrecv) != NK_InstanceVariableReadNode) continue;
      Scope *ws = comp_scope_of(c, wrecv);
      if ((ws ? ws->class_id : -1) != rcls) continue;
    }
    else {
      if (nt_kind(nt, wrecv) != NK_LocalVariableReadNode) continue;
      if (comp_scope_of(c, wrecv) != rsc) continue;
    }
    int args = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an < 2) continue;
    if (nwrites) (*nwrites)++;
    acc = ty_unify(acc, infer_type(c, av[1]));
  }
  return acc;
}

/* Unified KEY type of every `name[k] = v` write to this local, or TY_UNKNOWN
   when it has none. Keyed by (scope, name) rather than by a receiver node so a
   post-fixpoint caller can ask about a local it only holds the WRITE of.

   The key context pass (`mark_empty_hash_key_ctx`) answers the same question
   during the fixpoint, but it can only see key types that have settled by
   then; a key that is still open there -- a block's return value, say -- is
   invisible to it and the local falls through to a default. Post-fixpoint the
   type is known, so the default can be checked against it (#3397). */
TyKind local_aset_key_type(Compiler *c, Scope *sc, const char *name, int *nwrites) {
  const NodeTable *nt = c->nt;
  if (!sc || !name) return TY_UNKNOWN;
  TyKind acc = TY_UNKNOWN;
  for (int id = aw_first(c, name); id >= 0; id = aw_next[id]) {
    int wrecv = nt_ref(nt, id, "receiver");
    if (wrecv < 0 || nt_kind(nt, wrecv) != NK_LocalVariableReadNode) continue;
    const char *wn = nt_str(nt, wrecv, "name");
    if (!wn || !sp_streq(wn, name)) continue;
    if (comp_scope_of(c, wrecv) != sc) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an < 2) continue;
    if (nwrites) (*nwrites)++;
    acc = ty_unify(acc, infer_type(c, av[0]));
  }
  return acc;
}

/* Seed a hash parameter's value type from its own `param[k] = v` writes. The
   usage-driven hash promotion skips parameters (they are typed from call
   sites), so a param filled internally by `p[s] = int` and read back via
   `p.fetch(s)` defaults to str_poly through a monotonic cycle. Narrow it to
   the concrete hash its writes establish.

   ONLY string/symbol-keyed writes are considered: an int-keyed `p[i] = v` is
   ambiguous with array element assignment (e.g. an int_array RAM param filled
   by `ram[i] = b`), so int keys must not be read as hash evidence. */
static TyKind aset_value_type(Compiler *c, int recv) {
  return aset_value_type_ex(c, recv, NULL);
}

int infer_param_hash_value(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int p = 0; p < sc->nparams; p++) {
      if (p == sc->rest_idx || p == sc->kwrest_idx) continue;
      LocalVar *lv = scope_local(sc, sc->pnames[p]);
      if (!lv || lv->is_block_param) continue;
      TyKind cur = lv->type;
      /* An already-CONCRETE hash type is a caller's, and the object is shared:
         narrowing str->poly to str->str here does not convert anything, it
         reinterprets the caller's sp_StrPolyHash * through an sp_StrStrHash *
         parameter. The arms of one polymorphic dispatch then disagreed on the
         variant and the call passed the wrong struct (#3381). Seed only a
         parameter no call site has typed yet -- which is the case the
         narrowing exists for: an empty `{}` at the caller, still UNKNOWN, that
         the reverse binding then coerces to whatever this settles on. */
      int seedable = cur == TY_UNKNOWN || cur == TY_POLY;
      if (!seedable || lv->rbs_seeded) continue;
      /* An int-keyed `p[i] = v` is normally excluded: it is ambiguous with
         array-element assignment (an int_array RAM param filled by `ram[i]=b`).
         But when the param is already KNOWN to be a hash (its current type is a
         hash variant -- e.g. it received a `{}` literal from a caller), there
         is no array ambiguity, so int keys are valid hash evidence. Narrowing
         to the int-keyed variant then lets the caller's empty `{}` coerce to a
         matching hash, so an in-method `p[i]=v` mutates in place instead of a
         widen that a poly param drops (#2871). */
      int known_hash = ty_is_hash(cur);
      TyKind kt = TY_UNKNOWN, vt = TY_UNKNOWN;
      int saw = 0, ambiguous = 0;
      for (int id = aw_first(c, sc->pnames[p]); id >= 0; id = aw_next[id]) {
        int wr = nt_ref(nt, id, "receiver");
        if (wr < 0 || nt_kind(nt, wr) != NK_LocalVariableReadNode) continue;
        const char *wn = nt_str(nt, wr, "name");
        if (!wn || !sp_streq(wn, sc->pnames[p]) || comp_scope_of(c, wr) != sc) continue;
        int args = nt_ref(nt, id, "arguments");
        int an = 0;
        const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
        if (an < 2) continue;
        TyKind k = infer_type(c, av[0]);
        if (k != TY_STRING && k != TY_SYMBOL && !(known_hash && k == TY_INT)) { ambiguous = 1; break; }
        kt = ty_unify(kt, k);
        vt = ty_unify(vt, infer_type(c, av[1]));
        saw = 1;
      }
      if (!saw || ambiguous) continue;
      TyKind hv = ty_hash_of(kt, vt);
      if (hv == TY_UNKNOWN) continue;
      if (hv != cur && ty_hash_val(hv) != TY_POLY) { lv->type = hv; changed = 1; }
    }
  }
  return changed;
}

/* 1 if local `name` in scope `sc` has at least one write and every write
   assigns an empty `{}` hash literal -- i.e. it is a hash container whose
   contents come from elsewhere (passed by reference into a callee). Such a
   local can safely adopt a hash type from a parameter it is passed to. */
/* Shared cached write-index accessors (defined after the LWIndex machinery
   below): bucket walk over (scope, name) instead of a whole-table rescan per
   query. */
int local_all_writes_empty_hash(Compiler *c, Scope *sc, const char *name) {
  const NodeTable *nt = c->nt;
  int saw = 0;
  int si = (int)(sc - c->scopes);
  for (int r = lw_shared_first(c, name, si); r >= 0; r = lw_shared_next(r)) {
    int id = lw_shared_node(r);
    if (nt_kind(nt, id) != NK_LocalVariableWriteNode) continue;
    const char *wn = nt_str(nt, id, "name");
    if (!wn || !sp_streq(wn, name) || comp_scope_of(c, id) != sc) continue;
    int v = nt_ref(nt, id, "value");
    if (v < 0 || nt_kind(nt, v) != NK_HashNode) return 0;
    int hn = 0; nt_arr(nt, v, "elements", &hn);
    if (hn != 0) return 0;
    saw = 1;
  }
  return saw;
}

/* 1 if local `name` in scope `sc` has at least one write and every write
   assigns an empty `[]` array literal -- the array analogue of
   local_all_writes_empty_hash. Such a local carries no element evidence of
   its own, so it can adopt the poly element type a callee's push forced. */
static int local_all_writes_empty_array(Compiler *c, Scope *sc, const char *name) {
  const NodeTable *nt = c->nt;
  int saw = 0;
  int si = (int)(sc - c->scopes);
  for (int r = lw_shared_first(c, name, si); r >= 0; r = lw_shared_next(r)) {
    int id = lw_shared_node(r);
    if (nt_kind(nt, id) != NK_LocalVariableWriteNode) continue;
    const char *wn = nt_str(nt, id, "name");
    if (!wn || !sp_streq(wn, name) || comp_scope_of(c, id) != sc) continue;
    int v = nt_ref(nt, id, "value");
    if (v < 0 || nt_kind(nt, v) != NK_ArrayNode) return 0;
    int an = 0; nt_arr(nt, v, "elements", &an);
    if (an != 0) return 0;
    saw = 1;
  }
  return saw;
}

/* Per-pass index of local-variable write nodes keyed by (scope, name). The
   usage-driven promotion scans in infer_write_types ask "does local X in scope
   S have any write / an array-typed write"; without this index each such query
   re-scanned the entire node table, making a program with M such sites
   O(M * nodes) -- the dominant cost on large auto-generated model graphs. The
   index groups the write nodes once so each query walks only its own bucket. */
typedef struct {
  int *node;   /* local-write node ids */
  int *next;   /* chain: next record in the same bucket, or -1 */
  int *head;   /* hash buckets: head record index into node[], or -1 */
  int cap;     /* bucket count (power of two) */
} LWIndex;

static int lw_is_write_kind(NodeKind k) {
  return k == NK_LocalVariableWriteNode || k == NK_LocalVariableOrWriteNode ||
         k == NK_LocalVariableAndWriteNode || k == NK_LocalVariableOperatorWriteNode;
}

static unsigned lw_hash(const char *name, int scope) {
  unsigned h = 2166136261u ^ (unsigned)scope;
  for (const char *p = name; p && *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  return h;
}

static const NodeKind lw_write_kinds[4] = {
  NK_LocalVariableWriteNode, NK_LocalVariableOrWriteNode,
  NK_LocalVariableAndWriteNode, NK_LocalVariableOperatorWriteNode};

static void lw_index_build(Compiler *c, LWIndex *ix) {
  const NodeTable *nt = c->nt;
  /* Iterate only the write kinds (kind-grouped id lists) rather than the whole
     table: the write population is a small fraction of the node count, and
     this build runs per pass -- and per query while the scope index is
     unfrozen. Chain order becomes kind-grouped instead of globally ascending;
     every consumer is an order-independent existence/uniqueness walk. */
  int n = 0;
  for (int t = 0; t < 4; t++) {
    int kn; nt_nodes_of_kind(nt, lw_write_kinds[t], &kn); n += kn;
  }
  int cap = 16;
  while (cap < n * 2) cap <<= 1;
  ix->cap = cap;
  ix->node = (int *)malloc(sizeof(int) * (n > 0 ? n : 1));
  ix->next = (int *)malloc(sizeof(int) * (n > 0 ? n : 1));
  ix->head = (int *)malloc(sizeof(int) * cap);
  for (int i = 0; i < cap; i++) ix->head[i] = -1;
  int k = 0;
  for (int t = 0; t < 4; t++) {
    int kn; const int *ids = nt_nodes_of_kind(nt, lw_write_kinds[t], &kn);
    for (int j = 0; j < kn; j++) {
      int id = ids[j];
      const char *nm = nt_str(nt, id, "name");
      int sc = (int)(comp_scope_of(c, id) - c->scopes);
      unsigned h = lw_hash(nm, sc) & (unsigned)(cap - 1);
      ix->node[k] = id;
      ix->next[k] = ix->head[h];
      ix->head[h] = k;
      k++;
    }
  }
}

static void lw_index_free(LWIndex *ix) {
  free(ix->node); free(ix->next); free(ix->head);
}

/* First record index in the bucket for (scope, name); iterate via ix->next.
   Callers must still confirm scope/name (hash collisions) and node kind. */
static int lw_index_first(const LWIndex *ix, const char *name, int scope) {
  return ix->head[lw_hash(name, scope) & (unsigned)(ix->cap - 1)];
}

/* Long-lived LWIndex behind the local_all_writes_empty_* queries above.
   Rebuilt when the node table or scope shape moves (append-only table, so
   count is the growth signal; the scope-index epoch covers renames/reshapes
   that re-home writes without growing the table). */
static LWIndex lw_shared_ix;
static const NodeTable *lw_shared_nt = NULL;
static int lw_shared_ntc = -1;
static unsigned lw_shared_gen = 0;
int lw_shared_first(Compiler *c, const char *name, int scope) {
  unsigned gen = comp_scope_index_gen();
  /* While unfrozen, scope shape can move without the epoch ticking; rebuild
     per query (the build is one table walk -- the same order as the scan
     these helpers used to do, so the unfrozen phases pay what they always
     paid while the frozen fixpoint gets the cached bucket walk). */
  if (!comp_scope_index_is_frozen() ||
      lw_shared_nt != c->nt || lw_shared_ntc != c->nt->count || lw_shared_gen != gen) {
    if (lw_shared_nt) lw_index_free(&lw_shared_ix);
    lw_index_build(c, &lw_shared_ix);
    lw_shared_nt = c->nt; lw_shared_ntc = c->nt->count; lw_shared_gen = gen;
  }
  return lw_index_first(&lw_shared_ix, name, scope);
}
int lw_shared_node(int rec) { return lw_shared_ix.node[rec]; }
int lw_shared_next(int rec) { return lw_shared_ix.next[rec]; }

/* Per-pass index of instance-variable write nodes keyed by ivar name -- the
   ivar analogue of LWIndex. The usage-driven promotion scans below ask "does
   @x have a non-empty-hash / typed write"; without this each query re-scanned
   the whole node table, the dominant cost on ivar-heavy model graphs (the
   #1302 from_hash/to_hash shape). Indexes both plain and `||=` ivar writes;
   callers filter by node kind and class. Reuses LWIndex's layout (scope-less,
   so the bucket key is name only). */
static int ivw_is_write_kind(NodeKind k) {
  return k == NK_InstanceVariableWriteNode || k == NK_InstanceVariableOrWriteNode;
}

static unsigned ivw_hash(const char *name) {
  unsigned h = 2166136261u;
  for (const char *p = name; p && *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  return h;
}

static void ivw_index_build(Compiler *c, LWIndex *ix) {
  const NodeTable *nt = c->nt;
  int n = 0;
  for (int id = 0; id < nt->count; id++)
    if (ivw_is_write_kind(nt_kind(nt, id))) n++;
  int cap = 16;
  while (cap < n * 2) cap <<= 1;
  ix->cap = cap;
  ix->node = (int *)malloc(sizeof(int) * (n > 0 ? n : 1));
  ix->next = (int *)malloc(sizeof(int) * (n > 0 ? n : 1));
  ix->head = (int *)malloc(sizeof(int) * cap);
  for (int i = 0; i < cap; i++) ix->head[i] = -1;
  int k = 0;
  for (int id = 0; id < nt->count; id++) {
    if (!ivw_is_write_kind(nt_kind(nt, id))) continue;
    const char *nm = nt_str(nt, id, "name");
    unsigned h = ivw_hash(nm) & (unsigned)(cap - 1);
    ix->node[k] = id;
    ix->next[k] = ix->head[h];
    ix->head[h] = k;
    k++;
  }
}

/* First record index in the bucket for ivar `name`; iterate via ix->next.
   Callers must still confirm name (hash collisions), node kind, and class. */
static int ivw_index_first(const LWIndex *ix, const char *name) {
  return ix->head[ivw_hash(name) & (unsigned)(ix->cap - 1)];
}

/* `x, y = obj.m` where the callee's body ends in `return a, b` with statically
   known element types: yields those types so the destructured targets keep
   them instead of widening to poly with the tuple. The callee must be uniquely
   resolvable (object receiver with no subclass override, constant receiver, or
   self) and every element must infer to a concrete type. Returns the element
   count, or 0 when the shape doesn't apply. */
/* Does any node under `root`, other than the subtree rooted at `skip`, carry a
   ReturnNode? Used to reject a method with more than one return shape. */
static int subtree_has_return(const NodeTable *nt, int root, int skip) {
  if (root < 0 || root == skip) return 0;
  const char *ty = nt_type(nt, root);
  if (ty && sp_streq(ty, "ReturnNode")) return 1;
  if (ty && (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") ||
             sp_streq(ty, "ModuleNode"))) return 0;
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++)
    if (subtree_has_return(nt, nt_ref_at(nt, root, i), skip)) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, root, i, &n);
    for (int j = 0; j < n; j++)
      if (subtree_has_return(nt, ids[j], skip)) return 1;
  }
  return 0;
}

static int multi_return_elem_types(Compiler *c, int value, TyKind *out, int max) {
  const NodeTable *nt = c->nt;
  const char *vty = nt_type(nt, value);
  if (!vty || !sp_streq(vty, "CallNode")) return 0;
  const char *mn = nt_str(nt, value, "name");
  if (!mn) return 0;
  int recv = nt_ref(nt, value, "receiver");
  int mi = -1;
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
      int cid = comp_class_index(c, nt_str(nt, recv, "name"));
      if (cid < 0) return 0;
      mi = comp_cmethod_in_chain(c, cid, mn, NULL);
    }
    else {
      TyKind rt = infer_type(c, recv);
      if (!ty_is_object(rt)) return 0;
      int cid = ty_object_class(rt);
      mi = comp_method_in_chain(c, cid, mn, NULL);
      /* a subclass override could return different element types */
      for (int cj = 0; mi >= 0 && cj < c->nclasses; cj++) {
        int an = cj;
        while (an >= 0 && an != cid) an = c->classes[an].parent;
        if (an != cid || cj == cid) continue;
        if (comp_method_in_chain(c, cj, mn, NULL) != mi) return 0;
      }
    }
  }
  else {
    Scope *s = comp_scope_of(c, value);
    if (!s) return 0;
    if (s->class_id >= 0) mi = comp_method_in_chain(c, s->class_id, mn, NULL);
    else {
      /* a top-level `def k` is a free function, not in any class chain (#2924) */
      for (int si = 0; si < c->nscopes; si++)
        if (c->scopes[si].class_id < 0 && c->scopes[si].name &&
            sp_streq(c->scopes[si].name, mn) && c->scopes[si].def_node >= 0) { mi = si; break; }
    }
  }
  if (mi < 0) return 0;
  int def = c->scopes[mi].def_node;
  int body = def >= 0 ? nt_ref(nt, def, "body") : -1;
  int bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (!bb || bn <= 0) return 0;
  /* Only a method with ONE tuple shape can type its targets: an early
     `return nil, nil` alongside a final `return s, s` would otherwise pin
     the targets to the last shape and mistype the nil path. Bail whenever
     the body carries a return anywhere but its last statement. (#2924) */
  for (int i = 0; i < bn - 1; i++)
    if (nt_type(nt, bb[i]) && sp_streq(nt_type(nt, bb[i]), "ReturnNode")) return 0;
  if (subtree_has_return(nt, body, bb[bn - 1])) return 0;
  int last = bb[bn - 1];
  int an = 0;
  const int *av = NULL;
  if (nt_type(nt, last) && sp_streq(nt_type(nt, last), "ReturnNode")) {
    int args = nt_ref(nt, last, "arguments");
    av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  }
  /* A method whose last statement IS an array literal returns that array, and
     `a, b = m` destructures it -- so the literal's element types flow to the
     targets just as an explicit `return a, b` does (#2924). */
  else if (nt_type(nt, last) && sp_streq(nt_type(nt, last), "ArrayNode")) {
    av = nt_arr(nt, last, "elements", &an);
    for (int i = 0; av && i < an; i++)
      if (nt_type(nt, av[i]) && sp_streq(nt_type(nt, av[i]), "SplatNode")) return 0;
  }
  if (!av || an < 2 || an > max) return 0;
  for (int i = 0; i < an; i++) {
    TyKind et = infer_type(c, av[i]);
    if (et == TY_UNKNOWN || et == TY_POLY || et == TY_NIL || et == TY_VOID) return 0;
    out[i] = et;
  }
  return an;
}

/* `A, B = [x, y].map { ... }`: the tuple has exactly the literal receiver's
   element count and every element takes the block's (concrete) result type.
   Returns that count, or 0 when the shape doesn't apply. */
static int map_literal_elem_types(Compiler *c, int value, TyKind *out, int max) {
  const NodeTable *nt = c->nt;
  const char *vty = nt_type(nt, value);
  if (!vty || !sp_streq(vty, "CallNode")) return 0;
  const char *mn = nt_str(nt, value, "name");
  if (!mn || (!sp_streq(mn, "map") && !sp_streq(mn, "collect"))) return 0;
  int recv = nt_ref(nt, value, "receiver");
  if (recv < 0 || !sp_streq(nt_type(nt, recv) ? nt_type(nt, recv) : "", "ArrayNode")) return 0;
  int en = 0;
  nt_arr(nt, recv, "elements", &en);
  if (en < 2 || en > max) return 0;
  int blk = nt_ref(nt, value, "block");
  int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
  int bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (!bb || bn <= 0) return 0;
  TyKind et = infer_type(c, bb[bn - 1]);
  if (et == TY_UNKNOWN || et == TY_POLY || et == TY_NIL || et == TY_VOID) return 0;
  for (int i = 0; i < en; i++) out[i] = et;
  return en;
}

/* The element type a splice RHS (`a[s,l] = rhs` / `a[range] = rhs`) would
   contribute to the receiver: a typed array source contributes its element
   type; a poly array -- or nil, which only a poly array can hold --
   contributes TY_POLY; an empty-`[]` source (TY_UNKNOWN) contributes no
   evidence; a scalar contributes itself. A user object goes through its
   to_ary return type when it defines one (CRuby coerces the splice source);
   without to_ary it inserts as a single heterogeneous element. Stable across
   the fixpoint: scopes[].ret settles monotonically, and an unsettled ret
   simply contributes no evidence that iteration. */
static TyKind splice_incoming_elem(Compiler *c, int rhs) {
  TyKind t = infer_type(c, rhs);
  if (t == TY_POLY_ARRAY) return TY_POLY_ARRAY;  /* heterogeneous source */
  if (ty_is_array(t)) return ty_array_elem(t);
  if (ty_is_object(t)) {
    int mi = comp_method_in_chain(c, ty_object_class(t), "to_ary", NULL);
    if (mi >= 0) {
      TyKind r = (TyKind)c->scopes[mi].ret;
      if (r == TY_POLY_ARRAY) return TY_POLY_ARRAY;
      if (ty_is_array(r)) return ty_array_elem(r);
      return TY_UNKNOWN;
    }
    return t;
  }
  return t;   /* scalar (incl. TY_NIL, and TY_POLY = statically unknown) */
}

/* The array type an array-pattern scrutinee deconstructs to. A Struct/Data
   object has no member array of its own; #deconstruct boxes its members into a
   poly array, so its element/rest bindings are poly (not the parent's int
   default). Any other scrutinee keeps its own type. */
static TyKind pm_deconstruct_arr_ty(Compiler *c, TyKind scrutinee_t) {
  if (ty_is_object(scrutinee_t) && c->classes[ty_object_class(scrutinee_t)].is_struct)
    return TY_POLY_ARRAY;
  return scrutinee_t;
}

/* Recursively type every local bound anywhere inside a nested container
   pattern to boxed poly: values reached through a poly-valued container
   (a hash value, a find window, a capture under either) are sp_RbVal at
   the binding site. Monotonic unify, like the flat arms. */
static int pm_seed_locals_poly(Compiler *c, Scope *ms, int pat) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  if (pat < 0) return 0;
  const char *pty = nt_type(nt, pat);
  if (!pty) return 0;
  if (sp_streq(pty, "LocalVariableTargetNode")) {
    const char *lnm = nt_str(nt, pat, "name");
    LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
    if (lv && !lv->is_param && !lv->is_block_param) {
      TyKind mg = ty_unify(lv->type, TY_POLY);
      if (mg != lv->type) { lv->type = mg; changed = 1; }
    }
    return changed;
  }
  if (sp_streq(pty, "CapturePatternNode")) {
    changed |= pm_seed_locals_poly(c, ms, nt_ref(nt, pat, "target"));
    changed |= pm_seed_locals_poly(c, ms, nt_ref(nt, pat, "value"));
    return changed;
  }
  if (sp_streq(pty, "SplatNode"))
    return pm_seed_locals_poly(c, ms, nt_ref(nt, pat, "expression"));
  if (sp_streq(pty, "AssocNode"))
    return pm_seed_locals_poly(c, ms, nt_ref(nt, pat, "value"));
  if (sp_streq(pty, "AssocSplatNode"))
    return pm_seed_locals_poly(c, ms, nt_ref(nt, pat, "value"));
  if (sp_streq(pty, "ArrayPatternNode") || sp_streq(pty, "FindPatternNode") ||
      sp_streq(pty, "HashPatternNode")) {
    int n = 0;
    const int *kids = nt_arr(nt, pat, sp_streq(pty, "HashPatternNode") ? "elements" : "requireds", &n);
    for (int i = 0; i < n; i++) changed |= pm_seed_locals_poly(c, ms, kids[i]);
    int np = 0;
    const int *posts = nt_arr(nt, pat, "posts", &np);
    for (int i = 0; i < np; i++) changed |= pm_seed_locals_poly(c, ms, posts[i]);
    changed |= pm_seed_locals_poly(c, ms, nt_ref(nt, pat, "rest"));
    changed |= pm_seed_locals_poly(c, ms, nt_ref(nt, pat, "left"));
    changed |= pm_seed_locals_poly(c, ms, nt_ref(nt, pat, "right"));
    return changed;
  }
  return 0;
}

/* Is this pattern node a container whose inner bindings arrive boxed? */
static int pm_is_container_pat(const NodeTable *nt, int pat) {
  const char *pty = pat >= 0 ? nt_type(nt, pat) : NULL;
  return pty && (sp_streq(pty, "ArrayPatternNode") || sp_streq(pty, "FindPatternNode") ||
                 sp_streq(pty, "HashPatternNode"));
}

static int infer_case_pattern_locals(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  /* CaseMatchNode: `case X; in PATTERN; ...` — infer locals bound by pattern.
     Handles: bare LV (`in x`), guard (`in x if cond`), capture (`in P => x`),
     and array patterns (`in [first, *rest]` / `in Array(head, *tail)`). */
  NT_FOREACH_KIND(nt, NK_CaseMatchNode, id) {
    int pred = nt_ref(nt, id, "predicate");
    if (pred < 0) continue;
    TyKind scrutinee_t = infer_type(c, pred);
    /* A local-variable scrutinee was just reset to TY_UNKNOWN at the top of
       infer_write_types (its real type stashed in gc_root), so infer_type
       reads back UNKNOWN this iteration. Recover the prior-iteration type from
       gc_root -- same idiom the empty-collection promotion below uses -- so the
       binding arms are typed before the result local unifies them. */
    if (scrutinee_t == TY_UNKNOWN) {
      const char *pty0 = nt_type(nt, pred);
      if (pty0 && sp_streq(pty0, "LocalVariableReadNode")) {
        const char *pnm = nt_str(nt, pred, "name");
        LocalVar *plv = pnm ? scope_local(comp_scope_of(c, pred), pnm) : NULL;
        if (plv && !plv->is_param && !plv->is_block_param &&
            (TyKind)plv->gc_root != TY_UNKNOWN)
          scrutinee_t = (TyKind)plv->gc_root;
      }
    }
    int cn = 0;
    const int *conds = nt_arr(nt, id, "conditions", &cn);
    for (int ci = 0; ci < cn; ci++) {
      const char *cty = nt_type(nt, conds[ci]);
      if (!cty || !sp_streq(cty, "InNode")) continue;
      int pat = nt_ref(nt, conds[ci], "pattern");
      if (pat < 0) continue;
      Scope *ms = comp_scope_of(c, conds[ci]);
      const char *pty = nt_type(nt, pat);
      if (!pty) continue;
      int bind_lv_node = -1;
      int array_pat = -1;
      TyKind array_scrutinee = TY_UNKNOWN;
      if (sp_streq(pty, "LocalVariableTargetNode")) {
        /* in x */
        bind_lv_node = pat;
      }
      else if (sp_streq(pty, "IfNode")) {
        /* in x if guard — binding is in IfNode.statements body */
        int stmts = nt_ref(nt, pat, "statements");
        if (stmts >= 0 && nt_type(nt, stmts) &&
            sp_streq(nt_type(nt, stmts), "StatementsNode")) {
          int bn = 0;
          const int *body = nt_arr(nt, stmts, "body", &bn);
          for (int k = 0; k < bn; k++) {
            const char *bty = nt_type(nt, body[k]);
            if (bty && sp_streq(bty, "LocalVariableTargetNode")) {
              bind_lv_node = body[k]; break;
            }
          }
        }
      }
      else if (sp_streq(pty, "CapturePatternNode")) {
        /* in PATTERN => var */
        int tgt = nt_ref(nt, pat, "target");
        if (tgt >= 0 && nt_type(nt, tgt) &&
            sp_streq(nt_type(nt, tgt), "LocalVariableTargetNode"))
          bind_lv_node = tgt;
        /* inner ArrayPatternNode also gets element-level types */
        int val = nt_ref(nt, pat, "value");
        if (val >= 0 && nt_type(nt, val) &&
            sp_streq(nt_type(nt, val), "ArrayPatternNode")) {
          array_pat = val; array_scrutinee = pm_deconstruct_arr_ty(c, scrutinee_t);
        }
      }
      else if (sp_streq(pty, "ArrayPatternNode")) {
        /* in [first, *rest] or in Array(head, *tail) */
        array_pat = pat; array_scrutinee = pm_deconstruct_arr_ty(c, scrutinee_t);
      }
      else if (sp_streq(pty, "HashPatternNode")) {
        /* in {k:, k2: subpat} -- an AssocNode value that is an LV target binds
           the deconstructed value: the hash variant's value type for a hash
           scrutinee, boxed poly for a Struct/Data scrutinee (deconstruct_keys
           synthesizes a SymPolyHash), poly otherwise. */
        TyKind vt = TY_POLY;
        if (ty_is_hash(scrutinee_t)) vt = ty_hash_val(scrutinee_t);
        int pn = 0;
        const int *pelms = nt_arr(nt, pat, "elements", &pn);
        for (int k = 0; k < pn; k++) {
          const char *ety = nt_type(nt, pelms[k]);
          if (!ety || !sp_streq(ety, "AssocNode")) continue;
          int ptgt = nt_ref(nt, pelms[k], "value");
          if (ptgt < 0 || !nt_type(nt, ptgt)) continue;
          /* a nested container value ({a: {b:}}, {data: [*, y, *]}) delivers
             its inner bindings boxed */
          if (pm_is_container_pat(nt, ptgt)) {
            changed |= pm_seed_locals_poly(c, ms, ptgt);
            continue;
          }
          int btgt = ptgt;  /* `k: PAT => v` binds v to the value */
          if (sp_streq(nt_type(nt, ptgt), "CapturePatternNode")) {
            int cv = nt_ref(nt, ptgt, "value");
            if (pm_is_container_pat(nt, cv)) changed |= pm_seed_locals_poly(c, ms, cv);
            btgt = nt_ref(nt, ptgt, "target");
            if (btgt < 0 || !nt_type(nt, btgt)) continue;
          }
          if (!sp_streq(nt_type(nt, btgt), "LocalVariableTargetNode")) continue;
          const char *lnm = nt_str(nt, btgt, "name");
          LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
          if (!lv || lv->is_param || lv->is_block_param) continue;
          TyKind et = (vt != TY_UNKNOWN && vt != TY_NIL) ? vt : TY_POLY;
          TyKind mg = ty_unify(lv->type, et);
          if (mg != lv->type) { lv->type = mg; changed = 1; }
        }
      }
      else if (sp_streq(pty, "FindPatternNode")) {
        /* in [*head, a, b, *tail] -- the two splats bind to arrays of the
           scrutinee's element type; required LV targets bind to an element. */
        TyKind arr_t = ty_is_array(scrutinee_t) ? scrutinee_t : TY_POLY_ARRAY;
        TyKind elem_t = ty_is_array(scrutinee_t) ? ty_array_elem(scrutinee_t) : TY_POLY;
        int sides[2] = { nt_ref(nt, pat, "left"), nt_ref(nt, pat, "right") };
        for (int sidx = 0; sidx < 2; sidx++) {
          int sp = sides[sidx];
          if (sp < 0 || !nt_type(nt, sp) || !sp_streq(nt_type(nt, sp), "SplatNode")) continue;
          int inner = nt_ref(nt, sp, "expression");
          if (inner < 0 || !nt_type(nt, inner) ||
              !sp_streq(nt_type(nt, inner), "LocalVariableTargetNode")) continue;
          const char *snm = nt_str(nt, inner, "name");
          LocalVar *lv = snm ? scope_local(ms, snm) : NULL;
          if (!lv || lv->is_param || lv->is_block_param) continue;
          TyKind mg = ty_unify(lv->type, arr_t);
          if (mg != lv->type) { lv->type = mg; changed = 1; }
        }
        int rn = 0;
        const int *reqs = nt_arr(nt, pat, "requireds", &rn);
        for (int k = 0; k < rn; k++) {
          const char *lty2 = nt_type(nt, reqs[k]);
          if (!lty2) continue;
          int tgt = reqs[k];
          /* a `lit => x` window capture binds its target to an element */
          if (sp_streq(lty2, "CapturePatternNode")) {
            tgt = nt_ref(nt, reqs[k], "target");
            if (tgt < 0 || !nt_type(nt, tgt)) continue;
          }
          if (!sp_streq(nt_type(nt, tgt), "LocalVariableTargetNode")) continue;
          const char *lnm = nt_str(nt, tgt, "name");
          LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
          if (!lv || lv->is_param || lv->is_block_param) continue;
          TyKind et = (elem_t != TY_UNKNOWN) ? elem_t : TY_POLY;
          TyKind mg = ty_unify(lv->type, et);
          if (mg != lv->type) { lv->type = mg; changed = 1; }
        }
      }
      /* Bind simple LV target to scrutinee type */
      if (bind_lv_node >= 0 && scrutinee_t != TY_UNKNOWN) {
        const char *lnm = nt_str(nt, bind_lv_node, "name");
        LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
        if (lv && !lv->is_param && !lv->is_block_param) {
          TyKind mg = ty_unify(lv->type, scrutinee_t);
          if (mg != lv->type) { lv->type = mg; changed = 1; }
        }
      }
      /* Handle ArrayPatternNode requireds and rest splat */
      if (array_pat >= 0) {
        TyKind elem_t = ty_is_array(array_scrutinee) ? ty_array_elem(array_scrutinee) : TY_UNKNOWN;
        int apn = 0;
        const int *reqs = nt_arr(nt, array_pat, "requireds", &apn);
        for (int k = 0; k < apn; k++) {
          const char *lty2 = nt_type(nt, reqs[k]);
          if (!lty2) continue;
          /* a hash/find element pattern ([{name:}]) delivers its inner
             bindings boxed; nested array elements keep their own typing */
          if (sp_streq(lty2, "HashPatternNode") || sp_streq(lty2, "FindPatternNode")) {
            changed |= pm_seed_locals_poly(c, ms, reqs[k]);
            continue;
          }
          if (!sp_streq(lty2, "LocalVariableTargetNode")) continue;
          const char *lnm = nt_str(nt, reqs[k], "name");
          LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
          if (!lv || lv->is_param || lv->is_block_param) continue;
          /* A poly/untyped VALUE scrutinee yields boxed elements, so a required
             binding is poly -- not int. TY_INT here reinterpreted a boxed
             element's bits and produced garbage. Object scrutinees keep the
             legacy default (their members/#deconstruct are usually int-typed). */
          TyKind et = (elem_t != TY_UNKNOWN) ? elem_t
                    : ty_is_object(array_scrutinee) ? TY_INT : TY_POLY;
          TyKind mg = ty_unify(lv->type, et);
          if (mg != lv->type) { lv->type = mg; changed = 1; }
        }
        /* rest splat: *name gets array type */
        int rest_nid = nt_ref(nt, array_pat, "rest");
        if (rest_nid >= 0) {
          const char *rsty2 = nt_type(nt, rest_nid);
          int inner = -1;
          if (rsty2 && sp_streq(rsty2, "SplatNode"))
            inner = nt_ref(nt, rest_nid, "expression");
          if (inner >= 0 && nt_type(nt, inner) &&
              sp_streq(nt_type(nt, inner), "LocalVariableTargetNode")) {
            const char *rnm = nt_str(nt, inner, "name");
            LocalVar *lv = rnm ? scope_local(ms, rnm) : NULL;
            if (lv && !lv->is_param && !lv->is_block_param) {
              /* A poly/untyped VALUE scrutinee deconstructs to a boxed (poly)
                 array, so its rest slice is a poly array -- not an int array.
                 TY_INT_ARRAY reinterpreted sp_poly_slice's boxed poly array and
                 rendered garbage. Object scrutinees keep the legacy default. */
              TyKind rest_arr = ty_is_array(array_scrutinee) ? array_scrutinee
                              : ty_is_object(array_scrutinee) ? TY_INT_ARRAY : TY_POLY_ARRAY;
              TyKind mg = ty_unify(lv->type, rest_arr);
              if (mg != lv->type) { lv->type = mg; changed = 1; }
            }
          }
        }
      }
    }
  }
  return changed;
}

/* Widen each local `x` in a `x = @ivar` write to the ivar's (possibly
   just-widened) type, monotonically. Unlike infer_write_types this never resets
   a local, so it only lifts a local that reads a now-wider ivar and leaves every
   other local's carefully-derived type (pattern/massign/block bindings) intact
   -- the reconciliation the late ivar-widening fixpoint needs (#1793). */
int reconcile_locals_reading_ivars(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "LocalVariableWriteNode")) continue;
    int val_id = nt_ref(nt, id, "value");
    if (val_id < 0) continue;
    const char *vty = nt_type(nt, val_id);
    if (!vty || !sp_streq(vty, "InstanceVariableReadNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? scope_local(comp_scope_of(c, id), nm) : NULL;
    if (!lv || lv->is_param || lv->is_block_param || lv->rbs_seeded) continue;
    TyKind ivt = infer_type(c, val_id);
    if (ivt == TY_UNKNOWN) continue;
    TyKind m = ty_unify(lv->type, ivt);
    if (m != lv->type) { lv->type = m; changed = 1; }
  }
  return changed;
}

/* Element type contributed by a pushed value (see yield_aware_elem_ty). */
static TyKind push_elem_ty(Compiler *c, int node) { return yield_aware_elem_ty(c, node); }

int infer_write_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;

  /* Recompute non-param local types FRESH each iteration: reset to UNKNOWN
     (saving the old value), then unify all write-site RHS types. This lets
     a local NARROW as block-param/return inference improves, instead of
     monotonically widening to POLY from a stale early estimate. */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++) {
      LocalVar *lv = &c->scopes[s].locals[i];
      /* stash old type in gc_root (unused by codegen) so we can detect
         change; block params are typed elsewhere, so leave them alone */
      /* rbs_seeded: pinned externally (an RBS signature, a desugar-synthesized
         temp whose type IS its receiver's); the per-iteration reset must not
         wipe it -- users of such a temp can precede its own (late, synthesized)
         write in node order and would re-derive from UNKNOWN forever (#2723) */
      if (!lv->is_param && !lv->is_block_param && !lv->rbs_seeded) { lv->gc_root = (int)lv->type; lv->type = TY_UNKNOWN; }
    }
  /* Re-seed loop-growth bigint locals inside the recompute frame (the
     reset above would otherwise wipe the promotion each iteration). */
  infer_bigint_loop_locals(c);
  /* Seed pattern-bound case/in locals before the write-type loop reads
     them: `r = case x; in n then n; end` needs `n` typed before the
     result local unifies its arms, else it locks onto a stale estimate. */
  changed |= infer_case_pattern_locals(c);

  /* Index local-write nodes by (scope, name) for the usage-driven promotion
     scans further down (see the per-scope write-site lookups below). */
  LWIndex lw_ix;
  lw_index_build(c, &lw_ix);
  /* Index ivar-write nodes by name for the empty-hash / typed-write promotion
     guards on the InstanceVariableReadNode branches below. */
  LWIndex ivw_ix;
  ivw_index_build(c, &ivw_ix);

  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    const char *nm = NULL;
    TyKind newt = TY_UNKNOWN;
    if (sp_streq(ty, "LocalVariableWriteNode")) {
      nm = nt_str(nt, id, "name");
      int val_id = nt_ref(nt, id, "value");
      newt = infer_type(c, val_id);
      /* a `x = nil` write doesn't pin the type: flow it as TY_NIL so ty_unify
         can narrow it against an object write (NULL encodes nil); a purely-nil
         local is mapped to poly by a post-fixpoint backstop. */
      /* `x = y = nil` writes nil to every target; flow TY_NIL instead of the
         inner slot's unified type. */
      if (comp_nil_chain_bottom(nt, val_id) >= 0) newt = TY_NIL;
      /* Empty-collection literal `x = []` / `x = {}` returns TY_UNKNOWN from
         infer_type. If the container-fold from a prior iteration already gave
         this local a meaningful type (stored in gc_root), preserve it so that
         downstream uses like `x.map {...}` are not starved of type information. */
      if (newt == TY_UNKNOWN && nm) {
        const char *vty2 = nt_type(nt, val_id);
        int is_empty_col = vty2 && ((sp_streq(vty2, "ArrayNode") &&
          ({ int _n = 0; nt_arr(nt, val_id, "elements", &_n); _n; }) == 0) ||
          (sp_streq(vty2, "HashNode") &&
          ({ int _n2 = 0; nt_arr(nt, val_id, "elements", &_n2); _n2; }) == 0));
        if (is_empty_col) {
          Scope *s2 = comp_scope_of(c, id);
          LocalVar *lv2 = scope_local(s2, nm);
          if (lv2 && (TyKind)lv2->gc_root != TY_UNKNOWN) newt = (TyKind)lv2->gc_root;
        }
        /* `d = h.dup/clone`: inherit receiver's hash type from prior iteration */
        if (newt == TY_UNKNOWN) {
          const char *rvty2 = nt_type(nt, val_id);
          if (rvty2 && sp_streq(rvty2, "CallNode")) {
            const char *rvnm2 = nt_str(nt, val_id, "name");
            int rvrecv2 = nt_ref(nt, val_id, "receiver");
            if (rvrecv2 >= 0 && rvnm2 &&
                (sp_streq(rvnm2, "dup") || sp_streq(rvnm2, "clone"))) {
              const char *rrt2 = nt_type(nt, rvrecv2);
              if (rrt2 && sp_streq(rrt2, "LocalVariableReadNode")) {
                const char *rrn2 = nt_str(nt, rvrecv2, "name");
                LocalVar *rlv2 = rrn2 ? scope_local(comp_scope_of(c, rvrecv2), rrn2) : NULL;
                if (rlv2 && ty_is_hash((TyKind)rlv2->gc_root)) newt = (TyKind)rlv2->gc_root;
              }
            }
          }
        }
      }
    }
    else if (sp_streq(ty, "LocalVariableOperatorWriteNode")) {
      nm = nt_str(nt, id, "name");
      Scope *s = comp_scope_of(c, id);
      LocalVar *cur = nm ? scope_local(s, nm) : NULL;
      TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
      TyKind ct = cur ? (TyKind)cur->gc_root : TY_UNKNOWN; /* old type */
      if (ct == TY_STRING) newt = TY_STRING;
      else if (ty_is_numeric(ct) && ty_is_numeric(vt)) {
        if (ct == TY_FLOAT || vt == TY_FLOAT) newt = TY_FLOAT;
        else if (ct == TY_BIGINT || vt == TY_BIGINT) newt = TY_BIGINT;
        else newt = TY_INT;
      }
      else newt = ct;
    }
    else if (sp_streq(ty, "LocalVariableOrWriteNode") ||
             sp_streq(ty, "LocalVariableAndWriteNode")) {
      /* a ||= v / a &&= v : the variable can hold its prior value or v */
      nm = nt_str(nt, id, "name");
      Scope *s = comp_scope_of(c, id);
      LocalVar *cur = nm ? scope_local(s, nm) : NULL;
      TyKind ct = cur ? (TyKind)cur->gc_root : TY_UNKNOWN;
      newt = ty_unify(ct, infer_type(c, nt_ref(nt, id, "value")));
    }
    else if (sp_streq(ty, "MatchWriteNode")) {
      /* `/(?<n>..)/ =~ str` binds each named group to a local: a String when
         the group participated, nil otherwise (NULL-encoded), so type each
         target as a nilable String. */
      int tn = 0; const int *tv = nt_arr(nt, id, "targets", &tn);
      for (int ti = 0; ti < tn; ti++) {
        const char *tnm = nt_str(nt, tv[ti], "name");
        if (!tnm) continue;
        LocalVar *tlv = scope_local(comp_scope_of(c, tv[ti]), tnm);
        if (tlv && !tlv->is_param && !tlv->is_block_param) {
          /* see the note at the multi-write arm below: no `changed` for a
             plain local this pass reset. */
          tlv->type = ty_unify(tlv->type, TY_STRING);
        }
      }
      continue;
    }
    else {
      continue;
    }
    /* A void value assigned in value position (`v = always_raising_method`)
       is nil-ish: type the slot poly so it is declarable. The RHS call is
       emitted via emit_boxed, which evaluates it (it diverges) and yields nil. */
    if (newt == TY_VOID) newt = TY_POLY;
    if (!nm) continue;
    LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
    if (!lv || lv->is_block_param) continue;
    /* Params are typed from call sites (monotonic widen); a body assignment
       of a different type widens them too (e.g. `x = "s"` in an int param's
       body -> poly). Only widen -- never let an unknown RHS reset them. */
    if (lv->is_param) {
      if (newt != TY_UNKNOWN && !lv->rbs_seeded) {
        TyKind m2 = ty_unify(lv->type, newt);
        if (m2 != lv->type) { lv->type = m2; changed = 1; }
      }
      continue;
    }
    lv->type = ty_unify(lv->type, newt);
  }

  /* Second targeted pass for `x = recv.instance_eval/exec { ... }` (and
     trampoline calls): the call's value is the block's last expression, which
     may read a block-body local defined at a higher node id than this write.
     Those locals were just typed by the main loop above, so recompute here so
     `x` is not stranded at UNKNOWN by within-pass node ordering. */
  for (int id = 0; id < nt->count; id++) {
    if (!sp_streq(nt_type(nt, id) ? nt_type(nt, id) : "", "LocalVariableWriteNode")) continue;
    int val_id = nt_ref(nt, id, "value");
    if (val_id < 0 || !sp_streq(nt_type(nt, val_id) ? nt_type(nt, val_id) : "", "CallNode")) continue;
    if (nt_ref(nt, val_id, "block") < 0) continue;
    const char *vnm = nt_str(nt, val_id, "name");
    int vrecv = nt_ref(nt, val_id, "receiver");
    if (!vnm || vrecv < 0) continue;
    int is_ie = sp_streq(vnm, "instance_eval") || sp_streq(vnm, "instance_exec");
    if (!is_ie) {
      TyKind vrt = infer_type(c, vrecv);
      if (!ty_is_object(vrt) || !comp_trampoline_kind(c, ty_object_class(vrt), vnm, NULL)) continue;
    }
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? scope_local(comp_scope_of(c, id), nm) : NULL;
    if (!lv || lv->is_param || lv->is_block_param) continue;
    TyKind newt = infer_type(c, val_id);
    if (newt == TY_NIL) newt = TY_POLY;
    /* see the note at the multi-write arm below: no `changed` for a plain
       local this pass reset. */
    lv->type = ty_unify(lv->type, newt);
  }

  /* Multiple assignment `a, b = e0, e1`: each target gets its element's
     type (the RHS ArrayNode is a tuple here, not an array value). */
  for (int id = 0; id < nt->count; id++) {
    if (!sp_streq(nt_type(nt, id) ? nt_type(nt, id) : "", "MultiWriteNode")) continue;
    int ln = 0;
    const int *lefts = nt_arr(nt, id, "lefts", &ln);
    int value = nt_ref(nt, id, "value");
    const char *vty = nt_type(nt, value);
    /* `r, w = IO.pipe` -> both targets are IO handles. */
    if (ln == 2 && vty && sp_streq(vty, "CallNode") && nt_str(nt, value, "name") &&
        sp_streq(nt_str(nt, value, "name"), "pipe")) {
      int vrecv = nt_ref(nt, value, "receiver");
      if (vrecv >= 0 && nt_type(nt, vrecv) && sp_streq(nt_type(nt, vrecv), "ConstantReadNode") &&
          nt_str(nt, vrecv, "name") && sp_streq(nt_str(nt, vrecv, "name"), "IO")) {
        for (int i = 0; i < 2; i++) {
          if (!sp_streq(nt_type(nt, lefts[i]) ? nt_type(nt, lefts[i]) : "", "LocalVariableTargetNode")) continue;
          const char *lnm = nt_str(nt, lefts[i], "name");
          LocalVar *lv = lnm ? scope_local_intern(comp_scope_of(c, id), lnm) : NULL;
          if (lv && lv->type != TY_IO) { lv->type = TY_IO; changed = 1; }
        }
        continue;
      }
    }
    /* `x, y = obj.m` with a known multi-value return: element types flow to
       the targets (codegen unboxes each element from the tuple). */
    if (ln >= 2 && value >= 0 && nt_ref(nt, id, "rest") < 0) {
      int rn0 = 0;
      nt_arr(nt, id, "rights", &rn0);
      TyKind elems[16];
      int ecount = rn0 == 0 ? multi_return_elem_types(c, value, elems, 16) : 0;
      if (ecount == 0 && rn0 == 0) ecount = map_literal_elem_types(c, value, elems, 16);
      if (ecount == ln) {
        Scope *ms_mr = comp_scope_of(c, id);
        for (int i = 0; i < ln; i++) {
          const char *lty_mr = nt_type(nt, lefts[i]) ? nt_type(nt, lefts[i]) : "";
          if (sp_streq(lty_mr, "LocalVariableTargetNode")) {
            const char *lnm = nt_str(nt, lefts[i], "name");
            LocalVar *lv = lnm ? scope_local(ms_mr, lnm) : NULL;
            if (!lv || lv->is_param || lv->is_block_param) continue;
            /* No `changed` here: this pass reset every plain local to UNKNOWN at
   the top, so re-deriving one is not news -- reporting it would make
   the enclosing fixpoint see a change on every single iteration and
   never converge. The stash comparison at the end of this function is
   the change detector for these slots (same rule as slot_reset). */
            lv->type = ty_unify(lv->type, elems[i]);
          }
          else if (sp_streq(lty_mr, "InstanceVariableTargetNode") &&
                   ms_mr && ms_mr->class_id >= 0) {
            const char *ivnm = nt_str(nt, lefts[i], "name");
            int ivx = ivnm ? comp_ivar_index(&c->classes[ms_mr->class_id], ivnm) : -1;
            if (ivx < 0 || class_ivar_pinned(&c->classes[ms_mr->class_id], ivnm)) continue;
            TyKind mg = ty_unify(c->classes[ms_mr->class_id].ivar_types[ivx], elems[i]);
            if (mg != c->classes[ms_mr->class_id].ivar_types[ivx]) {
              c->classes[ms_mr->class_id].ivar_types[ivx] = mg; changed = 1;
            }
          }
          else if (sp_streq(lty_mr, "ConstantTargetNode")) {
            const char *cnm = nt_str(nt, lefts[i], "name");
            LocalVar *cv = cnm ? comp_const(c, cnm) : NULL;
            if (!cv) continue;
            /* SET, not unify: an early fixpoint round can guess a nested
               element as poly-array before the block body settles; a later
               round must be able to correct it (constants persist across
               rounds, unlike locals). Same convention as
               infer_multiwrite_const_types. */
            if (cv->type != elems[i]) { cv->type = elems[i]; changed = 1; }
          }
        }
        continue;  /* the generic poly-tuple widening below must not re-widen */
      }
    }
    if (!vty || !sp_streq(vty, "ArrayNode")) {
      /* scalar RHS (`a, b = 1`): the first target gets the scalar, the rest
         their slot default. Type every target as the scalar's kind. Array /
         hash RHS would splat and is handled elsewhere, so skip those. */
      int multi_src = vty && (sp_streq(vty, "CallNode") || sp_streq(vty, "SuperNode") ||
                              sp_streq(vty, "ForwardingSuperNode") || sp_streq(vty, "YieldNode"));
      if (vty && value >= 0 && !multi_src) {
        TyKind st = infer_type(c, value);
        if (st != TY_UNKNOWN && st != TY_NIL && !ty_is_array(st) && !ty_is_hash(st)) {
          for (int i = 0; i < ln; i++) {
            if (!sp_streq(nt_type(nt, lefts[i]) ? nt_type(nt, lefts[i]) : "", "LocalVariableTargetNode")) continue;
            const char *lnm = nt_str(nt, lefts[i], "name");
            LocalVar *lv = lnm ? scope_local(comp_scope_of(c, id), lnm) : NULL;
            if (!lv || lv->is_param || lv->is_block_param) continue;
            lv->type = ty_unify(lv->type, st);
          }
          /* the rest target under a scalar RHS collects [scalar] (or stays
             empty when fixed targets consumed it): an ARRAY of the scalar. */
          int rest_ms = nt_ref(nt, id, "rest");
          if (rest_ms >= 0 && nt_type(nt, rest_ms) && sp_streq(nt_type(nt, rest_ms), "SplatNode")) {
            int rin_ms = nt_ref(nt, rest_ms, "expression");
            if (rin_ms >= 0 && nt_type(nt, rin_ms) &&
                sp_streq(nt_type(nt, rin_ms), "LocalVariableTargetNode")) {
              const char *rnm_ms = nt_str(nt, rin_ms, "name");
              LocalVar *rlv_ms = rnm_ms ? scope_local(comp_scope_of(c, id), rnm_ms) : NULL;
              if (rlv_ms && !rlv_ms->is_param && !rlv_ms->is_block_param) {
                TyKind rat = ty_array_of(st);
                if (rat == TY_UNKNOWN) rat = TY_POLY_ARRAY;
                TyKind mg_r = ty_unify(rlv_ms->type, rat);
                if (mg_r != rlv_ms->type) { rlv_ms->type = mg_r; changed = 1; }
              }
            }
          }
        }
      }
      /* any expression returning a typed array: assign element types to targets */
      if (value >= 0) {
        TyKind st = infer_type(c, value);
        /* poly RHS: destructure gives poly elements */
        if (st == TY_POLY || st == TY_POLY_ARRAY) {
          Scope *ms_poly = comp_scope_of(c, id);
          for (int i = 0; i < ln; i++) {
            const char *lty_p = nt_type(nt, lefts[i]) ? nt_type(nt, lefts[i]) : "";
            if (sp_streq(lty_p, "LocalVariableTargetNode")) {
              const char *lnm_p = nt_str(nt, lefts[i], "name");
              LocalVar *lv_p = lnm_p ? scope_local(ms_poly, lnm_p) : NULL;
              if (!lv_p || lv_p->is_param || lv_p->is_block_param) continue;
              TyKind mg_p = ty_unify(lv_p->type, TY_POLY);
              /* plain locals are reset+recomputed each iteration; net change
                 is detected by the end-of-pass stash compare. Reporting the
                 re-derivation here reads as change every iteration and the
                 fixpoint never converges. Only a non-reset (pinned) slot's
                 transition is a real change. */
              if (mg_p != lv_p->type) { lv_p->type = mg_p; if (lv_p->rbs_seeded) changed = 1; }
            }
          }
        }
        if (ty_is_array(st)) {
          TyKind elem = ty_array_elem(st);
          int rn2 = 0;
          const int *rights2 = nt_arr(nt, id, "rights", &rn2);
          Scope *ms_arr = comp_scope_of(c, id);
          for (int i = 0; i < ln; i++) {
            const char *lty_ms = nt_type(nt, lefts[i]) ? nt_type(nt, lefts[i]) : "";
            if (sp_streq(lty_ms, "LocalVariableTargetNode")) {
              const char *lnm = nt_str(nt, lefts[i], "name");
              LocalVar *lv = lnm ? scope_local(ms_arr, lnm) : NULL;
              if (!lv || lv->is_param || lv->is_block_param) continue;
              lv->type = ty_unify(lv->type, elem);
            }
            else if (sp_streq(lty_ms, "InstanceVariableTargetNode") &&
                     ms_arr && ms_arr->class_id >= 0) {
              const char *ivnm = nt_str(nt, lefts[i], "name");
              int iv_ms = ivnm ? comp_ivar_index(&c->classes[ms_arr->class_id], ivnm) : -1;
              if (iv_ms < 0 || class_ivar_pinned(&c->classes[ms_arr->class_id], ivnm)) continue;
              TyKind mg = ty_unify(c->classes[ms_arr->class_id].ivar_types[iv_ms], elem);
              if (mg != c->classes[ms_arr->class_id].ivar_types[iv_ms]) {
                c->classes[ms_arr->class_id].ivar_types[iv_ms] = mg; changed = 1;
              }
            }
            else if (sp_streq(lty_ms, "ConstantTargetNode")) {
              const char *cnm_ms = nt_str(nt, lefts[i], "name");
              LocalVar *cv_ms = cnm_ms ? comp_const(c, cnm_ms) : NULL;
              if (!cv_ms) continue;
              TyKind mg_ms = ty_unify(cv_ms->type, elem);
              if (mg_ms != cv_ms->type) { cv_ms->type = mg_ms; changed = 1; }
            }
          }
          for (int j = 0; j < rn2; j++) {
            const char *lty_ms = nt_type(nt, rights2[j]) ? nt_type(nt, rights2[j]) : "";
            if (sp_streq(lty_ms, "LocalVariableTargetNode")) {
              const char *rnm2 = nt_str(nt, rights2[j], "name");
              LocalVar *lv = rnm2 ? scope_local(ms_arr, rnm2) : NULL;
              if (!lv || lv->is_param || lv->is_block_param) continue;
              lv->type = ty_unify(lv->type, elem);
            }
            else if (sp_streq(lty_ms, "InstanceVariableTargetNode") &&
                     ms_arr && ms_arr->class_id >= 0) {
              const char *ivnm2 = nt_str(nt, rights2[j], "name");
              int iv_ms2 = ivnm2 ? comp_ivar_index(&c->classes[ms_arr->class_id], ivnm2) : -1;
              if (iv_ms2 < 0 || class_ivar_pinned(&c->classes[ms_arr->class_id], ivnm2)) continue;
              TyKind mg2 = ty_unify(c->classes[ms_arr->class_id].ivar_types[iv_ms2], elem);
              if (mg2 != c->classes[ms_arr->class_id].ivar_types[iv_ms2]) {
                c->classes[ms_arr->class_id].ivar_types[iv_ms2] = mg2; changed = 1;
              }
            }
            else if (sp_streq(lty_ms, "ConstantTargetNode")) {
              const char *cnm_ms2 = nt_str(nt, rights2[j], "name");
              LocalVar *cv_ms2 = cnm_ms2 ? comp_const(c, cnm_ms2) : NULL;
              if (!cv_ms2) continue;
              TyKind mg_ms2 = ty_unify(cv_ms2->type, elem);
              if (mg_ms2 != cv_ms2->type) { cv_ms2->type = mg_ms2; changed = 1; }
            }
          }
          int rest_nid2 = nt_ref(nt, id, "rest");
          if (rest_nid2 >= 0) {
            const char *rsty2 = nt_type(nt, rest_nid2);
            int inner2 = -1;
            if (rsty2 && sp_streq(rsty2, "SplatNode"))
              inner2 = nt_ref(nt, rest_nid2, "expression");
            if (inner2 >= 0 && nt_type(nt, inner2) &&
                sp_streq(nt_type(nt, inner2), "LocalVariableTargetNode")) {
              const char *rnm3 = nt_str(nt, inner2, "name");
              LocalVar *lv3 = rnm3 ? scope_local(comp_scope_of(c, id), rnm3) : NULL;
              if (lv3 && !lv3->is_param && !lv3->is_block_param)
                lv3->type = ty_unify(lv3->type, st);
            }
          }
        }
      }
      continue;
    }
    int en = 0;
    const int *els = nt_arr(nt, value, "elements", &en);
    for (int i = 0; i < ln && i < en; i++) {
      const char *lty = nt_type(nt, lefts[i]);
      if (!lty) continue;
      if (sp_streq(lty, "LocalVariableTargetNode")) {
        const char *lnm = nt_str(nt, lefts[i], "name");
        TyKind et = infer_type(c, els[i]);
        if (et == TY_NIL) continue;
        LocalVar *lv = lnm ? scope_local(comp_scope_of(c, id), lnm) : NULL;
        if (!lv || lv->is_param || lv->is_block_param) continue;
        lv->type = ty_unify(lv->type, et);
      }
      else if (sp_streq(lty, "ConstantTargetNode")) {
        const char *cnm = nt_str(nt, lefts[i], "name");
        LocalVar *cv = cnm ? comp_const(c, cnm) : NULL;
        if (!cv) continue;
        TyKind et = infer_type(c, els[i]);
        if (et == TY_NIL) continue;
        TyKind mg = ty_unify(cv->type, et);
        if (mg != cv->type) { cv->type = mg; changed = 1; }
      }
      else if (sp_streq(lty, "InstanceVariableTargetNode")) {
        Scope *iv_sc = comp_scope_of(c, id);
        int iv_cid = iv_sc ? iv_sc->class_id : -1;
        if (iv_cid < 0) continue;
        const char *ivnm = nt_str(nt, lefts[i], "name");
        int iv_idx = ivnm ? comp_ivar_index(&c->classes[iv_cid], ivnm) : -1;
        if (iv_idx < 0 || class_ivar_pinned(&c->classes[iv_cid], ivnm)) continue;
        TyKind et = infer_type(c, els[i]);
        if (et == TY_NIL) continue;
        TyKind mg = ty_unify(c->classes[iv_cid].ivar_types[iv_idx], et);
        if (mg != c->classes[iv_cid].ivar_types[iv_idx]) {
          c->classes[iv_cid].ivar_types[iv_idx] = mg; changed = 1;
        }
      }
      else if (sp_streq(lty, "MultiTargetNode")) {
        /* (b, c) nested target: inner RHS must be an ArrayNode literal */
        const char *ety = nt_type(nt, els[i]);
        if (!ety || !sp_streq(ety, "ArrayNode")) continue;
        int inn = 0;
        const int *inner_els = nt_arr(nt, els[i], "elements", &inn);
        int inn2 = 0;
        const int *inner_lefts = nt_arr(nt, lefts[i], "lefts", &inn2);
        for (int j = 0; j < inn2 && j < inn; j++) {
          const char *ilty = nt_type(nt, inner_lefts[j]);
          if (!ilty || !sp_streq(ilty, "LocalVariableTargetNode")) continue;
          const char *lnm2 = nt_str(nt, inner_lefts[j], "name");
          TyKind et2 = infer_type(c, inner_els[j]);
          if (et2 == TY_NIL) continue;
          LocalVar *lv2 = lnm2 ? scope_local(comp_scope_of(c, id), lnm2) : NULL;
          if (!lv2 || lv2->is_param || lv2->is_block_param) continue;
          lv2->type = ty_unify(lv2->type, et2);
        }
      }
    }
    /* Under-filled literal RHS (`a, b, c = [1, 2]`): targets past the supplied
       elements land nil, so widen those locals to poly like a plain `x = nil`. */
    Scope *usc = comp_scope_of(c, id);
    for (int i = en; i < ln; i++) {
      const char *lty = nt_type(nt, lefts[i]);
      if (!lty || !sp_streq(lty, "LocalVariableTargetNode")) continue;
      const char *lnm = nt_str(nt, lefts[i], "name");
      LocalVar *lv = lnm ? scope_local(usc, lnm) : NULL;
      if (!lv || lv->is_param || lv->is_block_param) continue;
      TyKind mg = ty_unify(lv->type, TY_POLY);
      if (mg != lv->type) { lv->type = mg; changed = 1; }
    }
    /* rights targets (post-splat fixed targets) */
    int rn = 0;
    const int *rights = nt_arr(nt, id, "rights", &rn);
    int blen_r = en - ln - rn; if (blen_r < 0) blen_r = 0;
    for (int j = 0; j < rn; j++) {
      int ridx = ln + blen_r + j;
      const char *rty3 = nt_type(nt, rights[j]);
      if (!rty3) continue;
      TyKind et;
      if (ridx >= en) {
        /* Underflow (`a, *b, c = [1]`): this post-splat target lands nil, so
           widen it to poly rather than typing it from a reused leading element. */
        et = TY_POLY;
      }
      else {
        et = infer_type(c, els[ridx]);
        if (et == TY_NIL) continue;
      }
      if (sp_streq(rty3, "LocalVariableTargetNode")) {
        const char *rnm2 = nt_str(nt, rights[j], "name");
        LocalVar *lv = rnm2 ? scope_local(comp_scope_of(c, id), rnm2) : NULL;
        if (!lv || lv->is_param || lv->is_block_param) continue;
        lv->type = ty_unify(lv->type, et);
      }
      else if (sp_streq(rty3, "ConstantTargetNode")) {
        const char *cnm2 = nt_str(nt, rights[j], "name");
        LocalVar *cv2 = cnm2 ? comp_const(c, cnm2) : NULL;
        if (!cv2) continue;
        TyKind mg3 = ty_unify(cv2->type, et);
        if (mg3 != cv2->type) { cv2->type = mg3; changed = 1; }
      }
      else if (sp_streq(rty3, "InstanceVariableTargetNode")) {
        Scope *iv_sc3 = comp_scope_of(c, id);
        int iv_cid3 = iv_sc3 ? iv_sc3->class_id : -1;
        if (iv_cid3 < 0) continue;
        const char *ivnm3 = nt_str(nt, rights[j], "name");
        int iv_idx3 = ivnm3 ? comp_ivar_index(&c->classes[iv_cid3], ivnm3) : -1;
        if (iv_idx3 < 0 || class_ivar_pinned(&c->classes[iv_cid3], ivnm3)) continue;
        TyKind mg4 = ty_unify(c->classes[iv_cid3].ivar_types[iv_idx3], et);
        if (mg4 != c->classes[iv_cid3].ivar_types[iv_idx3]) {
          c->classes[iv_cid3].ivar_types[iv_idx3] = mg4; changed = 1;
        }
      }
    }
    /* rest (splat) target: elements [ln, en-rn) become a typed array */
    int rest_nid = nt_ref(nt, id, "rest");
    if (rest_nid >= 0) {
      const char *rsty = nt_type(nt, rest_nid);
      int inner = -1;
      if (rsty && sp_streq(rsty, "SplatNode"))
        inner = nt_ref(nt, rest_nid, "expression");
      if (inner >= 0 && nt_type(nt, inner) &&
          sp_streq(nt_type(nt, inner), "LocalVariableTargetNode")) {
        const char *rnm = nt_str(nt, inner, "name");
        int rstart = ln, rend = en - rn;
        if (rend < rstart) rend = rstart;
        TyKind rest_elem = TY_UNKNOWN;
        for (int i = rstart; i < rend; i++)
          rest_elem = ty_unify(rest_elem, infer_type(c, els[i]));
        TyKind rest_arr = (rest_elem != TY_UNKNOWN) ? ty_array_of(rest_elem) : TY_INT_ARRAY;
        LocalVar *lv = rnm ? scope_local(comp_scope_of(c, id), rnm) : NULL;
        if (lv && !lv->is_param && !lv->is_block_param)
          lv->type = ty_unify(lv->type, rest_arr);
      }
    }
  }

  /* MatchRequiredNode: `value => pattern` — infer locals from pattern shape. */
  NT_FOREACH_KIND(nt, NK_MatchRequiredNode, id) {
    int value = nt_ref(nt, id, "value");
    int pattern = nt_ref(nt, id, "pattern");
    if (value < 0 || pattern < 0) continue;
    const char *pty = nt_type(nt, pattern);
    if (!pty) continue;
    Scope *ms = comp_scope_of(c, id);
    if (sp_streq(pty, "ArrayPatternNode")) {
      int rn = 0;
      const int *reqs = nt_arr(nt, pattern, "requireds", &rn);
      /* Try to get types from a literal ArrayNode value. */
      const char *vty = nt_type(nt, value);
      int en = 0;
      const int *els = (vty && sp_streq(vty, "ArrayNode")) ? nt_arr(nt, value, "elements", &en) : NULL;
      TyKind arr_elem = TY_UNKNOWN;
      TyKind vinf_a = infer_type(c, value);
      if (ty_is_array(vinf_a)) arr_elem = ty_array_elem(vinf_a);
      else if (ty_is_object(vinf_a) && c->classes[ty_object_class(vinf_a)].is_struct)
        arr_elem = TY_POLY;   /* Struct/Data #deconstruct boxes members to poly */
      int pon = 0;
      const int *posts = nt_arr(nt, pattern, "posts", &pon);
      for (int i = 0; i < rn + pon; i++) {
        int tnode = i < rn ? reqs[i] : posts[i - rn];
        const char *lty2 = nt_type(nt, tnode);
        if (!lty2 || !sp_streq(lty2, "LocalVariableTargetNode")) continue;
        const char *lnm = nt_str(nt, tnode, "name");
        LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
        if (!lv || lv->is_param || lv->is_block_param) continue;
        TyKind et = (els && i < rn && i < en) ? infer_type(c, els[i]) : arr_elem;
        if (et == TY_UNKNOWN || et == TY_NIL) continue;
        TyKind mg = ty_unify(lv->type, et);
        if (mg != lv->type) { lv->type = mg; changed = 1; }
      }
      /* `*mid` binds the middle slice as the scrutinee's array kind */
      int ap_rest = nt_ref(nt, pattern, "rest");
      if (ap_rest >= 0 && nt_type(nt, ap_rest) &&
          sp_streq(nt_type(nt, ap_rest), "SplatNode")) {
        int rex = nt_ref(nt, ap_rest, "expression");
        if (rex >= 0 && nt_type(nt, rex) &&
            sp_streq(nt_type(nt, rex), "LocalVariableTargetNode")) {
          const char *rnm = nt_str(nt, rex, "name");
          LocalVar *rlv = rnm ? scope_local(ms, rnm) : NULL;
          TyKind at = ty_is_array(vinf_a) ? vinf_a
                    : (arr_elem == TY_POLY ? TY_POLY_ARRAY : TY_UNKNOWN);
          if (rlv && !rlv->is_param && !rlv->is_block_param && at != TY_UNKNOWN) {
            TyKind mg = ty_unify(rlv->type, at);
            if (mg != rlv->type) { rlv->type = mg; changed = 1; }
          }
        }
      }
    }
    else if (sp_streq(pty, "HashPatternNode")) {
      int pn = 0;
      const int *pelms = nt_arr(nt, pattern, "elements", &pn);
      /* Try to match keys from a literal HashNode value. */
      const char *vty = nt_type(nt, value);
      int vn = 0;
      const int *velms = (vty && sp_streq(vty, "HashNode")) ? nt_arr(nt, value, "elements", &vn) : NULL;
      for (int i = 0; i < pn; i++) {
        const char *ety = nt_type(nt, pelms[i]);
        if (!ety || !sp_streq(ety, "AssocNode")) continue;
        int pkey = nt_ref(nt, pelms[i], "key");
        int ptgt = nt_ref(nt, pelms[i], "value");
        if (ptgt < 0) continue;
        const char *tty = nt_type(nt, ptgt);
        if (!tty || !sp_streq(tty, "LocalVariableTargetNode")) continue;
        const char *lnm = nt_str(nt, ptgt, "name");
        LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
        if (!lv || lv->is_param || lv->is_block_param) continue;
        /* find matching key in value hash */
        const char *pkey_val = (pkey >= 0 && nt_type(nt, pkey) &&
          sp_streq(nt_type(nt, pkey), "SymbolNode")) ? nt_str(nt, pkey, "value") : NULL;
        TyKind et = TY_UNKNOWN;
        if (pkey_val && velms) {
          for (int j = 0; j < vn; j++) {
            int vkey = nt_ref(nt, velms[j], "key");
            const char *vkty = vkey >= 0 ? nt_type(nt, vkey) : NULL;
            const char *vkval = (vkty && sp_streq(vkty, "SymbolNode")) ? nt_str(nt, vkey, "value") : NULL;
            if (vkval && sp_streq(vkval, pkey_val)) { et = infer_type(c, nt_ref(nt, velms[j], "value")); break; }
          }
        }
        if (et == TY_UNKNOWN) {
          /* Struct/Data #deconstruct_keys yields a poly-valued hash. */
          TyKind vinf_h = infer_type(c, value);
          if (ty_is_object(vinf_h) && c->classes[ty_object_class(vinf_h)].is_struct) et = TY_POLY;
        }
        if (et == TY_UNKNOWN || et == TY_NIL) continue;
        TyKind mg = ty_unify(lv->type, et);
        if (mg != lv->type) { lv->type = mg; changed = 1; }
      }
      /* `**rest` binds the leftover pairs as a hash of the scrutinee's variant */
      int hp_rest = nt_ref(nt, pattern, "rest");
      if (hp_rest >= 0 && nt_type(nt, hp_rest) &&
          sp_streq(nt_type(nt, hp_rest), "AssocSplatNode")) {
        int rin = nt_ref(nt, hp_rest, "value");
        if (rin >= 0 && nt_type(nt, rin) &&
            sp_streq(nt_type(nt, rin), "LocalVariableTargetNode")) {
          const char *rnm = nt_str(nt, rin, "name");
          LocalVar *rlv = rnm ? scope_local(ms, rnm) : NULL;
          TyKind ht = infer_type(c, value);
          if (rlv && !rlv->is_param && !rlv->is_block_param && ty_is_hash(ht)) {
            TyKind mg = ty_unify(rlv->type, ht);
            if (mg != rlv->type) { rlv->type = mg; changed = 1; }
          }
        }
      }
    }
  }

  changed |= infer_case_pattern_locals(c);

  /* case/when with a lambda predicate: Ruby dispatches `pattern === x` and
     Proc#=== calls the lambda with the scrutinee, so the lambda's parameter
     takes the predicate's type (#2439). */
  for (int id = 0; id < nt->count; id++) {
    const char *cty9 = nt_type(nt, id);
    if (!cty9 || !sp_streq(cty9, "CaseNode")) continue;
    int cpred = nt_ref(nt, id, "predicate");
    if (cpred < 0) continue;
    TyKind cpt = infer_type(c, cpred);
    if (cpt == TY_UNKNOWN || cpt == TY_VOID || cpt == TY_NIL) continue;
    int cnw = 0; const int *cwhens = nt_arr(nt, id, "conditions", &cnw);
    for (int w = 0; w < cnw; w++) {
      int wc = 0; const int *conds = nt_arr(nt, cwhens[w], "conditions", &wc);
      for (int j = 0; j < wc; j++) {
        if (!nt_type(nt, conds[j]) || !sp_streq(nt_type(nt, conds[j]), "LambdaNode")) continue;
        int bp = nt_ref(nt, conds[j], "parameters");
        int binner = bp >= 0 ? nt_ref(nt, bp, "parameters") : -1;
        int pn = binner >= 0 ? binner : bp;
        if (pn < 0) continue;
        int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
        if (rn < 1) continue;
        const char *pnm = nt_str(nt, reqs[0], "name");
        Scope *lsc = pnm ? comp_scope_of(c, conds[j]) : NULL;
        LocalVar *plv = lsc ? scope_local(lsc, pnm) : NULL;
        if (plv && plv->type != cpt) { plv->type = cpt; changed = 1; }
      }
    }
  }

  /* Fold container usage into the local type so an empty `[]` / `{}` gets
     its element / key+value type from how it is filled. `a << x` /
     `a.push(x)` / `a[i] = x` (int key) -> array; `h[k] = v` / `h[k] op= v`
     (string key) -> hash. Part of the recompute frame so it survives reset. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    int recv, kt = TY_UNKNOWN, vt = TY_UNKNOWN, is_push = 0, is_idx_write = 0, is_splice = 0;
    if (sp_streq(ty, "CallNode")) {
      recv = nt_ref(nt, id, "receiver");
      const char *name = nt_str(nt, id, "name");
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (name && (((sp_streq(name, "push") || sp_streq(name, "append")) && an >= 1) ||
                   (sp_streq(name, "<<") && an == 1))) {
        /* `<<` is ambiguous (Array#push vs Integer#<< shift): a numeric-assigned
           receiver is a shift, so don't promote its slot to an array. push/append
           take any number of arguments; every one of them is element evidence. */
        if (sp_streq(name, "<<") && recv_has_scalar_numeric_write(c, recv)) continue;
        is_push = 1; vt = push_elem_ty(c, argv[0]);
        for (int ai = 1; ai < an; ai++) vt = ty_unify(vt, push_elem_ty(c, argv[ai]));
      }
      else if (name && (sp_streq(name, "unshift") || sp_streq(name, "prepend")) && an >= 1) {
        /* unshift(v, ...): every argument is element evidence, like push
           (a foreign value used to store its raw bits into the typed slots) */
        is_push = 1; vt = push_elem_ty(c, argv[0]);
        for (int ai = 1; ai < an; ai++) vt = ty_unify(vt, push_elem_ty(c, argv[ai]));
      }
      else if (name && sp_streq(name, "insert") && an >= 2) {
        /* insert(i, v, ...): the values from position 1 on are evidence */
        is_push = 1; vt = push_elem_ty(c, argv[1]);
        for (int ai = 2; ai < an; ai++) vt = ty_unify(vt, push_elem_ty(c, argv[ai]));
      }
      else if (name && sp_streq(name, "concat") && an == 1) {
        /* concat(other): the other array's elements splice in */
        is_push = 1; vt = splice_incoming_elem(c, argv[0]);
      }
      else if (name && sp_streq(name, "default_proc=") && an == 1) {
        /* installing a default proc needs the poly-valued variant (the proc
           can return any value, and only those variants carry the dproc
           slot): widen a typed-value hash local accordingly (#2371). */
        if (recv < 0) continue;
        const char *dpty = nt_type(nt, recv);
        if (!dpty || !sp_streq(dpty, "LocalVariableReadNode")) continue;
        const char *dpnm = nt_str(nt, recv, "name");
        Scope *dpsc = dpnm ? comp_scope_of(c, recv) : NULL;
        LocalVar *dplv = dpsc ? scope_local(dpsc, dpnm) : NULL;
        if (dplv && !dplv->is_param && !dplv->is_block_param && ty_is_hash(dplv->type)) {
          TyKind kt2 = ty_hash_key(dplv->type);
          TyKind want2 = kt2 == TY_SYMBOL ? TY_SYM_POLY_HASH
                       : kt2 == TY_STRING ? TY_STR_POLY_HASH : TY_POLY_POLY_HASH;
          if (dplv->type != want2) { dplv->type = want2; changed = 1; }
        }
        continue;
      }
      else if (name && sp_streq(name, "replace") && an == 1) {
        /* replace(other) splices the WHOLE other container in: a hash local
           must be able to hold other's variant (transform_keys! desugars to
           replace and may change the key type). Differing variants widen the
           local to the universally-boxed PolyPoly hash. */
        TyKind ot = infer_type(c, argv[0]);
        if (recv < 0 || !ty_is_hash(ot)) continue;
        const char *rpty = nt_type(nt, recv);
        if (!rpty || !sp_streq(rpty, "LocalVariableReadNode")) continue;
        const char *rpnm = nt_str(nt, recv, "name");
        Scope *rpsc = rpnm ? comp_scope_of(c, recv) : NULL;
        LocalVar *rplv = rpsc ? scope_local(rpsc, rpnm) : NULL;
        if (rplv && !rplv->is_param && !rplv->is_block_param &&
            ty_is_hash(rplv->type) && rplv->type != ot &&
            rplv->type != TY_POLY_POLY_HASH) {
          rplv->type = TY_POLY_POLY_HASH;
          changed = 1;
        }
        continue;
      }
      else if (name && sp_streq(name, "[]=") && an == 2) {
        is_idx_write = 1; kt = infer_type(c, argv[0]); vt = infer_type(c, argv[1]);
        /* a range key is a splice: the RHS contributes element evidence */
        if (kt == TY_RANGE) { is_splice = 1; vt = splice_incoming_elem(c, argv[1]); }
        /* an empty [] / {} literal value carries no element type but is
           definite container evidence: treat it as poly so an int-keyed
           write can still settle the hash variant (`h = {}; h[k] = []`
           stayed Str-keyed and stored the int key as a pointer, #2442) */
        else if (vt == TY_UNKNOWN && nt_type(nt, argv[1]) &&
                 (sp_streq(nt_type(nt, argv[1]), "ArrayNode") ||
                  sp_streq(nt_type(nt, argv[1]), "HashNode"))) {
          int ven9 = 0; nt_arr(nt, argv[1], "elements", &ven9);
          if (ven9 == 0) vt = TY_POLY;
        }
      }
      else if (name && sp_streq(name, "store") && an == 2) {
        /* Hash#store is []= (#2433) */
        is_idx_write = 1; kt = infer_type(c, argv[0]); vt = infer_type(c, argv[1]);
      }
      else if (name && (sp_streq(name, "merge!") || sp_streq(name, "update")) && an >= 1) {
        /* merging hashes into an empty-{} local writes their keys/values:
           key+value evidence exactly like []= (#2434) */
        TyKind mat = infer_type(c, argv[0]);
        if (!ty_is_hash(mat)) continue;
        is_idx_write = 1; kt = ty_hash_key(mat); vt = ty_hash_val(mat);
        for (int ai = 1; ai < an; ai++) {
          TyKind mai = infer_type(c, argv[ai]);
          if (!ty_is_hash(mai)) { is_idx_write = 0; break; }
          kt = ty_unify(kt, ty_hash_key(mai));
          vt = ty_unify(vt, ty_hash_val(mai));
        }
        if (!is_idx_write) continue;
      }
      else if (name && sp_streq(name, "[]=") && an == 3) {
        /* a[start, len] = rhs: a splice over the (start, len) span */
        is_idx_write = 1; is_splice = 1; vt = splice_incoming_elem(c, argv[2]);
      }
      else if (name && sp_streq(name, "fill") && an >= 1 && an <= 3 &&
               nt_ref(nt, id, "block") < 0) {
        /* arr.fill(v[, start[, len]]) writes v into every element slot of the
           span: the value is element evidence exactly like a splice, so a typed
           array whose elements cannot hold the value widens to a poly array
           (previously the raw bits were stored: [1,2,3].fill(:a) filled the int
           array with the symbol id). The block form fill([start[, len]]) { |i| }
           carries no value argument and is not handled here. */
        is_idx_write = 1; is_splice = 1; vt = infer_type(c, argv[0]);
        kt = TY_INT;  /* a positional span, never hash evidence */
      }
      else if (name && (sp_streq(name, "fetch") ||
                        (sp_streq(name, "[]") && an == 1)) && an >= 1) {
        /* hash.fetch(key,..) / hash[key]: promote TY_UNKNOWN local to a typed hash.
           Only fires when the slot is currently TY_UNKNOWN (empty hash).
           A 2-arg [] is a string/array slice, never a hash read — only the
           1-arg form is key-lookup evidence (fetch keeps >=1: (key, default)). */
        TyKind rslot = TY_UNKNOWN;
        const char *rrty = nt_type(nt, recv);
        const char *rnm2 = NULL;
        if (rrty && sp_streq(rrty, "LocalVariableReadNode")) {
          rnm2 = nt_str(nt, recv, "name");
          LocalVar *lv2 = rnm2 ? scope_local(comp_scope_of(c, recv), rnm2) : NULL;
          if (lv2) rslot = lv2->type;
        }
        else if (rrty && sp_streq(rrty, "InstanceVariableReadNode")) {
          /* an already-typed ivar hash must not be re-promoted: unifying e.g.
             a str_str_hash with the promotion's str_poly target would widen the
             slot to poly. Only an untyped (empty-{}) ivar promotes here. */
          rslot = infer_type(c, recv);
          /* Fixpoint-ordering hazard: a param-fed ivar (`@s = s`) reads
             UNKNOWN before the param's call-site type arrives, and a read
             like `@s[i]` would mis-promote it to a hash. Promote from reads
             only when every assignment to the ivar is an empty `{}` literal
             (the actual empty-hash case) — a syntactic test that is stable
             across fixpoint iterations. */
          if (rslot == TY_UNKNOWN) {
            const char *pin = nt_str(nt, recv, "name");
            int blocked = 0;
            for (int _r = ivw_index_first(&ivw_ix, pin); _r >= 0 && !blocked; _r = ivw_ix.next[_r]) {
              int wi = ivw_ix.node[_r];
              const char *wnm = nt_str(nt, wi, "name");
              if (!wnm || !pin || !sp_streq(wnm, pin)) continue;
              int wv = nt_ref(nt, wi, "value");
              const char *wvty = wv >= 0 ? nt_type(nt, wv) : NULL;
              int is_empty_hash = 0;
              if (wvty && sp_streq(wvty, "HashNode")) {
                int hn = 0; nt_arr(nt, wv, "elements", &hn);
                if (hn == 0) is_empty_hash = 1;
              }
              if (!is_empty_hash) blocked = 1;
            }
            if (blocked) continue;
          }
        }
        if (rslot != TY_UNKNOWN) continue;  /* already typed, skip */
        /* Only promote via [] read if the receiver local has at least one
           write site in its scope. Pure block params have no write site and
           get their type from infer_block_params; promoting them here to
           TY_STR_POLY_HASH before is_block_param is set creates a TY_POLY
           that ty_unify can never narrow back to the yield arg type. */
        if (rrty && sp_streq(rrty, "LocalVariableReadNode") && rnm2) {
          Scope *recv_scope = comp_scope_of(c, recv);
          int recv_sid = (int)(recv_scope - c->scopes);
          int has_write = 0;
          for (int _r = lw_index_first(&lw_ix, rnm2, recv_sid); _r >= 0 && !has_write; _r = lw_ix.next[_r]) {
            int _wi = lw_ix.node[_r];
            if (comp_scope_of(c, _wi) != recv_scope) continue;
            const char *_wnm = nt_str(nt, _wi, "name");
            if (_wnm && sp_streq(_wnm, rnm2)) has_write = 1;
          }
          if (!has_write) continue;
        }
        kt = infer_type(c, argv[0]);
        if (kt == TY_SYMBOL) { vt = TY_INT; /* dummy: sym hash val is always poly */ }
        else if (kt == TY_STRING) {
          /* Seed the value type from the hash's `[]=` writes so an int-valued
             string-keyed hash filled by `@h[s] = int` stays str_int_hash
             instead of widening to str_poly (which never narrows back). */
          TyKind wv = aset_value_type(c, recv);
          vt = (wv == TY_INT || wv == TY_STRING) ? wv : TY_POLY;
        }
        /* An int-key bare read (`x[i]`) is NOT strong hash evidence: arrays
           index by int too, and an array-returning method assigned to `x` may
           not have settled its element type yet, so promoting here would lock
           the slot to a hash before the array write is recognized. A genuine
           int-keyed hash is typed by its `[]=` writes or literal instead. */
        else continue;
      }
      else continue;
    }
    else if (sp_streq(ty, "IndexOperatorWriteNode") ||
             sp_streq(ty, "IndexOrWriteNode") ||
             sp_streq(ty, "IndexAndWriteNode")) {
      /* h[k] op= v / h[k] ||= v / h[k] &&= v: same promotion as h[k] = v. */
      is_idx_write = 1;
      recv = nt_ref(nt, id, "receiver");
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an != 1) continue;
      kt = infer_type(c, argv[0]); vt = infer_type(c, nt_ref(nt, id, "value"));
    }
    else {
      continue;
    }
    if (recv < 0) continue;
    const char *rty = nt_type(nt, recv);
    /* fold into a local's type or an ivar's type (an empty `@buf=[]` filled by
       `@buf << x` infers its element type the same way a local does) */
    TyKind *slot = NULL;
    int slot_reset = 0;  /* slot is a plain local, reset+recomputed per iteration:
                            net change is the stash compare's job, not this site's */
    const char *watch_nm = NULL;  /* ivar name for SP_IVWATCH, NULL for locals */
    if (rty && sp_streq(rty, "LocalVariableReadNode")) {
      const char *rnm = nt_str(nt, recv, "name");
      Scope *lsc = rnm ? comp_scope_of(c, recv) : NULL;
      LocalVar *lv = lsc ? scope_local(lsc, rnm) : NULL;
      if (!lv || lv->is_block_param) continue;
      /* A parameter is typed from its call sites, not from its uses -- except
         that a push through it MUTATES the caller's own array, so an element
         the bound type cannot hold has to widen it (and, through the reverse
         binding in bind_call_params, the caller's local too). Widening only:
         an UNKNOWN parameter still takes its type from the call site (#2989). */
      if (lv->is_param) {
        if (!is_push || !ty_is_array(lv->type) || lv->type == TY_POLY_ARRAY ||
            lv->rbs_seeded) continue;
        if (vt == TY_UNKNOWN || vt == ty_array_elem(lv->type)) continue;
        lv->type = TY_POLY_ARRAY; lv->push_widened = 1; changed = 1;
        continue;
      }
      /* A bare `x[i]` read OR an `x[i] = v` element assignment must not promote
         `x` to a hash if `x` elsewhere gets an array-typed write (`x = a.split`
         etc.): it is an array indexed/assigned by position, and the hash type
         would otherwise collide with it. Mirrors the ivar guard. */
      if (!is_push && lv->type == TY_UNKNOWN) {
        int lsc_sid = (int)(lsc - c->scopes);
        int has_array_write = 0;
        for (int _r = lw_index_first(&lw_ix, rnm, lsc_sid); _r >= 0 && !has_array_write; _r = lw_ix.next[_r]) {
          int w = lw_ix.node[_r];
          if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
          const char *wn = nt_str(nt, w, "name");
          if (!wn || !sp_streq(wn, rnm) || comp_scope_of(c, w) != lsc) continue;
          int wv = nt_ref(nt, w, "value");
          if (wv >= 0 && ty_is_array(infer_type(c, wv))) has_array_write = 1;
        }
        if (has_array_write) continue;
      }
      slot = &lv->type;
      slot_reset = !lv->is_param && !lv->is_block_param && !lv->rbs_seeded;
    }
    else if (rty && sp_streq(rty, "InstanceVariableReadNode")) {
      const char *inm = nt_str(nt, recv, "name");
      Scope *s = comp_scope_of(c, recv);
      int ivar_cls_id = s->class_id;
      if (ivar_cls_id < 0) ivar_cls_id = comp_class_index(c, "Toplevel");
      if (ivar_cls_id < 0) continue;
      ClassInfo *ci = &c->classes[ivar_cls_id];
      int iv = inm ? comp_ivar_index(ci, inm) : -1;
      if (iv < 0) continue;
      if (class_ivar_pinned(ci, inm)) continue;  /* --rbs seed pins are authoritative */
      slot = &ci->ivar_types[iv];
      watch_nm = inm;
      /* If the slot is TY_UNKNOWN but has a direct InstanceVariableWriteNode
         that assigns a typed value OR an empty array/hash literal (e.g.
         @buf = [nil]*7 or @free = []), skip usage-driven hash promotion
         (but allow push-driven array promotion through). Without this guard,
         @free[0] read promotes @free to poly_poly_hash before @free = []
         has been processed as an array. */
      /* A typed (non-nil) construction write — `@a = [x]*n`, `@a = arr.map{}`,
         or an `@a = []` literal — means this ivar is an array filled by index,
         not a hash. Skip usage-driven hash promotion for both plain reads and
         `@a[k]=v` index-writes. A genuine hash (`@h = {}`) infers UNKNOWN from
         its empty literal and is unaffected. */
      if (!is_push && *slot == TY_UNKNOWN && inm) {
        int has_typed_write = 0;
        for (int _r = ivw_index_first(&ivw_ix, inm); _r >= 0 && !has_typed_write; _r = ivw_ix.next[_r]) {
          int _wi = ivw_ix.node[_r];
          if (nt_kind(nt, _wi) != NK_InstanceVariableWriteNode) continue;
          const char *_wnm = nt_str(nt, _wi, "name");
          if (!_wnm || !sp_streq(_wnm, inm)) continue;
          Scope *_ws = comp_scope_of(c, _wi);
          int _ws_cls = _ws ? _ws->class_id : -1;
          if (_ws_cls < 0) _ws_cls = comp_class_index(c, "Toplevel");
          if (_ws_cls != ivar_cls_id) continue;
          int _wval = nt_ref(nt, _wi, "value");
          if (_wval < 0) continue;
          TyKind _wt = infer_type(c, _wval);
          if (_wt != TY_UNKNOWN && _wt != TY_NIL) { has_typed_write = 1; break; }
          /* @ivar = [] literal: this slot is an array, not subject to
             hash-promotion from [] read or [0]= write. Empty {} does NOT
             block promotion — the hash type is determined by key/value usage. */
          const char *_wvty = nt_type(nt, _wval);
          if (_wvty && sp_streq(_wvty, "ArrayNode"))
            has_typed_write = 1;
        }
        if (has_typed_write) continue;
      }
      /* `@s << x` on an ivar with a STRING write anywhere is a string
         append, not an array push: without this the push promoted the
         still-UNKNOWN slot to str_array before the write merge saw the
         string, and the union settled poly (#3227 P4). */
      if (is_push && (*slot == TY_UNKNOWN || *slot == TY_STRING ||
                      *slot == TY_STRBUF) && inm) {
        int has_string_write = 0;
        for (int _r = ivw_index_first(&ivw_ix, inm); _r >= 0 && !has_string_write; _r = ivw_ix.next[_r]) {
          int _wi = ivw_ix.node[_r];
          if (nt_kind(nt, _wi) != NK_InstanceVariableWriteNode) continue;
          const char *_wnm = nt_str(nt, _wi, "name");
          if (!_wnm || !sp_streq(_wnm, inm)) continue;
          Scope *_ws = comp_scope_of(c, _wi);
          int _ws_cls = _ws ? _ws->class_id : -1;
          if (_ws_cls < 0) _ws_cls = comp_class_index(c, "Toplevel");
          if (_ws_cls != ivar_cls_id) continue;
          int _wval = nt_ref(nt, _wi, "value");
          if (_wval < 0) continue;
          TyKind _wt = infer_type(c, _wval);
          if (_wt == TY_STRING || _wt == TY_STRBUF) { has_string_write = 1; break; }
        }
        if (has_string_write) continue;
      }
    }
    else if (is_push && rty && sp_streq(rty, "CallNode")) {
      /* `getter_method << x` where getter returns @ivar: trace through
         to that ivar so cross-class lazy-init getters get widened. */
      int recv_args = nt_ref(nt, recv, "arguments");
      int recv_argc = 0;
      if (recv_args >= 0) nt_arr(nt, recv_args, "arguments", &recv_argc);
      if (recv_argc != 0) continue;
      const char *mname = nt_str(nt, recv, "name");
      if (!mname) continue;
      Scope *caller = comp_scope_of(c, recv);
      if (!caller || caller->class_id < 0) continue;
      int defcls2 = caller->class_id;
      int getter_mi = comp_method_in_chain(c, caller->class_id, mname, &defcls2);
      const char *inm2 = NULL;
      char reader_iv[300];
      if (getter_mi < 0) {
        /* no hand-written getter: an attr_reader pushes into its backing
           ivar @<name> the same way `@<name> << x` does (#3139) */
        int rdefcls = caller->class_id;
        if (!comp_reader_in_chain(c, caller->class_id, mname, &rdefcls)) continue;
        snprintf(reader_iv, sizeof reader_iv, "@%s", mname);
        inm2 = reader_iv;
        defcls2 = rdefcls;
      }
      else {
        int last2 = scope_body_last(c, getter_mi);
        if (last2 < 0 || !nt_type(nt, last2) ||
            !sp_streq(nt_type(nt, last2), "InstanceVariableReadNode")) continue;
        inm2 = nt_str(nt, last2, "name");
      }
      if (!inm2) continue;
      ClassInfo *ci2 = &c->classes[defcls2];
      int iv2 = comp_ivar_index(ci2, inm2);
      if (iv2 < 0) continue;
      if (class_ivar_pinned(ci2, inm2)) continue;  /* --rbs seed pins are authoritative */
      slot = &ci2->ivar_types[iv2];
    }
    else continue;

    TyKind before = *slot;
    if (is_push) {
      /* explicit push/append: definitely array.  A PolyArray stays PolyArray
         regardless of the pushed value type; mixing typed arrays widens to
         PolyArray (ty_unify would return TY_POLY scalar, so use array-aware
         widening instead). */
      if (vt == TY_UNKNOWN) continue;
      /* If a [] read already promoted this slot to a hash type, the push
         wins: a variable that is pushed to is an array, not a hash.
         Reset the slot so the array promotion below can fire. */
      if (ty_is_hash(*slot)) *slot = TY_UNKNOWN;
      if (*slot != TY_UNKNOWN && !ty_is_array(*slot)) continue;
      if (*slot == TY_POLY_ARRAY) continue;  /* already widest array type */
      TyKind want = ty_array_of(vt);
      if (*slot != TY_UNKNOWN && want != *slot) want = TY_POLY_ARRAY;
      *slot = want;
    }
    else if (is_splice) {
      /* arr[s,l] = rhs / arr[range] = rhs: a source whose elements the typed
         receiver CONCRETELY cannot hold widens the slot to a poly array
         (mirrors push, monotonic and fixpoint-stable). A TY_POLY value is
         exempt -- statically unknown but usually the matching kind at
         runtime; the emitters keep their runtime dispatch/conversion for it.
         A 3-arg []= is unambiguous array evidence for an UNKNOWN slot; a
         range key alone is not (h[1..2] = v is a legal hash write). */
      if (vt == TY_UNKNOWN || vt == TY_POLY) { /* no evidence / exempt */ }
      else if (ty_is_array(*slot)) {
        if (*slot != TY_POLY_ARRAY && vt != ty_array_elem(*slot)) *slot = TY_POLY_ARRAY;
      }
      else if (*slot == TY_UNKNOWN && kt != TY_RANGE) *slot = ty_array_of(vt);
    }
    else if (*slot == TY_POLY_POLY_HASH) {
      /* already widest hash type; no further promotion needed */
    }
    else if (kt == TY_INT && *slot != TY_UNKNOWN && ty_is_array(*slot)) {
      /* int-key element write into a typed array: a value its element type
         CONCRETELY cannot hold widens the slot to a poly array, mirroring
         `a << x` -- the poly emitters then store the value exactly as CRuby
         does (the former bail left e.g. `a[0] = "s"` on an int array to emit
         invalid C through the typed setter). A TY_POLY value is exempt: the
         typed setter's runtime conversion (sp_poly_to_i etc.) is the
         long-standing intended path for it. */
      if (vt != TY_UNKNOWN && vt != TY_POLY &&
          *slot != TY_POLY_ARRAY && vt != ty_array_elem(*slot))
        *slot = TY_POLY_ARRAY;
    }
    else if (kt == TY_INT) {
      /* int key []= on a non-array slot: infer an int-keyed hash */
      if (vt == TY_UNKNOWN) continue;
      if (*slot != TY_UNKNOWN && !ty_is_hash(*slot)) continue;
      TyKind hv = ty_hash_of(TY_INT, vt);
      if (hv == TY_UNKNOWN) hv = TY_POLY_POLY_HASH;  /* int key + unknown val type */
      if (*slot != TY_UNKNOWN && *slot != hv) {
        /* widen to poly-poly if mismatch */
        if (ty_is_hash(*slot)) { *slot = TY_POLY_POLY_HASH; }
        continue;
      }
      *slot = hv;
    }
    else if (kt == TY_STRING) {
      if (vt == TY_UNKNOWN) continue;
      TyKind hv = ty_hash_of(TY_STRING, vt);
      if (hv == TY_UNKNOWN) hv = TY_STR_POLY_HASH;  /* mixed values */
      if (*slot != TY_UNKNOWN && !ty_is_hash(*slot)) continue;
      /* a str-keyed hash that has seen >1 value type widens to StrPoly */
      if (*slot != TY_UNKNOWN && *slot != hv &&
          (*slot == TY_STR_INT_HASH || *slot == TY_STR_STR_HASH || *slot == TY_STR_POLY_HASH))
        hv = TY_STR_POLY_HASH;
      *slot = hv;
    }
    else if (kt == TY_SYMBOL) {
      /* symbol key -> SymPolyHash (boxed values) */
      if (vt == TY_UNKNOWN) continue;
      if (*slot != TY_UNKNOWN && *slot != TY_SYM_POLY_HASH) continue;
      *slot = TY_SYM_POLY_HASH;
    }
    else if (kt != TY_UNKNOWN) {
      /* non-standard key type (array, object, etc.): heterogeneous hash */
      if (vt == TY_UNKNOWN) continue;
      if (*slot != TY_UNKNOWN && !ty_is_hash(*slot)) continue;
      *slot = TY_POLY_POLY_HASH;
    }
    sp_ivwatch(watch_nm, is_push ? "usage_push" : (is_idx_write ? "usage_idxwrite" : "usage_read"), before, *slot);
    if (*slot != before && !slot_reset) changed = 1;
  }

  /* Propagate container widening across direct local aliases (`b = a`): the
     fold above runs AFTER the write-site unification, so an alias assigned
     before its source widened would keep the narrower array kind -- two C
     representations for one runtime object, which reads garbage. Whichever
     side of the alias is the poly array wins, in both directions, to a local
     fixpoint. Params stay excluded (their types are call-site unified). */
  {
    int prop = 1;
    while (prop) {
      prop = 0;
      NT_FOREACH_KIND(nt, NK_LocalVariableWriteNode, id) {
        int v = nt_ref(nt, id, "value");
        const char *vty = v >= 0 ? nt_type(nt, v) : NULL;
        if (!vty || !sp_streq(vty, "LocalVariableReadNode")) continue;
        const char *dn = nt_str(nt, id, "name");
        const char *sn = nt_str(nt, v, "name");
        LocalVar *dst = dn ? scope_local(comp_scope_of(c, id), dn) : NULL;
        LocalVar *src = sn ? scope_local(comp_scope_of(c, v), sn) : NULL;
        if (!dst || !src || dst == src) continue;
        if (dst->is_param || dst->is_block_param || src->is_param || src->is_block_param) continue;
        if (!ty_is_array(dst->type) || !ty_is_array(src->type)) continue;
        if (dst->type == src->type) continue;
        if (dst->type == TY_POLY_ARRAY) { src->type = TY_POLY_ARRAY; prop = 1; changed = 1; }
        else if (src->type == TY_POLY_ARRAY) { dst->type = TY_POLY_ARRAY; prop = 1; changed = 1; }
      }
    }
  }

  /* The container-usage fold above can widen a hash's value layout after the
     main write-site scan has already typed a destination local. Reconcile the
     direct `value = hash[key]` shape now that the receiver is final: a boxed
     hash value must flow into a boxed local, rather than being coerced through
     an earlier scalar slot (for example, false becoming integer zero).

     Keep this deliberately local to the affected assignment. The normal node
     cache rebuild propagates the new slot type to its reads, while occurrence
     typing can still narrow guarded reads later in analysis. */
  NT_FOREACH_KIND(nt, NK_LocalVariableWriteNode, id) {
    int value = nt_ref(nt, id, "value");
    if (value < 0 || nt_kind(nt, value) != NK_CallNode) continue;
    const char *name = nt_str(nt, value, "name");
    if (!name || !sp_streq(name, "[]") || nt_ref(nt, value, "block") >= 0) continue;
    int args = nt_ref(nt, value, "arguments");
    int argc = 0;
    if (args >= 0) nt_arr(nt, args, "arguments", &argc);
    if (argc != 1) continue;
    int recv = nt_ref(nt, value, "receiver");
    TyKind rt = recv >= 0 ? infer_type(c, recv) : TY_UNKNOWN;
    if (!ty_is_hash(rt) || ty_hash_val(rt) != TY_POLY) continue;
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? scope_local(comp_scope_of(c, id), nm) : NULL;
    if (!lv || lv->is_param || lv->is_block_param || lv->rbs_seeded) continue;
    TyKind merged = ty_unify(lv->type, TY_POLY);
    if (merged != lv->type) { lv->type = merged; changed = 1; }
  }

  /* Second pass: re-compute proc_ret for proc-typed locals after body-internal
     locals have been typed. The first pass resets all locals to TY_UNKNOWN, so
     computing proc_ret there would see stale TY_UNKNOWN for variables assigned
     inside the proc body. Running after the first pass ensures those locals
     have their correct types (e.g. `x = 10` -> TY_INT) before proc_node_ret
     evaluates the body's return type. */
  NT_FOREACH_KIND(nt, NK_LocalVariableWriteNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
    if (!lv || lv->type != TY_PROC) continue;
    int vnode = nt_ref(nt, id, "value");
    TyKind pr = vnode >= 0 ? proc_ret_of(c, vnode) : TY_UNKNOWN;
    if (pr != TY_UNKNOWN && (TyKind)lv->proc_ret != pr) { lv->proc_ret = (int)pr; changed = 1; }
  }

  /* detect change vs the stashed old types */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++) {
      LocalVar *lv = &c->scopes[s].locals[i];
      if (!lv->is_param && !lv->is_block_param && (TyKind)lv->gc_root != lv->type) changed = 1;
    }
  lw_index_free(&lw_ix);
  lw_index_free(&ivw_ix);
  return changed;
}

/* Positional-only variant of bind_call_params for a call through a bound
   __bam wrapper: the Method's self slot carries param[0], so call arg k binds
   param[k + shift]. The wrapper's params are plain requireds (no splat or
   keyword shapes to handle). */
static int bind_call_args_shifted(Compiler *c, int call_id, int mi, int shift) {
  const NodeTable *nt = c->nt;
  Scope *m = &c->scopes[mi];
  int args = nt_ref(nt, call_id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int changed = 0;
  for (int k = 0; k < argc && k + shift < m->nparams; k++) {
    LocalVar *p = scope_local(m, m->pnames[k + shift]);
    if (!p || p->rbs_seeded) continue;
    TyKind at = infer_type(c, argv[k]);
    if (at == TY_VOID || at == TY_NIL) at = TY_POLY;
    TyKind merged = ty_unify(p->type, at);
    if (merged != p->type) { p->type = merged; changed = 1; }
  }
  return changed;
}

/* Unify a call's argument types into method scope `mi`'s parameters. */
int bind_call_params(Compiler *c, int call_id, int mi) {
  if (mi < 0) return 0;
  const NodeTable *nt = c->nt;
  Scope *m = &c->scopes[mi];
  int args = nt_ref(nt, call_id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  int changed = 0;
  /* `callee(...)`: the arg list is a single ForwardingArgumentsNode. Bind the
     callee's params from the enclosing `def foo(...)` method's synthesized
     __fwd_* params, positionally, so the callee's return type resolves (#1288). */
  if (argc == 1 && argv && nt_type(nt, argv[0]) &&
      sp_streq(nt_type(nt, argv[0]), "ForwardingArgumentsNode")) {
    Scope *encl = comp_scope_of(c, argv[0]);
    if (!encl) return 0;
    int n = m->nparams < encl->nparams ? m->nparams : encl->nparams;
    for (int k = 0; k < n; k++) {
      LocalVar *p = scope_local(m, m->pnames[k]);
      LocalVar *ep = scope_local(encl, encl->pnames[k]);
      if (!p || p->rbs_seeded || !ep || ep->type == TY_UNKNOWN) continue;
      TyKind merged = ty_unify(p->type, ep->type);
      if (merged != p->type) { p->type = merged; changed = 1; }
    }
    return changed;
  }
  /* Separate positional args from the trailing keyword-hash arg (if any). */
  int kwh = -1;
  int pos_argc = argc;
  if (argc > 0 && nt_type(nt, argv[argc - 1]) &&
      sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
    kwh = argv[argc - 1];
    pos_argc = argc - 1;
  }
  /* Don't bind individual args to the *rest slot; it stays TY_POLY_ARRAY. */
  int max_bind = m->nparams;
  if (m->rest_idx >= 0 && max_bind > m->rest_idx) max_bind = m->rest_idx;
  int n = pos_argc < max_bind ? pos_argc : max_bind;
  for (int k = 0; k < n; k++) {
    const char *apty = argv ? nt_type(nt, argv[k]) : NULL;
    /* A single SplatNode spreads its array across every remaining fixed param,
       not just this position. Bind each from the array's element type so a
       splat-only call site (`f(*args)`) still types — and therefore emits —
       the callee, then stop (the splat consumes the rest of the positionals). */
    if (apty && sp_streq(apty, "SplatNode")) {
      int inner = nt_ref(nt, argv[k], "expression");
      TyKind arr = inner >= 0 ? infer_type(c, inner) : TY_UNKNOWN;
      TyKind at = ty_is_array(arr) ? ty_array_elem(arr) : TY_POLY;
      if (at == TY_VOID || at == TY_NIL) at = TY_POLY;
      for (int pk = k; pk < max_bind; pk++) {
        if (!m->pnames[pk]) continue;
        LocalVar *p = scope_local(m, m->pnames[pk]);
        if (!p || p->rbs_seeded) continue;
        TyKind merged = ty_unify(p->type, at);
        if (merged != p->type) { p->type = merged; changed = 1; }
      }
      break;
    }
    TyKind at = infer_type(c, argv[k]);
    LocalVar *p = scope_local(m, m->pnames[k]);
    if (!p || p->rbs_seeded) continue;
    /* Post-convergence backstop only: an empty array-literal arg fills a
       parameter that no other call site typed, as an (empty) poly array so
       the callee's array methods dispatch. Never during the fixpoint -- a
       concrete kind from another call site must win the unification. */
    if (g_final_bind_pass && p->type == TY_UNKNOWN && at == TY_UNKNOWN &&
        apty && sp_streq(apty, "ArrayNode")) {
      int en0 = 0; nt_arr(nt, argv[k], "elements", &en0);
      if (en0 == 0) { p->type = TY_POLY_ARRAY; changed = 1; continue; }
    }
    /* A void arg (`sink(always_raising_method)`) is nil-ish in value position:
       bind the param poly so it is declarable; the arg is emitted via
       emit_boxed (it diverges and yields nil). */
    if (at == TY_VOID) at = TY_POLY;
    /* A nil arg narrows against an object param (NULL encodes nil) but widens
       any non-object param to poly. Pass nil through to ty_unify only while
       the param is still unknown or already an object. */
    if (at == TY_NIL && p->type != TY_UNKNOWN && p->type != TY_NIL && !ty_is_object(p->type)) at = TY_POLY;
    /* Two array parameters meet as the poly ARRAY, not the poly SCALAR: a
       param typed as one array kind at one call site and a different array
       kind at another is still a container, so its array methods (pop, <<)
       stay dispatchable (#2989, #3137). A push-widened param is the special
       case of this where the widening came from a use rather than a call. */
    TyKind merged;
    if (ty_is_array(p->type) && ty_is_array(at) && p->type != at)
      /* Two array kinds meet as the poly SCALAR, not the poly ARRAY: the
         boxed value keeps its concrete array class, so the callee's array
         methods dispatch through the poly runtime by cls_id, and every call
         site pays an O(1) box instead of the O(n) element-boxing rebuild a
         poly-ARRAY parameter forces on a concrete typed-array argument
         (which regressed optcarrot 22% -- the empty-`[]`-seeded ivar arg
         types POLY_ARRAY for the whole fixpoint and only re-narrows to the
         int array after it, so the conflict is usually transient, not real). */
      merged = TY_POLY;
    else if (p->push_widened && ty_is_array(at))
      merged = TY_POLY_ARRAY;
    else
      merged = ty_unify(p->type, at);
    if (merged != p->type) { p->type = merged; changed = 1; }
    /* Reverse binding: an empty-`{}`-only local passed to a hash parameter is
       that hash container, filled inside the callee through the reference.
       Type the local as the param's hash so it is constructed (sp_<H>Hash_new)
       rather than passed as a NULL-deref'ing poly nil. */
    if (ty_is_hash(p->type) && apty && sp_streq(apty, "LocalVariableReadNode")) {
      const char *an = nt_str(nt, argv[k], "name");
      Scope *asc = an ? comp_scope_of(c, argv[k]) : NULL;
      LocalVar *al = asc ? scope_local(asc, an) : NULL;
      if (al && !al->is_param && !al->is_block_param &&
          (al->type == TY_UNKNOWN || al->type == TY_POLY) &&
          local_all_writes_empty_hash(c, asc, an)) {
        al->type = p->type; changed = 1;
      }
    }
    /* Same, but the callee takes the hash opaquely (a TY_POLY param). An empty
       `{}` passed there is written inside the callee through the reference; the
       poly-value hash accessors (`h[k] ||= v` boxed set) only persist to a
       PolyPoly hash, so a StrPoly default silently dropped an int-keyed write
       (#3158). Type the caller's `{}` as PolyPoly so any key persists. */
    if (p->type == TY_POLY && apty && sp_streq(apty, "LocalVariableReadNode")) {
      const char *an = nt_str(nt, argv[k], "name");
      Scope *asc = an ? comp_scope_of(c, argv[k]) : NULL;
      LocalVar *al = asc ? scope_local(asc, an) : NULL;
      if (al && !al->is_param && !al->is_block_param &&
          (al->type == TY_UNKNOWN || al->type == TY_POLY || ty_is_hash(al->type)) &&
          al->type != TY_POLY_POLY_HASH &&
          local_all_writes_empty_hash(c, asc, an)) {
        al->type = TY_POLY_POLY_HASH; changed = 1;
      }
    }
    /* Reverse binding for an ARRAY argument: the callee mutates the very array
       the caller holds, so a push inside the method that widened the parameter
       to a poly array widens the caller's local too. Without this the caller
       kept its narrow element type and the in-method push emitted an
       ill-typed sp_StrArray_push of a symbol (#2989). Only the widening
       direction, only to the poly array, so it stays monotonic. */
    if (p->push_widened && apty && sp_streq(apty, "LocalVariableReadNode")) {
      const char *an = nt_str(nt, argv[k], "name");
      Scope *asc = an ? comp_scope_of(c, argv[k]) : NULL;
      LocalVar *al = asc ? scope_local(asc, an) : NULL;
      if (al && ty_is_array(al->type) && al->type != TY_POLY_ARRAY &&
          !al->is_param && !al->is_block_param &&
          local_all_writes_empty_array(c, asc, an)) {
        al->type = TY_POLY_ARRAY; changed = 1;
      }
    }
    if (merged == TY_PROC) {
      TyKind pr = proc_ret_of(c, argv[k]);
      if (pr != TY_UNKNOWN && p->proc_ret != (int)pr) { p->proc_ret = (int)pr; changed = 1; }
    }
  }
  /* Post-splat required params: bind from the end of the positional args. */
  if (m->rest_idx >= 0 && m->npost_rest > 0) {
    for (int j = 0; j < m->npost_rest; j++) {
      int pi = m->rest_idx + 1 + j;
      int ai = pos_argc - m->npost_rest + j;
      if (pi >= m->nparams || ai < 0 || ai >= pos_argc || !argv) continue;
      LocalVar *p = scope_local(m, m->pnames[pi]);
      if (!p || p->rbs_seeded) continue;
      TyKind at = infer_type(c, argv[ai]);
      if (at == TY_NIL && p->type != TY_UNKNOWN && p->type != TY_NIL && !ty_is_object(p->type)) at = TY_POLY;
      TyKind merged = ty_unify(p->type, at);
      if (merged != p->type) { p->type = merged; changed = 1; }
    }
  }
  /* Keyword arguments: match KeywordHashNode elements to named params. */
  if (kwh >= 0) {
    int en = 0;
    const int *elems = nt_arr(nt, kwh, "elements", &en);
    /* Check for a double-splat (**h) covering all keyword params. */
    TyKind ds_val = TY_UNKNOWN;
    for (int e = 0; e < en; e++) {
      const char *ety = nt_type(nt, elems[e]);
      if (ety && sp_streq(ety, "AssocSplatNode")) {
        int inner = nt_ref(nt, elems[e], "value");
        if (inner >= 0) {
          TyKind ht = infer_type(c, inner);
          if (ty_is_hash(ht)) ds_val = ty_hash_val(ht);
          /* A poly splat source (a hash read out of a poly container, e.g. a
             `map { |c| f(**c) }` block param) forwards poly values -- bind the
             callee's keyword params poly, not the source's hash type (#2885). */
          else if (ht == TY_POLY) ds_val = TY_POLY;
        }
        else {
          /* Anonymous `**` forwards the enclosing __anon_kwrest (a SymPolyHash
             with poly values). Bind the callee's keyword params as poly rather
             than falling through to the no-keyword backstop, which would mis-seed
             the callee's first positional param with a hash type. */
          Scope *esc = comp_scope_of(c, elems[e]);
          if (esc && esc->kwrest_idx >= 0 && esc->kwrest_idx < esc->nparams &&
              esc->pnames && esc->pnames[esc->kwrest_idx] &&
              sp_streq(esc->pnames[esc->kwrest_idx], "__anon_kwrest"))
            ds_val = TY_POLY;
        }
        break;
      }
    }
    if (ds_val != TY_UNKNOWN) {
      /* Bind all keyword params of the callee from the splat hash value type. */
      TyKind at = (ds_val == TY_POLY) ? TY_POLY : ds_val;
      if (at == TY_NIL) at = TY_POLY;
      for (int i = 0; i < m->nparams; i++) {
        /* The keyword-rest param receives the whole forwarded hash, not the
           splat's value type -- leave it as its hash type. The positional
           rest stays TY_POLY_ARRAY: an unconsumed **h degrades to one
           positional element inside it, never a type on the slot itself. */
        if (i == m->kwrest_idx) continue;
        if (i == m->rest_idx) continue;
        if (!m->pnames[i]) continue;
        LocalVar *p = scope_local(m, m->pnames[i]);
        if (!p || p->rbs_seeded) continue;
        TyKind merged = ty_unify(p->type, at);
        if (merged != p->type) { p->type = merged; changed = 1; }
      }
    }
else {
      int any_kw_bound = 0;
      for (int e = 0; e < en; e++) {
        int key = nt_ref(nt, elems[e], "key");
        int val = nt_ref(nt, elems[e], "value");
        if (key < 0 || val < 0) continue;
        const char *kty = nt_type(nt, key);
        const char *kname = (kty && sp_streq(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
        if (!kname) continue;
        LocalVar *p = scope_local(m, kname);
        if (!p || p->rbs_seeded) continue;
        TyKind at = infer_type(c, val);
        TyKind merged = ty_unify(p->type, at);
        if (merged != p->type) { p->type = merged; changed = 1; }
        any_kw_bound = 1;
      }
      /* Ruby collapses trailing kwargs into a positional hash parameter when
         the callee has no named keyword params (e.g. `def f(opts = {})`
         called as `f(key: val)`). Bind the next unbound positional param
         to TY_SYM_POLY_HASH so the backstop doesn't kill the method. */
      if (!any_kw_bound && pos_argc < max_bind && max_bind > 0) {
        LocalVar *p = m->pnames[pos_argc] ? scope_local(m, m->pnames[pos_argc]) : NULL;
        if (p && !p->rbs_seeded) {
          TyKind merged = ty_unify(p->type, TY_SYM_POLY_HASH);
          if (merged != p->type) { p->type = merged; changed = 1; }
        }
      }
    }
  }
  return changed;
}

/* Propagate param types from each prep-chain source scope (the transplanted
   module method) to the shadow scope it calls via super. The shadow scope has
   no AST call site, so bind_call_params never runs for it. */
int propagate_prep_params(Compiler *c) {
  int changed = 0;
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    for (int k = 0; k < cls->nprep_chain; k++) {
      const char *from_name = cls->prep_from[k];
      const char *to_name   = cls->prep_to[k];
      int from_mi = comp_method_in_class(c, ci, from_name);
      int to_mi = -1;
      for (int s = 0; s < c->nscopes; s++) {
        if (c->scopes[s].class_id == ci && !c->scopes[s].is_cmethod &&
            c->scopes[s].name && sp_streq(c->scopes[s].name, to_name)) {
          to_mi = s; break;
        }
      }
      if (from_mi < 0 || to_mi < 0) continue;
      Scope *fs = &c->scopes[from_mi];
      Scope *ts = &c->scopes[to_mi];
      int n = fs->nparams < ts->nparams ? fs->nparams : ts->nparams;
      for (int i = 0; i < n; i++) {
        LocalVar *fp = scope_local(fs, fs->pnames[i]);
        LocalVar *tp = scope_local(ts, ts->pnames[i]);
        if (!fp || !tp || fp->type == TY_UNKNOWN || tp->rbs_seeded) continue;
        TyKind merged = ty_unify(tp->type, fp->type);
        if (merged != tp->type) { tp->type = merged; changed = 1; }
      }
    }
  }
  return changed;
}

/* Optional parameters get a type from their default value too. */
int infer_default_param_types(Compiler *c) {
  int changed = 0;
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int i = 0; i < sc->nparams; i++) {
      if (sc->pdefault[i] < 0) continue;
      TyKind dt = infer_type(c, sc->pdefault[i]);
      /* An empty hash `{}` default returns TY_UNKNOWN from infer_type; treat
         it as TY_SYM_POLY_HASH since it is used as a kwargs receiver. An empty
         `[]` default is likewise a poly-array accumulator (`def f(n, acc = [])`,
         #2919) -- without a concrete type the param, and any method returning
         it, stay untyped. */
      if (dt == TY_UNKNOWN) {
        const char *dty = nt_type(c->nt, sc->pdefault[i]);
        if (dty && (sp_streq(dty, "HashNode") || sp_streq(dty, "KeywordHashNode"))) {
          int dn = 0; nt_arr(c->nt, sc->pdefault[i], "elements", &dn);
          if (dn == 0) dt = TY_SYM_POLY_HASH;
        }
        else if (dty && sp_streq(dty, "ArrayNode")) {
          int dn = 0; nt_arr(c->nt, sc->pdefault[i], "elements", &dn);
          if (dn == 0) dt = TY_POLY_ARRAY;
        }
      }
      if (dt == TY_NIL || dt == TY_UNKNOWN) continue;
      LocalVar *p = scope_local(sc, sc->pnames[i]);
      if (!p || p->rbs_seeded) continue;
      TyKind merged = ty_unify(p->type, dt);
      if (merged != p->type) { p->type = merged; changed = 1; }
    }
  }
  return changed;
}

/* Methods that only Strings respond to -- definitive evidence that a
   receiver is a String. (length/size/etc are shared with containers and so
   are deliberately excluded to keep the inference conservative.) */
int is_string_only_method(const char *m) {
  static const char *const set[] = {
    "split", "strip", "lstrip", "rstrip", "chomp", "chop", "upcase",
    "downcase", "capitalize", "swapcase", "gsub", "sub", "tr", "tr_s",
    "squeeze", "scan", "start_with?", "end_with?", "each_char", "chars",
    "center", "ljust", "rjust", "to_str", "encode", "unpack", "match?",
    "partition", "rpartition", "succ", "hex", "oct", "codepoints", "scrub",
    "crypt", "delete_prefix", "delete_suffix", "casecmp", "casecmp?",
    "force_encoding", NULL };
  for (int i = 0; set[i]; i++) if (sp_streq(m, set[i])) return 1;
  return 0;
}

/* Infer still-unknown params from ivar hash operations in the method body.
   For `def []=(key, val); @h[key] = val; end` where @h is a known hash type,
   infer key/val from the hash's key/value types.  Also handles `[]` reads.
   Runs post-fixpoint so ivar types are stable before this fires. */
int infer_params_from_ivar_hash_ops(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    int is_set = sp_streq(name, "[]=");
    int is_get = sp_streq(name, "[]");
    if (!is_set && !is_get) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || !sp_streq(rty, "InstanceVariableReadNode")) continue;
    const char *inm = nt_str(nt, recv, "name");
    if (!inm) continue;
    Scope *s = comp_scope_of(c, id);
    if (!s || s->class_id < 0) continue;
    ClassInfo *ci = &c->classes[s->class_id];
    int iv = comp_ivar_index(ci, inm);
    if (iv < 0) continue;
    TyKind ht = ci->ivar_types[iv];
    if (!ty_is_hash(ht) || ht == TY_POLY_POLY_HASH) continue;
    TyKind hk = ty_hash_key(ht);
    TyKind hv = ty_hash_val(ht);
    int args = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    /* [](key) => key is hash key type; []=(key, val) => key + val */
    if (an >= 1 && argv && hk != TY_UNKNOWN) {
      const char *aty = nt_type(nt, argv[0]);
      if (aty && sp_streq(aty, "LocalVariableReadNode")) {
        const char *anm = nt_str(nt, argv[0], "name");
        LocalVar *lv = anm ? scope_local(s, anm) : NULL;
        if (lv && lv->is_param && lv->type == TY_UNKNOWN) {
          lv->type = hk; changed = 1;
        }
      }
    }
    if (is_set && an >= 2 && argv && hv != TY_UNKNOWN) {
      const char *aty = nt_type(nt, argv[1]);
      if (aty && sp_streq(aty, "LocalVariableReadNode")) {
        const char *anm = nt_str(nt, argv[1], "name");
        LocalVar *lv = anm ? scope_local(s, anm) : NULL;
        if (lv && lv->is_param && lv->type == TY_UNKNOWN) {
          lv->type = hv; changed = 1;
        }
      }
    }
  }
  return changed;
}

/* Infer a still-unknown parameter as a typed hash when the body indexes
   it with a literal key: `param["key"]` → str_poly_hash,
   `param[:sym]` → sym_poly_hash. Runs in the fixpoint alongside
   infer_string_params so methods with no concrete-typed caller still
   resolve their hash param type from body usage. */
int infer_hash_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  static const char *const hash_only_meths[] = {
    "keys","values","each_pair","merge","merge!","update","has_key?","key?","fetch","store",
    "delete","transform_values","transform_keys","to_h","each_with_object",NULL
  };
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    /* Index-or-write on an unknown param hash (h[k] ||= v / &&= / op=): infer the
       variant from the key + value types, mirroring the []= case below. These are
       not CallNodes, so the CallNode path never reaches them; without this a hash
       passed in empty (`{}` infers TY_UNKNOWN) stays unresolved and the method's
       return type degrades to poly, which the caller then rejects. */
    if (sp_streq(ty, "IndexOrWriteNode") || sp_streq(ty, "IndexAndWriteNode") ||
        sp_streq(ty, "IndexOperatorWriteNode")) {
      int wrecv = nt_ref(nt, id, "receiver");
      if (wrecv < 0) continue;
      const char *wrty = nt_type(nt, wrecv);
      if (!wrty || !sp_streq(wrty, "LocalVariableReadNode")) continue;
      const char *wrnm = nt_str(nt, wrecv, "name");
      if (!wrnm) continue;
      Scope *ws = comp_scope_of(c, id);
      LocalVar *wlv = scope_local(ws, wrnm);
      if (!wlv || !wlv->is_param || wlv->type != TY_UNKNOWN) continue;
      int wargs = nt_ref(nt, id, "arguments");
      int wan = 0; const int *wargv = wargs >= 0 ? nt_arr(nt, wargs, "arguments", &wan) : NULL;
      if (wan < 1) continue;
      TyKind wkt = infer_type(c, wargv[0]);
      TyKind wvt = infer_type(c, nt_ref(nt, id, "value"));
      TyKind wwant = TY_UNKNOWN;
      if (wkt == TY_STRING) wwant = (wvt == TY_STRING) ? TY_STR_STR_HASH : TY_STR_POLY_HASH;
      else if (wkt == TY_SYMBOL) wwant = TY_SYM_POLY_HASH;
      else if (wkt == TY_INT) wwant = TY_POLY_POLY_HASH;
      if (wwant == TY_UNKNOWN) continue;
      wlv->type = wwant; changed = 1;
      continue;
    }
    if (!sp_streq(ty, "CallNode")) continue;
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || !sp_streq(rty, "LocalVariableReadNode")) continue;
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = scope_local(s, nt_str(nt, recv, "name"));
    if (!lv || !lv->is_param || lv->type != TY_UNKNOWN) continue;
    /* Literal-key [] / fetch: infer specific variant */
    if (sp_streq(name, "[]") || sp_streq(name, "fetch")) {
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 1) continue;
      const char *kty = argv ? nt_type(nt, argv[0]) : NULL;
      if (!kty) continue;
      TyKind want = TY_UNKNOWN;
      if (sp_streq(kty, "StringNode") || sp_streq(kty, "InterpolatedStringNode"))
        want = TY_STR_POLY_HASH;
      else if (sp_streq(kty, "SymbolNode"))
        want = TY_SYM_POLY_HASH;
      if (want == TY_UNKNOWN) continue;
      lv->type = want; changed = 1;
      continue;
    }
    /* []=: infer hash variant from key + value types */
    if (sp_streq(name, "[]=")) {
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 2) continue;
      TyKind kt2 = infer_type(c, argv[0]);
      TyKind vt2 = infer_type(c, argv[1]);
      TyKind want = TY_UNKNOWN;
      if (kt2 == TY_STRING) {
        want = (vt2 == TY_STRING) ? TY_STR_STR_HASH : TY_STR_POLY_HASH;
      }
      else if (kt2 == TY_SYMBOL) want = TY_SYM_POLY_HASH;
      else if (kt2 == TY_INT)    want = TY_POLY_POLY_HASH;
      if (want == TY_UNKNOWN) continue;
      lv->type = want; changed = 1;
      continue;
    }
    /* Hash-only methods: widen to str_poly_hash (most common variant) */
    for (int k = 0; hash_only_meths[k]; k++) {
      if (sp_streq(name, hash_only_meths[k])) { lv->type = TY_STR_POLY_HASH; changed = 1; break; }
    }
  }
  return changed;
}

/* Infer a still-unknown parameter as poly_array when the body calls an
   array-only method on it: push/pop/shift/unshift/concat/length/size/empty?.
   Does NOT fire on << (overlaps with Integer/String) or arithmetic ops.
   Runs inside the fixpoint so array params without typed callers still resolve. */
int infer_array_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  /* Genuinely array-only names may override a caller-side hash widening
     (a param that receives push() is an array, not a hash); names Hash also
     answers (compact, flatten, each_with_index, each_with_object, zip) only
     fill a parameter that is still UNKNOWN, so a hash-typed caller wins. */
  static const char *const arr_only_meths[] = {
    "push","pop","shift","unshift","concat","transpose",
    "combination","permutation",NULL
  };
  static const char *const arr_or_hash_meths[] = {
    "flatten","compact","each_with_index","each_with_object","zip",NULL
  };
  int changed = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    int strong = 0, weak = 0;
    for (int k = 0; arr_only_meths[k]; k++) if (sp_streq(name, arr_only_meths[k])) { strong = 1; break; }
    if (!strong)
      for (int k = 0; arr_or_hash_meths[k]; k++) if (sp_streq(name, arr_or_hash_meths[k])) { weak = 1; break; }
    if (!strong && !weak) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || !sp_streq(rty, "LocalVariableReadNode")) continue;
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = scope_local(s, nt_str(nt, recv, "name"));
    if (lv && lv->is_param && !lv->rbs_seeded &&
        (lv->type == TY_UNKNOWN || (strong && ty_is_hash(lv->type)))) {
      lv->type = TY_POLY_ARRAY; changed = 1;
    }
  }
  return changed;
}

/* Infer a still-unknown parameter as String when the body calls a
   String-only method on it (a param with no concrete-typed caller). */
int infer_string_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *name = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    if (!name || recv < 0 || !is_string_only_method(name)) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || !sp_streq(rty, "LocalVariableReadNode")) continue;
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = scope_local(s, nt_str(nt, recv, "name"));
    if (lv && lv->is_param && lv->type == TY_UNKNOWN) { lv->type = TY_STRING; changed = 1; }
  }
  return changed;
}

static int fwd_callable_def(Compiler *c, int ref, int *out_body, int *out_pn);

/* Widen the required parameters of a proc/lambda operand of Proc#>> / #<< to
   POLY. Such an operand receives (or produces) the composition's dynamic
   intermediate value, whose type is only known through the other proc, so an
   otherwise-untyped param must not default to int (which bakes a wrong-type
   dispatch into the body). #2650 */
static int widen_proc_params_poly(Compiler *c, int ref) {
  NodeTable *nt = (NodeTable *)c->nt;
  int body = -1, pn = -1;
  if (!fwd_callable_def(c, ref, &body, &pn) || pn < 0 || body < 0) return 0;
  Scope *sc = comp_scope_of(c, body);
  if (!sc) return 0;
  int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
  int changed = 0;
  for (int k = 0; k < rn && reqs; k++) {
    const char *pnm = nt_str(nt, reqs[k], "name");
    LocalVar *lv = pnm ? scope_local(sc, pnm) : NULL;
    if (lv && !lv->rbs_seeded && lv->type != TY_POLY) { lv->type = TY_POLY; changed = 1; }
  }
  return changed;
}

int infer_param_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (sp_streq(ty, "SuperNode") || sp_streq(ty, "ForwardingSuperNode")) {
      Scope *s = comp_scope_of(c, id);
      if (s->class_id < 0 || !s->name) continue;
      int p = c->classes[s->class_id].parent;
      if (p < 0) continue;
      int pmi = comp_method_in_chain(c, p, s->name, NULL);
      if (pmi < 0) continue;
      if (sp_streq(ty, "ForwardingSuperNode")) {
        /* bare `super` forwards all current params to parent */
        Scope *pm = &c->scopes[pmi];
        int n = s->nparams < pm->nparams ? s->nparams : pm->nparams;
        if (pm->rest_idx >= 0 && n > pm->rest_idx) n = pm->rest_idx;
        for (int k = 0; k < n; k++) {
          LocalVar *src = scope_local(s, s->pnames[k]);
          LocalVar *dst = scope_local(pm, pm->pnames[k]);
          if (!src || !dst || dst->rbs_seeded) continue;
          TyKind at = src->type;
          if (at == TY_UNKNOWN) continue;
          TyKind mg = ty_unify(dst->type, at);
          if (mg != dst->type) { dst->type = mg; changed = 1; }
        }
      }
      else {
        changed |= bind_call_params(c, id, pmi);
      }
      continue;
    }
    /* op-assign on an object slot: `lv OP= rhs` / `@iv OP= rhs` is an
       implicit call to `lv.OP(rhs)` -- bind the RHS type to the method param. */
    if (sp_streq(ty, "LocalVariableOperatorWriteNode") ||
        sp_streq(ty, "InstanceVariableOperatorWriteNode")) {
      const char *nm  = nt_str(nt, id, "name");
      const char *op  = nt_str(nt, id, "binary_operator");
      int val         = nt_ref(nt, id, "value");
      if (!op || val < 0) continue;
      TyKind slot_t = TY_UNKNOWN;
      if (sp_streq(ty, "LocalVariableOperatorWriteNode")) {
        Scope *s2 = comp_scope_of(c, id);
        LocalVar *lv2 = nm ? scope_local(s2, nm) : NULL;
        slot_t = lv2 ? lv2->type : TY_UNKNOWN;
      }
      else {
        Scope *s2 = comp_scope_of(c, id);
        if (s2->class_id < 0) continue;
        int iidx = nm ? comp_ivar_index(&c->classes[s2->class_id], nm) : -1;
        slot_t = iidx >= 0 ? c->classes[s2->class_id].ivar_types[iidx] : TY_UNKNOWN;
      }
      /* For TY_POLY slots, scan all user classes for a matching operator method. */
      int cid2 = -1;
      if (ty_is_object(slot_t)) cid2 = ty_object_class(slot_t);
      else if (slot_t == TY_POLY) {
        for (int _sc = 0; _sc < c->nclasses; _sc++) {
          if (comp_method_in_chain(c, _sc, op, NULL) >= 0) { cid2 = _sc; break; }
        }
      }
      if (cid2 < 0) continue;
      int mi2 = comp_method_in_chain(c, cid2, op, NULL);
      if (mi2 < 0) continue;
      Scope *ms2 = &c->scopes[mi2];
      if (ms2->nparams < 1) continue;
      LocalVar *pp = scope_local(ms2, ms2->pnames[0]);
      if (!pp || pp->rbs_seeded) continue;
      TyKind at2 = infer_type(c, val);
      TyKind mg2 = ty_unify(pp->type, at2);
      if (mg2 != pp->type) { pp->type = mg2; changed = 1; }
      continue;
    }
    if (!sp_streq(ty, "CallNode")) continue;
    if (nt_int(nt, id, "rt_probe", 0)) continue;  /* analysis-only respond_to? probe:
       its dummy-argument shape must not type real method params */
    const char *name = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");

    /* `raise Cls, arg` constructs `Cls.new(arg)` for a user exception
       subclass, so seed Cls#initialize's first param from arg's type --
       without this the param stays TY_UNKNOWN and the constructor gets
       marked unreachable, dropping the initialize call (#1415). */
    if (recv < 0 && name && sp_streq(name, "raise")) {
      int rargs = nt_ref(nt, id, "arguments");
      int ran = 0; const int *rav = rargs >= 0 ? nt_arr(nt, rargs, "arguments", &ran) : NULL;
      if (ran >= 2 && nt_type(nt, rav[0]) &&
          (sp_streq(nt_type(nt, rav[0]), "ConstantReadNode") || sp_streq(nt_type(nt, rav[0]), "ConstantPathNode"))) {
        const char *rcn = nt_str(nt, rav[0], "name");
        int rci = rcn ? comp_class_index(c, rcn) : -1;
        if (rci >= 0 && class_is_exc_subclass(c, rci)) {
          int imi = comp_method_in_chain(c, rci, "initialize", NULL);
          if (imi >= 0 && c->scopes[imi].nparams >= 1) {
            LocalVar *ip = scope_local(&c->scopes[imi], c->scopes[imi].pnames[0]);
            TyKind at = infer_type(c, rav[1]);
            if (ip && !ip->rbs_seeded && at != TY_UNKNOWN) {
              TyKind m = ty_unify(ip->type, at);
              if (m != ip->type) { ip->type = m; changed = 1; }
            }
          }
        }
      }
    }

    /* `obj.dup` / `obj.clone` for a user object call the class's initialize_copy
       hook (in codegen) with the original as the sole argument. That call has no
       Ruby CallNode, so seed the hook's first param to the receiver's class here
       -- otherwise it stays TY_UNKNOWN and the backstop prunes the method. */
    if (recv >= 0 && name && (sp_streq(name, "dup") || sp_streq(name, "clone"))) {
      TyKind drt = infer_type(c, recv);
      if (ty_is_object(drt)) {
        int dcid = ty_object_class(drt);
        int dmi = comp_method_in_chain(c, dcid, "initialize_copy", NULL);
        if (dmi >= 0 && c->scopes[dmi].nparams >= 1) {
          LocalVar *dp = scope_local(&c->scopes[dmi], c->scopes[dmi].pnames[0]);
          if (dp && !dp->rbs_seeded) {
            TyKind m = ty_unify(dp->type, ty_object(dcid));
            if (m != dp->type) { dp->type = m; changed = 1; }
          }
        }
      }
    }

    /* <method>.call(args): bind the call-site arg types to the target
       method's params (the Method ABI is the only call site for a method
       reached solely via method(:sym)). */
    if (recv >= 0 && name && (sp_streq(name, "call") || sp_streq(name, "[]") || sp_streq(name, "()")) &&
        infer_type(c, recv) == TY_METHOD) {
      int mn = method_recv_node(c, recv);
      int tmi = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
      if (tmi >= 0) {
        int shift = method_call_param_shift(c, mn, tmi);
        if (shift) changed |= bind_call_args_shifted(c, id, tmi, shift);
        else changed |= bind_call_params(c, id, tmi);
      }
      continue;
    }

    /* <unbound>.bind_call(obj, args...): the args after the receiver bind to
       the target's params, same as <method>.call -- without this the target's
       params (and so its return) stay untyped and the method emits void,
       breaking the value use (#3246). */
    if (recv >= 0 && name && sp_streq(name, "bind_call") &&
        infer_type(c, recv) == TY_METHOD) {
      int mn = method_recv_node(c, recv);
      int tmi = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
      if (tmi >= 0) {
        int bargs = nt_ref(nt, id, "arguments");
        int ban = 0; const int *bav = bargs >= 0 ? nt_arr(nt, bargs, "arguments", &ban) : NULL;
        Scope *bm = &c->scopes[tmi];
        for (int k = 1; k < ban && k - 1 < bm->nparams; k++) {
          LocalVar *bp = scope_local(bm, bm->pnames[k - 1]);
          if (!bp || bp->rbs_seeded) continue;
          TyKind bat = infer_type(c, bav[k]);
          if (bat == TY_VOID || bat == TY_NIL) bat = TY_POLY;
          TyKind bmg = ty_unify(bp->type, bat);
          if (bmg != bp->type) { bp->type = bmg; changed = 1; }
        }
        continue;
      }
    }

    /* <method>.to_proc stored as a Proc: its .call sites are likewise the
       only way the target method is reached, so bind their arg types to the
       target's params (the emitted trampoline calls the real C signature). */
    if (recv >= 0 && name && (sp_streq(name, "call") || sp_streq(name, "[]") || sp_streq(name, "()")) &&
        infer_type(c, recv) == TY_PROC) {
      int mn = proc_to_proc_method_node(c, recv);
      int tmi = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
      if (tmi >= 0) {
        int shift = method_call_param_shift(c, mn, tmi);
        if (shift) changed |= bind_call_args_shifted(c, id, tmi, shift);
        else changed |= bind_call_params(c, id, tmi);
        continue;
      }
    }

    /* proc >> proc / proc << proc: widen both operands' params to POLY so the
       dynamic intermediate value flows through the boxed side-channel. #2650 */
    if (recv >= 0 && name && (sp_streq(name, ">>") || sp_streq(name, "<<")) &&
        infer_type(c, recv) == TY_PROC) {
      int aargs = nt_ref(nt, id, "arguments");
      int an = 0; const int *aav = aargs >= 0 ? nt_arr(nt, aargs, "arguments", &an) : NULL;
      if (an == 1 && aav && infer_type(c, aav[0]) == TY_PROC) {
        changed |= widen_proc_params_poly(c, recv);
        changed |= widen_proc_params_poly(c, aav[0]);
      }
    }

    /* proc.curry: the deferred realization passes each accumulated argument
       boxed (its static type is unknown at the curry site), so the target's
       params must read them from the boxed side-channel -> widen to POLY.
       Without this a String/object arg reaches an int-typed param as a raw
       pointer value (#3183). */
    if (recv >= 0 && name && sp_streq(name, "curry") &&
        infer_type(c, recv) == TY_PROC) {
      changed |= widen_proc_params_poly(c, recv);
    }

    if (recv < 0) {
      /* bare `new(args)` inside a class method constructs the enclosing
         (possibly specialized) class -> bind args to that class's
         initialize, so the subclass constructor's params get typed. */
      if (name && sp_streq(name, "new")) {
        Scope *s = comp_scope_of(c, id);
        if (s && s->is_cmethod && s->class_id >= 0) {
          int initmi = comp_method_in_chain(c, s->class_id, "initialize", NULL);
          if (initmi >= 0) changed |= bind_call_params(c, id, initmi);
        }
        continue;
      }
      int mi = comp_method_index(c, name);
      int caller_cid = -1;
      /* bare call inside an instance_eval/exec block: dispatch on the
         receiver's class so its params get the call-site arg types. */
      int iec = ie_class_of(c, id);
      if (mi < 0 && iec >= 0) {
        int def_cid = -1;
        mi = comp_method_in_chain(c, iec, name, &def_cid);
        if (mi >= 0) caller_cid = def_cid >= 0 ? def_cid : iec;
      }
      if (mi < 0) {
        Scope *self = comp_scope_of(c, id);
        if (self->class_id >= 0) {
          caller_cid = self->class_id;
          int def_cid = -1;
          mi = comp_method_in_chain(c, self->class_id, name, &def_cid);
          if (mi >= 0 && def_cid >= 0) caller_cid = def_cid;
          /* inside a class method: also check sibling class methods */
          if (mi < 0 && self->is_cmethod)
            mi = comp_cmethod_in_chain(c, self->class_id, name, NULL);
        }
      }
      if (mi < 0) mi = comp_included_method_index(c, name);
      changed |= bind_call_params(c, id, mi);
      /* Propagate to descendant classes that directly override the same method.
         When Base#foo calls bar(arg), and Sub overrides bar, Sub#bar must also
         receive the same arg types so the cls_id-switch dispatch is type-safe.
         Also handles the case where only descendants define the method (mi < 0
         from base chain, e.g. Base.find calls adapter_find defined only in
         Article and Comment descendants). */
      if (caller_cid >= 0) {
        Scope *caller_sc = comp_scope_of(c, id);
        int is_cm = caller_sc ? caller_sc->is_cmethod : 0;
        for (int k = 0; k < c->nclasses; k++) {
          if (k == caller_cid) continue;
          int is_desc = 0;
          for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
            if (p == caller_cid) { is_desc = 1; break; }
          if (!is_desc) continue;
          int dmi = is_cm ? comp_cmethod_in_class(c, k, name) :
                            comp_method_in_class(c, k, name);
          if (dmi >= 0) changed |= bind_call_params(c, id, dmi);
        }
      }
      continue;
    }
    /* `Module.accessor.cmethod(args)` folded to a constant: bind args to the
       resolved class method's params (so it is not dropped as untyped). */
    {
      int fold_ci = comp_sg_reader_const(c, recv);
      if (fold_ci >= 0) {
        int fmi = comp_cmethod_in_chain(c, fold_ci, name, NULL);
        if (fmi >= 0) { changed |= bind_call_params(c, id, fmi); continue; }
      }
      int cand[32];
      int ncand = comp_sg_reader_candidates(c, recv, cand, 32);
      if (ncand >= 2) {
        int bound = 0;
        for (int k = 0; k < ncand; k++) {
          int cmi = comp_cmethod_in_chain(c, cand[k], name, NULL);
          if (cmi >= 0) { changed |= bind_call_params(c, id, cmi); bound = 1; }
        }
        if (bound) continue;
      }
    }
    /* Class.new -> initialize params; Class.cmethod -> cmethod params */
    {
      const char *rty = nt_type(nt, recv);
      /* M::Sub.new(...) — resolve by the final path component */
      if (rty && sp_streq(rty, "ConstantPathNode")) {
        const char *cn = nt_str(nt, recv, "name");
        int ci = cn ? comp_class_index(c, cn) : -1;
        if (ci >= 0 && sp_streq(name, "new")) {
          int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
          if (ucnew >= 0)
            changed |= bind_call_params(c, id, ucnew);
          else
            changed |= bind_call_params(c, id, comp_method_in_chain(c, ci, "initialize", NULL));
        }
        else if (ci >= 0)
          changed |= bind_call_params(c, id, comp_cmethod_in_chain(c, ci, name, NULL));
      }
      if (rty && sp_streq(rty, "ConstantReadNode")) {
        int ci = comp_class_index(c, nt_str(nt, recv, "name"));
        if (ci >= 0) {
          if (sp_streq(name, "new") && c->classes[ci].is_struct) {
            /* Struct construction: positional args set member ivars in order. */
            ClassInfo *cls = &c->classes[ci];
            int args = nt_ref(nt, id, "arguments");
            int an = 0;
            const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
            int kwh = (an == 1 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) ? argv[0] : -1;
            for (int a = 0; a < cls->nivars; a++) {
              /* a member not supplied at this construction can be nil */
              const char *mname = cls->ivars[a] + 1;
              int kn = 0;
              const int *ke = kwh >= 0 ? nt_arr(nt, kwh, "elements", &kn) : NULL;
              int vnode = -1;
              if (kwh >= 0) {
                for (int e = 0; e < kn; e++) {
                  int key = nt_ref(nt, ke[e], "key");
                  if (key >= 0 && nt_type(nt, key) && sp_streq(nt_type(nt, key), "SymbolNode") &&
                      nt_str(nt, key, "value") && sp_streq(nt_str(nt, key, "value"), mname)) { vnode = nt_ref(nt, ke[e], "value"); break; }
                }
              }
              else if (a < an) vnode = argv[a];
              if (class_ivar_pinned(cls, cls->ivars[a])) continue;
              TyKind at = vnode >= 0 ? infer_type(c, vnode) : TY_NIL;
              TyKind m = ty_unify(cls->ivar_types[a], at);
              if (m != cls->ivar_types[a]) { cls->ivar_types[a] = m; changed = 1; }
            }
            continue;
          }
          if (sp_streq(name, "new")) {
            int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
            if (ucnew >= 0)
              changed |= bind_call_params(c, id, ucnew);
            else
              changed |= bind_call_params(c, id, comp_method_in_chain(c, ci, "initialize", NULL));
          }
          else
            changed |= bind_call_params(c, id, comp_cmethod_in_chain(c, ci, name, NULL));
          continue;
        }
      }
      if (sp_streq(name, "new")) continue;
    }
    /* obj.method -> instance method params */
    TyKind rt = infer_type(c, recv);
    if (ty_is_object(rt)) {
      int cid3 = ty_object_class(rt);
      int mi3 = comp_method_in_chain(c, cid3, name, NULL);
      /* Comparable: `a < b` etc. on an object with `<=>` but no direct `<`
         bind the argument to `<=>` param instead. */
      if (mi3 < 0 && (sp_streq(name, "<") || sp_streq(name, ">") ||
                      sp_streq(name, "<=") || sp_streq(name, ">=")))
        mi3 = comp_method_in_chain(c, cid3, "<=>", NULL);
      changed |= bind_call_params(c, id, mi3);
      /* Also propagate to descendant overrides: codegen will emit a cls_id
         switch that calls each override, so each must have the right param
         types. */
      for (int k = 0; k < c->nclasses; k++) {
        int is_desc = 0;
        for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
          if (p == cid3) { is_desc = 1; break; }
        if (!is_desc) continue;
        int dmi3 = comp_method_in_class(c, k, name);
        if (dmi3 >= 0) changed |= bind_call_params(c, id, dmi3);
      }
    }
    else if (rt == TY_POLY) {
      /* poly receiver: the call may dispatch to any user method of this name,
         so bind every candidate's params (they would otherwise stay UNKNOWN
         and fail to compile). EXCEPT an operator whose every argument is a
         builtin scalar: the runtime poly operator serves those tags before
         any user-class arm, so binding would only poison the user method's
         params -- a poly block-param's `x + 1` must not widen Set#+'s
         operand (which then breaks Set#| for every caller). */
      static const char *const POLY_SCALAR_OPS[] = {
        "+", "-", "*", "/", "%", "**", "&", "|", "^", "<<", ">>",
        /* Comparisons against a scalar: sp_poly_eq / sp_poly_cmp serve the
           scalar tags directly, and a user-class receiver reaches its own
           `==`/`<=>` through the runtime dispatch with a boxed (poly) operand.
           Binding the scalar arg here would poison a user comparison's param --
           e.g. `node == :sym` (node poly) widening Set#=='s `other` to Symbol,
           which then breaks Set#subset?'s `other.include?` (#2877). */
        "==", "!=", "eql?", "<=>", "<", "<=", ">", ">=", NULL };
      int op_scalar = 0;
      if (name)
        for (int o = 0; POLY_SCALAR_OPS[o]; o++)
          if (sp_streq(name, POLY_SCALAR_OPS[o])) { op_scalar = 1; break; }
      if (op_scalar) {
        int args2 = nt_ref(nt, id, "arguments");
        int ac2 = 0;
        const int *av2 = args2 >= 0 ? nt_arr(nt, args2, "arguments", &ac2) : NULL;
        if (ac2 == 0) op_scalar = 0;
        for (int a2 = 0; a2 < ac2 && op_scalar; a2++) {
          TyKind at2 = infer_type(c, av2[a2]);
          if (!(at2 == TY_INT || at2 == TY_FLOAT || at2 == TY_STRING ||
                at2 == TY_BOOL || at2 == TY_SYMBOL)) op_scalar = 0;
        }
      }
      if (!op_scalar)
        for (int k = 0; k < c->nclasses; k++)
          changed |= bind_call_params(c, id, comp_method_in_chain(c, k, name, NULL));
    }
  }
  return changed;
}

/* `for x in coll` binds x to the collection's element type (int for a
   range, the array element type for an array). */
int infer_for_index(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  NT_FOREACH_KIND(nt, NK_ForNode, id) {
    int idx = nt_ref(nt, id, "index");
    int coll = nt_ref(nt, id, "collection");
    if (idx < 0 || coll < 0) continue;
    const char *idx_ty = nt_type(nt, idx);
    /* for a, b in coll: MultiTargetNode with LocalVariableTargetNode children */
    if (idx_ty && sp_streq(idx_ty, "MultiTargetNode")) {
      int ln = 0;
      const int *lefts = nt_arr(nt, idx, "lefts", &ln);
      TyKind ct2 = infer_type(c, coll);
      /* Each destructured variable gets the element type of the inner array,
         or TY_POLY if the collection element is not a concrete typed array. */
      TyKind inner = TY_POLY;
      if (ty_is_array(ct2)) {
        TyKind et2 = ty_array_elem(ct2);
        if (ty_is_array(et2)) inner = ty_array_elem(et2);
      }
      Scope *ms = comp_scope_of(c, idx);
      for (int i = 0; i < ln; i++) {
        const char *lnm = nt_str(nt, lefts[i], "name");
        if (!lnm) continue;
        LocalVar *lv = scope_local_intern(ms, lnm);
        lv->is_block_param = 1;
        if (lv->type != inner) { lv->type = inner; changed = 1; }
      }
      continue;
    }
    const char *vn = nt_str(nt, idx, "name");
    if (!vn) continue;
    TyKind ct = infer_type(c, coll);
    TyKind et = ct == TY_RANGE ? TY_INT : ty_is_array(ct) ? ty_array_elem(ct) : TY_UNKNOWN;
    if (et == TY_UNKNOWN) continue;
    LocalVar *lv = scope_local_intern(comp_scope_of(c, idx), vn);
    lv->is_block_param = 1;  /* iteration-bound: survives the write-types reset */
    if (lv->type != et) { lv->type = et; changed = 1; }
  }
  return changed;
}

/* `catch { |tag| ... }` binds its block param to the auto-generated tag,
   which codegen mints as a content-unique heap string. */
int infer_catch_block_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "catch") || nt_ref(nt, id, "receiver") >= 0) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0;
    if (args >= 0) nt_arr(nt, args, "arguments", &an);
    if (an > 0) continue;                       /* explicit tag: no block param */
    int blk = nt_ref(nt, id, "block");
    const char *bp0 = blk >= 0 ? block_param_name(c, blk, 0) : NULL;
    if (!bp0) continue;
    LocalVar *lv = scope_local_intern(comp_scope_of(c, id), bp0);
    lv->is_block_param = 1;   /* survives the write-types reset */
    if (lv->type != TY_STRING) { lv->type = TY_STRING; changed = 1; }
  }
  return changed;
}

/* Name of a block's idx-th required parameter, or NULL. */
const char *block_param_name(Compiler *c, int block, int idx) {
  int bp = nt_ref(c->nt, block, "parameters");      /* BlockParametersNode */
  if (bp < 0) return NULL;
  /* numbered block params: `{ _1 }`, `{ it }` → NumberedParametersNode */
  const char *bpty = nt_type(c->nt, bp);
  if (bpty && sp_streq(bpty, "NumberedParametersNode")) {
    int max = (int)nt_int(c->nt, bp, "maximum", 0);
    if (idx >= max) return NULL;
    static const char *names[] = {"_1","_2","_3","_4","_5","_6","_7","_8","_9"};
    return (idx < 9) ? names[idx] : NULL;
  }
  int pn = nt_ref(c->nt, bp, "parameters");          /* ParametersNode */
  if (pn < 0) return NULL;
  int n = 0;
  const int *reqs = nt_arr(c->nt, pn, "requireds", &n);
  if (idx < n) return nt_str(c->nt, reqs[idx], "name");
  return NULL;
}

/* The name of a block's trailing rest parameter (`|*a|`), or NULL if the block
   has none or it is anonymous (`|*|`). The rest collects the arguments past the
   required ones into an array. */
const char *block_rest_name(Compiler *c, int block) {
  int bp = nt_ref(c->nt, block, "parameters");      /* BlockParametersNode */
  if (bp < 0) return NULL;
  const char *bpty = nt_type(c->nt, bp);
  if (bpty && sp_streq(bpty, "NumberedParametersNode")) return NULL;
  int pn = nt_ref(c->nt, bp, "parameters");          /* ParametersNode */
  if (pn < 0) return NULL;
  int rest = nt_ref(c->nt, pn, "rest");
  if (rest < 0) return NULL;
  const char *rty = nt_type(c->nt, rest);
  if (!rty || !sp_streq(rty, "RestParameterNode")) return NULL;  /* must be `*name` */
  return nt_str(c->nt, rest, "name");
}

/* Name of a block's idx-th optional parameter (`|a, b=10|`), or NULL. */
const char *block_opt_name(Compiler *c, int block, int idx) {
  int bp = nt_ref(c->nt, block, "parameters");
  if (bp < 0) return NULL;
  const char *bpty = nt_type(c->nt, bp);
  if (bpty && sp_streq(bpty, "NumberedParametersNode")) return NULL;
  int pn = nt_ref(c->nt, bp, "parameters");
  if (pn < 0) return NULL;
  int n = 0;
  const int *opts = nt_arr(c->nt, pn, "optionals", &n);
  if (idx < n) return nt_str(c->nt, opts[idx], "name");
  return NULL;
}

/* The default-value node of a block's idx-th optional parameter, or -1. */
int block_opt_default(Compiler *c, int block, int idx) {
  int bp = nt_ref(c->nt, block, "parameters");
  if (bp < 0) return -1;
  int pn = nt_ref(c->nt, bp, "parameters");
  if (pn < 0) return -1;
  int n = 0;
  const int *opts = nt_arr(c->nt, pn, "optionals", &n);
  if (idx < n) return nt_ref(c->nt, opts[idx], "value");
  return -1;
}

/* Name of a block's idx-th post-required parameter (`|a, *b, c|` -> c), or NULL. */
const char *block_post_name(Compiler *c, int block, int idx) {
  int bp = nt_ref(c->nt, block, "parameters");
  if (bp < 0) return NULL;
  const char *bpty = nt_type(c->nt, bp);
  if (bpty && sp_streq(bpty, "NumberedParametersNode")) return NULL;
  int pn = nt_ref(c->nt, bp, "parameters");
  if (pn < 0) return NULL;
  int n = 0;
  const int *posts = nt_arr(c->nt, pn, "posts", &n);
  if (idx < n) return nt_str(c->nt, posts[idx], "name");
  return NULL;
}

/* 1 when the block carries ANY rest marker: `*name`, a bare `*`, or the
   implicit rest of a trailing comma (`|a, |`). block_rest_name answers only
   the named form; the distribution (and the auto-splat gate) needs them all. */
int block_rest_marker(Compiler *c, int block) {
  int bp = nt_ref(c->nt, block, "parameters");
  if (bp < 0) return 0;
  const char *bpty = nt_type(c->nt, bp);
  if (bpty && sp_streq(bpty, "NumberedParametersNode")) return 0;
  int pn = nt_ref(c->nt, bp, "parameters");
  if (pn < 0) return 0;
  return nt_ref(c->nt, pn, "rest") >= 0;
}

/* Name of a block's `**kw` keyword-rest parameter, or NULL (also NULL for
   the anonymous `**`). */
const char *block_kwrest_name(Compiler *c, int block) {
  int bp = nt_ref(c->nt, block, "parameters");
  if (bp < 0) return NULL;
  const char *bpty = nt_type(c->nt, bp);
  if (bpty && sp_streq(bpty, "NumberedParametersNode")) return NULL;
  int pn = nt_ref(c->nt, bp, "parameters");
  if (pn < 0) return NULL;
  int kw = nt_ref(c->nt, pn, "keyword_rest");
  if (kw < 0) return NULL;
  const char *kty = nt_type(c->nt, kw);
  if (!kty || !sp_streq(kty, "KeywordRestParameterNode")) return NULL;
  return nt_str(c->nt, kw, "name");
}

/* Name of a block's idx-th keyword parameter (`|a:, b: 5|`), or NULL. */
const char *block_keyword_name(Compiler *c, int block, int idx) {
  int bp = nt_ref(c->nt, block, "parameters");
  if (bp < 0) return NULL;
  const char *bpty = nt_type(c->nt, bp);
  if (bpty && sp_streq(bpty, "NumberedParametersNode")) return NULL;
  int pn = nt_ref(c->nt, bp, "parameters");
  if (pn < 0) return NULL;
  int n = 0;
  const int *kws = nt_arr(c->nt, pn, "keywords", &n);
  if (idx < n) return nt_str(c->nt, kws[idx], "name");
  return NULL;
}

/* Default-value node of a block's idx-th keyword parameter (only present for an
   OptionalKeywordParameterNode `k: 5`), or -1. */
int block_keyword_default(Compiler *c, int block, int idx) {
  int bp = nt_ref(c->nt, block, "parameters");
  if (bp < 0) return -1;
  int pn = nt_ref(c->nt, bp, "parameters");
  if (pn < 0) return -1;
  int n = 0;
  const int *kws = nt_arr(c->nt, pn, "keywords", &n);
  if (idx >= n) return -1;
  const char *kty = nt_type(c->nt, kws[idx]);
  if (kty && sp_streq(kty, "OptionalKeywordParameterNode")) return nt_ref(c->nt, kws[idx], "value");
  return -1;
}

int block_param_is_multi(Compiler *c, int block, int idx) {
  int bp = nt_ref(c->nt, block, "parameters");
  if (bp < 0) return 0;
  int pn = nt_ref(c->nt, bp, "parameters");
  if (pn < 0) return 0;
  int n = 0;
  const int *reqs = nt_arr(c->nt, pn, "requireds", &n);
  if (idx >= n) return 0;
  const char *ty = nt_type(c->nt, reqs[idx]);
  return (ty && sp_streq(ty, "MultiTargetNode"));
}

int block_param_multi_count(Compiler *c, int block, int idx) {
  int bp = nt_ref(c->nt, block, "parameters");
  if (bp < 0) return 0;
  int pn = nt_ref(c->nt, bp, "parameters");
  if (pn < 0) return 0;
  int n = 0;
  const int *reqs = nt_arr(c->nt, pn, "requireds", &n);
  if (idx >= n) return 0;
  int lc = 0;
  nt_arr(c->nt, reqs[idx], "lefts", &lc);
  return lc;
}

const char *block_param_multi_leaf(Compiler *c, int block, int idx, int leaf_idx) {
  int bp = nt_ref(c->nt, block, "parameters");
  if (bp < 0) return NULL;
  int pn = nt_ref(c->nt, bp, "parameters");
  if (pn < 0) return NULL;
  int n = 0;
  const int *reqs = nt_arr(c->nt, pn, "requireds", &n);
  if (idx >= n) return NULL;
  int lc = 0;
  const int *lefts = nt_arr(c->nt, reqs[idx], "lefts", &lc);
  if (!lefts || leaf_idx >= lc) return NULL;
  return nt_str(c->nt, lefts[leaf_idx], "name");
}

/* First YieldNode belonging to scope `si`, or -1. */
int first_yield(Compiler *c, int si) {
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (ty && sp_streq(ty, "YieldNode") && c->nscope[id] == si) return id;
  }
  return -1;
}

/* Arguments node of the first `<&block-param>.call(...)` in scope `si`, or
   -1. Lets block-param inference treat block.call like a yield. */
int first_block_call_args(Compiler *c, int si) {
  Scope *m = &c->scopes[si];
  if (!m->blk_param || !m->blk_param[0]) return -1;
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (!ty || !sp_streq(ty, "CallNode") || c->nscope[id] != si) continue;
    const char *nm = nt_str(c->nt, id, "name");
    if (!nm || !sp_streq(nm, "call")) continue;
    int recv = nt_ref(c->nt, id, "receiver");
    if (recv < 0 || !nt_type(c->nt, recv) || !sp_streq(nt_type(c->nt, recv), "LocalVariableReadNode")) continue;
    const char *rn = nt_str(c->nt, recv, "name");
    if (rn && sp_streq(rn, m->blk_param)) return nt_ref(c->nt, id, "arguments");
  }
  return -1;
}

/* Arguments node of the first receiverless `instance_exec(args, &<blk>)` in
   scope `si` that forwards the scope's own block param, or -1. A receiverless
   instance_exec invokes the block with `args` (self is unchanged by the
   rebind), so it types the block exactly like a yield of `args`. */
int first_ie_exec_args(Compiler *c, int si) {
  Scope *m = &c->scopes[si];
  if (!m->blk_param || !m->blk_param[0]) return -1;
  for (int id = 0; id < c->nt->count; id++) {
    if (c->nscope[id] != si) continue;
    const char *ty = nt_type(c->nt, id);
    if (!ty || !sp_streq(ty, "CallNode") || nt_ref(c->nt, id, "receiver") >= 0) continue;
    const char *nm = nt_str(c->nt, id, "name");
    if (!nm || !sp_streq(nm, "instance_exec")) continue;
    int blk = nt_ref(c->nt, id, "block");
    if (blk < 0 || !nt_type(c->nt, blk) || !sp_streq(nt_type(c->nt, blk), "BlockArgumentNode")) continue;
    int expr = nt_ref(c->nt, blk, "expression");
    if (expr < 0 || !nt_type(c->nt, expr) || !sp_streq(nt_type(c->nt, expr), "LocalVariableReadNode")) continue;
    const char *en = nt_str(c->nt, expr, "name");
    if (en && sp_streq(en, m->blk_param)) return nt_ref(c->nt, id, "arguments");
  }
  return -1;
}

int a_proc_params_node(Compiler *c, int create); /* forward decl */

/* Follow a chain of pure `...` forwarders (a method whose whole body is a
   single `target(...)` call) starting at `mi` until reaching the method that
   actually yields (or owns the &block). Returns that method's index, or -1.
   Lets a block passed to a forwarder be typed from the real yielder's args. */
static int forwarding_yield_target(Compiler *c, int mi, int depth) {
  if (mi < 0 || depth > 16) return -1;
  Scope *m = &c->scopes[mi];
  if (m->yields || (m->blk_param && m->blk_param[0])) return mi;
  int body = m->body;
  if (body < 0 || nt_kind(c->nt, body) != NK_StatementsNode) return -1;
  int n = 0; const int *st = nt_arr(c->nt, body, "body", &n);
  if (n != 1) return -1;
  int call = st[0];
  if (nt_kind(c->nt, call) != NK_CallNode || nt_ref(c->nt, call, "receiver") >= 0) return -1;
  int args = nt_ref(c->nt, call, "arguments");
  int ac = 0; const int *av = args >= 0 ? nt_arr(c->nt, args, "arguments", &ac) : NULL;
  if (ac != 1 || !av || !nt_type(c->nt, av[0]) ||
      !sp_streq(nt_type(c->nt, av[0]), "ForwardingArgumentsNode")) return -1;
  const char *tn = nt_str(c->nt, call, "name");
  if (!tn) return -1;
  int t = comp_method_index(c, tn);
  if (t < 0 && m->class_id >= 0) t = comp_method_in_chain(c, m->class_id, tn, NULL);
  return forwarding_yield_target(c, t, depth + 1);
}

/* Bind block parameter types for supported iteration methods. */
/* Desugar a forwarded callable *value* -- `recv.<iter>(&f)` where `f` is a Proc
   value or a Method object rather than the active inlined &block -- into the
   equivalent literal block `recv.<iter> { |__fwd_k...| f.call(__fwd_k...) }`.
   The existing literal-block emitters then lower it for ANY iterator, instead of
   a per-callable, per-iterator special case. This mirrors Ruby's own `&obj` =>
   `obj.to_proc` model: once a callable is wrapped as a block, it is just a
   block. The synthetic block's param arity and types come from ty_block_yield
   (the builtin block-protocol oracle), so hash `each` (2 params),
   each_with_index, ranges etc. desugar correctly -- not only 1-arg array maps.
   (A `&:sym` block already lowers via its own to_proc path and is left alone.)

   The callable expression is re-evaluated once per element, so this is
   restricted to side-effect-free forms: a local or ivar read, or a `method(:m)`
   call (a deterministic method-object lookup). The active inlined &block (a
   forward whose expression names the enclosing method's block param) is left to
   the inline-forward path. Runs in the inference fixpoint; once a call is
   rewritten its block is a BlockNode, so it is never revisited. */
/* Required-param count of a forwarded callable expression `ex`, or -1 if it
   cannot be determined statically. Chooses the hash-pair calling convention: a
   1-param callable receives the [k,v] pair as one array, a 2-param one is called
   positionally (matching CRuby's proc auto-splat of the yielded pair). */
static int fwd_callable_arity(Compiler *c, int ex) {
  NodeTable *nt = (NodeTable *)c->nt;
  const char *exty = nt_type(nt, ex);
  if (!exty) return -1;
  int create = -1;
  if (sp_streq(exty, "LambdaNode") || is_proc_create(c, ex)) create = ex;
  else if (sp_streq(exty, "LocalVariableReadNode")) {
    const char *vn = nt_str(nt, ex, "name");
    Scope *sc = vn ? comp_scope_of(c, ex) : NULL;
    for (int w = 0; vn && w < nt->count; w++) {
      const char *wty = nt_type(nt, w);
      if (!wty || !sp_streq(wty, "LocalVariableWriteNode")) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != sc) continue;
      int val = nt_ref(nt, w, "value");
      if (val >= 0 && is_proc_create(c, val)) { create = val; break; }
    }
  }
  if (create < 0) return -1;
  int pn = a_proc_params_node(c, create);
  if (pn < 0) return -1;
  int rn = 0; nt_arr(nt, pn, "requireds", &rn);
  return rn;
}

/* A block's `*rest` param always binds an Array (CRuby packs the extra
   yielded values); type every BlockNode rest local poly-array so bodies that
   read it infer correctly on the specialized iterator lowerings too (the
   yield-consumed path already types it as part of its arg-distribution
   analysis; this is idempotent there). Lambdas are excluded -- their typed
   prologue owns its params. */
int type_block_rest_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "BlockNode")) continue;
    const char *rn = block_rest_name(c, id);
    if (!rn || !*rn) continue;
    Scope *bs = comp_scope_of(c, id);
    if (!bs) continue;
    LocalVar *lv = scope_local_intern(bs, rn);
    lv->is_block_param = 1;
    if (lv->type != TY_POLY_ARRAY) { lv->type = TY_POLY_ARRAY; changed = 1; }
  }
  return changed;
}

/* Specialized builtin-iterator lowerings that bind only named block params:
   a block `*rest` param there would bind nil (or emit a misdeclared body), so
   reject loudly. Receivers that resolve to user objects (or unknown) dispatch
   through the yield/invoke path, which binds rest correctly -- only builtin
   container/range/int receivers reach the specialized lowerings. The families
   that DO bind rest (map/collect, select/reject/filter, each/reverse_each,
   each_with_index, times/upto/downto/step, find/detect via their own
   emitters) are deliberately absent from this list. */
void check_block_rest_support(Compiler *c) {
  static const char *const no_rest[] = {
    "flat_map", "collect_concat", "sort_by", "min_by", "max_by", "group_by",
    "partition", "sum", "count", "all?", "any?", "none?", "one?",
    "take_while", "drop_while", "each_with_object", "each_slice", "each_cons",
    "uniq", "chunk_while", "slice_when", "tally", "filter_map", "find_index",
    "index", "cycle", "each_entry", "flat_map!", "sort_by!", "map!",
    "collect!", "select!", "reject!", "keep_if", "delete_if", "bsearch",
    NULL };
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockNode")) continue;
    const char *rn = block_rest_name(c, blk);
    if (!rn || !*rn) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int hit = 0;
    for (int k = 0; no_rest[k]; k++) if (sp_streq(nm, no_rest[k])) { hit = 1; break; }
    if (!hit) continue;
    int recv = nt_ref(nt, id, "receiver");
    TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
    if (!(ty_is_array(rt) || ty_is_hash(rt) || rt == TY_RANGE || rt == TY_INT ||
          rt == TY_ENUMERATOR || rt == TY_STRING)) continue;
    {
      /* strip the internal __bp rename suffix from the reported param name */
      char disp[128]; snprintf(disp, sizeof disp, "%s", rn);
      char *bp = strstr(disp, "__bp"); if (bp) *bp = 0;
      fprintf(stderr, "spinel: %s:%d: a block splat parameter (*%s) is not supported by the `%s` lowering\n",
              nt->source_file ? nt->source_file : "source.rb",
              (int)nt_int(nt, id, "node_line", 0), disp, nm);
    }
    exit(1);
  }
}

/* `send(:m, args)` / `__send__("m", args)` / `public_send(:m, args)` with NO
   explicit receiver -> a direct implicit-self call to `m` with the remaining
   args. The literal symbol/string name resolves statically, the same model as
   the textual `recv.send(:m)` receiver rewrite in spinel_parse.c; a non-literal
   name (`send(meth)`) has no static target and is left alone. Done on the AST,
   not textually, so a `send(:` inside a string or comment can't be mis-matched
   (the bare token has no `.` anchor). #1261. */
/* `recv.public_method(:sym)` -> `recv.method(:sym)`: the same bound Method,
   reusing all the `method(:sym)` machinery (reachability, arity, call). The
   private/protected visibility distinction is not modeled. #2687 */
/* Does any node under `root` have kind `k`? Descends every ref/arr field. */
static int subtree_has_kind(const NodeTable *nt, int root, NodeKind k, int depth) {
  if (root < 0 || root >= nt->count || depth > 200) return 0;
  if (nt_kind(nt, root) == k) return 1;
  const SpNode *nd = &nt->nodes[root];
  for (int i = 0; i < nd->nr; i++)
    if (subtree_has_kind(nt, nd->r[i].ref, k, depth + 1)) return 1;
  for (int i = 0; i < nd->na; i++)
    for (int j = 0; j < nd->a[i].n; j++)
      if (subtree_has_kind(nt, nd->a[i].ids[j], k, depth + 1)) return 1;
  return 0;
}

/* Repoint every SelfNode under `root` at a fresh ConstantReadNode(cname): the
   PARENT's ref/arr slot is retargeted (a node's type string is immutable).
   Does not descend into nested class/module/def bodies, whose self differs. */
static void subtree_self_to_const(Compiler *c, int root, const char *cname, int depth) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (root < 0 || root >= nt->count || depth > 200) return;
  NodeKind k = nt_kind(nt, root);
  if (k == NK_ClassNode || k == NK_ModuleNode || k == NK_DefNode) return;
  SpNode *nd = &nt->nodes[root];
  for (int i = 0; i < nd->nr; i++) {
    int ch = nd->r[i].ref;
    if (ch >= 0 && nt_kind(nt, ch) == NK_SelfNode) {
      int cr = nt_new_node(nt, "ConstantReadNode");
      if (cr < 0) return;
      nt_node_set_str(nt, cr, "name", cname);
      comp_grow_node_arrays(c);
      c->nscope[cr] = c->nscope[root];
      nd = &nt->nodes[root];        /* nt_new_node may realloc nt->nodes */
      nd->r[i].ref = cr;
    }
    else subtree_self_to_const(c, ch, cname, depth + 1);
    nd = &nt->nodes[root];
  }
  for (int i = 0; i < nd->na; i++) {
    for (int j = 0; j < nd->a[i].n; j++) {
      int ch = nd->a[i].ids[j];
      if (ch >= 0 && nt_kind(nt, ch) == NK_SelfNode) {
        int cr = nt_new_node(nt, "ConstantReadNode");
        if (cr < 0) return;
        nt_node_set_str(nt, cr, "name", cname);
        comp_grow_node_arrays(c);
        c->nscope[cr] = c->nscope[root];
        nd = &nt->nodes[root];
        nd->a[i].ids[j] = cr;
      }
      else subtree_self_to_const(c, ch, cname, depth + 1);
      nd = &nt->nodes[root];
    }
  }
}

/* The value forms of class_eval / class_exec (and module_*): the block is
   evaluated with self = the class, and the call's value is the block's. A
   pure-def body is a compile-time reopen (class_eval_reopen_class) and is left
   to that path; any def-containing body stays out. For a value body, rewrite

     C.class_eval { body }      ->  (-> () { body }).call
     C.class_exec(a) { |x| b }  ->  (->(x) { b }).call(a)

   reusing the block's own parameters/body nodes, with SelfNode occurrences in
   the body repointed at the receiver constant. #2697 */
int desugar_class_eval_value(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "class_eval") && !sp_streq(nm, "class_exec") &&
                !sp_streq(nm, "module_eval") && !sp_streq(nm, "module_exec"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_ConstantReadNode) continue;
    const char *cname = nt_str(nt, recv, "name");
    if (!cname || comp_class_index(c, cname) < 0) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) continue;
    int body = nt_ref(nt, blk, "body");
    if (body < 0) continue;
    if (subtree_has_kind(nt, body, NK_DefNode, 0)) continue;   /* reopen path */

    /* the block's params bind the exec args; collect their names */
    const char *pnames[8]; int pvals[8]; int np = 0;
    {
      int bparams = nt_ref(nt, blk, "parameters");
      int params = bparams >= 0 ? nt_ref(nt, bparams, "parameters") : -1;
      int rn = 0; const int *reqs = params >= 0 ? nt_arr(nt, params, "requireds", &rn) : NULL;
      int an2 = nt_ref(nt, id, "arguments");
      int ac2 = 0; const int *av2 = an2 >= 0 ? nt_arr(nt, an2, "arguments", &ac2) : NULL;
      if (rn > 8 || rn != ac2) continue;   /* arity mismatch / exotic params: leave it */
      int ok = 1;
      for (int k = 0; k < rn && ok; k++) {
        pnames[k] = nt_str(nt, reqs[k], "name");
        pvals[k] = av2[k];
        if (!pnames[k]) ok = 0;
      }
      /* NOTE on shadowing: spinel interns block params in the ENCLOSING scope
         (a known representation), so the spliced `param = arg` write binds the
         same slot the block body already reads -- exactly a regular block's
         behavior here. */
      if (!ok) continue;
      np = rn;
    }

    subtree_self_to_const(c, body, cname, 0);
    int base = nt->count;
    /* `param = arg` writes prepended to the body, then the body statements */
    int items[8 + 64]; int ni = 0;
    for (int k = 0; k < np; k++) {
      int w = nt_new_node(nt, "LocalVariableWriteNode");
      if (w < 0) { ni = -1; break; }
      nt_node_set_str(nt, w, "name", pnames[k]);
      nt_node_set_ref(nt, w, "value", pvals[k]);
      items[ni++] = w;
    }
    if (ni < 0) continue;
    {
      int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
      if (bn > 64) continue;
      for (int k = 0; k < bn; k++) items[ni++] = bb[k];
    }
    int stmts = nt_new_node(nt, "StatementsNode");
    /* a ParenthesesNode, not a BeginNode: begin carries its own exception
       region, which swallows a raise out of the spliced body before the
       enclosing handler sees it -- parens are the plain multi-statement
       expression (#2723) */
    int beg = nt_new_node(nt, "ParenthesesNode");
    if (stmts < 0 || beg < 0) continue;
    nt_node_set_arr(nt, stmts, "body", items, ni);
    nt_node_set_ref(nt, beg, "body", stmts);
    /* the call becomes a transparent alias of the begin's value */
    nt_node_set_str(nt, id, "name", "itself");
    nt_node_set_ref(nt, id, "receiver", beg);
    nt_node_set_ref(nt, id, "block", -1);
    nt_node_set_ref(nt, id, "arguments", -1);

    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    /* splice the body into the enclosing scope: re-home every node that lived
       in the BLOCK's scope (nested blocks keep their own) */
    {
      Scope *bs = comp_scope_of(c, body);
      int stack[256]; int sp = 0; stack[sp++] = body;
      int bsi = bs ? (int)(bs - c->scopes) : -1;
      while (sp > 0 && bsi >= 0) {
        int nid = stack[--sp];
        if (nid < 0 || nid >= nt->count) continue;
        if (c->nscope[nid] == bsi) c->nscope[nid] = encl;
        const SpNode *nd = &nt->nodes[nid];
        for (int i2 = 0; i2 < nd->nr && sp < 250; i2++) stack[sp++] = nd->r[i2].ref;
        for (int i2 = 0; i2 < nd->na; i2++)
          for (int j2 = 0; j2 < nd->a[i2].n && sp < 250; j2++) stack[sp++] = nd->a[i2].ids[j2];
      }
    }
    changed = 1;
  }
  return changed;
}

/* Kernel functions spinel dispatches globally: a receiverless call to one of
   these inside an instance_eval splice must stay receiverless (CRuby finds
   them through the receiver's ancestry via Kernel; spinel's equivalents are
   free functions). */
static int ie_kernel_global(const char *n) {
  static const char *const K[] = {
    "puts", "print", "p", "pp", "warn", "raise", "require", "require_relative",
    "printf", "sprintf", "format", "rand", "srand", "sleep", "exit", "abort",
    "loop", "lambda", "proc", "catch", "throw", "gets", "binding",
    "block_given?", "at_exit", "caller", "freeze", "frozen?", NULL };
  for (int i = 0; K[i]; i++) if (sp_streq(n, K[i])) return 1;
  return 0;
}

/* Repoint self and receiverless calls in an instance_eval body at the bound
   receiver temp: SelfNode becomes a read of `tmp`, and a receiverless CallNode
   (other than a Kernel global) gains `tmp` as its receiver -- instance_eval
   dispatches those on the new self. Skips nested class/module/def bodies. */
/* Whether the subtree reads (or writes) local `name` anywhere: an unused
   instance_eval/exec block param must not get a synthesized binding, since an
   unread local never gets a C declaration (#2734). */
static int ie_subtree_uses_local(const NodeTable *nt, int root, const char *name, int depth) {
  if (root < 0 || depth > 200) return 0;
  const char *ty = nt_type(nt, root);
  if (ty && (sp_streq(ty, "LocalVariableReadNode") ||
             sp_streq(ty, "LocalVariableWriteNode") ||
             sp_streq(ty, "LocalVariableTargetNode") ||
             sp_streq(ty, "LocalVariableOperatorWriteNode") ||
             sp_streq(ty, "LocalVariableOrWriteNode") ||
             sp_streq(ty, "LocalVariableAndWriteNode"))) {
    const char *nm = nt_str(nt, root, "name");
    if (nm && sp_streq(nm, name)) return 1;
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++)
    if (ie_subtree_uses_local(nt, nt_ref_at(nt, root, i), name, depth + 1)) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, root, i, &n);
    for (int k = 0; k < n; k++)
      if (ie_subtree_uses_local(nt, ids[k], name, depth + 1)) return 1;
  }
  return 0;
}
static void ie_subtree_retarget(Compiler *c, int root, const char *tmp, int depth) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (root < 0 || root >= nt->count || depth > 200) return;
  NodeKind k = nt_kind(nt, root);
  if (k == NK_ClassNode || k == NK_ModuleNode || k == NK_DefNode) return;
  if (k == NK_CallNode) {
    const char *nm = nt_str(nt, root, "name");
    if (nm && nt_ref(nt, root, "receiver") < 0 && !ie_kernel_global(nm)) {
      int rd = nt_new_node(nt, "LocalVariableReadNode");
      if (rd < 0) return;
      nt_node_set_str(nt, rd, "name", tmp);
      comp_grow_node_arrays(c);
      c->nscope[rd] = c->nscope[root];
      nt_node_set_ref(nt, root, "receiver", rd);
      nt_node_set_int(nt, root, "vcall", 0);   /* no longer a bare identifier */
    }
  }
  SpNode *nd = &nt->nodes[root];
  for (int i = 0; i < nd->nr; i++) {
    int ch = nd->r[i].ref;
    if (ch >= 0 && nt_kind(nt, ch) == NK_SelfNode) {
      int rd = nt_new_node(nt, "LocalVariableReadNode");
      if (rd < 0) return;
      nt_node_set_str(nt, rd, "name", tmp);
      comp_grow_node_arrays(c);
      c->nscope[rd] = c->nscope[root];
      nd = &nt->nodes[root];
      nd->r[i].ref = rd;
    }
    else ie_subtree_retarget(c, ch, tmp, depth + 1);
    nd = &nt->nodes[root];
  }
  for (int i = 0; i < nd->na; i++) {
    for (int j = 0; j < nd->a[i].n; j++) {
      int ch = nd->a[i].ids[j];
      if (ch >= 0 && nt_kind(nt, ch) == NK_SelfNode) {
        int rd = nt_new_node(nt, "LocalVariableReadNode");
        if (rd < 0) return;
        nt_node_set_str(nt, rd, "name", tmp);
        comp_grow_node_arrays(c);
        c->nscope[rd] = c->nscope[root];
        nd = &nt->nodes[root];
        nd->a[i].ids[j] = rd;
      }
      else ie_subtree_retarget(c, ch, tmp, depth + 1);
      nd = &nt->nodes[root];
    }
  }
}

/* instance_eval / instance_exec with a block on a BUILTIN receiver: splice the
   body inline with self bound to a temp (#2634). User-object receivers keep
   the dedicated codegen path (ie_direct), which handles their ivars/methods.

     "abc".instance_eval { upcase }        ->  begin __ieN = "abc"; __ieN.upcase end
     "xy".instance_exec(3) { |n| self*n }  ->  begin n = 3; __ieN = "xy"; __ieN*__ieN? .. end

   instance_eval also yields self to a sole block param. def-containing bodies
   are singleton definitions and stay out (the documented dsm limit). */
int desugar_instance_eval_builtin(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "instance_eval") && !sp_streq(nm, "instance_exec"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    TyKind rt = infer_type(c, recv);
    /* builtin value receivers only; user objects ride ie_direct, and an
       unresolved receiver may still become one */
    if (!(rt == TY_STRING || rt == TY_INT || rt == TY_FLOAT || rt == TY_SYMBOL ||
          rt == TY_BOOL || rt == TY_RANGE || rt == TY_TIME || rt == TY_REGEX ||
          rt == TY_COMPLEX || rt == TY_RATIONAL ||
          ty_is_array(rt) || ty_is_hash(rt))) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) continue;
    int body = nt_ref(nt, blk, "body");
    if (body < 0) continue;
    if (subtree_has_kind(nt, body, NK_DefNode, 0)) continue;

    int is_exec = sp_streq(nm, "instance_exec");
    const char *pnames[8]; int np = 0;
    {
      int bparams = nt_ref(nt, blk, "parameters");
      int params = bparams >= 0 ? nt_ref(nt, bparams, "parameters") : -1;
      int rn = 0; const int *reqs = params >= 0 ? nt_arr(nt, params, "requireds", &rn) : NULL;
      int an2 = nt_ref(nt, id, "arguments");
      int ac2 = 0; nt_arr(nt, an2 >= 0 ? an2 : -1, "arguments", &ac2);
      if (is_exec) { if (rn > 8 || rn != ac2) continue; }
      else { if (ac2 != 0 || rn > 1) continue; }   /* instance_eval { |s| }: s = self */
      int ok = 1;
      for (int k = 0; k < rn && ok; k++)
        if (!(pnames[k] = nt_str(nt, reqs[k], "name"))) ok = 0;
      if (!ok) continue;
      np = rn;
    }

    char tmp[32];
    snprintf(tmp, sizeof tmp, "__ie_%d", id);
    /* the temp is a synthesized local: scope construction already ran, so
       intern it (typed as the receiver) or no declaration is ever emitted */
    {
      Scope *es = comp_scope_of(c, id);
      LocalVar *lv = es ? scope_local_intern(es, tmp) : NULL;
      if (!lv) continue;
      lv->type = rt;
      /* seeded: the temp's type IS the receiver's static type, and the write
         pass's per-run reset must not wipe it -- node order processes users of
         the temp before its own (late, synthesized) write, so an unseeded temp
         would strand every user at UNKNOWN forever (#2723) */
      lv->rbs_seeded = 1;
    }
    ie_subtree_retarget(c, body, tmp, 0);

    int base = nt->count;
    int items[8 + 66]; int ni = 0;
    /* bind the receiver first (evaluated once), then the params */
    int wrecv = nt_new_node(nt, "LocalVariableWriteNode");
    if (wrecv < 0) continue;
    nt_node_set_str(nt, wrecv, "name", tmp);
    nt_node_set_ref(nt, wrecv, "value", recv);
    items[ni++] = wrecv;
    if (is_exec) {
      int an2 = nt_ref(nt, id, "arguments");
      int ac2 = 0; const int *av2 = nt_arr(nt, an2, "arguments", &ac2);
      for (int k = 0; k < np; k++) {
        /* an unused param never gets a C declaration; splice the arg bare so
           its side effects still run (#2734) */
        if (!ie_subtree_uses_local(nt, body, pnames[k], 0)) { items[ni++] = av2[k]; continue; }
        int w = nt_new_node(nt, "LocalVariableWriteNode");
        if (w < 0) { ni = -1; break; }
        nt_node_set_str(nt, w, "name", pnames[k]);
        nt_node_set_ref(nt, w, "value", av2[k]);
        items[ni++] = w;
      }
    }
    else if (np == 1 && ie_subtree_uses_local(nt, body, pnames[0], 0)) {
      /* instance_eval yields self to the sole param; an unused param binds
         nothing (#2734) */
      int rd = nt_new_node(nt, "LocalVariableReadNode");
      int w = nt_new_node(nt, "LocalVariableWriteNode");
      if (rd < 0 || w < 0) continue;
      nt_node_set_str(nt, rd, "name", tmp);
      nt_node_set_str(nt, w, "name", pnames[0]);
      nt_node_set_ref(nt, w, "value", rd);
      items[ni++] = w;
    }
    if (ni < 0) continue;
    {
      int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
      if (bn > 64) continue;
      for (int k = 0; k < bn; k++) items[ni++] = bb[k];
    }
    int stmts = nt_new_node(nt, "StatementsNode");
    /* a ParenthesesNode, not a BeginNode: begin carries its own exception
       region, which swallows a raise out of the spliced body before the
       enclosing handler sees it -- parens are the plain multi-statement
       expression (#2723) */
    int beg = nt_new_node(nt, "ParenthesesNode");
    if (stmts < 0 || beg < 0) continue;
    nt_node_set_arr(nt, stmts, "body", items, ni);
    nt_node_set_ref(nt, beg, "body", stmts);
    nt_node_set_str(nt, id, "name", "itself");
    nt_node_set_ref(nt, id, "receiver", beg);
    nt_node_set_ref(nt, id, "block", -1);
    nt_node_set_ref(nt, id, "arguments", -1);

    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    {
      Scope *bs = comp_scope_of(c, body);
      int stack[256]; int sp = 0; stack[sp++] = body;
      int bsi = bs ? (int)(bs - c->scopes) : -1;
      while (sp > 0 && bsi >= 0) {
        int nid = stack[--sp];
        if (nid < 0 || nid >= nt->count) continue;
        if (c->nscope[nid] == bsi) c->nscope[nid] = encl;
        const SpNode *nd = &nt->nodes[nid];
        for (int i2 = 0; i2 < nd->nr && sp < 250; i2++) stack[sp++] = nd->r[i2].ref;
        for (int i2 = 0; i2 < nd->na; i2++)
          for (int j2 = 0; j2 < nd->a[i2].n && sp < 250; j2++) stack[sp++] = nd->a[i2].ids[j2];
      }
    }
    changed = 1;
  }
  return changed;
}

/* A method call on a local statically holding one BUILTIN class constant
   dispatches like the constant itself: retarget the receiver at the AST so
   `k = Array; k.new(3, 0)` rides every Array.new arm (#2715). User classes
   already resolve through class_var_static_ci at the dispatch sites. */
static unsigned bcv_key_hash(const char *name, const Scope *sc) {
  unsigned h = 5381;
  for (const char *p = name; *p; p++) h = h * 33u + (unsigned char)*p;
  size_t s = (size_t)(const void *)sc;
  for (unsigned b = 0; b < sizeof s; b++) h = h * 33u + (unsigned char)(s >> (b * 8));
  return h;
}

int desugar_builtin_class_var_recv(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;

  /* Index every LocalVariableWriteNode by (variable name, scope) once, so
     resolving a receiver's static class scans only the writes that could
     actually bind it instead of the whole node table per receiver. Turns the
     pass from O(receivers * N) into O(N) -- the quadratic that stalled the
     lobsters tree (#3115). Scope belongs in the key because a hot name reused
     across many scopes otherwise leaves one long chain whose every entry needs
     its scope resolved. Inlines the old builtin_class_var_static_name
     resolution over the index. */
  int nbuckets = 16;
  while (nbuckets < n0) nbuckets <<= 1;
  int *head = malloc((size_t)nbuckets * sizeof(int));
  int *wnext = malloc((size_t)(n0 > 0 ? n0 : 1) * sizeof(int));
  if (!head || !wnext) { free(head); free(wnext); return 0; }
  for (int i = 0; i < nbuckets; i++) head[i] = -1;
  unsigned mask = (unsigned)nbuckets - 1;
  for (int w = 0; w < n0; w++) {
    if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
    const char *wn = nt_str(nt, w, "name");
    if (!wn) continue;
    unsigned h = bcv_key_hash(wn, comp_scope_of(c, w)) & mask;
    wnext[w] = head[h];
    head[h] = w;
  }

  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_LocalVariableReadNode) continue;
    const char *vn = nt_str(nt, recv, "name");
    if (!vn) continue;
    Scope *sc = comp_scope_of(c, recv);
    unsigned h = bcv_key_hash(vn, sc) & mask;
    /* every write of this local in this scope must assign the SAME builtin (or
       user-class) constant, else the local is dynamic and does not retarget */
    const char *cn = NULL;
    int bail = 0;
    for (int w = head[h]; w >= 0; w = wnext[w]) {
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != sc) continue;
      int val = nt_ref(nt, w, "value");
      const char *vcn = (val >= 0 && nt_kind(nt, val) == NK_ConstantReadNode)
                        ? nt_str(nt, val, "name") : NULL;
      if (!vcn || !(is_builtin_class_name(vcn) || comp_class_index(c, vcn) >= 0)) { bail = 1; break; }
      if (cn && !sp_streq(cn, vcn)) { bail = 1; break; }
      cn = vcn;
    }
    if (bail || !cn) continue;
    int cr = nt_new_node(nt, "ConstantReadNode");
    if (cr < 0) continue;
    nt_node_set_str(nt, cr, "name", cn);
    comp_grow_node_arrays(c);
    c->nscope[cr] = c->nscope[id];
    nt_node_set_ref(nt, id, "receiver", cr);
    changed = 1;
  }
  free(head); free(wnext);
  return changed;
}

/* Proc#>> / #<< with a Method operand: wrap the Method side in #to_proc at the
   AST, so composition always runs proc-to-proc. The to_proc emission builds a
   real trampoline proc that publishes its boxed result through the return
   slot; the raw sp_method_to_proc tramp does not, which is why composing the
   Method directly mis-typed the intermediate (#2692). */
int desugar_compose_method_operand(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, ">>") && !sp_streq(nm, "<<"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (recv < 0 || an != 1 || !av) continue;
    TyKind rt = infer_type(c, recv), at = infer_type(c, av[0]);
    int r_m = rt == TY_METHOD, a_m = at == TY_METHOD;
    if (!r_m && !a_m) continue;
    if (!(rt == TY_METHOD || rt == TY_PROC) || !(at == TY_METHOD || at == TY_PROC)) continue;
    int a0 = av[0];
    int base = nt->count;
    if (r_m) {
      int tp = nt_new_node(nt, "CallNode");
      if (tp < 0) continue;
      nt_node_set_ref(nt, tp, "receiver", recv);
      nt_node_set_str(nt, tp, "name", "to_proc");
      nt_node_set_ref(nt, tp, "arguments", -1);
      nt_node_set_ref(nt, tp, "block", -1);
      nt_node_set_ref(nt, id, "receiver", tp);
    }
    if (a_m) {
      int tp = nt_new_node(nt, "CallNode");
      int na = nt_new_node(nt, "ArgumentsNode");
      if (tp < 0 || na < 0) continue;
      nt_node_set_ref(nt, tp, "receiver", a0);
      nt_node_set_str(nt, tp, "name", "to_proc");
      nt_node_set_ref(nt, tp, "arguments", -1);
      nt_node_set_ref(nt, tp, "block", -1);
      nt_node_set_arr(nt, na, "arguments", &tp, 1);
      nt_node_set_ref(nt, id, "arguments", na);
    }
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* A user `<=>` shared by multiple classes (a Comparable base with several
   subclasses) must take its operand boxed: the cmp-hook dispatch hands it
   any sibling in the hierarchy, and a monomorphic call site would otherwise
   specialize the param to one subclass, failing cross-subclass sorts closed
   (and calling that subclass's methods on a sibling cast). */
int widen_shared_cmp_params(Compiler *c) {
  int changed = 0;
  for (int mi = 0; mi < c->nscopes; mi++) {
    Scope *m = &c->scopes[mi];
    if (!m->name || !sp_streq(m->name, "<=>") || m->class_id < 0) continue;
    if (m->nparams < 1) continue;
    int users = 0;
    for (int k = 0; k < c->nclasses && users < 2; k++)
      if (comp_method_in_chain(c, k, "<=>", NULL) == mi) users++;
    if (users < 2) continue;
    LocalVar *p = scope_local(m, m->pnames[0]);
    if (p && p->type != TY_POLY) { p->type = TY_POLY; changed = 1; }
  }
  return changed;
}

/* method.curry -> method.to_proc.curry: the Proc curry machinery (arity
   typing, boxed accumulation, param widening) then applies unchanged. The
   rewrite fires once: afterwards curry's receiver is the synthesized
   to_proc call, which infers TY_PROC. */
int desugar_method_curry(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "curry")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    if (infer_type(c, recv) != TY_METHOD) continue;
    int base = nt->count;
    int tp = nt_new_node(nt, "CallNode");
    if (tp < 0) continue;
    nt_node_set_ref(nt, tp, "receiver", recv);
    nt_node_set_str(nt, tp, "name", "to_proc");
    nt_node_set_ref(nt, tp, "arguments", -1);
    nt_node_set_ref(nt, tp, "block", -1);
    nt_node_set_ref(nt, id, "receiver", tp);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* n.times.with_index / upto / downto (and with_object/each_with_index):
   a blockless Integer enumerator types as a range, which has no with_index
   arm. Interpose `.each` -- range.each stays an external Enumerator ahead
   of these chains, whose machinery already serves both the blockless and
   the block forms. Fires once: afterwards the receiver is the each call. */
int desugar_int_enum_with_index(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "with_index") && !sp_streq(nm, "with_object") &&
                !sp_streq(nm, "each_with_index"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_CallNode) continue;
    if (nt_ref(nt, recv, "block") >= 0) continue;
    const char *rnm = nt_str(nt, recv, "name");
    if (!rnm || (!sp_streq(rnm, "times") && !sp_streq(rnm, "upto") &&
                 !sp_streq(rnm, "downto"))) continue;
    if (infer_type(c, recv) != TY_RANGE) continue;
    int base = nt->count;
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0 && (sp_streq(nm, "with_index") || sp_streq(nm, "each_with_index"))) {
      /* Block form returns the Integer RECEIVER (CRuby: the enumerator's
         underlying each return), not the range: hoist the receiver into a
         temp, run the chain for effect, and make the original call a
         transparent `.itself` on `(t = n; t.times.each.with_index {..}; t)`
         so the value is the int evaluated once. */
      int ircv = nt_ref(nt, recv, "receiver");
      if (ircv < 0) continue;
      char tmpn[32]; snprintf(tmpn, sizeof tmpn, "_spwi%d", id);
      /* register_locals already ran: intern the temp into the enclosing
         scope now; its type comes from the following inference passes. */
      { int encl0 = c->nscope[id];
        if (encl0 >= 0 && encl0 < c->nscopes)
          scope_local_intern(&c->scopes[encl0], tmpn); }
      int w = nt_new_node(nt, "LocalVariableWriteNode");
      int rd1 = nt_new_node(nt, "LocalVariableReadNode");
      int rd2 = nt_new_node(nt, "LocalVariableReadNode");
      int ec = nt_new_node(nt, "CallNode");
      int inner = nt_new_node(nt, "CallNode");
      int stmts = nt_new_node(nt, "StatementsNode");
      int paren = nt_new_node(nt, "ParenthesesNode");
      if (w < 0 || rd1 < 0 || rd2 < 0 || ec < 0 || inner < 0 ||
          stmts < 0 || paren < 0) continue;
      nt_node_set_str(nt, w, "name", tmpn);
      nt_node_set_ref(nt, w, "value", ircv);
      nt_node_set_str(nt, rd1, "name", tmpn);
      nt_node_set_str(nt, rd2, "name", tmpn);
      nt_node_set_ref(nt, recv, "receiver", rd1);
      nt_node_set_ref(nt, ec, "receiver", recv);
      nt_node_set_str(nt, ec, "name", "each");
      nt_node_set_ref(nt, ec, "arguments", -1);
      nt_node_set_ref(nt, ec, "block", -1);
      nt_node_set_str(nt, inner, "name", nm);
      nt_node_set_ref(nt, inner, "receiver", ec);
      nt_node_set_ref(nt, inner, "arguments", nt_ref(nt, id, "arguments"));
      nt_node_set_ref(nt, inner, "block", blk);
      { int items[3] = { w, inner, rd2 };
        nt_node_set_arr(nt, stmts, "body", items, 3); }
      nt_node_set_ref(nt, paren, "body", stmts);
      nt_node_set_str(nt, id, "name", "itself");
      nt_node_set_ref(nt, id, "receiver", paren);
      nt_node_set_ref(nt, id, "block", -1);
      nt_node_set_ref(nt, id, "arguments", -1);
    }
    else {
      int ec = nt_new_node(nt, "CallNode");
      if (ec < 0) continue;
      nt_node_set_ref(nt, ec, "receiver", recv);
      nt_node_set_str(nt, ec, "name", "each");
      nt_node_set_ref(nt, ec, "arguments", -1);
      nt_node_set_ref(nt, ec, "block", -1);
      nt_node_set_ref(nt, id, "receiver", ec);
    }
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* reduce(&pr) / inject(init, &pr): forward the proc through a literal
   two-param block calling it -- `{ |__fa, __fb| pr.call(__fa, __fb) }` -- so
   the fold machinery sees an ordinary block (#2684). The fold emitter places
   the proc call's prelude inside the loop (see emit_reduce_block_expr). */
/* Does the subtree contain any of the given kinds? */
static int subtree_has_any_kind(const NodeTable *nt, int root, const NodeKind *ks, int nk, int depth) {
  if (root < 0 || root >= nt->count || depth > 200) return 0;
  NodeKind k = nt_kind(nt, root);
  for (int i = 0; i < nk; i++) if (k == ks[i]) return 1;
  const SpNode *nd = &nt->nodes[root];
  for (int i = 0; i < nd->nr; i++)
    if (subtree_has_any_kind(nt, nd->r[i].ref, ks, nk, depth + 1)) return 1;
  for (int i = 0; i < nd->na; i++)
    for (int j = 0; j < nd->a[i].n; j++)
      if (subtree_has_any_kind(nt, nd->a[i].ids[j], ks, nk, depth + 1)) return 1;
  return 0;
}

/* Does the subtree read local `nm` under a proc-create (a capture)? */
static int subtree_proc_captures_name(Compiler *c, int root, const char *nm, int in_proc, int depth) {
  const NodeTable *nt = c->nt;
  if (root < 0 || root >= nt->count || depth > 200) return 0;
  int now_proc = in_proc || is_proc_create(c, root);
  if (now_proc && nt_kind(nt, root) == NK_LocalVariableReadNode) {
    const char *rn = nt_str(nt, root, "name");
    if (rn && sp_streq(rn, nm)) return 1;
  }
  const SpNode *nd = &nt->nodes[root];
  for (int i = 0; i < nd->nr; i++)
    if (subtree_proc_captures_name(c, nd->r[i].ref, nm, now_proc, depth + 1)) return 1;
  for (int i = 0; i < nd->na; i++)
    for (int j = 0; j < nd->a[i].n; j++)
      if (subtree_proc_captures_name(c, nd->a[i].ids[j], nm, now_proc, depth + 1)) return 1;
  return 0;
}

/* Rename every local read/write of `oldn` under `root` to `newn`. */
static void subtree_rename_local(NodeTable *nt, int root, const char *oldn, const char *newn, int depth) {
  if (root < 0 || root >= nt->count || depth > 200) return;
  NodeKind k = nt_kind(nt, root);
  if (k == NK_LocalVariableReadNode || k == NK_LocalVariableWriteNode ||
      k == NK_LocalVariableOrWriteNode || k == NK_LocalVariableAndWriteNode ||
      k == NK_LocalVariableOperatorWriteNode || k == NK_LocalVariableTargetNode) {
    const char *nm = nt_str(nt, root, "name");
    if (nm && sp_streq(nm, oldn)) nt_node_set_str(nt, root, "name", newn);
  }
  const SpNode *nd = &nt->nodes[root];
  for (int i = 0; i < nd->nr; i++) subtree_rename_local(nt, nd->r[i].ref, oldn, newn, depth + 1);
  for (int i = 0; i < nd->na; i++)
    for (int j = 0; j < nd->a[i].n; j++) subtree_rename_local(nt, nd->a[i].ids[j], oldn, newn, depth + 1);
}

/* An INLINED iterator block whose param is captured by a proc it creates:
   `[1,2].map { |i| ->{ i } }`. The block's binding lives in the loop and each
   iteration must capture a FRESH cell, so wrap the body in an immediately-
   called lambda that owns the variable -- `{ |i| (->(i){ body }).call(i) }` --
   and the enclosing-proc-param capture machinery does the rest (#2648).
   Bodies with control flow that must reach the ITERATION (next/break/return/
   redo) are left alone: inside the wrapper they would bind to the lambda. */
int desugar_block_capture_wrap(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    if (is_proc_create(c, id)) continue;             /* a proc literal is not an iterator */
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) continue;
    if (nt_int(nt, blk, "cap_wrapped", 0)) continue;   /* fixpoint: wrap once */
    int body = nt_ref(nt, blk, "body");
    int bparams = nt_ref(nt, blk, "parameters");
    int params = bparams >= 0 ? nt_ref(nt, bparams, "parameters") : -1;
    int rn = 0; const int *reqs = params >= 0 ? nt_arr(nt, params, "requireds", &rn) : NULL;
    if (body < 0 || rn < 1 || rn > 4 || !reqs) continue;
    /* only fire when a proc INSIDE the body captures one of the block params */
    int captured = 0;
    const char *pn[4];
    for (int k = 0; k < rn; k++) {
      pn[k] = nt_str(nt, reqs[k], "name");
      if (!pn[k]) { captured = -1; break; }
      if (subtree_proc_captures_name(c, body, pn[k], 0, 0)) captured = 1;
    }
    if (captured != 1) continue;
    static const NodeKind bad[] = { NK_NextNode, NK_BreakNode, NK_ReturnNode, NK_RedoNode, NK_YieldNode };
    if (subtree_has_any_kind(nt, body, bad, 5, 0)) continue;

    int base = nt->count;
    /* The wrapper's params get FRESH names and the body's references are
       renamed to them: the scope table is shared per-name, so celling the
       original name would also derail the outer loop's own plain binding. */
    char wn[4][48];
    int wreqs[4], wreads[4]; int ok = 1;
    for (int k = 0; k < rn && ok; k++) {
      snprintf(wn[k], sizeof wn[k], "__cap_%d_%s", id, pn[k]);
      wreqs[k] = nt_new_node(nt, "RequiredParameterNode");
      wreads[k] = nt_new_node(nt, "LocalVariableReadNode");
      if (wreqs[k] < 0 || wreads[k] < 0) { ok = 0; break; }
      nt_node_set_str(nt, wreqs[k], "name", wn[k]);
      nt_node_set_str(nt, wreads[k], "name", pn[k]);   /* call arg reads the ORIGINAL */
    }
    if (ok) for (int k = 0; k < rn; k++) subtree_rename_local(nt, body, pn[k], wn[k], 0);
    if (!ok) continue;
    int wparams = nt_new_node(nt, "ParametersNode");
    int wlam = nt_new_node(nt, "LambdaNode");
    int wargs = nt_new_node(nt, "ArgumentsNode");
    int wcall = nt_new_node(nt, "CallNode");
    int nbody = nt_new_node(nt, "StatementsNode");
    if (wparams < 0 || wlam < 0 || wargs < 0 || wcall < 0 || nbody < 0) continue;
    nt_node_set_arr(nt, wparams, "requireds", wreqs, rn);
    /* a LambdaNode's "parameters" IS the ParametersNode (see a_proc_params_node) */
    nt_node_set_ref(nt, wlam, "parameters", wparams);
    nt_node_set_ref(nt, wlam, "body", body);
    nt_node_set_arr(nt, wargs, "arguments", wreads, rn);
    nt_node_set_ref(nt, wcall, "receiver", wlam);
    nt_node_set_str(nt, wcall, "name", "call");
    nt_node_set_ref(nt, wcall, "arguments", wargs);
    nt_node_set_ref(nt, wcall, "block", -1);
    nt_node_set_arr(nt, nbody, "body", &wcall, 1);
    nt_node_set_ref(nt, blk, "body", nbody);
    nt_node_set_int(nt, blk, "cap_wrapped", 1);

    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

int desugar_reduce_proc_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "reduce") && !sp_streq(nm, "inject"))) continue;
    if (nt_ref(nt, id, "receiver") < 0) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || nt_kind(nt, blk) != NK_BlockArgumentNode) continue;
    int ex = nt_ref(nt, blk, "expression");
    if (ex < 0) continue;
    const char *exty = nt_type(nt, ex);
    int simple = exty && (sp_streq(exty, "LocalVariableReadNode") ||
                          sp_streq(exty, "InstanceVariableReadNode") ||
                          sp_streq(exty, "LambdaNode"));
    if (!simple || infer_type(c, ex) != TY_PROC) continue;

    int base = nt->count;
    char pn[2][40]; int reqs[2], reads[2];
    int ok = 1;
    for (int k = 0; k < 2 && ok; k++) {
      snprintf(pn[k], sizeof pn[k], "__fold_%d_%d", id, k);
      reqs[k] = nt_new_node(nt, "RequiredParameterNode");
      reads[k] = nt_new_node(nt, "LocalVariableReadNode");
      if (reqs[k] < 0 || reads[k] < 0) { ok = 0; break; }
      nt_node_set_str(nt, reqs[k], "name", pn[k]);
      nt_node_set_str(nt, reads[k], "name", pn[k]);
    }
    if (!ok) continue;
    int params = nt_new_node(nt, "ParametersNode");
    int bparams = nt_new_node(nt, "BlockParametersNode");
    int callargs = nt_new_node(nt, "ArgumentsNode");
    int callnode = nt_new_node(nt, "CallNode");
    int body = nt_new_node(nt, "StatementsNode");
    int blocknode = nt_new_node(nt, "BlockNode");
    if (params < 0 || bparams < 0 || callargs < 0 || callnode < 0 || body < 0 || blocknode < 0) continue;
    nt_node_set_arr(nt, params, "requireds", reqs, 2);
    nt_node_set_ref(nt, bparams, "parameters", params);
    nt_node_set_arr(nt, callargs, "arguments", reads, 2);
    nt_node_set_ref(nt, callnode, "receiver", ex);
    nt_node_set_str(nt, callnode, "name", "call");
    nt_node_set_ref(nt, callnode, "arguments", callargs);
    nt_node_set_ref(nt, callnode, "block", -1);
    nt_node_set_arr(nt, body, "body", &callnode, 1);
    nt_node_set_ref(nt, blocknode, "parameters", bparams);
    nt_node_set_ref(nt, blocknode, "body", body);
    nt_node_set_ref(nt, id, "block", blocknode);

    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    Scope *bs = comp_scope_of(c, blocknode);
    for (int k = 0; k < 2; k++) {
      LocalVar *lv = scope_local_intern(bs, pn[k]);
      if (lv) lv->is_block_param = 1;
    }
    changed = 1;
  }
  return changed;
}

/* ENV's enumeration/read-only surface rides a StrStr-hash snapshot: retarget
   the receiver at a receiverless __env_to_h call, and the whole Hash machinery
   serves keys/each/select/count{...}/inspect/... (#2742). Mutators and the
   direct read/write arms (\[\], \[\]=, fetch sans block, delete, store) stay on
   the real environment. */
static int env_enum_method(const char *n) {
  static const char *const M[] = {
    "keys", "values", "each", "each_pair", "each_key", "each_value",
    "each_entry", "to_h", "to_a", "select", "filter", "reject", "any?",
    "all?", "none?", "one?", "find", "detect", "find_all", "min_by", "max_by",
    "sort", "sort_by", "map", "collect", "flat_map", "filter_map", "group_by",
    "partition", "sum", "reduce", "inject", "invert", "key", "rassoc",
    "assoc", "slice", "except", "values_at", "count", "inspect", "hash",
    "empty?",
    /* the wider Enumerable/query surface (#2832) */
    "to_hash", "entries", "first", "min", "max", "minmax", "tally", "uniq",
    "zip", "take", "take_while", "drop", "drop_while", "each_slice",
    "each_cons", "each_with_index", "each_with_object", "find_index", "grep",
    "chunk", "chunk_while", "slice_when", "collect_concat",
    "reverse_each", "value?", "has_value?", "lazy", NULL };
  for (int i = 0; M[i]; i++) if (sp_streq(n, M[i])) return 1;
  return 0;
}
/* Dir surface renames and re-shapes (#2822, #2824, #2825, #2826, #2827,
   #2829): aliases retarget in place; foreach/each_child/glob-with-block become
   entries/children/glob followed by .each; a chdir block splices with a
   save/restore; a bare chdir gains Dir.home as its argument. */
/* Dir includes Enumerable in Ruby. Spinel's Dir is an opaque handle with its
   own #each / #entries, so route the Enumerable surface through #entries --
   the same materialize-then-dispatch the Enumerable methods already take on a
   Range or a user each (#3366). #each / #each_child / #read and the handle
   readers keep their dedicated arms. */
static int dir_enumerable_name(const char *nm) {
  if (!nm) return 0;
  static const char *const E[] = {
    "to_a", "map", "collect", "select", "filter", "reject", "sort", "sort_by",
    "min", "max", "min_by", "max_by", "count", "include?", "member?", "find",
    "detect", "each_entry", "each_with_index", "each_with_object", "group_by",
    "partition", "flat_map", "reduce", "inject", "sum", "tally", "first",
    "take", "drop", "take_while", "drop_while", "zip", "each_slice",
    "each_cons", "any?", "all?", "none?", "one?", "filter_map", "find_index",
    "chunk_while", "slice_when", "uniq", "reverse_each", "lazy", NULL };
  for (int i = 0; E[i]; i++) if (sp_streq(nm, E[i])) return 1;
  return 0;
}
int desugar_dir_surface(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    {
      const char *dnm = nt_str(nt, id, "name");
      int drecv = nt_ref(nt, id, "receiver");
      if (dnm && drecv >= 0 && dir_enumerable_name(dnm) &&
          infer_type(c, drecv) == TY_DIR) {
        int base = nt->count;
        int ent = nt_new_node(nt, "CallNode");
        if (ent >= 0) {
          nt_node_set_ref(nt, ent, "receiver", drecv);
          nt_node_set_str(nt, ent, "name", "entries");
          nt_node_set_ref(nt, ent, "arguments", -1);
          nt_node_set_ref(nt, ent, "block", -1);
          nt_node_set_ref(nt, id, "receiver", ent);
          comp_grow_node_arrays(c);
          int encl = c->nscope[id];
          for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
          changed = 1;
          continue;
        }
      }
    }
    const char *nm = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    /* a call chained onto an ENV mutator's result must land on ENV itself,
       not the detached snapshot: (ENV.m1(a).m2(b)) -> (ENV.m1(a); ENV.m2(b))
       (#2844) */
    if (nm && recv >= 0 && nt_kind(nt, recv) == NK_CallNode) {
      const char *mrn = NULL;
      {
        int mrecv = nt_ref(nt, recv, "receiver");
        if (mrecv >= 0 && nt_kind(nt, mrecv) == NK_ConstantReadNode)
          mrn = nt_str(nt, mrecv, "name");
      }
      const char *mnm = nt_str(nt, recv, "name");
      if (mrn && mnm && sp_streq(mrn, "ENV") &&
          (sp_streq(mnm, "clear") || sp_streq(mnm, "delete_if") ||
           sp_streq(mnm, "keep_if") || sp_streq(mnm, "update") ||
           sp_streq(mnm, "merge!") || sp_streq(mnm, "replace"))) {
        int envc = nt_new_node(nt, "ConstantReadNode");
        int outer = nt_new_node(nt, "CallNode");
        int estmts = nt_new_node(nt, "StatementsNode");
        int eparen = nt_new_node(nt, "ParenthesesNode");
        if (envc < 0 || outer < 0 || estmts < 0 || eparen < 0) continue;
        nt_node_set_str(nt, envc, "name", "ENV");
        nt_node_set_str(nt, outer, "name", nm);
        nt_node_set_ref(nt, outer, "receiver", envc);
        nt_node_set_ref(nt, outer, "arguments", nt_ref(nt, id, "arguments"));
        nt_node_set_ref(nt, outer, "block", nt_ref(nt, id, "block"));
        { int eitems[2] = { recv, outer };
          nt_node_set_arr(nt, estmts, "body", eitems, 2); }
        nt_node_set_ref(nt, eparen, "body", estmts);
        nt_node_set_str(nt, id, "name", "itself");
        nt_node_set_ref(nt, id, "receiver", eparen);
        nt_node_set_ref(nt, id, "arguments", -1);
        nt_node_set_ref(nt, id, "block", -1);
        comp_grow_node_arrays(c);
        { int eencl = c->nscope[id];
          c->nscope[envc] = eencl; c->nscope[outer] = eencl;
          c->nscope[estmts] = eencl; c->nscope[eparen] = eencl; }
        changed = 1;
        continue;
      }
    }
    /* hash.lazy enumerates [key, value] pairs: route through to_a so the
       (working) array lazy chain serves it (#2845) */
    if (nm && recv >= 0 && sp_streq(nm, "lazy") && nt_ref(nt, id, "block") < 0) {
      TyKind hrt = infer_type(c, recv);
      if (ty_is_hash(hrt)) {
        int toa2 = nt_new_node(nt, "CallNode");
        if (toa2 >= 0) {
          nt_node_set_str(nt, toa2, "name", "to_a");
          nt_node_set_ref(nt, toa2, "receiver", recv);
          nt_node_set_ref(nt, toa2, "arguments", -1);
          nt_node_set_ref(nt, toa2, "block", -1);
          comp_grow_node_arrays(c);
          c->nscope[toa2] = c->nscope[id];
          nt_node_set_ref(nt, id, "receiver", toa2);
          changed = 1;
          continue;
        }
      }
    }
    /* Kernel#open(path, ...) is File.open when no user method shadows it (#2816) */
    if (nm && recv < 0 && sp_streq(nm, "open") &&
        comp_method_index(c, "open") < 0 &&
        nt_ref(nt, id, "arguments") >= 0) {
      int fr2 = nt_new_node(nt, "ConstantReadNode");
      if (fr2 >= 0) {
        nt_node_set_str(nt, fr2, "name", "File");
        nt_node_set_ref(nt, id, "receiver", fr2);
        comp_grow_node_arrays(c);
        c->nscope[fr2] = c->nscope[id];
        changed = 1;
      }
      continue;
    }
    if (!nm || recv < 0 || nt_kind(nt, recv) != NK_ConstantReadNode) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn) continue;
    /* IO.read/write/readlines/binread/foreach are the File forms (#2793) */
    if (sp_streq(rn, "IO") &&
        (sp_streq(nm, "read") || sp_streq(nm, "write") || sp_streq(nm, "binread") ||
         sp_streq(nm, "binwrite") || sp_streq(nm, "readlines") || sp_streq(nm, "foreach"))) {
      nt_node_set_str(nt, recv, "name", "File");
      rn = "File";
      changed = 1;
    }
    /* File.foreach(path){|l|} -> (File.readlines(path).each{|l|}; nil): the
       block form returns nil, the blockless form an Enumerator (#2777, #2833) */
    if (sp_streq(rn, "File") && sp_streq(nm, "foreach")) {
      int fblk = nt_ref(nt, id, "block");
      int ic = nt_new_node(nt, "CallNode");
      if (ic < 0) continue;
      nt_node_set_str(nt, ic, "name", "readlines");
      nt_node_set_ref(nt, ic, "receiver", recv);
      nt_node_set_ref(nt, ic, "arguments", nt_ref(nt, id, "arguments"));
      nt_node_set_ref(nt, ic, "block", -1);
      if (fblk >= 0 && nt_kind(nt, fblk) == NK_BlockNode) {
        int ec = nt_new_node(nt, "CallNode");
        int nil2 = nt_new_node(nt, "NilNode");
        int fstmts = nt_new_node(nt, "StatementsNode");
        int fparen = nt_new_node(nt, "ParenthesesNode");
        if (ec < 0 || nil2 < 0 || fstmts < 0 || fparen < 0) continue;
        nt_node_set_str(nt, ec, "name", "each");
        nt_node_set_ref(nt, ec, "receiver", ic);
        nt_node_set_ref(nt, ec, "arguments", -1);
        nt_node_set_ref(nt, ec, "block", fblk);
        { int items2[2] = { ec, nil2 };
          nt_node_set_arr(nt, fstmts, "body", items2, 2); }
        nt_node_set_ref(nt, fparen, "body", fstmts);
        nt_node_set_str(nt, id, "name", "itself");
        nt_node_set_ref(nt, id, "receiver", fparen);
      }
      else {
        /* no block: enumerate the lines array */
        nt_node_set_str(nt, id, "name", "each");
        nt_node_set_ref(nt, id, "receiver", ic);
      }
      nt_node_set_ref(nt, id, "arguments", -1);
      nt_node_set_ref(nt, id, "block", fblk >= 0 && nt_kind(nt, fblk) == NK_BlockNode ? -1 : -1);
      comp_grow_node_arrays(c);
      { int encl3 = c->nscope[id];
        for (int j3 = ic; j3 < nt->count; j3++) c->nscope[j3] = encl3; }
      changed = 1;
      continue;
    }
    if (!sp_streq(rn, "Dir")) continue;
    int blk = nt_ref(nt, id, "block");
    int args = nt_ref(nt, id, "arguments");
    int an = 0; nt_arr(nt, args >= 0 ? args : -1, "arguments", &an);

    /* plain aliases */
    if (sp_streq(nm, "getwd")) { nt_node_set_str(nt, id, "name", "pwd"); changed = 1; continue; }
    if (sp_streq(nm, "delete") || sp_streq(nm, "unlink")) {
      nt_node_set_str(nt, id, "name", "rmdir"); changed = 1; continue;
    }
    if (sp_streq(nm, "[]")) { nt_node_set_str(nt, id, "name", "glob"); changed = 1; continue; }

    /* bare chdir goes home */
    if (sp_streq(nm, "chdir") && an == 0 && blk < 0) {
      int hc = nt_new_node(nt, "CallNode");
      int hr = nt_new_node(nt, "ConstantReadNode");
      int na = nt_new_node(nt, "ArgumentsNode");
      if (hc < 0 || hr < 0 || na < 0) continue;
      nt_node_set_str(nt, hr, "name", "Dir");
      nt_node_set_str(nt, hc, "name", "home");
      nt_node_set_ref(nt, hc, "receiver", hr);
      nt_node_set_ref(nt, hc, "arguments", -1);
      nt_node_set_ref(nt, hc, "block", -1);
      nt_node_set_arr(nt, na, "arguments", &hc, 1);
      nt_node_set_ref(nt, id, "arguments", na);
      comp_grow_node_arrays(c);
      int encl0 = c->nscope[id];
      c->nscope[hc] = encl0; c->nscope[hr] = encl0; c->nscope[na] = encl0;
      changed = 1; continue;
    }

    /* block forms: X(args) { } -> X'(args).each { } */
    const char *inner = NULL;
    if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) {
      if (sp_streq(nm, "foreach")) inner = "entries";
      else if (sp_streq(nm, "each_child")) inner = "children";
      else if (sp_streq(nm, "glob")) inner = "glob";
    }
    if (inner) {
      int ic = nt_new_node(nt, "CallNode");
      if (ic < 0) continue;
      nt_node_set_str(nt, ic, "name", inner);
      nt_node_set_ref(nt, ic, "receiver", recv);
      nt_node_set_ref(nt, ic, "arguments", args);
      nt_node_set_ref(nt, ic, "block", -1);
      nt_node_set_str(nt, id, "name", "each");
      nt_node_set_ref(nt, id, "receiver", ic);
      nt_node_set_ref(nt, id, "arguments", -1);
      comp_grow_node_arrays(c);
      c->nscope[ic] = c->nscope[id];
      changed = 1; continue;
    }
    /* blockless foreach/each_child still enumerate */
    if (sp_streq(nm, "foreach")) { nt_node_set_str(nt, id, "name", "entries"); changed = 1; continue; }
    if (sp_streq(nm, "each_child")) { nt_node_set_str(nt, id, "name", "children"); changed = 1; continue; }

    /* chdir(d) { body }: save, switch, run, restore -- the paren splice */
    if (sp_streq(nm, "chdir") && blk >= 0 && nt_kind(nt, blk) == NK_BlockNode && an >= 1) {
      int body = nt_ref(nt, blk, "body");
      if (body < 0) continue;
      if (subtree_has_kind(nt, body, NK_DefNode, 0)) continue;
      int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
      if (bn > 60) continue;
      char sav[32], valn[32];
      snprintf(sav, sizeof sav, "__cd_sav_%d", id);
      snprintf(valn, sizeof valn, "__cd_val_%d", id);
      Scope *es = comp_scope_of(c, id);
      LocalVar *slv = es ? scope_local_intern(es, sav) : NULL;
      LocalVar *vlv = es ? scope_local_intern(es, valn) : NULL;
      if (!slv || !vlv) continue;
      slv->type = TY_STRING; slv->rbs_seeded = 1;
      int aan = 0; const int *aav = nt_arr(nt, args, "arguments", &aan);
      if (aan < 1) continue;
      int base = nt->count;
      /* __sav = Dir.pwd */
      int pwdc = nt_new_node(nt, "CallNode");
      int pwdr = nt_new_node(nt, "ConstantReadNode");
      int wsav = nt_new_node(nt, "LocalVariableWriteNode");
      /* Dir.chdir(<arg>) */
      int cd1 = nt_new_node(nt, "CallNode");
      int cd1r = nt_new_node(nt, "ConstantReadNode");
      int cd1a = nt_new_node(nt, "ArgumentsNode");
      /* __val = (body...) */
      int pstmts = nt_new_node(nt, "StatementsNode");
      int paren = nt_new_node(nt, "ParenthesesNode");
      int wval = nt_new_node(nt, "LocalVariableWriteNode");
      /* Dir.chdir(__sav) */
      int cd2 = nt_new_node(nt, "CallNode");
      int cd2r = nt_new_node(nt, "ConstantReadNode");
      int cd2a = nt_new_node(nt, "ArgumentsNode");
      int rsav = nt_new_node(nt, "LocalVariableReadNode");
      int rval = nt_new_node(nt, "LocalVariableReadNode");
      int ostmts = nt_new_node(nt, "StatementsNode");
      int oparen = nt_new_node(nt, "ParenthesesNode");
      if (pwdc<0||pwdr<0||wsav<0||cd1<0||cd1r<0||cd1a<0||pstmts<0||paren<0||wval<0||
          cd2<0||cd2r<0||cd2a<0||rsav<0||rval<0||ostmts<0||oparen<0) continue;
      nt_node_set_str(nt, pwdr, "name", "Dir");
      nt_node_set_str(nt, pwdc, "name", "pwd");
      nt_node_set_ref(nt, pwdc, "receiver", pwdr);
      nt_node_set_ref(nt, pwdc, "arguments", -1);
      nt_node_set_ref(nt, pwdc, "block", -1);
      nt_node_set_str(nt, wsav, "name", sav);
      nt_node_set_ref(nt, wsav, "value", pwdc);
      nt_node_set_str(nt, cd1r, "name", "Dir");
      nt_node_set_str(nt, cd1, "name", "chdir");
      nt_node_set_ref(nt, cd1, "receiver", cd1r);
      { int a0 = aav[0]; nt_node_set_arr(nt, cd1a, "arguments", &a0, 1); }
      nt_node_set_ref(nt, cd1, "arguments", cd1a);
      nt_node_set_ref(nt, cd1, "block", -1);
      { int items[60]; for (int k = 0; k < bn; k++) items[k] = bb[k];
        nt_node_set_arr(nt, pstmts, "body", items, bn); }
      nt_node_set_ref(nt, paren, "body", pstmts);
      nt_node_set_str(nt, wval, "name", valn);
      nt_node_set_ref(nt, wval, "value", paren);
      nt_node_set_str(nt, cd2r, "name", "Dir");
      nt_node_set_str(nt, cd2, "name", "chdir");
      nt_node_set_ref(nt, cd2, "receiver", cd2r);
      nt_node_set_str(nt, rsav, "name", sav);
      { int rs = rsav; nt_node_set_arr(nt, cd2a, "arguments", &rs, 1); }
      nt_node_set_ref(nt, cd2, "arguments", cd2a);
      nt_node_set_ref(nt, cd2, "block", -1);
      nt_node_set_str(nt, rval, "name", valn);
      { int items[4] = { wsav, cd1, wval, cd2 };
        int all[5]; for (int k = 0; k < 4; k++) all[k] = items[k]; all[4] = rval;
        nt_node_set_arr(nt, ostmts, "body", all, 5); }
      nt_node_set_ref(nt, oparen, "body", ostmts);
      nt_node_set_str(nt, id, "name", "itself");
      nt_node_set_ref(nt, id, "receiver", oparen);
      nt_node_set_ref(nt, id, "arguments", -1);
      nt_node_set_ref(nt, id, "block", -1);
      comp_grow_node_arrays(c);
      int encl = c->nscope[id];
      for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
      /* re-home the block body into the enclosing scope */
      {
        Scope *bs = comp_scope_of(c, body);
        int bsi = bs ? (int)(bs - c->scopes) : -1;
        int stack[256]; int sp = 0; stack[sp++] = body;
        while (sp > 0 && bsi >= 0) {
          int nid = stack[--sp];
          if (nid < 0 || nid >= nt->count) continue;
          if (c->nscope[nid] == bsi) c->nscope[nid] = encl;
          const SpNode *nd = &nt->nodes[nid];
          for (int i2 = 0; i2 < nd->nr && sp < 250; i2++) stack[sp++] = nd->r[i2].ref;
          for (int i2 = 0; i2 < nd->na; i2++)
            for (int j2 = 0; j2 < nd->a[i2].n && sp < 250; j2++) stack[sp++] = nd->a[i2].ids[j2];
        }
      }
      changed = 1; continue;
    }
  }
  return changed;
}

/* `a !~ b` where a's class defines `=~` (and no `!~` of its own): Object#!~ is
   !(a =~ b). Rewrite the CallNode into `!` over a FRESH `=~` node -- renaming
   in codegen poisoned the node-type cache (`!~` bool vs `=~` any, see #3018's
   note), a fresh node types independently (#3019). Receivers without a user
   `=~` (regex operands, bool/nil raises) keep their dedicated paths. */
int desugar_user_not_match(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "!~")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    TyKind rt = infer_type(c, recv);
    if (!ty_is_object(rt)) continue;
    int cid = ty_object_class(rt);
    if (cid < 0 || comp_method_in_chain(c, cid, "=~", NULL) < 0) continue;
    if (comp_method_in_chain(c, cid, "!~", NULL) >= 0) continue;  /* user !~ wins */
    int inner = nt_new_node(nt, "CallNode");
    nt_node_set_str(nt, inner, "name", "=~");
    nt_node_set_ref(nt, inner, "receiver", recv);
    nt_node_set_ref(nt, inner, "arguments", nt_ref(nt, id, "arguments"));
    nt_node_set_ref(nt, inner, "block", -1);
    nt_node_set_str(nt, id, "name", "!");
    nt_node_set_ref(nt, id, "receiver", inner);
    nt_node_set_ref(nt, id, "arguments", -1);
    comp_grow_node_arrays(c);
    c->nscope[inner] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

int desugar_env_enum(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    if (!nm || recv < 0 || nt_kind(nt, recv) != NK_ConstantReadNode) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || !sp_streq(rn, "ENV")) continue;
    int is_enum = env_enum_method(nm);
    /* fetch WITH a block rides the snapshot's block-aware Hash#fetch (#2745) */
    if (!is_enum && sp_streq(nm, "fetch") && nt_ref(nt, id, "block") >= 0) is_enum = 1;
    if (!is_enum) continue;
    /* ENV keys are Strings at the C level: a statically non-String argument to
       the string-keyed queries is CRuby's TypeError, not a silent miss (#3000).
       Rewrite the call into the raise before the snapshot desugar. */
    if (sp_streq(nm, "assoc") || sp_streq(nm, "key") || sp_streq(nm, "slice") ||
        sp_streq(nm, "values_at")) {
      int qargs = nt_ref(nt, id, "arguments");
      int qn = 0; const int *qav = qargs >= 0 ? nt_arr(nt, qargs, "arguments", &qn) : NULL;
      const char *badc = NULL;
      for (int k = 0; k < qn && !badc; k++) {
        TyKind at = infer_type(c, qav[k]);
        badc = at == TY_SYMBOL ? "Symbol" : at == TY_INT ? "Integer"
             : at == TY_FLOAT ? "Float" : at == TY_NIL ? "nil"
             : at == TY_BOOL ? "Boolean" : NULL;
      }
      if (badc) {
        char msg[128];
        snprintf(msg, sizeof msg, "no implicit conversion of %s into String", badc);
        int ecn = nt_new_node(nt, "ConstantReadNode");
        nt_node_set_str(nt, ecn, "name", "TypeError");
        int emn = nt_new_node(nt, "StringNode");
        nt_node_set_str(nt, emn, "content", msg);
        int ea[2] = { ecn, emn };
        int eargs = nt_new_node(nt, "ArgumentsNode");
        nt_node_set_arr(nt, eargs, "arguments", ea, 2);
        nt_node_set_str(nt, id, "name", "raise");
        nt_node_set_ref(nt, id, "receiver", -1);
        nt_node_set_ref(nt, id, "arguments", eargs);
        nt_node_set_ref(nt, id, "block", -1);
        comp_grow_node_arrays(c);
        c->nscope[ecn] = c->nscope[emn] = c->nscope[eargs] = c->nscope[id];
        changed = 1;
        continue;
      }
    }
    int snap = nt_new_node(nt, "CallNode");
    if (snap < 0) continue;
    nt_node_set_str(nt, snap, "name", "__env_to_h");
    nt_node_set_ref(nt, snap, "receiver", -1);
    nt_node_set_ref(nt, snap, "arguments", -1);
    nt_node_set_ref(nt, snap, "block", -1);
    comp_grow_node_arrays(c);
    c->nscope[snap] = c->nscope[id];
    /* the plain-Enumerable names ride the pair ARRAY (the typed-hash surface
       does not carry them); hash-native names stay on the snapshot (#2832) */
    {
      static const char *const VIA_A[] = {
        "first", "min", "max", "minmax", "tally", "uniq", "zip", "take",
        "take_while", "drop", "drop_while", "each_slice", "each_cons",
        "each_with_index", "each_with_object", "find_index", "grep", "chunk",
        "chunk_while", "slice_when", "collect_concat", "reverse_each",
        "lazy", NULL };
      int via_a = 0;
      for (int q = 0; VIA_A[q]; q++) if (sp_streq(nm, VIA_A[q])) { via_a = 1; break; }
      if (via_a) {
        int toa = nt_new_node(nt, "CallNode");
        if (toa < 0) continue;
        nt_node_set_str(nt, toa, "name", "to_a");
        nt_node_set_ref(nt, toa, "receiver", snap);
        nt_node_set_ref(nt, toa, "arguments", -1);
        nt_node_set_ref(nt, toa, "block", -1);
        comp_grow_node_arrays(c);
        c->nscope[toa] = c->nscope[id];
        nt_node_set_ref(nt, id, "receiver", toa);
      }
      else nt_node_set_ref(nt, id, "receiver", snap);
    }
    /* aliases the hash surface spells differently */
    if (sp_streq(nm, "to_hash")) nt_node_set_str(nt, id, "name", "to_h");
    else if (sp_streq(nm, "entries")) nt_node_set_str(nt, id, "name", "to_a");
    else if (sp_streq(nm, "value?")) nt_node_set_str(nt, id, "name", "has_value?");
    else if (sp_streq(nm, "collect_concat")) nt_node_set_str(nt, id, "name", "flat_map");
    changed = 1;
  }
  return changed;
}

int desugar_public_method(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "public_method")) continue;
    if (!method_sym_arg(c, id)) continue;   /* literal symbol/string arg only */
    nt_node_set_str(nt, id, "name", "method");
    changed = 1;
  }
  return changed;
}

/* Is `n` a value spinel can materialize with #to_a -- i.e. an operand a chain
   may concatenate? Arrays/ranges/hashes/enumerators answer directly; a user
   object qualifies when its class defines #each (the Enumerable contract). */
static int chain_operand_ok(Compiler *c, int n) {
  if (n < 0) return 0;
  TyKind t = infer_type(c, n);
  if (ty_is_array(t) || ty_is_hash(t) || t == TY_RANGE || t == TY_ENUMERATOR) return 1;
  /* An empty array literal never narrows, so `a = []; a.chain.to_a` leaves the
     receiver UNKNOWN (#2474 / #2468). #chain is Enumerable-specific and a
     user-defined #chain is excluded by the caller, so an untyped operand is
     taken at its word; if it turns out to have no #to_a, that call reports it. */
  if (t == TY_UNKNOWN) return 1;
  if (ty_is_object(t)) {
    int ci = ty_object_class(t);
    return ci >= 0 && comp_method_in_chain(c, ci, "each", NULL) >= 0;
  }
  return 0;
}

/* Synthesize `<n>.to_a` (a fresh CallNode), or -1 on node-table OOM. */
static int chain_mk_to_a(Compiler *c, int n) {
  NodeTable *nt = (NodeTable *)c->nt;
  int call = nt_new_node(nt, "CallNode");
  if (call < 0) return -1;
  nt_node_set_ref(nt, call, "receiver", n);
  nt_node_set_str(nt, call, "name", "to_a");
  nt_node_set_ref(nt, call, "arguments", -1);
  nt_node_set_ref(nt, call, "block", -1);
  return call;
}

/* Synthesize `<a> + <b>`, or -1 on node-table OOM. */
static int chain_mk_concat(Compiler *c, int a, int b) {
  NodeTable *nt = (NodeTable *)c->nt;
  int args = nt_new_node(nt, "ArgumentsNode");
  if (args < 0) return -1;
  nt_node_set_arr(nt, args, "arguments", &b, 1);
  int call = nt_new_node(nt, "CallNode");
  if (call < 0) return -1;
  nt_node_set_ref(nt, call, "receiver", a);
  nt_node_set_str(nt, call, "name", "+");
  nt_node_set_ref(nt, call, "arguments", args);
  nt_node_set_ref(nt, call, "block", -1);
  return call;
}

/* `recv.chain(a, b)` and `enum + enum` -> `__enum_chain(recv.to_a + a.to_a + b.to_a)`.
   Ruby's chain is lazy over its sources; spinel materializes them at build time
   and hands the concatenation to a snapshot enumerator, which serves every
   terminal the sources support (#to_a, #each, #map, #next, ...). Reusing #to_a
   is what lets a Struct, a user Enumerable, or another enumerator be an operand:
   each already knows how to materialize itself. #2545 / #2548 / #2551 */
int desugar_enumerable_chain(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    if (!nm || recv < 0) continue;
    if (nt_ref(nt, id, "block") >= 0) continue;   /* chain{} is not a thing; leave it */
    int args = nt_ref(nt, id, "arguments");
    int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;

    int is_chain = sp_streq(nm, "chain");
    /* Enumerator#+ only: `+` is overwhelmingly numeric/array/string, so require
       BOTH operands to be enumerators before touching it. */
    int is_plus = sp_streq(nm, "+") && argc == 1 && argv &&
                  infer_type(c, recv) == TY_ENUMERATOR &&
                  infer_type(c, argv[0]) == TY_ENUMERATOR;
    if (!is_chain && !is_plus) continue;
    /* `arr.chain` with no argument is just the receiver's own elements (#2468),
       so argc == 0 is valid for chain (but `+` always has its operand). */
    if (argc > 32 || (argc > 0 && !argv) || (is_plus && argc != 1)) continue;

    if (is_chain) {
      /* a user-defined #chain wins over Enumerable's */
      TyKind rt = infer_type(c, recv);
      if (ty_is_object(rt)) {
        int ci = ty_object_class(rt);
        if (ci >= 0 && comp_method_in_chain(c, ci, "chain", NULL) >= 0) continue;
      }
      if (!chain_operand_ok(c, recv)) continue;
    }
    int ok = 1;
    for (int k = 0; k < argc && ok; k++) if (!chain_operand_ok(c, argv[k])) ok = 0;
    if (!ok) continue;

    int saved[32];
    for (int k = 0; k < argc; k++) saved[k] = argv[k];  /* copy before realloc */
    int base = nt->count;
    int acc = chain_mk_to_a(c, recv);
    for (int k = 0; k < argc && acc >= 0; k++) {
      int t = chain_mk_to_a(c, saved[k]);
      acc = (t >= 0) ? chain_mk_concat(c, acc, t) : -1;
    }
    if (acc < 0) continue;   /* node-table OOM: leave the call alone */
    int newargs = nt_new_node(nt, "ArgumentsNode");
    if (newargs < 0) continue;
    nt_node_set_arr(nt, newargs, "arguments", &acc, 1);
    nt_node_set_str(nt, id, "name", "__enum_chain");
    nt_node_set_ref(nt, id, "receiver", -1);
    nt_node_set_ref(nt, id, "arguments", newargs);

    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

int desugar_implicit_send(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;        /* implicit self only */
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "send") && !sp_streq(nm, "__send__") &&
                !sp_streq(nm, "public_send"))) continue;
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;
    const char *a0ty = nt_type(nt, argv[0]);
    const char *mname = NULL;
    if (a0ty && sp_streq(a0ty, "SymbolNode")) mname = nt_str(nt, argv[0], "value");
    else if (a0ty && sp_streq(a0ty, "StringNode")) mname = nt_str(nt, argv[0], "content");
    if (!mname || !*mname) continue;                      /* non-literal name: leave it */
    if (sp_streq(mname, "send") || sp_streq(mname, "__send__") ||
        sp_streq(mname, "public_send")) continue;          /* don't re-trigger next pass */
    int nrest = argc - 1;
    if (nrest > 64) continue;                             /* absurd arity: leave it */
    int rest[64];
    for (int k = 0; k < nrest; k++) rest[k] = argv[k + 1];  /* copy before realloc */
    char namebuf[256];
    snprintf(namebuf, sizeof namebuf, "%s", mname);        /* copy before realloc */
    /* public_send dispatches only public methods: stamp the retargeted call
       so codegen raises NoMethodError for a private/protected target */
    int vis_enf = sp_streq(nm, "public_send");
    int base = nt->count;
    int newargs = nt_new_node(nt, "ArgumentsNode");
    if (newargs < 0) continue;
    nt_node_set_arr(nt, newargs, "arguments", rest, nrest);
    nt_node_set_str(nt, id, "name", namebuf);              /* retarget the call */
    nt_node_set_ref(nt, id, "arguments", newargs);         /* drop the name arg */
    if (vis_enf) nt_node_set_str(nt, id, "vis_enforce", "1");
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* `recv.public_send(:m, args)` with a literal name -> a direct `recv.m(args)`
   call stamped `vis_enforce`, so codegen raises NoMethodError for a
   private/protected target. send/__send__ keep the visibility-blind textual
   rewrite in spinel_parse.c (CRuby's send ignores visibility). Mirrors
   desugar_implicit_send's node-retarget model. */
int desugar_public_send_recv(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    if (nt_ref(nt, id, "receiver") < 0) continue;          /* explicit receiver only */
    const char *nm = nt_str(nt, id, "name");
    /* Also handle send/__send__ here, but ONLY when they target another
       send-family method (the nested `d.send(:send, :greet)` case, #2688):
       simple `d.send(:m)` is already lowered textually in spinel_parse.c, and
       the send-of-send it leaves behind unwinds one layer per pass here. */
    int is_pub = nm && sp_streq(nm, "public_send");
    int is_snd = nm && (sp_streq(nm, "send") || sp_streq(nm, "__send__"));
    if (!is_pub && !is_snd) continue;
    /* A blank-slate receiver has no #send / #public_send (only __send__ is
       BasicObject's): leave the call unretargeted, and the blank-slate gate
       raises CRuby's NoMethodError for the send itself (#2725). */
    if (!sp_streq(nm, "__send__")) {
      int bsrecv = nt_ref(nt, id, "receiver");
      TyKind bsrt = bsrecv >= 0 ? infer_type(c, bsrecv) : TY_UNKNOWN;
      if (ty_is_object(bsrt) && class_is_blank_slate(c, ty_object_class(bsrt))) continue;
      /* A socket's #send is the datagram write, not Object#send: CRuby picks
         by the receiver's class, and `u.send("ping", 0, host, port)` would
         otherwise retarget to a method named "ping" (#2922). */
      if (sp_streq(nm, "send") && bsrt == TY_IO && sp_feature_required("socket")) continue;
    }
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;
    const char *a0ty = nt_type(nt, argv[0]);
    const char *mname = NULL;
    if (a0ty && sp_streq(a0ty, "SymbolNode")) mname = nt_str(nt, argv[0], "value");
    else if (a0ty && sp_streq(a0ty, "StringNode")) mname = nt_str(nt, argv[0], "content");
    if (!mname || !*mname) continue;                       /* runtime name: dyn_send_arms */
    int m_is_send = sp_streq(mname, "send") || sp_streq(mname, "__send__") ||
                    sp_streq(mname, "public_send");
    (void)m_is_send;
    /* send/__send__ that spinel_parse.c already lowered never reach here;
       the ones it leaves are the send-of-send residue (`d.send(:greet)` after
       stripping the outer :send), which we finish retargeting. */
    int nrest = argc - 1;
    if (nrest > 64) continue;
    int rest[64];
    for (int k = 0; k < nrest; k++) rest[k] = argv[k + 1];
    char namebuf[256];
    snprintf(namebuf, sizeof namebuf, "%s", mname);
    int base = nt->count;
    int newargs = nt_new_node(nt, "ArgumentsNode");
    if (newargs < 0) continue;
    nt_node_set_arr(nt, newargs, "arguments", rest, nrest);
    nt_node_set_str(nt, id, "name", namebuf);
    nt_node_set_ref(nt, id, "arguments", newargs);
    if (is_pub && !m_is_send) nt_node_set_str(nt, id, "vis_enforce", "1");
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* n.step(to: X[, by: Y]) is the keyword form of n.step(X, Y) (by defaults to 1).
   The step passes read positional arguments, so a lone KeywordHashNode argument
   is otherwise mis-read as an integer limit (an int-from-pointer miscompile).
   Rewrite the to:/by: form into the positional list before those passes run. */
int desugar_step_kwargs(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "step")) continue;
    if (nt_ref(nt, id, "receiver") < 0) continue;         /* Numeric#step has a receiver */
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int ac = 0; const int *av = nt_arr(nt, args, "arguments", &ac);
    if (ac != 1 || !av) continue;
    int kh = av[0];
    if (!nt_type(nt, kh) || !sp_streq(nt_type(nt, kh), "KeywordHashNode")) continue;
    int to_v = -1, by_v = -1, other = 0;
    int en = 0; const int *els = nt_arr(nt, kh, "elements", &en);
    for (int i = 0; i < en; i++) {
      if (!nt_type(nt, els[i]) || !sp_streq(nt_type(nt, els[i]), "AssocNode")) { other = 1; break; }
      int key = nt_ref(nt, els[i], "key");
      const char *kn = (key >= 0 && nt_type(nt, key) && sp_streq(nt_type(nt, key), "SymbolNode"))
                       ? nt_str(nt, key, "value") : NULL;
      if (kn && sp_streq(kn, "to")) to_v = nt_ref(nt, els[i], "value");
      else if (kn && sp_streq(kn, "by")) by_v = nt_ref(nt, els[i], "value");
      else { other = 1; break; }
    }
    if (other || to_v < 0) continue;   /* only the to:[/by:] form; `to` is required */
    int sc = c->nscope[id];
    int base = nt->count;
    if (by_v < 0) {
      by_v = nt_new_node(nt, "IntegerNode");
      if (by_v < 0) continue;
      nt_node_set_int(nt, by_v, "value", 1);
    }
    int pos[2] = { to_v, by_v };
    nt_node_set_arr(nt, args, "arguments", pos, 2);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = sc;
    changed = 1;
  }
  return changed;
}

/* A receiverless `instance_exec(&b)` at top level (or in a free function) has an
   implicit self, so instance_exec rebinds self to the current self -- i.e. it
   does not change self at all, and is exactly `<block>.call(<args>)`. Rewrite it
   so the value form (`x = run { }`) lowers like any block-call forward instead of
   stranding an un-emittable top-level instance_exec (which links to an undefined
   function). Class-level instance_exec forwarders DO rebind self to the instance
   and are handled by their own trampoline splice, so they are left untouched. */
int desugar_toplevel_instance_exec(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;         /* implicit self only */
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "instance_exec")) continue;
    int sc = c->nscope[id];
    if (sc < 0 || sc >= c->nscopes || c->scopes[sc].class_id >= 0) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockArgumentNode")) continue;
    int bexpr = nt_ref(nt, blk, "expression");
    if (bexpr < 0) continue;                               /* anonymous `&`: no name to call */
    nt_node_set_ref(nt, id, "receiver", bexpr);            /* receiver = forwarded block */
    nt_node_set_str(nt, id, "name", "call");
    nt_node_set_ref(nt, id, "block", -1);                  /* the block is now the receiver */
    changed = 1;
  }
  return changed;
}

/* `binding.local_variable_get(:name)` with a literal symbol naming an in-scope
   local is the idiom for reading a reserved-word parameter (`def f(then:);
   binding.local_variable_get(:then); end`), the only way to reference such a
   name -- a bare `then` is a keyword. An AOT compiler has no reified Binding, but
   this statically-decidable form is exactly the value of that local, so rewrite
   it to `<local>.itself` (an identity that yields the local's value). Other
   binding uses have no static answer and are rejected in codegen. */
int desugar_binding_lvget(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "local_variable_get")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) continue;
    if (nt_ref(nt, recv, "receiver") >= 0) continue;      /* binding must be receiverless */
    const char *rnm = nt_str(nt, recv, "name");
    if (!rnm || !sp_streq(rnm, "binding")) continue;
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (ac != 1 || !av || !nt_type(nt, av[0]) || !sp_streq(nt_type(nt, av[0]), "SymbolNode")) continue;
    const char *vn = nt_str(nt, av[0], "value");
    if (!vn) continue;
    int sc = c->nscope[id];
    if (sc < 0 || sc >= c->nscopes || !scope_local(&c->scopes[sc], vn)) continue;
    char *vnbuf = malloc(strlen(vn) + 1);   /* copy before nt_new_node may realloc vn's storage */
    if (!vnbuf) continue;
    strcpy(vnbuf, vn);
    int base = nt->count;
    int lread = nt_new_node(nt, "LocalVariableReadNode");
    if (lread < 0) { free(vnbuf); continue; }
    nt_node_set_str(nt, lread, "name", vnbuf);
    free(vnbuf);
    nt_node_set_ref(nt, id, "receiver", lread);            /* <local>.itself */
    nt_node_set_str(nt, id, "name", "itself");
    nt_node_set_ref(nt, id, "arguments", -1);
    /* The `binding` receiver is now orphaned; rename it to a sentinel no pass
       matches so the binding reject (which scans all nodes, including
       unreferenced ones) does not fire on it. */
    nt_node_set_str(nt, recv, "name", "__orphaned__");
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = sc;
    changed = 1;
  }
  return changed;
}

/* `recv.send(name_expr, args)` with a NON-literal name and an explicit receiver:
   lower it to a static dispatch over the method names that appear as symbol
   literals in the program. For each candidate name `m` we synthesize an ordinary
   `recv.m(args)` call; analyze types each (honoring arity), and codegen keeps the
   ones that resolve on the receiver's type and emits `name == :m1 ? recv.m1(args)
   : ... : NoMethodError` (result poly). A runtime name that is not one of those
   literals -- or whose call does not resolve on the receiver -- is not
   dispatchable and raises NoMethodError. The literal-name forms are rewritten
   earlier (spinel_parse.c / desugar_implicit_send); this covers a name known only
   at runtime but drawn from the program's closed set of symbol literals. The arm
   node ids are stashed on the send under "dyn_send_arms" for codegen. */
int desugar_dynamic_send(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int changed = 0;
  static const char *const sends[] = { "send", "__send__", "public_send", NULL };
  /* a user-defined method named send/etc. resolves normally; don't intercept */
  for (int s = 0; s < c->nscopes; s++) { const char *sn = c->scopes[s].name;
    if (sn) for (int k = 0; sends[k]; k++) if (sp_streq(sn, sends[k])) return 0; }
  /* quick out: nothing to do unless some not-yet-lowered explicit-receiver send
     with a runtime name exists (the common case has none, so skip the scans). */
  { int any = 0;
    for (int id = 0; id < n0 && !any; id++) {
      if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
      const char *nm = nt_str(nt, id, "name"); if (!nm) continue;
      int is = 0; for (int k = 0; sends[k]; k++) if (sp_streq(nm, sends[k])) { is = 1; break; }
      if (!is || nt_ref(nt, id, "receiver") < 0) continue;
      int dn = 0; nt_arr(nt, id, "dyn_send_arms", &dn); if (dn > 0) continue;
      int a = nt_ref(nt, id, "arguments"); if (a < 0) continue;
      int ac = 0; const int *av = nt_arr(nt, a, "arguments", &ac);
      if (ac < 1 || !av) continue;
      const char *a0 = nt_type(nt, av[0]);
      if (a0 && (sp_streq(a0, "SymbolNode") || sp_streq(a0, "StringNode"))) continue;
      any = 1;
    }
    if (!any) return 0;
  }
  /* collect distinct symbol/string-literal names = candidate method names (send
     accepts either; a string name interns to the same symbol at the call). */
  char **cand = NULL; int ncand = 0, candcap = 0;
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    const char *v = NULL;
    if (ty && sp_streq(ty, "SymbolNode")) v = nt_str(nt, id, "value");
    else if (ty && sp_streq(ty, "StringNode")) v = nt_str(nt, id, "content");
    if (!v || !*v) continue;
    int skip = 0;
    for (int k = 0; sends[k]; k++) if (sp_streq(v, sends[k])) { skip = 1; break; }  /* avoid send-of-send recursion */
    for (int k = 0; !skip && k < ncand; k++) if (sp_streq(cand[k], v)) skip = 1;
    if (skip) continue;
    if (ncand == candcap) { candcap = candcap ? candcap * 2 : 16; cand = (char **)realloc(cand, sizeof(char *) * candcap); }
    cand[ncand++] = strdup(v);
  }
  if (ncand == 0 || ncand > 128) { for (int k = 0; k < ncand; k++) free(cand[k]); free(cand); return 0; }
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int is_send = 0; for (int k = 0; sends[k]; k++) if (sp_streq(nm, sends[k])) { is_send = 1; break; }
    if (!is_send) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;                            /* implicit self handled elsewhere */
    { int dn = 0; nt_arr(nt, id, "dyn_send_arms", &dn); if (dn > 0) continue; }  /* already lowered */
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;
    const char *a0 = nt_type(nt, argv[0]);
    if (a0 && (sp_streq(a0, "SymbolNode") || sp_streq(a0, "StringNode"))) continue;  /* literal: handled earlier */
    int nrest = argc - 1;
    if (nrest > 64) continue;
    int rest[64]; for (int k = 0; k < nrest; k++) rest[k] = argv[k + 1];  /* copy before realloc */
    int base = nt->count;
    int arms[128]; int narm = 0;
    for (int k = 0; k < ncand; k++) {
      int na = nt_new_node(nt, "ArgumentsNode"); if (na < 0) break;
      if (nrest) nt_node_set_arr(nt, na, "arguments", rest, nrest);
      int call = nt_new_node(nt, "CallNode"); if (call < 0) break;
      nt_node_set_ref(nt, call, "receiver", recv);
      nt_node_set_str(nt, call, "name", cand[k]);
      nt_node_set_ref(nt, call, "arguments", na);
      /* public_send arms enforce visibility at the dispatch site */
      if (sp_streq(nm, "public_send")) nt_node_set_str(nt, call, "vis_enforce", "1");
      arms[narm++] = call;
    }
    nt_node_set_arr(nt, id, "dyn_send_arms", arms, narm);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  for (int k = 0; k < ncand; k++) free(cand[k]);
  free(cand);
  return changed;
}

/* `recv.respond_to?(:m)` with an explicit receiver and a literal method name:
   synthesize a probe `recv.m` call. The analyze fixpoint types the probe with
   the ordinary resolver, so its inferred type tells codegen whether spinel can
   actually dispatch `m` on that receiver (UNKNOWN = it cannot). The probe id is
   stashed on the respond_to? node under "rt_probes" and is analysis-only -- it is
   never emitted. The codegen fold reads it for primitive/builtin receivers,
   deriving the answer from the real dispatch instead of a hand-maintained method
   list; user-object receivers keep their visibility-aware chain resolution. The
   probe carries no arguments: builtin method inference keys on the receiver type
   and name (not arity), so an arg-taking method like `+`/`[]` still types. */
int desugar_respond_to_probe(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  /* a user-defined respond_to? resolves normally; don't intercept */
  for (int s = 0; s < c->nscopes; s++) { const char *sn = c->scopes[s].name;
    if (sn && sp_streq(sn, "respond_to?")) return 0; }
  int n0 = nt->count;
  int changed = 0;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "respond_to?")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;                         /* implicit self handled in the fold */
    { int pn = 0; nt_arr(nt, id, "rt_probes", &pn); if (pn > 0) continue; }  /* already probed */
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;
    const char *aty = nt_type(nt, argv[0]);
    const char *qm = NULL;
    if (aty && sp_streq(aty, "SymbolNode")) qm = nt_str(nt, argv[0], "value");
    else if (aty && sp_streq(aty, "StringNode")) {
      qm = nt_str(nt, argv[0], "content");
      if (!qm) qm = nt_str(nt, argv[0], "unescaped");
    }
    if (!qm || !*qm) continue;                      /* non-literal name: not foldable */
    int base = nt->count;
    /* Probe two call shapes and let codegen answer true if EITHER types, since a
       single shape cannot satisfy every method: a block method (`each`, `map`)
       rejects a positional argument but needs a block, while an operator (`+`,
       `[]`) needs an argument. One probe carries an empty block (no arg), the
       other one dummy argument (the receiver, always type-available). A plain
       no-arg method (`upcase`) types under either. Builtin method inference keys
       on the receiver type and name, so a recognized method types by name; an
       unrecognized one is UNKNOWN under both. */
    int probes[3]; int np = 0;
    /* shape 0: recv.m  -- no argument, no block. Resolves the blockless
       enumerator forms (`each`, `reverse_each` infer TY_ENUMERATOR) and plain
       no-arg methods, using only codegen-safe inference. Every probe carries
       the rt_probe flag: it is analysis-only, and the param-binding passes
       must not let its dummy shapes type real lambda/method parameters. */
    {
      int na = nt_new_node(nt, "ArgumentsNode");
      int probe = nt_new_node(nt, "CallNode");
      if (na >= 0 && probe >= 0) {
        nt_node_set_ref(nt, probe, "receiver", recv);
        nt_node_set_str(nt, probe, "name", qm);
        nt_node_set_ref(nt, probe, "arguments", na);
        nt_node_set_int(nt, probe, "rt_probe", 1);
        probes[np++] = probe;
      }
    }
    /* shape 1: recv.m { }  -- block, no argument */
    {
      int na = nt_new_node(nt, "ArgumentsNode");
      int blkbody = nt_new_node(nt, "StatementsNode");
      int blk = nt_new_node(nt, "BlockNode");
      int probe = nt_new_node(nt, "CallNode");
      if (na >= 0 && blkbody >= 0 && blk >= 0 && probe >= 0) {
        nt_node_set_ref(nt, blk, "body", blkbody);
        nt_node_set_ref(nt, probe, "receiver", recv);
        nt_node_set_str(nt, probe, "name", qm);
        nt_node_set_ref(nt, probe, "arguments", na);
        nt_node_set_ref(nt, probe, "block", blk);
        nt_node_set_int(nt, probe, "rt_probe", 1);
        probes[np++] = probe;
      }
    }
    /* shape 2: recv.m(recv)  -- one dummy argument, no block */
    {
      int dummy_args[1] = { recv };
      int na = nt_new_node(nt, "ArgumentsNode");
      int probe = nt_new_node(nt, "CallNode");
      if (na >= 0 && probe >= 0) {
        nt_node_set_arr(nt, na, "arguments", dummy_args, 1);
        nt_node_set_ref(nt, probe, "receiver", recv);
        nt_node_set_str(nt, probe, "name", qm);
        nt_node_set_ref(nt, probe, "arguments", na);
        nt_node_set_int(nt, probe, "rt_probe", 1);
        probes[np++] = probe;
      }
    }
    /* Sync the parallel arrays for every node allocated in this iteration --
       even a partial shape (an allocation failed mid-shape, so no probe was
       added) leaves nodes past `base` whose c->nscope would otherwise stay
       uninitialized, desyncing the arrays from nt->count for later passes. */
    if (nt->count > base) {
      comp_grow_node_arrays(c);
      int encl = c->nscope[id];
      for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    }
    if (np == 0) continue;
    nt_node_set_arr(nt, id, "rt_probes", probes, np);
    changed = 1;
  }
  return changed;
}

/* `:sym.to_proc.call(recv, *args)` -> `recv.sym(*args)`. An explicit Symbol#to_proc
   followed by a call applies the named method to the first argument; with both the
   symbol and the call site statically known, it rewrites to an ordinary method call
   and the normal dispatch handles it. (The `&:sym` block form lowers separately; a
   to_proc whose receiver isn't a literal symbol, or that isn't immediately called,
   is left alone.) Mirrors desugar_implicit_send's node-retarget model. */
int desugar_symbol_to_proc_call(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;  /* snapshot: synthetic nodes are appended past here */
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "call")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) continue;
    const char *rnm = nt_str(nt, recv, "name");
    if (!rnm || !sp_streq(rnm, "to_proc")) continue;
    int rargs = nt_ref(nt, recv, "arguments");
    if (rargs >= 0) { int rc = 0; nt_arr(nt, rargs, "arguments", &rc); if (rc != 0) continue; }
    int sym = nt_ref(nt, recv, "receiver");
    if (sym < 0 || !nt_type(nt, sym) || !sp_streq(nt_type(nt, sym), "SymbolNode")) continue;
    const char *mname = nt_str(nt, sym, "value");
    if (!mname || !*mname) continue;
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;            /* needs the receiver argument */
    int newrecv = argv[0];
    int nrest = argc - 1;
    if (nrest > 64) continue;
    int rest[64];
    for (int k = 0; k < nrest; k++) rest[k] = argv[k + 1];  /* copy before realloc */
    char namebuf[256];
    snprintf(namebuf, sizeof namebuf, "%s", mname);         /* copy before realloc */
    int base = nt->count;
    int newargs = nt_new_node(nt, "ArgumentsNode");
    if (newargs < 0) continue;
    nt_node_set_arr(nt, newargs, "arguments", rest, nrest);
    nt_node_set_ref(nt, id, "receiver", newrecv);           /* receiver = first arg */
    nt_node_set_str(nt, id, "name", namebuf);               /* call the named method */
    nt_node_set_ref(nt, id, "arguments", newargs);          /* drop the receiver arg */
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* Resolve a forwarded callable reference (`&inline_lambda` / `&proc_var` /
   `&method(:m)`) to the body statements and parameters of its definition.
   Returns 1 with *out_body / *out_pn set, else 0. Mirrors fwd_callable_arity's
   resolution but exposes the body so a caller can inspect how a param is used. */
static int fwd_callable_def(Compiler *c, int ref, int *out_body, int *out_pn) {
  NodeTable *nt = (NodeTable *)c->nt;
  const char *ty = nt_type(nt, ref);
  if (!ty) return 0;
  if (sp_streq(ty, "CallNode") && nt_str(nt, ref, "name") &&
      sp_streq(nt_str(nt, ref, "name"), "method")) {
    int mi = method_obj_target_mi(c, ref);
    if (mi < 0) return 0;
    int dn = c->scopes[mi].def_node;
    *out_body = c->scopes[mi].body;
    *out_pn = dn >= 0 ? nt_ref(nt, dn, "parameters") : -1;
    return *out_body >= 0;
  }
  /* `<callable>.to_proc` is the callable itself (Method#to_proc / Proc#to_proc
     are identity for these purposes): resolve through the receiver so a curried
     `method(:m).to_proc.curry` sees the target's arity and return type (#3183). */
  if (sp_streq(ty, "CallNode") && nt_str(nt, ref, "name") &&
      sp_streq(nt_str(nt, ref, "name"), "to_proc")) {
    int r = nt_ref(nt, ref, "receiver");
    return r >= 0 ? fwd_callable_def(c, r, out_body, out_pn) : 0;
  }
  int create = -1;
  if (sp_streq(ty, "LambdaNode") || is_proc_create(c, ref)) create = ref;
  else if (sp_streq(ty, "LocalVariableReadNode")) {
    const char *vn = nt_str(nt, ref, "name");
    Scope *sc = vn ? comp_scope_of(c, ref) : NULL;
    for (int w = 0; vn && w < nt->count; w++) {
      const char *wty = nt_type(nt, w);
      if (!wty || !sp_streq(wty, "LocalVariableWriteNode")) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != sc) continue;
      int val = nt_ref(nt, w, "value");
      if (val >= 0 && is_proc_create(c, val)) { create = val; break; }
      /* the write's value may itself resolve through this walker -- a stored
         Method (`m = method(:greet); g = m.to_proc.curry`) or a chained
         local. Depth-guard against cyclic assignments (#3244). */
      if (val >= 0) {
        static int fcd_depth = 0;
        if (fcd_depth < 64) {
          fcd_depth++;
          int ok2 = fwd_callable_def(c, val, out_body, out_pn);
          fcd_depth--;
          if (ok2) return 1;
        }
      }
    }
    /* a method param holding the callable: resolve through a call site's
       argument expression (the first site passing a proc literal wins) */
    if (create < 0 && vn && sc && sc->name) {
      int pidx = -1;
      for (int pi = 0; pi < sc->nparams; pi++)
        if (sc->pnames[pi] && sp_streq(sc->pnames[pi], vn)) { pidx = pi; break; }
      if (pidx >= 0) {
        NT_FOREACH_KIND(nt, NK_CallNode, cs2) {
          if (comp_scope_of(c, cs2) == sc) continue;   /* not our own body */
          const char *cn2 = nt_str(nt, cs2, "name");
          if (!cn2 || !sp_streq(cn2, sc->name) || nt_ref(nt, cs2, "receiver") >= 0) continue;
          int a3 = nt_ref(nt, cs2, "arguments");
          int ac3 = 0; const int *av3 = a3 >= 0 ? nt_arr(nt, a3, "arguments", &ac3) : NULL;
          if (pidx < ac3 && av3 && nt_type(nt, av3[pidx]) &&
              (sp_streq(nt_type(nt, av3[pidx]), "LambdaNode") || is_proc_create(c, av3[pidx]))) {
            create = av3[pidx];
            break;
          }
        }
      }
    }
  }
  if (create < 0) return 0;
  *out_body = a_proc_body(c, create);
  *out_pn = a_proc_params_node(c, create);
  return *out_body >= 0;
}

/* Unify into *acc the element type pushed onto a local named `memo` (`memo << e`
   / `memo.push(e)`) anywhere in the subtree rooted at `id`. */
static void ewo_scan_pushes(Compiler *c, int id, const char *memo, TyKind *acc) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (id < 0) return;
  const char *ty = nt_type(nt, id);
  if (ty && sp_streq(ty, "CallNode")) {
    const char *nm = nt_str(nt, id, "name");
    int rcv = nt_ref(nt, id, "receiver");
    const char *rty = rcv >= 0 ? nt_type(nt, rcv) : NULL;
    if (nm && rty && sp_streq(rty, "LocalVariableReadNode") &&
        nt_str(nt, rcv, "name") && sp_streq(nt_str(nt, rcv, "name"), memo) &&
        (sp_streq(nm, "<<") || sp_streq(nm, "push"))) {
      int args = nt_ref(nt, id, "arguments");
      int an = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      for (int k = 0; k < an; k++) *acc = ty_unify(*acc, infer_type(c, argv[k]));
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(nt, id, i); if (ch >= 0) ewo_scan_pushes(c, ch, memo, acc); }
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) if (ids[k] >= 0) ewo_scan_pushes(c, ids[k], memo, acc); }
}

/* The element type an `each_with_object([])` array accumulator is filled with,
   inferred from how its memo param (block param 1) is used. Scans the block body
   for pushes onto memo; when the body merely forwards to a callable
   (`callable.call(elem, memo)` -- the value-forwarding desugar), follows into the
   callable's definition and scans its 2nd param the same way. Returns the unified
   pushed element type, or TY_UNKNOWN when no push is found (callers keep the
   empty-`[]` int_array default). */
TyKind ewo_memo_elem_type(Compiler *c, int callid) {
  NodeTable *nt = (NodeTable *)c->nt;
  int block = nt_ref(nt, callid, "block");
  const char *bty = block >= 0 ? nt_type(nt, block) : NULL;
  if (!bty || !sp_streq(bty, "BlockNode")) return TY_UNKNOWN;  /* not yet a literal block */
  const char *memo = block_param_name(c, block, 1);
  int body = nt_ref(nt, block, "body");
  if (!memo || body < 0) return TY_UNKNOWN;

  /* Direct: the block body itself fills memo. */
  TyKind acc = TY_UNKNOWN;
  ewo_scan_pushes(c, body, memo, &acc);
  if (acc != TY_UNKNOWN) return acc;

  /* Forwarded: a single `callable.call(elem, memo)` -- follow into the callable. */
  int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
  if (bn != 1 || !bb) return TY_UNKNOWN;
  int call = bb[0];
  if (!nt_type(nt, call) || !sp_streq(nt_type(nt, call), "CallNode")) return TY_UNKNOWN;
  if (!nt_str(nt, call, "name") || !sp_streq(nt_str(nt, call, "name"), "call")) return TY_UNKNOWN;
  int rcv = nt_ref(nt, call, "receiver");
  int cargs = nt_ref(nt, call, "arguments");
  int cn = 0; const int *cargv = cargs >= 0 ? nt_arr(nt, cargs, "arguments", &cn) : NULL;
  if (rcv < 0 || cn < 1 || !cargv) return TY_UNKNOWN;
  int last = cargv[cn - 1];
  if (!nt_type(nt, last) || !sp_streq(nt_type(nt, last), "LocalVariableReadNode")) return TY_UNKNOWN;
  if (!nt_str(nt, last, "name") || !sp_streq(nt_str(nt, last, "name"), memo)) return TY_UNKNOWN;

  int cb_body = -1, cb_pn = -1;
  if (!fwd_callable_def(c, rcv, &cb_body, &cb_pn) || cb_pn < 0) return TY_UNKNOWN;
  int rn = 0; const int *reqs = nt_arr(nt, cb_pn, "requireds", &rn);
  if (rn < 2 || !reqs) return TY_UNKNOWN;  /* the callable's memo is its 2nd param */
  const char *cb_memo = nt_str(nt, reqs[1], "name");
  if (!cb_memo) return TY_UNKNOWN;
  TyKind acc2 = TY_UNKNOWN;
  ewo_scan_pushes(c, cb_body, cb_memo, &acc2);
  return acc2;
}

/* The arity and body-return type of the proc a curry was built from. */
static int curry_proc_base(Compiler *c, int recv, int *arity, TyKind *ret) {
  NodeTable *nt = (NodeTable *)c->nt;
  int body = -1, pn = -1;
  if (!fwd_callable_def(c, recv, &body, &pn)) return 0;
  int rn = 0; if (pn >= 0) nt_arr(nt, pn, "requireds", &rn);
  *arity = rn;
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  *ret = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_NIL;
  return 1;
}

/* Walk a curry chain to its base proc, counting args applied through `node`
   (`proc.curry` -> 0, each `[arg]` / `.call(arg)` adds 1, a var resolves to its
   assigned curry expression). Sets *applied, *arity, *ret on success. */
static int curry_chain(Compiler *c, int node, int *applied, int *arity, TyKind *ret, int depth) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (depth > 64) return 0;  /* guard against cyclic var assignments (a=b; b=a) */
  const char *ty = nt_type(nt, node);
  if (!ty) return 0;
  if (sp_streq(ty, "CallNode")) {
    const char *nm = nt_str(nt, node, "name");
    int recv = nt_ref(nt, node, "receiver");
    if (!nm || recv < 0) return 0;
    if (sp_streq(nm, "curry")) {
      if (!curry_proc_base(c, recv, arity, ret)) return 0;
      *applied = 0;
      return 1;
    }
    if (sp_streq(nm, "[]") || sp_streq(nm, "call") || sp_streq(nm, "()")) {
      if (!curry_chain(c, recv, applied, arity, ret, depth + 1)) return 0;
      /* one application per argument: curry[a, b] applies two */
      int a2 = nt_ref(nt, node, "arguments");
      int ac2 = 0;
      if (a2 >= 0) nt_arr(nt, a2, "arguments", &ac2);
      *applied += ac2;
      return 1;
    }
    return 0;
  }
  if (sp_streq(ty, "LocalVariableReadNode")) {
    const char *vn = nt_str(nt, node, "name");
    Scope *sc = vn ? comp_scope_of(c, node) : NULL;
    for (int w = 0; vn && w < nt->count; w++) {
      if (!nt_type(nt, w) || !sp_streq(nt_type(nt, w), "LocalVariableWriteNode")) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != sc) continue;
      int val = nt_ref(nt, w, "value");
      if (val >= 0) return curry_chain(c, val, applied, arity, ret, depth + 1);
    }
    return 0;
  }
  return 0;
}

/* Does applying one more arg at curry-application `node` reach the base proc's
   arity (completing it)? Sets *out_ret to the proc's return type. Returns 1 when
   `node` is a recognized curry chain. */
int curry_apply_info(Compiler *c, int node, int *out_complete, TyKind *out_ret) {
  int applied = 0, arity = 0; TyKind ret = TY_UNKNOWN;
  if (!curry_chain(c, node, &applied, &arity, &ret, 0)) return 0;
  *out_complete = (arity > 0 && applied >= arity);
  *out_ret = ret;
  return 1;
}

/* `recv.to_h { |e| [k, v] }` -> `recv.map { |e| [k, v] }.to_h`. The block-taking
   to_h maps each element to a [key, value] pair and collects the pairs into a
   hash; map already lowers the block for any iterable and the blockless to_h
   already builds a typed hash from an array of pairs, so rewriting onto that
   pair reuses both instead of adding a bespoke hash-building iterator. */
int desugar_to_h_block(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "to_h")) continue;
    int recv = nt_ref(nt, id, "receiver");
    int blk = nt_ref(nt, id, "block");
    if (recv < 0 || blk < 0) continue;                 /* need a receiver and a block */
    if (!nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockNode")) continue;
    /* Only builtin iterables lower onto map{}.to_h. A Struct/Data or other user
       object with a block-taking to_h has its own member-pair path; rewriting it
       onto map would change the element protocol and mistype the result. */
    TyKind rt = infer_type(c, recv);
    if (!ty_is_array(rt) && !ty_is_hash(rt) && rt != TY_RANGE && rt != TY_ENUMERATOR) continue;
    int base = nt->count;
    int mapargs = nt_new_node(nt, "ArgumentsNode");
    int mapcall = nt_new_node(nt, "CallNode");
    if (mapargs < 0 || mapcall < 0) continue;          /* node-table OOM: leave as-is */
    nt_node_set_arr(nt, mapargs, "arguments", NULL, 0); /* map takes no positional args */
    nt_node_set_ref(nt, mapcall, "receiver", recv);
    nt_node_set_str(nt, mapcall, "name", "map");
    nt_node_set_ref(nt, mapcall, "arguments", mapargs);
    nt_node_set_ref(nt, mapcall, "block", blk);
    nt_node_set_ref(nt, id, "receiver", mapcall);      /* to_h now consumes the mapped pairs */
    nt_node_set_ref(nt, id, "block", -1);              /* and no longer carries the block */
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl; /* new nodes share the scope */
    changed = 1;
  }
  return changed;
}

/* Descend `root`'s subtree (bounded to scope `sc`) tracking the nearest enclosing
   StatementsNode entry (curr_st/curr_idx). On reaching `target`, report that entry
   -- the innermost same-scope top-level statement whose subtree contains target.
   A single O(N) pass that prunes at nested scope boundaries. */
static int tp_find_stmt(Compiler *c, int root, int target, int sc,
                        int curr_st, int curr_idx, int *out_st, int *out_idx) {
  if (root < 0 || c->nscope[root] != sc) return 0;   /* out of scope -> prune */
  if (root == target) {
    if (curr_st < 0) return 0;                        /* no enclosing statement */
    *out_st = curr_st; *out_idx = curr_idx; return 1;
  }
  NodeTable *nt = (NodeTable *)c->nt;
  int is_stmt = nt_type(nt, root) && sp_streq(nt_type(nt, root), "StatementsNode");
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) {
    int ch = nt_ref_at(nt, root, i);
    if (ch >= 0 && tp_find_stmt(c, ch, target, sc, curr_st, curr_idx, out_st, out_idx)) return 1;
  }
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *a = nt_arr_at(nt, root, i, &n);
    for (int k = 0; k < n; k++) {
      if (a[k] < 0) continue;
      int nst = is_stmt ? root : curr_st, nidx = is_stmt ? k : curr_idx;
      if (tp_find_stmt(c, a[k], target, sc, nst, nidx, out_st, out_idx)) return 1;
    }
  }
  return 0;
}

/* Locate the innermost same-scope StatementsNode and the index of the top-level
   statement whose subtree contains `id`. Returns 1 with *out_st/*out_idx set. */
static int tp_enclosing_stmt(Compiler *c, int id, int *out_st, int *out_idx) {
  int sc = c->nscope[id];
  if (sc < 0 || sc >= c->nscopes) return 0;
  return tp_find_stmt(c, c->scopes[sc].body, id, sc, -1, -1, out_st, out_idx);
}

/* `recv.iter(&obj)` where obj is a user object defining `to_proc`: Ruby calls
   obj.to_proc exactly ONCE to obtain the block. Hoist `__tproc_N = obj.to_proc`
   to the statement enclosing the call and rewrite the block argument to the
   hoisted local, so the value-callable desugar below forwards the once-computed
   proc (mirroring Ruby's `&obj` => `obj.to_proc` model, evaluated once). Declines
   -- leaving a loud reject -- when the enclosing statement can't be located. */
int desugar_to_proc_block_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockArgumentNode")) continue;
    int ex = nt_ref(nt, blk, "expression");
    if (ex < 0) continue;
    const char *exty = nt_type(nt, ex);
    if (!exty || sp_streq(exty, "SymbolNode")) continue;  /* &:sym lowers separately */
    /* Only a user object defining #to_proc (not a Proc/Method value or symbol). */
    TyKind ct = infer_type(c, ex);
    if (!ty_is_object(ct)) continue;
    int cid = ty_object_class(ct);
    if (cid < 0 || comp_method_in_chain(c, cid, "to_proc", NULL) < 0) continue;
    int st = -1, idx = -1;
    if (!tp_enclosing_stmt(c, id, &st, &idx)) continue;  /* can't hoist -> leave (reject) */
    int encl = c->nscope[id];
    int base = nt->count;
    int exclone = nt_clone_subtree(nt, ex);
    int tpcall = nt_new_node(nt, "CallNode");
    int wnode = nt_new_node(nt, "LocalVariableWriteNode");
    int rd = nt_new_node(nt, "LocalVariableReadNode");
    if (exclone < 0 || tpcall < 0 || wnode < 0 || rd < 0) continue;
    char tpname[48];
    snprintf(tpname, sizeof tpname, "__tproc_%d", id);
    nt_node_set_ref(nt, tpcall, "receiver", exclone);
    nt_node_set_str(nt, tpcall, "name", "to_proc");
    nt_node_set_ref(nt, tpcall, "arguments", -1);
    nt_node_set_ref(nt, tpcall, "block", -1);
    nt_node_set_str(nt, wnode, "name", tpname);
    nt_node_set_ref(nt, wnode, "value", tpcall);
    nt_node_set_str(nt, rd, "name", tpname);
    nt_node_set_ref(nt, blk, "expression", rd);  /* block arg now &__tproc_N */
    /* insert `wnode` before body[idx] in the enclosing StatementsNode */
    int bn = 0; const int *body = nt_arr(nt, st, "body", &bn);
    int *nb = malloc(sizeof(int) * (size_t)(bn + 1));
    if (!nb) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int k = 0; k < idx; k++) nb[k] = body[k];
    nb[idx] = wnode;
    for (int k = idx; k < bn; k++) nb[k + 1] = body[k];
    nt_node_set_arr(nt, st, "body", nb, bn + 1);
    free(nb);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    LocalVar *lv = scope_local_intern(comp_scope_of(c, id), tpname);
    lv->type = TY_PROC;
    changed = 1;
  }
  return changed;
}

/* `m(&(a >> b))`: an inline proc-composition (or any Proc-valued expression
   that is not already a simple read) as a block argument. The block-argument
   lowering wants a value it can name, so hoist the expression into a temp on
   the enclosing statement and pass that. The same composition assigned to a
   local first already compiled; this makes the inline form agree. (#3117) */
int desugar_proc_expr_block_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockArgumentNode")) continue;
    int ex = nt_ref(nt, blk, "expression");
    if (ex < 0) continue;
    const char *exty = nt_type(nt, ex);
    if (!exty || !sp_streq(exty, "CallNode")) continue;
    /* only the Proc combinators: everything else keeps its own lowering
       (&:sym, &obj.to_proc, &method(:m), a lambda literal, ...) */
    const char *exn = nt_str(nt, ex, "name");
    if (!exn || (!sp_streq(exn, ">>") && !sp_streq(exn, "<<"))) continue;
    if (infer_type(c, ex) != TY_PROC) continue;
    int st = -1, idx = -1;
    if (!tp_enclosing_stmt(c, id, &st, &idx)) continue;
    int encl = c->nscope[id];
    int base = nt->count;
    int wnode = nt_new_node(nt, "LocalVariableWriteNode");
    int rd = nt_new_node(nt, "LocalVariableReadNode");
    if (wnode < 0 || rd < 0) continue;
    char bname[48];
    snprintf(bname, sizeof bname, "__blkexpr_%d", id);
    nt_node_set_str(nt, wnode, "name", bname);
    nt_node_set_ref(nt, wnode, "value", ex);
    nt_node_set_str(nt, rd, "name", bname);
    nt_node_set_ref(nt, blk, "expression", rd);
    int bn = 0; const int *body = nt_arr(nt, st, "body", &bn);
    int *nb = malloc(sizeof(int) * (size_t)(bn + 1));
    if (!nb) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int k = 0; k < idx; k++) nb[k] = body[k];
    nb[idx] = wnode;
    for (int k = idx; k < bn; k++) nb[k + 1] = body[k];
    nt_node_set_arr(nt, st, "body", nb, bn + 1);
    free(nb);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    LocalVar *lv = scope_local_intern(comp_scope_of(c, id), bname);
    lv->type = TY_PROC;
    changed = 1;
  }
  return changed;
}

/* `f(**obj)` where obj is a user object defining `#to_hash`: Ruby converts it
   through to_hash. Rewrite the splat's value from `obj` to `obj.to_hash` so the
   existing double-splat machinery (which pre-evaluates the source hash into a
   temp -- once) forwards the converted hash. Mirrors the `to_ary` splice
   coercion; once rewritten the value is a to_hash call and is not revisited. */
int desugar_to_hash_splat(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "AssocSplatNode")) continue;
    int val = nt_ref(nt, id, "value");
    if (val < 0) continue;
    if (nt_type(nt, val) && sp_streq(nt_type(nt, val), "CallNode") &&
        nt_str(nt, val, "name") && sp_streq(nt_str(nt, val, "name"), "to_hash")) continue;
    TyKind t = infer_type(c, val);
    if (!ty_is_object(t)) continue;
    int cid = ty_object_class(t);
    if (cid < 0 || comp_method_in_chain(c, cid, "to_hash", NULL) < 0) continue;
    int base = nt->count;
    int clone = nt_clone_subtree(nt, val);
    int call = nt_new_node(nt, "CallNode");
    if (clone < 0 || call < 0) {
      /* A partial allocation (clone appended nodes but the call node failed)
         leaves nodes past `base` whose c->nscope would otherwise stay
         uninitialized, desyncing the arrays from nt->count for later passes. */
      if (nt->count > base) {
        comp_grow_node_arrays(c);
        int encl = c->nscope[id];
        for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
      }
      continue;
    }
    nt_node_set_ref(nt, call, "receiver", clone);
    nt_node_set_str(nt, call, "name", "to_hash");
    nt_node_set_ref(nt, call, "arguments", -1);
    nt_node_set_ref(nt, call, "block", -1);
    nt_node_set_ref(nt, id, "value", call);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* `def lz; [1,2,3].lazy.map { }; end; lz.first` -- a lazy chain returned from a
   parameterless method. A lazy value has no runtime representation to return,
   so the method body is not emittable at all; splice a clone of the chain into
   the call site instead, which is exactly what a local alias already gets. The
   method then has no callers left and drops out as dead code. Restricted by
   lazy_method_chain to a self-free body, so the clone means the same thing
   where it lands. */
int desugar_lazy_method_call(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_CallNode) continue;
    int chain = lazy_method_chain(c, recv);
    if (chain < 0) continue;
    int base = nt->count;
    int clone = nt_clone_subtree(nt, chain);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    if (clone < 0) continue;
    nt_node_set_ref(nt, id, "receiver", clone);
    /* the cloned stages carry block parameters that were interned in the
       CALLEE's scope; re-intern them here or their locals are never declared.
       Locals ASSIGNED inside a cloned block body need it too -- a block that
       writes a temp before using it emitted an undeclared identifier (#3367). */
    for (int j = base; j < nt->count; j++) {
      NodeKind jk = nt_kind(nt, j);
      if (jk == NK_BlockNode) {
        for (int k = 0; k < 4; k++) {
          const char *bp = block_param_name(c, j, k);
          if (!bp) break;
          scope_local_intern(&c->scopes[encl], bp);
        }
        continue;
      }
      if (jk == NK_LocalVariableWriteNode || jk == NK_LocalVariableTargetNode ||
          jk == NK_LocalVariableOperatorWriteNode || jk == NK_LocalVariableOrWriteNode ||
          jk == NK_LocalVariableAndWriteNode) {
        const char *wn = nt_str(nt, j, "name");
        if (wn) scope_local_intern(&c->scopes[encl], wn);
      }
    }
    changed = 1;
  }
  return changed;
}

int desugar_value_callable_forwards(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;  /* snapshot: synthetic nodes are appended past here */
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockArgumentNode")) continue;
    int ex = nt_ref(nt, blk, "expression");
    if (ex < 0) continue;  /* anonymous `&`: inline-forward path, not a value */
    const char *exty = nt_type(nt, ex);
    if (!exty) continue;
    int simple_ref = sp_streq(exty, "LocalVariableReadNode") ||
                     sp_streq(exty, "InstanceVariableReadNode");
    /* `&method(:m)`: a deterministic method-object lookup, safe to re-evaluate */
    int method_obj = sp_streq(exty, "CallNode") && nt_str(nt, ex, "name") &&
                     sp_streq(nt_str(nt, ex, "name"), "method");
    /* `&->(x){...}`: an inline lambda literal, equivalent to the block itself;
       building it per element has no observable side effect */
    int inline_lambda = sp_streq(exty, "LambdaNode");
    if (!simple_ref && !method_obj && !inline_lambda) continue;
    TyKind ct = infer_type(c, ex);
    /* A poly local can hold a callable produced by an operation whose static
       type stays poly -- e.g. `procs.reduce(:>>)`, a composed Proc. Forward it
       as a value callable too (its `.call` dispatches at runtime); restricted
       to a bare local/ivar read so re-evaluation is side-effect-free (#3167). */
    if (ct != TY_PROC && ct != TY_METHOD && !(ct == TY_POLY && simple_ref)) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    int encl = c->nscope[id];
    TyKind rt = infer_type(c, recv);
    /* Hash `each`/`each_pair` yields the [k,v] pair, forwarded as a single array
       argument `c.call([k, v])` (built below) -- correct for every arity: a
       1-param callable gets the pair, a 2-param proc auto-splats it, a 2-param
       lambda raises exactly as CRuby's `Hash#each(&lambda)` does. A Method object
       takes the pair via its array ABI; a proc/lambda value's param is typed as
       the pair array by the call-site argument inference (a container arg
       overrides the bare-int default). */
    TyKind pty[4];
    int arity;
    if (sp_streq(name, "each_with_object")) {
      /* each_with_object(init) { |elem, memo| }: two params, the element and the
         accumulator. Array receivers only (a `{}` hash memo is unsupported even
         for a literal block). The memo type is recovered from how the callable
         fills it (ewo_memo_elem_type) by infer_block_params, so seed it UNKNOWN;
         the wrap_pair / hash logic below does not apply. */
      if (!ty_is_array(rt)) continue;
      arity = 2;
      pty[0] = ty_array_elem(rt);
      pty[1] = TY_UNKNOWN;
      /* an empty `{}` memo makes the accumulator a general boxed hash, so the
         memo param is typed accordingly (an empty `[]` stays UNKNOWN and is
         recovered from its fills by ewo_memo_elem_type). */
      int ewo_a = nt_ref(nt, id, "arguments");
      int ewo_ac = 0; const int *ewo_av = ewo_a >= 0 ? nt_arr(nt, ewo_a, "arguments", &ewo_ac) : NULL;
      if (ewo_ac >= 1 && ewo_av) {
        const char *seedty = nt_type(nt, ewo_av[0]);
        int seed_n = 0;
        if (seedty && sp_streq(seedty, "HashNode") &&
            (nt_arr(nt, ewo_av[0], "elements", &seed_n), seed_n == 0))
          pty[1] = TY_POLY_POLY_HASH;
      }
    }
    else {
      arity = ty_block_yield(rt, name, pty, 4);
      if (arity < 1) continue;  /* not a context-free iterator (or recv unresolved) */
    }

    /* Hash forwarding. A Method object takes any hash iterator's yield directly
       through its array ABI (the [k,v] pair as one array for `each`, the bare
       key/value for `each_key`/`each_value`). A proc/lambda VALUE is reliable
       only for the `each` pair forwarded to a single param, whose pair-array
       type the call-site inference recovers (a container overriding the bare-int
       default). Every other hash + proc/lambda combination -- inline lambdas
       (cloned, no write to type from), multi-param procs (CRuby auto-splat), and
       the scalar `each_key`/`each_value` yields -- needs cross-procedural param
       typing not yet modeled, so decline to the pre-existing path. */
    int wrap_pair = 0;
    if (ty_is_hash(rt)) {
      if (ct == TY_METHOD) {
        wrap_pair = (arity == 2);  /* each: pair as array; each_key/value: bare value */
      }
      else {
        /* a proc/lambda (value or inline): the call-site inference types its
           params from the forwarded call. `each` to a 1-param callable gets the
           [k,v] pair as one array; to a 2-param one, k and v positionally
           (auto-splat by arity); each_key/each_value pass the bare key/value. */
        int cpc = fwd_callable_arity(c, ex);
        if (arity == 2 && cpc == 1) wrap_pair = 1;
        else if (arity == 2 && cpc == 2) wrap_pair = 0;
        else if (arity == 1) wrap_pair = 0;
        else continue;  /* arity-2 with unresolved callable arity */
      }
    }

    int base = nt->count;
    int proc_clone = nt_clone_subtree(nt, ex);  /* re-read the proc per element */
    if (proc_clone < 0) continue;

    int reqs[4], reads[4];
    char pn[48];
    int alloc_ok = 1;
    for (int k = 0; k < arity; k++) {
      snprintf(pn, sizeof pn, "__fwd_%d_%d", id, k);
      reqs[k] = nt_new_node(nt, "RequiredParameterNode");
      nt_node_set_str(nt, reqs[k], "name", pn);
      reads[k] = nt_new_node(nt, "LocalVariableReadNode");
      nt_node_set_str(nt, reads[k], "name", pn);
      if (reqs[k] < 0 || reads[k] < 0) { alloc_ok = 0; break; }
    }
    if (!alloc_ok) continue;  /* node-table OOM: leave the call in its &-form */
    int params = nt_new_node(nt, "ParametersNode");
    nt_node_set_arr(nt, params, "requireds", reqs, arity);
    int bparams = nt_new_node(nt, "BlockParametersNode");
    nt_node_set_ref(nt, bparams, "parameters", params);

    int callargs = nt_new_node(nt, "ArgumentsNode");
    if (wrap_pair) {
      int pairarr = nt_new_node(nt, "ArrayNode");
      nt_node_set_arr(nt, pairarr, "elements", reads, 2);
      nt_node_set_arr(nt, callargs, "arguments", &pairarr, 1);
    }
    else nt_node_set_arr(nt, callargs, "arguments", reads, arity);
    int callnode = nt_new_node(nt, "CallNode");
    nt_node_set_ref(nt, callnode, "receiver", proc_clone);
    nt_node_set_str(nt, callnode, "name", "call");
    nt_node_set_ref(nt, callnode, "arguments", callargs);
    nt_node_set_ref(nt, callnode, "block", -1);

    int body = nt_new_node(nt, "StatementsNode");
    nt_node_set_arr(nt, body, "body", &callnode, 1);
    int blocknode = nt_new_node(nt, "BlockNode");
    if (params < 0 || bparams < 0 || callargs < 0 || callnode < 0 || body < 0 ||
        blocknode < 0)
      continue;  /* node-table OOM: a -1 id is an out-of-bounds node index below */
    nt_node_set_ref(nt, blocknode, "parameters", bparams);
    nt_node_set_ref(nt, blocknode, "body", body);

    nt_node_set_ref(nt, id, "block", blocknode);  /* call now takes a literal block */

    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;

    Scope *bs = comp_scope_of(c, blocknode);
    for (int k = 0; k < arity; k++) {
      snprintf(pn, sizeof pn, "__fwd_%d_%d", id, k);
      LocalVar *lv = scope_local_intern(bs, pn);
      lv->is_block_param = 1;
      lv->type = pty[k];
    }
    changed = 1;
  }
  return changed;
}

/* ---- Destructuring block parameters -----------------------------------
 * A parenthesized block parameter (`|a, (b, c), d|`, `|a, (*), b|`) is parsed
 * as a MultiTargetNode in the requireds list. The per-iterator binding sites
 * only bind plain named params, so a MultiTargetNode param silently bound its
 * inner names to nil. Rather than teach every iterator emitter to destructure,
 * desugar each such param to a fresh throwaway param plus a prepended
 * destructuring assignment -- reusing the fully-working MultiWriteNode codegen
 * (`b, c = __destr`). One rewrite covers every binding site (each/map/proc.call/
 * yield/...). Runs before scope building so the new param and targets are
 * interned normally. */

static int bdp_fill_targets(NodeTable *nt, int src, int dst);

/* Convert one param-side destructuring target to its assignment-side form:
   RequiredParameterNode -> LocalVariableTargetNode, SplatNode's inner target
   likewise, nested MultiTargetNode recursively. Other nodes pass through.
   Returns the new node id, or -1 on node-table OOM. */
static int bdp_convert_target(NodeTable *nt, int node) {
  if (node < 0) return node;
  const char *ty = nt_type(nt, node);
  if (!ty) return node;
  if (sp_streq(ty, "RequiredParameterNode")) {
    const char *nm = nt_str(nt, node, "name");
    char *nmbuf = NULL;  /* copy: nt_new_node may realloc nm's storage */
    if (nm) {
      nmbuf = malloc(strlen(nm) + 1);
      if (!nmbuf) return -1;
      strcpy(nmbuf, nm);
    }
    int t = nt_new_node(nt, "LocalVariableTargetNode");
    if (t < 0) { free(nmbuf); return -1; }
    if (nmbuf) { nt_node_set_str(nt, t, "name", nmbuf); free(nmbuf); }
    return t;
  }
  if (sp_streq(ty, "SplatNode")) {
    int expr = nt_ref(nt, node, "expression");
    int cv = expr >= 0 ? bdp_convert_target(nt, expr) : -1;
    if (expr >= 0 && cv < 0) return -1;
    int s = nt_new_node(nt, "SplatNode");
    if (s < 0) return -1;
    if (expr >= 0) nt_node_set_ref(nt, s, "expression", cv);
    return s;
  }
  if (sp_streq(ty, "MultiTargetNode")) {
    int m = nt_new_node(nt, "MultiTargetNode");
    if (m < 0) return -1;
    if (!bdp_fill_targets(nt, node, m)) return -1;
    return m;
  }
  return node;
}

/* Copy the converted lefts/rest/rights of param-side MultiTargetNode `src` into
   `dst` (a MultiWriteNode or nested MultiTargetNode). Array results from nt_arr
   are copied before any node creation, since nt_new_node may realloc storage. */
static int bdp_fill_targets(NodeTable *nt, int src, int dst) {
  int nl = 0; const int *l0 = nt_arr(nt, src, "lefts", &nl);
  int *lc = NULL;
  if (nl > 0) { lc = malloc(sizeof(int) * nl); if (!lc) return 0; memcpy(lc, l0, sizeof(int) * nl); }
  int rest = nt_ref(nt, src, "rest");
  int nr = 0; const int *r0 = nt_arr(nt, src, "rights", &nr);
  int *rc = NULL;
  if (nr > 0) { rc = malloc(sizeof(int) * nr); if (!rc) { free(lc); return 0; } memcpy(rc, r0, sizeof(int) * nr); }
  int ok = 1;
  if (ok && nl > 0) {
    for (int i = 0; i < nl && ok; i++) { lc[i] = bdp_convert_target(nt, lc[i]); if (lc[i] < 0) ok = 0; }
    if (ok) nt_node_set_arr(nt, dst, "lefts", lc, nl);
  }
  if (ok && rest >= 0) {
    int cr = bdp_convert_target(nt, rest);
    if (cr < 0) ok = 0; else nt_node_set_ref(nt, dst, "rest", cr);
  }
  if (ok && nr > 0) {
    for (int i = 0; i < nr && ok; i++) { rc[i] = bdp_convert_target(nt, rc[i]); if (rc[i] < 0) ok = 0; }
    if (ok) nt_node_set_arr(nt, dst, "rights", rc, nr);
  }
  free(lc); free(rc);
  return ok;
}

/* True if a param-side target binds at least one name (so an assignment is
   worth prepending). A purely anonymous splat (`(*)`) binds nothing. */
static int bdp_has_name(NodeTable *nt, int node) {
  if (node < 0) return 0;
  const char *ty = nt_type(nt, node);
  if (!ty) return 0;
  if (sp_streq(ty, "RequiredParameterNode") || sp_streq(ty, "LocalVariableTargetNode"))
    return nt_str(nt, node, "name") != NULL;
  if (sp_streq(ty, "SplatNode")) return bdp_has_name(nt, nt_ref(nt, node, "expression"));
  if (sp_streq(ty, "MultiTargetNode")) {
    int n = 0; const int *l = nt_arr(nt, node, "lefts", &n);
    for (int i = 0; i < n; i++) if (bdp_has_name(nt, l[i])) return 1;
    if (bdp_has_name(nt, nt_ref(nt, node, "rest"))) return 1;
    int r = 0; const int *rr = nt_arr(nt, node, "rights", &r);
    for (int i = 0; i < r; i++) if (bdp_has_name(nt, rr[i])) return 1;
    return 0;
  }
  /* Any other target (ivar/gvar/cvar/const/...) binds a name; bdp_fill_targets
     passes it through unchanged to the MultiWriteNode codegen. */
  return 1;
}

int desugar_block_destructure_params(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int L = 0; L < n0; L++) {
    const char *ty = nt_type(nt, L);
    /* DefNode too: `def m((a, b))` destructures an array parameter with the
       same MultiTargetNode shape blocks use (its "parameters" is the bare
       ParametersNode; the conditional unwrap below handles both). */
    if (!ty || (!sp_streq(ty, "BlockNode") && !sp_streq(ty, "LambdaNode") &&
                !sp_streq(ty, "DefNode"))) continue;
    int bp = nt_ref(nt, L, "parameters");
    if (bp < 0) continue;
    const char *bpty = nt_type(nt, bp);
    int pn = (bpty && sp_streq(bpty, "BlockParametersNode")) ? nt_ref(nt, bp, "parameters") : bp;
    if (pn < 0) continue;
    const char *pnty = nt_type(nt, pn);
    if (!pnty || !sp_streq(pnty, "ParametersNode")) continue;
    int nreq = 0; const int *reqs0 = nt_arr(nt, pn, "requireds", &nreq);
    if (nreq == 0) continue;
    int has_multi = 0;
    for (int k = 0; k < nreq; k++) {
      const char *rty = nt_type(nt, reqs0[k]);
      if (rty && sp_streq(rty, "MultiTargetNode")) { has_multi = 1; break; }
    }
    if (!has_multi) continue;
    /* copy requireds: nt_new_node below may realloc the node storage reqs0 points into */
    int *reqs = malloc(sizeof(int) * nreq);
    if (!reqs) continue;
    memcpy(reqs, reqs0, sizeof(int) * nreq);
    int body = nt_ref(nt, L, "body");

    int *newreqs = malloc(sizeof(int) * nreq);
    int *mws = malloc(sizeof(int) * nreq);
    if (!newreqs || !mws) { free(reqs); free(newreqs); free(mws); continue; }
    int nmw = 0, ok = 1;
    for (int k = 0; k < nreq && ok; k++) {
      const char *rty = nt_type(nt, reqs[k]);
      if (!rty || !sp_streq(rty, "MultiTargetNode")) { newreqs[k] = reqs[k]; continue; }
      int bind = bdp_has_name(nt, reqs[k]);
      char nm[48]; snprintf(nm, sizeof nm, "__destr_%d_%d", L, k);
      int rp = nt_new_node(nt, "RequiredParameterNode");
      if (rp < 0) { ok = 0; break; }
      nt_node_set_str(nt, rp, "name", nm);
      newreqs[k] = rp;
      if (!bind) continue;  /* anonymous splat: nothing to assign */
      int rd = nt_new_node(nt, "LocalVariableReadNode");
      int mw = nt_new_node(nt, "MultiWriteNode");
      if (rd < 0 || mw < 0) { ok = 0; break; }
      nt_node_set_str(nt, rd, "name", nm);
      if (!bdp_fill_targets(nt, reqs[k], mw)) { ok = 0; break; }
      nt_node_set_ref(nt, mw, "value", rd);
      mws[nmw++] = mw;
    }
    if (!ok) { free(reqs); free(newreqs); free(mws); continue; }
    /* prepend the destructuring assignments (in param order) to the block body */
    if (nmw > 0) {
      int is_stmts = body >= 0 && nt_type(nt, body) && sp_streq(nt_type(nt, body), "StatementsNode");
      int obn = 0; const int *ob0 = is_stmts ? nt_arr(nt, body, "body", &obn) : NULL;
      int keep_body = (!is_stmts && body >= 0) ? 1 : 0;
      int cnt = nmw + (is_stmts ? obn : keep_body);
      int *bb = malloc(sizeof(int) * (cnt > 0 ? cnt : 1));
      if (!bb) ok = 0;
      else {
        for (int i = 0; i < nmw; i++) bb[i] = mws[i];
        if (is_stmts) {
          for (int i = 0; i < obn; i++) bb[nmw + i] = ob0[i];
          nt_node_set_arr(nt, body, "body", bb, cnt);
        }
        else {
          if (keep_body) bb[nmw] = body;
          int st = nt_new_node(nt, "StatementsNode");
          if (st < 0) ok = 0;
          else { nt_node_set_arr(nt, st, "body", bb, cnt); nt_node_set_ref(nt, L, "body", st); }
        }
        free(bb);
      }
    }
    /* Only swap in the throwaway params once the assignments are prepended, so a
       mid-transform OOM never leaves params rebound with nothing destructured. */
    if (!ok) { free(reqs); free(newreqs); free(mws); continue; }
    nt_node_set_arr(nt, pn, "requireds", newreqs, nreq);
    changed = 1;
    free(reqs); free(newreqs); free(mws);
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* Propagate `proc.call(args)` argument types onto the proc literal `create`'s
   required params: a concrete arg overrides a param still at its bare-int
   default (the fallback guess, no real evidence), otherwise unify. Returns 1 if
   any param type changed. Shared by the local-proc and inline-lambda call sites. */
static int cs_type_params(Compiler *c, int create, const int *argv, int argc) {
  NodeTable *nt = (NodeTable *)c->nt;
  int pn = a_proc_params_node(c, create);
  if (pn < 0) return 0;
  int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
  Scope *bs = comp_scope_of(c, create);
  int changed = 0;
  /* CRuby proc auto-splat: a single Array passed to a non-lambda proc taking
     more than one positional is destructured across the params, so each binds
     the array's element type (not the whole array). Lambdas are strict-arity
     and never auto-splat. */
  const char *cty = nt_type(nt, create);
  const char *cnm = nt_str(nt, create, "name");
  int is_lambda = (cty && sp_streq(cty, "LambdaNode")) || (cnm && sp_streq(cnm, "lambda"));
  if (!is_lambda && rn >= 2 && argc == 1) {
    TyKind a0 = infer_type(c, argv[0]);
    if (ty_is_array(a0)) {
      /* An unknown element type falls back to poly so the params stay boxed
         rather than defaulting to TY_INT (which would miscompile non-int
         elements passed at runtime). */
      TyKind et = ty_array_elem(a0);
      if (et == TY_UNKNOWN) et = TY_POLY;
      for (int k = 0; k < rn; k++) {
        const char *p = nt_str(nt, reqs[k], "name");
        if (!p) continue;
        LocalVar *lv = scope_local(bs, p);
        if (!lv) continue;
        TyKind merged = (lv->type == TY_INT) ? et : ty_unify(lv->type, et);
        if (merged != lv->type) { lv->type = merged; changed = 1; }
      }
      return changed;
    }
  }
  for (int k = 0; k < rn && k < argc; k++) {
    const char *p = nt_str(nt, reqs[k], "name");
    if (!p) continue;
    LocalVar *lv = scope_local(bs, p);
    if (!lv) continue;
    TyKind at = infer_type(c, argv[k]);
    if (at == TY_UNKNOWN || at == lv->type) continue;
    TyKind merged = (lv->type == TY_INT) ? at : ty_unify(lv->type, at);
    if (merged != lv->type) { lv->type = merged; changed = 1; }
  }
  return changed;
}

/* Register numbered params (_1.._9) used in a proc-literal body: they have
   no parameters node, so derive them from the body's local reads. Poly-typed
   boxed slots like every other first-class proc parameter. */
static int register_proc_numbered(Compiler *c, int create) {
  int body = a_proc_body(c, create);
  if (body < 0) return 0;
  ANameSet used = {0};
  a_collect_used(c, body, &used);
  Scope *bs = comp_scope_of(c, create);
  int changed = 0;
  for (int i = 0; i < used.n; i++) {
    const char *nm = used.v[i];
    if (!(nm && nm[0] == '_' && nm[1] >= '1' && nm[1] <= '9' && nm[2] == '\0')) continue;
    LocalVar *lv = scope_local_intern(bs, nm);
    lv->is_block_param = 1;
    if (lv->type != TY_POLY) { lv->type = TY_POLY; changed = 1; }
  }
  free(used.v);
  return changed;
}

/* A proc/lambda literal that is passed as a positional call argument escapes
   into an opaque method parameter: the callee invokes it through the generic
   type-erased sp_Proc* ABI (`pr.call(x)`), where every argument rides the boxed
   _sp_proc_poly_args side-channel. Its own arg types are therefore not knowable
   at the definition, so its un-typed params must default to poly (read the
   boxed slot) rather than int -- an int slot value-truncates a float arg to 0. */
static const NodeTable *ple_nt = NULL;
static int ple_ntc = -1;
static char *ple_escaped = NULL;
/* Writes whose value is a proc/lambda literal, collected once per build so
   marking a bare local read doesn't rescan the whole node table per argument
   (each such write is (scope, name) -> literal id). Proc-literal writes are
   rare, so a flat list walked per mark is plenty. */
static int *ple_pw = NULL;      /* triples: scope index, write node, literal */
static int ple_npw = 0;
/* Mark node `x` as escaping. If `x` is a bare local read of a proc, mark the
   proc LITERAL assigned to that local too: `f = ->(a){...}; g([f])` escapes the
   literal even though the container/arg holds a reference, not the literal
   itself (#3175). */
static void ple_mark_escaped(Compiler *c, int x) {
  const NodeTable *nt = c->nt;
  int n = ple_ntc;
  if (x < 0 || x >= n || !ple_escaped) return;
  ple_escaped[x] = 1;
  const char *xty = nt_type(nt, x);
  if (!xty || !sp_streq(xty, "LocalVariableReadNode")) return;
  const char *vn = nt_str(nt, x, "name");
  Scope *sc = vn ? comp_scope_of(c, x) : NULL;
  if (!vn || !sc) return;
  int si = (int)(sc - c->scopes);
  for (int i = 0; i < ple_npw; i++) {
    if (ple_pw[i * 3] != si) continue;
    const char *wn = nt_str(nt, ple_pw[i * 3 + 1], "name");
    if (!wn || !sp_streq(wn, vn)) continue;
    ple_escaped[ple_pw[i * 3 + 2]] = 1;
  }
}
static void ple_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  int n = nt->count;
  free(ple_escaped);
  ple_escaped = calloc((size_t)(n > 0 ? n : 1), 1);
  ple_nt = nt; ple_ntc = n;
  if (!ple_escaped) return;
  /* Collect proc-literal local writes first: ple_mark_escaped consults this
     instead of rescanning the table per marked argument. */
  free(ple_pw); ple_pw = NULL; ple_npw = 0;
  {
    int cap = 0;
    NT_FOREACH_KIND(nt, NK_LocalVariableWriteNode, w) {
      int val = nt_ref(nt, w, "value");
      if (val < 0 || val >= n || !nt_type(nt, val) ||
          !(sp_streq(nt_type(nt, val), "LambdaNode") || is_proc_create(c, val)))
        continue;
      Scope *sc = comp_scope_of(c, w);
      if (!sc) continue;
      if (ple_npw >= cap) {
        cap = cap ? cap * 2 : 64;
        int *np = realloc(ple_pw, sizeof(int) * 3 * (size_t)cap);
        if (!np) break;
        ple_pw = np;
      }
      ple_pw[ple_npw * 3] = (int)(sc - c->scopes);
      ple_pw[ple_npw * 3 + 1] = w;
      ple_pw[ple_npw * 3 + 2] = val;
      ple_npw++;
    }
  }
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    int ca = nt_ref(nt, id, "arguments");
    if (ca < 0) continue;
    int an = 0; const int *av = nt_arr(nt, ca, "arguments", &an);
    for (int k = 0; k < an; k++) ple_mark_escaped(c, av[k]);
  }
  /* A proc literal stored as a hash value or array element also escapes: it is
     read back as a boxed value and invoked through the type-erased ABI, so its
     args ride the boxed side-channel just like a proc passed as a call
     argument (#3178). */
  NT_FOREACH_KIND(nt, NK_HashNode, id) {
    int en = 0; const int *el = nt_arr(nt, id, "elements", &en);
    for (int k = 0; k < en; k++) {
      if (el[k] < 0 || !nt_type(nt, el[k]) || !sp_streq(nt_type(nt, el[k]), "AssocNode")) continue;
      ple_mark_escaped(c, nt_ref(nt, el[k], "value"));
    }
  }
  NT_FOREACH_KIND(nt, NK_ArrayNode, id) {
    int en = 0; const int *el = nt_arr(nt, id, "elements", &en);
    for (int k = 0; k < en; k++) ple_mark_escaped(c, el[k]);
  }
  /* A proc literal RETURNED from a method (an explicit `return ->(x){...}` or
     the body tail that is the implicit return) escapes: the caller invokes it
     through the type-erased ABI, so its own params must read the boxed side-
     channel rather than default to int (#3175). */
  NT_FOREACH_KIND(nt, NK_ReturnNode, id) {
    int ra = nt_ref(nt, id, "arguments");
    int rn = 0; const int *rv = ra >= 0 ? nt_arr(nt, ra, "arguments", &rn) : NULL;
    for (int k = 0; k < rn; k++) ple_mark_escaped(c, rv[k]);
  }
  NT_FOREACH_KIND(nt, NK_DefNode, id) {
    int body = nt_ref(nt, id, "body");
    if (body < 0 || !nt_type(nt, body) || !sp_streq(nt_type(nt, body), "StatementsNode")) continue;
    int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
    if (bn > 0 && bb[bn - 1] >= 0 && bb[bn - 1] < n &&
        nt_type(nt, bb[bn - 1]) && sp_streq(nt_type(nt, bb[bn - 1]), "LambdaNode"))
      ple_escaped[bb[bn - 1]] = 1;
  }
  /* A proc literal that is a BLOCK's tail value escapes too: a collecting
     iterator (`(0..2).map { ->(s){ s } }`) boxes it into the result array,
     from which it is invoked through the type-erased ABI (#3242). */
  NT_FOREACH_KIND(nt, NK_BlockNode, id) {
    int body = nt_ref(nt, id, "body");
    if (body < 0 || !nt_type(nt, body) || !sp_streq(nt_type(nt, body), "StatementsNode")) continue;
    int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
    if (bn > 0 && bb[bn - 1] >= 0 && bb[bn - 1] < n &&
        nt_type(nt, bb[bn - 1]) &&
        (sp_streq(nt_type(nt, bb[bn - 1]), "LambdaNode") || is_proc_create(c, bb[bn - 1])))
      ple_escaped[bb[bn - 1]] = 1;
  }
}
static int proc_literal_escapes_as_arg(Compiler *c, int lit) {
  const NodeTable *nt = c->nt;
  if (ple_nt != nt || ple_ntc != nt->count) ple_build(c);
  return ple_escaped && lit >= 0 && lit < nt->count && ple_escaped[lit];
}

int infer_block_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;

  /* Splat-rest / trailing-post params of proc literals: register them on the
     proc's scope so they are locals, not "uncaptured outer variables". The
     rest binds as a PolyArray built from the boxed arg side-channel; posts
     bind as boxed values -- both permanently poly (the callee cannot see its
     call sites' element types through a first-class proc). */
  NT_FOREACH_KIND(nt, NK_LambdaNode, id) {
    if (1) {
      int create = id;
      changed |= register_proc_numbered(c, create);
      int pn = a_proc_params_node(c, create);
      if (pn >= 0) {
        Scope *bs = comp_scope_of(c, create);
        int r = nt_ref(nt, pn, "rest");
        const char *rt = r >= 0 ? nt_type(nt, r) : NULL;
        const char *rname = (rt && sp_streq(rt, "RestParameterNode")) ? nt_str(nt, r, "name") : NULL;
        if (rname && rname[0]) {
          LocalVar *lv = scope_local_intern(bs, rname);
          lv->is_block_param = 1;
          if (lv->type != TY_POLY_ARRAY) { lv->type = TY_POLY_ARRAY; changed = 1; }
        }
        int np = 0; const int *posts = nt_arr(nt, pn, "posts", &np);
        for (int j = 0; j < np; j++) {
          const char *pname = nt_str(nt, posts[j], "name");
          if (!pname) continue;
          LocalVar *lv = scope_local_intern(bs, pname);
          lv->is_block_param = 1;
          if (lv->type != TY_POLY) { lv->type = TY_POLY; changed = 1; }
        }
        int nop = 0; const int *opts = nt_arr(nt, pn, "optionals", &nop);
        for (int j = 0; j < nop; j++) {
          const char *pname = nt_str(nt, opts[j], "name");
          if (!pname) continue;
          LocalVar *lv = scope_local_intern(bs, pname);
          lv->is_block_param = 1;
          if (lv->type != TY_POLY) { lv->type = TY_POLY; changed = 1; }
        }
      }
    }
  }
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *cn2 = nt_str(nt, id, "name");
    if (!cn2 || (!sp_streq(cn2, "proc") && !sp_streq(cn2, "lambda"))) continue;
    if (nt_ref(nt, id, "receiver") >= 0 || nt_ref(nt, id, "block") < 0) continue;
    changed |= register_proc_numbered(c, id);
    int pn = a_proc_params_node(c, id);
    if (pn < 0) continue;
    Scope *bs = comp_scope_of(c, id);
    int r = nt_ref(nt, pn, "rest");
    const char *rt = r >= 0 ? nt_type(nt, r) : NULL;
    const char *rname = (rt && sp_streq(rt, "RestParameterNode")) ? nt_str(nt, r, "name") : NULL;
    if (rname && rname[0]) {
      LocalVar *lv = scope_local_intern(bs, rname);
      lv->is_block_param = 1;
      if (lv->type != TY_POLY_ARRAY) { lv->type = TY_POLY_ARRAY; changed = 1; }
    }
    int np = 0; const int *posts = nt_arr(nt, pn, "posts", &np);
    for (int j = 0; j < np; j++) {
      const char *pname = nt_str(nt, posts[j], "name");
      if (!pname) continue;
      LocalVar *lv = scope_local_intern(bs, pname);
      lv->is_block_param = 1;
      if (lv->type != TY_POLY) { lv->type = TY_POLY; changed = 1; }
    }
    int nop = 0; const int *opts = nt_arr(nt, pn, "optionals", &nop);
    for (int j = 0; j < nop; j++) {
      const char *pname = nt_str(nt, opts[j], "name");
      if (!pname) continue;
      LocalVar *lv = scope_local_intern(bs, pname);
      lv->is_block_param = 1;
      if (lv->type != TY_POLY) { lv->type = TY_POLY; changed = 1; }
    }
    /* Keyword params (`proc { |a:, b: 5| }`): the call-site kwargs arrive as a
       boxed hash on the proc ABI, so the param binds a boxed value. */
    int nkw = 0; const int *kws = nt_arr(nt, pn, "keywords", &nkw);
    for (int j = 0; j < nkw; j++) {
      const char *pname = nt_str(nt, kws[j], "name");
      if (!pname) continue;
      LocalVar *lv = scope_local_intern(bs, pname);
      lv->is_block_param = 1;
      if (lv->type != TY_POLY) { lv->type = TY_POLY; changed = 1; }
    }
  }

  /* `->(x, ...) {}` (LambdaNode): its params live in the enclosing scope (no
     separate scope), like block params. Register them here; the int default is
     applied later, AFTER the call-site arg-type seeding below, so a `->(t){...}`
     later called as `f.call("x")` types `t` from the call (string) instead of
     unifying a premature int default with it into poly (#1372). */
  NT_FOREACH_KIND(nt, NK_LambdaNode, id) {
    int pn = nt_ref(nt, id, "parameters");      /* ParametersNode (1 level, unlike blocks) */
    if (pn < 0) continue;
    int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
    Scope *bs = comp_scope_of(c, id);
    for (int k = 0; k < rn; k++) {
      const char *p = nt_str(nt, reqs[k], "name");
      if (!p) continue;
      LocalVar *lv = scope_local_intern(bs, p); lv->is_block_param = 1;
    }
  }

  /* Hash.new { |hash, key| } : hash is the StrPolyHash, key the string key. */
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *cname = nt_str(nt, id, "name");
    if (!cname || !sp_streq(cname, "new")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "ConstantReadNode")) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || !sp_streq(rn, "Hash")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0) continue;
    int pn = nt_ref(nt, blk, "parameters");
    if (pn < 0) continue;
    int inner = nt_ref(nt, pn, "parameters");
    int pnode = inner >= 0 ? inner : pn;
    int rnp = 0; const int *reqs = nt_arr(nt, pnode, "requireds", &rnp);
    Scope *bs = comp_scope_of(c, blk);
    for (int k = 0; k < rnp; k++) {
      const char *p = nt_str(nt, reqs[k], "name");
      if (!p) continue;
      TyKind want = (k == 0) ? TY_POLY_POLY_HASH : TY_POLY;
      LocalVar *lv = scope_local_intern(bs, p); lv->is_block_param = 1;
      if (lv->type != want) { lv->type = want; changed = 1; }
    }
  }

  /* recv.instance_eval { |me| } : the block params all receive the receiver
     (Ruby yields self), typed as the receiver's object type. tap/then/
     yield_self also yield self to the block param (they do not rebind self, so
     only the param is typed here, not implicit-self calls) -- without this a
     `list.tap { |x| .. }` left x UNKNOWN, so a push of x into a typed array
     could not widen the array's element type (#3144). */
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *cname = nt_str(nt, id, "name");
    if (!cname) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    TyKind rt = infer_type(c, recv);
    int yields_self = sp_streq(cname, "tap") || sp_streq(cname, "then") ||
                      sp_streq(cname, "yield_self");
    if (yields_self) {
      /* tap/then/yield_self yield self to the block param for ANY receiver
         type (a string, an int, an object), so type the param as rt. */
      if (rt == TY_UNKNOWN) continue;
    }
    else {
      if (!ty_is_object(rt)) continue;
      if (!sp_streq(cname, "instance_eval") &&
          comp_trampoline_kind(c, ty_object_class(rt), cname, NULL) != 1) continue;
    }
    int blk = nt_ref(nt, id, "block");
    if (blk < 0) continue;
    int pn = nt_ref(nt, blk, "parameters");
    if (pn < 0) continue;
    Scope *bs = comp_scope_of(c, blk);
    const char *pnty = nt_type(nt, pn);
    if (pnty && sp_streq(pnty, "NumberedParametersNode")) {
      /* `{ _1.method }` : _1.._N all receive self (the receiver). */
      int maxn = (int)nt_int(nt, pn, "maximum", 0);
      for (int k = 1; k <= maxn; k++) {
        char nm[16]; snprintf(nm, sizeof nm, "_%d", k);
        LocalVar *lv = scope_local_intern(bs, nm); lv->is_block_param = 1;
        if (lv->type != rt) { lv->type = rt; changed = 1; }
      }
      continue;
    }
    int inner = nt_ref(nt, pn, "parameters");
    int pnode = inner >= 0 ? inner : pn;
    int rnp = 0; const int *reqs = nt_arr(nt, pnode, "requireds", &rnp);
    for (int k = 0; k < rnp; k++) {
      const char *p = nt_str(nt, reqs[k], "name");
      if (!p) continue;
      LocalVar *lv = scope_local_intern(bs, p); lv->is_block_param = 1;
      if (lv->type != rt) { lv->type = rt; changed = 1; }
    }
  }

  /* recv.instance_exec(args) { |params| } : block params take the call-site
     arg types (strict arity). */
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *cname = nt_str(nt, id, "name");
    if (!cname) continue;
    int xrecv = nt_ref(nt, id, "receiver");
    if (xrecv < 0) {
      /* receiverless instance_exec inside an instance method: params still
         take the call-site arg types; the receiver (self) is irrelevant here. */
      if (!sp_streq(cname, "instance_exec") || ie_implicit_self_class(c, id) < 0) continue;
    }
    else if (!sp_streq(cname, "instance_exec")) {
      TyKind xrt = infer_type(c, xrecv);
      if (!ty_is_object(xrt) ||
          comp_trampoline_kind(c, ty_object_class(xrt), cname, NULL) != 2) continue;
    }
    int blk = nt_ref(nt, id, "block");
    if (blk < 0) continue;
    int pn = nt_ref(nt, blk, "parameters");
    if (pn < 0) continue;
    int iargs = nt_ref(nt, id, "arguments");
    int iac = 0; const int *iav = iargs >= 0 ? nt_arr(nt, iargs, "arguments", &iac) : NULL;
    Scope *bs = comp_scope_of(c, blk);
    /* A trailing `k: v` call-site hash is not a positional arg; bind keyword
       block params to it by name. */
    int kwhash = ie_call_kwhash(c, id);
    if (kwhash >= 0) iac -= 1;
    const char *pnty = nt_type(nt, pn);
    if (pnty && sp_streq(pnty, "NumberedParametersNode")) {
      /* `{ _1 + _2 }` / `{ it ... }` (it normalizes to _1): bind _1.._N to the
         call-site arg types. */
      int maxn = (int)nt_int(nt, pn, "maximum", 0);
      for (int k = 0; k < maxn && k < iac; k++) {
        char npn[16]; snprintf(npn, sizeof npn, "_%d", k + 1);
        TyKind at = infer_type(c, iav[k]);
        LocalVar *lv = scope_local_intern(bs, npn); lv->is_block_param = 1;
        if (at != TY_UNKNOWN && lv->type != at) { lv->type = at; changed = 1; }
      }
      continue;
    }
    int inner = nt_ref(nt, pn, "parameters");
    int pnode = inner >= 0 ? inner : pn;
    int rnp = 0; const int *reqs = nt_arr(nt, pnode, "requireds", &rnp);
    /* mixed-args trampoline (`instance_exec(x, @base, 7, &b)`): bind each block
       param to the trampoline body's arg (caller arg substituted for a
       trampoline param read), not the caller's args. */
    int tramp_argc = !sp_streq(cname, "instance_exec") ? ie_tramp_effective_argc(c, id) : -1;
    /* auto-splat: a single array arg destructured across N>=2 params binds
       each to the element type. A sole splat (`instance_exec(*arr) { |a, b| }`)
       spreads the same way -- unwrap it to its array operand. A splat also
       spreads across a single param (`instance_exec(*arr) { |a| }` binds `a`
       to `arr[0]`), unlike a directly-passed array (which binds the whole array
       to a lone param), so allow `rnp >= 1` when explicitly splatted. */
    int arg0 = (iac == 1 && iav) ? iav[0] : -1;
    int is_splat = arg0 >= 0 && nt_type(nt, arg0) && sp_streq(nt_type(nt, arg0), "SplatNode");
    if (is_splat) arg0 = nt_ref(nt, arg0, "expression");
    if (tramp_argc < 0 && iac == 1 && (rnp >= 2 || (rnp >= 1 && is_splat)) && arg0 >= 0) {
      TyKind a0 = infer_type(c, arg0);
      if (ty_is_array(a0)) {
        TyKind et = ty_array_elem(a0);
        for (int k = 0; k < rnp; k++) {
          const char *p = nt_str(nt, reqs[k], "name");
          if (!p) continue;
          LocalVar *lv = scope_local_intern(bs, p); lv->is_block_param = 1;
          if (et != TY_UNKNOWN && lv->type != et) { lv->type = et; changed = 1; }
        }
        continue;
      }
    }
    for (int k = 0; k < rnp; k++) {
      const char *p = nt_str(nt, reqs[k], "name");
      if (!p) continue;
      int an = tramp_argc >= 0 ? ie_tramp_effective_arg(c, id, k) : (k < iac ? iav[k] : -1);
      if (an < 0) continue;
      TyKind at = infer_type(c, an);
      LocalVar *lv = scope_local_intern(bs, p); lv->is_block_param = 1;
      if (at != TY_UNKNOWN && lv->type != at) { lv->type = at; changed = 1; }
    }
    /* keyword block params (`|k:, j: 5|`): match the call-site `k: v` hash by
       name; an omitted optional keyword takes its default expr's type. */
    int nkw = 0; const int *kws = nt_arr(nt, pnode, "keywords", &nkw);
    for (int k = 0; k < nkw; k++) {
      const char *kpty = nt_type(nt, kws[k]);
      const char *kpn = nt_str(nt, kws[k], "name");
      if (!kpn) continue;
      int vn = ie_kwhash_value(c, kwhash, kpn);
      TyKind kt = TY_UNKNOWN;
      if (vn >= 0) kt = infer_type(c, vn);
      else if (kpty && sp_streq(kpty, "OptionalKeywordParameterNode")) {
        int dv = nt_ref(nt, kws[k], "value");
        if (dv >= 0) kt = infer_type(c, dv);
      }
      LocalVar *lv = scope_local_intern(bs, kpn); lv->is_block_param = 1;
      if (kt != TY_UNKNOWN && lv->type != kt) { lv->type = kt; changed = 1; }
    }
  }

  /* Fiber.new { |first| ... }: the block param receives the resume value,
     which is always a poly (boxed) value at the runtime ABI boundary. */
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *cname = nt_str(nt, id, "name");
    if (!cname || !sp_streq(cname, "new")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || !nt_type(nt, recv)) continue;
    const char *rrty = nt_type(nt, recv);
    int is_const = sp_streq(rrty, "ConstantReadNode") ||
                   (sp_streq(rrty, "ConstantPathNode") && nt_ref(nt, recv, "parent") < 0);
    if (!is_const) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || !sp_streq(rn, "Fiber")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0) continue;
    int pn = nt_ref(nt, blk, "parameters");
    if (pn < 0) continue;
    int inner = nt_ref(nt, pn, "parameters");
    int pnode = inner >= 0 ? inner : pn;
    int rnp = 0; const int *reqs = nt_arr(nt, pnode, "requireds", &rnp);
    Scope *bs = comp_scope_of(c, blk);
    for (int k = 0; k < rnp; k++) {
      const char *p = nt_str(nt, reqs[k], "name");
      if (!p) continue;
      LocalVar *lv = scope_local_intern(bs, p); lv->is_block_param = 1;
      if (lv->type == TY_UNKNOWN) { lv->type = TY_POLY; changed = 1; }
    }
  }

  /* Proc/lambda call-site param inference: `f.call(:a)` propagates arg types
     to the proc's params (e.g. `t` gets TY_SYMBOL instead of the default TY_INT). */
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *cname = nt_str(nt, id, "name");
    if (!cname || (!sp_streq(cname, "call") && !sp_streq(cname, "()") && !sp_streq(cname, "[]"))) continue;
    if (nt_int(nt, id, "rt_probe", 0)) continue;  /* analysis-only respond_to? probe */
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || infer_type(c, recv) != TY_PROC) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty) continue;
    int call_args = nt_ref(nt, id, "arguments");
    int argc = 0; const int *argv = NULL;
    if (call_args >= 0) argv = nt_arr(nt, call_args, "arguments", &argc);
    if (argc == 0) continue;
    /* The receiver is itself a proc/lambda literal -- e.g. a desugared inline
       `&->(x){...}` clone, whose params no var write would let us find -- so type
       its own params directly from the call args. */
    if (sp_streq(rty, "LambdaNode") || is_proc_create(c, recv)) {
      if (cs_type_params(c, recv, argv, argc)) changed = 1;
      continue;
    }
    if (!sp_streq(rty, "LocalVariableReadNode")) continue;
    const char *varname = nt_str(nt, recv, "name");
    if (!varname) continue;
    Scope *call_scope = comp_scope_of(c, id);
    /* otherwise a local holding a proc: type the proc literal assigned to it */
    for (int w = 0; w < nt->count; w++) {
      const char *wty = nt_type(nt, w);
      if (!wty || !sp_streq(wty, "LocalVariableWriteNode")) continue;
      const char *wname = nt_str(nt, w, "name");
      if (!wname || !sp_streq(wname, varname)) continue;
      if (comp_scope_of(c, w) != call_scope) continue;
      int val = nt_ref(nt, w, "value");
      if (val < 0 || !is_proc_create(c, val)) continue;
      if (cs_type_params(c, val, argv, argc)) changed = 1;
    }
  }

  /* Lambda param int default, applied AFTER the call-site seeding above so it
     only fills params no call site typed -- the arithmetic-proc fallback,
     matching the proc-literal default loop below (#1372). */
  NT_FOREACH_KIND(nt, NK_LambdaNode, id) {
    int pn = nt_ref(nt, id, "parameters");
    if (pn < 0) continue;
    int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
    Scope *bs = comp_scope_of(c, id);
    TyKind deflt = proc_literal_escapes_as_arg(c, id) ? TY_POLY : TY_INT;
    for (int k = 0; k < rn; k++) {
      const char *p = nt_str(nt, reqs[k], "name");
      if (!p) continue;
      LocalVar *lv = scope_local(bs, p);
      if (lv && lv->type == TY_UNKNOWN) { lv->type = deflt; changed = 1; }
    }
  }

  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    int block = nt_ref(nt, id, "block");
    if (block < 0) continue;
    const char *name = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    if (!name) continue;

    /* proc {} / lambda {} / Proc.new {}: type the literal's block params.
       Without call-site arg-type inference (a later slice) default required
       params to int -- covers the common arithmetic proc and is overridden
       by any stronger inference that runs first. */
    if (is_proc_literal(c, id)) {
      Scope *bs = comp_scope_of(c, block);
      TyKind deflt = proc_literal_escapes_as_arg(c, id) ? TY_POLY : TY_INT;
      for (int k = 0; ; k++) {
        const char *bp = block_param_name(c, block, k);
        if (!bp) break;
        LocalVar *lv = scope_local_intern(bs, bp); lv->is_block_param = 1;
        if (lv->type == TY_UNKNOWN) { lv->type = deflt; changed = 1; }
      }
      continue;
    }

    /* Array.new(n) { |i| ... }: i is the integer index */
    if (recv >= 0 && sp_streq(name, "new") && nt_type(nt, recv) &&
        sp_streq(nt_type(nt, recv), "ConstantReadNode") && nt_str(nt, recv, "name") &&
        sp_streq(nt_str(nt, recv, "name"), "Array")) {
      const char *p0 = block_param_name(c, block, 0);
      if (p0) { LocalVar *l = scope_local_intern(comp_scope_of(c, block), p0); l->is_block_param = 1;
                if (l->type != TY_INT) { l->type = TY_INT; changed = 1; } }
      continue;
    }

    /* File.open(args) { |f| ... }: f is a TY_POLY file handle */
    if (recv >= 0 && sp_streq(name, "open") && nt_type(nt, recv) &&
        sp_streq(nt_type(nt, recv), "ConstantReadNode") && nt_str(nt, recv, "name") &&
        (sp_streq(nt_str(nt, recv, "name"), "File") ||
         sp_streq(nt_str(nt, recv, "name"), "IO"))) {
      const char *p0 = block_param_name(c, block, 0);
      if (p0) { LocalVar *l = scope_local_intern(comp_scope_of(c, block), p0); l->is_block_param = 1;
                if (l->type != TY_POLY) { l->type = TY_POLY; changed = 1; } }
      continue;
    }

    /* struct.to_h { |k, v| ... }: k is a member symbol, v its (poly) value */
    if (recv >= 0 && sp_streq(name, "to_h")) {
      TyKind rt0 = infer_type(c, recv);
      if (ty_is_object(rt0) && c->classes[ty_object_class(rt0)].is_struct) {
        const char *kp = block_param_name(c, block, 0);
        const char *vp = block_param_name(c, block, 1);
        Scope *bs = comp_scope_of(c, block);
        if (kp) { LocalVar *l = scope_local_intern(bs, kp); l->is_block_param = 1; if (l->type != TY_SYMBOL) { l->type = TY_SYMBOL; changed = 1; } }
        if (vp) { LocalVar *l = scope_local_intern(bs, vp); l->is_block_param = 1; if (l->type != TY_POLY) { l->type = TY_POLY; changed = 1; } }
        continue;
      }
    }

    /* call to a user yielding method: block params take the yield arg types */
    {
      int mi = -1;
      if (recv < 0) {
        mi = comp_method_index(c, name);
        if (mi < 0) {
          Scope *self = comp_scope_of(c, id);
          if (self->class_id >= 0) {
            mi = comp_method_in_chain(c, self->class_id, name, NULL);
            /* inside a class method, a bare call also reaches sibling class
               methods (self is the class there) */
            if (mi < 0 && self->is_cmethod)
              mi = comp_cmethod_in_chain(c, self->class_id, name, NULL);
          }
        }
      }
      else {
        TyKind rt0 = infer_type(c, recv);
        if (ty_is_object(rt0)) mi = comp_method_in_chain(c, ty_object_class(rt0), name, NULL);
        /* Class.new { |...| }: the yielding method is Class#initialize */
        if (mi < 0 && sp_streq(name, "new") &&
            nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")) {
          const char *cname = nt_str(nt, recv, "name");
          int cid = cname ? comp_class_index(c, cname) : -1;
          if (cid >= 0) mi = comp_method_in_chain(c, cid, "initialize", NULL);
        }
        /* Class.method { ... }: look up the class method */
        if (mi < 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")) {
          const char *cname = nt_str(nt, recv, "name");
          int cid = cname ? comp_class_index(c, cname) : -1;
          if (cid >= 0) mi = comp_cmethod_in_chain(c, cid, name, NULL);
        }
        /* A poly / not-yet-resolved receiver (`arr[i].m { }`, a hash value
           read whose element class hasn't settled): the concrete class is
           unknown here, but codegen still inlines the method by runtime type,
           so the block's params must be declared. If exactly one user class
           defines a method by this name that yields or forwards a block, adopt
           it -- its param registration below then runs (types stay poly, which
           the boxed inline uses). (#2448) */
        if (mi < 0 && (rt0 == TY_POLY || rt0 == TY_UNKNOWN)) {
          int found = -1, ndef = 0;
          for (int k = 0; k < c->nclasses; k++) {
            int km = comp_method_in_chain(c, k, name, NULL);
            if (km < 0) continue;
            Scope *ks = &c->scopes[km];
            int forwards = ks->yields || (ks->blk_param && ks->blk_param[0]) ||
                           forwarding_yield_target(c, km, 0) >= 0;
            if (!forwards) continue;
            ndef++; found = km;
          }
          if (ndef == 1) mi = found;
        }
      }
      /* A block passed to a pure `...` forwarder is really consumed by the
         method the forward eventually reaches; type its params from there. */
      int yld_mi = mi;
      if (mi >= 0 && !c->scopes[mi].yields &&
          !(c->scopes[mi].blk_param && c->scopes[mi].blk_param[0])) {
        int t = forwarding_yield_target(c, mi, 0);
        if (t >= 0) yld_mi = t;
      }
      if (yld_mi >= 0 && c->scopes[yld_mi].yields) {
        int yn = first_yield(c, yld_mi);
        int ya = yn >= 0 ? nt_ref(nt, yn, "arguments") : first_block_call_args(c, yld_mi);
        if (ya < 0) ya = first_ie_exec_args(c, yld_mi);  /* instance_exec(args, &b) */
        int yc = 0;
        const int *yargs = ya >= 0 ? nt_arr(nt, ya, "arguments", &yc) : NULL;
        Scope *bs = comp_scope_of(c, block);
        /* CRuby auto-splat: one (non-splat) Array yielded to a block taking
           more than one binding slot -- or at least one slot plus a rest
           marker -- binds elements (mirrors emit_block_invoke's gate). The
           array's runtime length is unknown, so every destructured param can
           bind nil; widen them through poly rather than the bare element
           type (an int slot would render a missing position as 0). */
        int p_pre = 0; while (block_param_name(c, block, p_pre)) p_pre++;
        int p_opt = 0; while (block_opt_name(c, block, p_opt)) p_opt++;
        int p_post = 0; while (block_post_name(c, block, p_post)) p_post++;
        TyKind as_elem = TY_UNKNOWN;
        if (yc == 1 &&
            (p_pre + p_opt + p_post > 1 ||
             (p_pre + p_opt + p_post >= 1 && block_rest_marker(c, block))) &&
            !(nt_type(nt, yargs[0]) && sp_streq(nt_type(nt, yargs[0]), "SplatNode"))) {
          TyKind yat = infer_type(c, yargs[0]);
          if (ty_is_array(yat)) as_elem = ty_unify(ty_array_elem(yat), TY_POLY);
        }
        for (int k = 0; k < yc; k++) {
          const char *bp = block_param_name(c, block, k);
          if (!bp) continue;
          LocalVar *lv = scope_local_intern(bs, bp); lv->is_block_param = 1;
          /* unify position k across EVERY yield in the method: a mixed-type
             yielder (a heterogeneous Struct's synthesized each) delivers the
             union, not the first yield's type */
          TyKind at = as_elem;
          if (at == TY_UNKNOWN) {
            at = infer_type(c, yargs[k]);
            for (int _yi = 0; _yi < nt->count; _yi++) {
              if (nt_kind(nt, _yi) != NK_YieldNode) continue;
              if (c->nscope[_yi] != yld_mi || _yi == yn) continue;
              int _ya2 = nt_ref(nt, _yi, "arguments");
              int _yc2 = 0;
              const int *_yv2 = _ya2 >= 0 ? nt_arr(nt, _ya2, "arguments", &_yc2) : NULL;
              if (k < _yc2) at = ty_unify(at, infer_type(c, _yv2[k]));
            }
          }
          TyKind m = ty_unify(lv->type, at);
          if (m != lv->type) { lv->type = m; changed = 1; }
        }
        /* Params beyond the first yield's arity might still be nil if there
           are other yields with fewer args. Find the min yield arity. */
        int min_yc = yc;
        for (int _yi = 0; _yi < nt->count; _yi++) {
          if (!nt_type(nt, _yi) || !sp_streq(nt_type(nt, _yi), "YieldNode")) continue;
          if (c->nscope[_yi] != yld_mi) continue;
          int _ya = nt_ref(nt, _yi, "arguments");
          int _yc = 0;
          if (_ya >= 0) nt_arr(nt, _ya, "arguments", &_yc);
          if (_yc < min_yc) min_yc = _yc;
        }
        /* Block params at index >= min_yc can receive nil — widen to poly. */
        for (int k = min_yc; ; k++) {
          const char *bp = block_param_name(c, block, k);
          if (!bp) break;
          LocalVar *lv = scope_local_intern(bs, bp); lv->is_block_param = 1;
          TyKind m = ty_unify(lv->type, TY_POLY);
          if (m != lv->type) { lv->type = m; changed = 1; }
        }
        /* A trailing rest param (`|*a|`) collects the yielded arguments past the
           requireds into an array; emit_block_invoke binds it. Scoped to this
           yield-consumed block so iteration/escaped-proc blocks are unaffected. */
        const char *brest = block_rest_name(c, block);
        if (brest) {
          LocalVar *lv = scope_local_intern(bs, brest); lv->is_block_param = 1;
          if (lv->type != TY_POLY_ARRAY) { lv->type = TY_POLY_ARRAY; changed = 1; }
        }
        /* Optional block params (`|a, b=10|`): a yielded arg at the optional's
           position types it; an omitted optional takes its default's type. */
        int nreq_b = 0; while (block_param_name(c, block, nreq_b)) nreq_b++;
        for (int oi = 0; ; oi++) {
          const char *op = block_opt_name(c, block, oi);
          if (!op) break;
          int yi = nreq_b + oi;
          TyKind ot;
          if (as_elem != TY_UNKNOWN) {
            /* destructured: an optional binds an element or its default */
            int dv = block_opt_default(c, block, oi);
            ot = ty_unify(as_elem, dv >= 0 ? infer_type(c, dv) : TY_NIL);
          }
          else if (yi < yc) ot = infer_type(c, yargs[yi]);
          else { int dv = block_opt_default(c, block, oi); ot = dv >= 0 ? infer_type(c, dv) : TY_NIL; }
          LocalVar *lv = scope_local_intern(bs, op); lv->is_block_param = 1;
          TyKind m = ty_unify(lv->type, ot);
          if (m != lv->type) { lv->type = m; changed = 1; }
        }
        /* Post-required block params (`|a, *b, c|` -> c): an element (or the
           slot nil) in destructure mode, the positional yield arg otherwise;
           the runtime consumption point is unknown here, so unify with poly. */
        for (int qi = 0; ; qi++) {
          const char *qp = block_post_name(c, block, qi);
          if (!qp) break;
          TyKind qt2 = as_elem != TY_UNKNOWN ? as_elem : TY_POLY;
          LocalVar *lv = scope_local_intern(bs, qp); lv->is_block_param = 1;
          TyKind m = ty_unify(lv->type, ty_unify(qt2, TY_POLY));
          if (m != lv->type) { lv->type = m; changed = 1; }
        }
        /* Keyword block params (`|a:, b: 5|`): type from the trailing yielded
           kwargs hash by name; an omitted optional keyword takes its default. */
        int ykw = (yc > 0 && nt_type(nt, yargs[yc - 1]) &&
                   sp_streq(nt_type(nt, yargs[yc - 1]), "KeywordHashNode")) ? yargs[yc - 1] : -1;
        for (int ki = 0; ; ki++) {
          const char *kp = block_keyword_name(c, block, ki);
          if (!kp) break;
          int vn = ykw >= 0 ? ie_kwhash_value(c, ykw, kp) : -1;
          TyKind kt = TY_UNKNOWN;
          if (vn >= 0) kt = infer_type(c, vn);
          else { int dv = block_keyword_default(c, block, ki); if (dv >= 0) kt = infer_type(c, dv); }
          LocalVar *lv = scope_local_intern(bs, kp); lv->is_block_param = 1;
          if (kt != TY_UNKNOWN) { TyKind m = ty_unify(lv->type, kt); if (m != lv->type) { lv->type = m; changed = 1; } }
        }
        /* `**kw` keyword-rest: always a hash (an empty one when the yield
           carries no keyword arguments, matching CRuby) */
        {
          const char *kwr = block_kwrest_name(c, block);
          if (kwr) {
            LocalVar *lv = scope_local_intern(bs, kwr); lv->is_block_param = 1;
            TyKind m = ty_unify(lv->type, TY_POLY_POLY_HASH);
            if (m != lv->type) { lv->type = m; changed = 1; }
          }
        }
        continue;
      }
      /* Method with a named &block param (not inlined): blk_param.call(args)
         inside the method body determines the arg types for the call-site block. */
      if (mi >= 0 && !c->scopes[mi].yields &&
          c->scopes[mi].blk_param && c->scopes[mi].blk_param[0]) {
        const char *bpname = c->scopes[mi].blk_param;
        Scope *bs = comp_scope_of(c, block);
        for (int bid = 0; bid < nt->count; bid++) {
          const char *bty2 = nt_type(nt, bid);
          if (!bty2 || !sp_streq(bty2, "CallNode")) continue;
          const char *bcn = nt_str(nt, bid, "name");
          if (!bcn || !sp_streq(bcn, "call")) continue;
          int brecv = nt_ref(nt, bid, "receiver");
          if (brecv < 0) continue;
          const char *brecvty = nt_type(nt, brecv);
          if (!brecvty || !sp_streq(brecvty, "LocalVariableReadNode")) continue;
          const char *brecvnm = nt_str(nt, brecv, "name");
          if (!brecvnm || !sp_streq(brecvnm, bpname)) continue;
          if (comp_scope_of(c, bid) != &c->scopes[mi]) continue;
          int ba = nt_ref(nt, bid, "arguments");
          int barc = 0; const int *barg = NULL;
          if (ba >= 0) barg = nt_arr(nt, ba, "arguments", &barc);
          if (barc == 0) continue;
          for (int k = 0; k < barc; k++) {
            const char *bp = block_param_name(c, block, k);
            if (!bp) continue;
            LocalVar *lv = scope_local_intern(bs, bp); lv->is_block_param = 1;
            TyKind at = infer_type(c, barg[k]);
            if (at == TY_UNKNOWN || at == lv->type) continue;
            TyKind merged = ty_unify(lv->type, at);
            if (merged != lv->type) { lv->type = merged; changed = 1; }
          }
        }
        continue;
      }
    }

    if (recv < 0) continue;
    TyKind rt = infer_type(c, recv);
    /* A Range Enumerable served by materializing to an int array (each_slice/
       each_cons block forms, ...): type the block params as the array version,
       matching infer_call's redispatch and the codegen mirrors. */
    if (rt == TY_RANGE && range_enum_redispatch(c, id)) rt = TY_INT_ARRAY;
    if (ty_is_hash(rt) && hash_enum_redispatch(c, id)) rt = TY_POLY_ARRAY;
    const char *p0 = block_param_name(c, block, 0);
    if (!p0 && !block_param_is_multi(c, block, 0)) continue;

    /* then / yield_self: block param receives the receiver value */
    if ((sp_streq(name, "then") || sp_streq(name, "yield_self")) && p0) {
      Scope *bs = comp_scope_of(c, block);
      LocalVar *lv = scope_local_intern(bs, p0); lv->is_block_param = 1;
      TyKind m = ty_unify(lv->type, rt == TY_NIL ? TY_POLY : rt);
      if (m != lv->type) { lv->type = m; changed = 1; }
      continue;
    }

    TyKind pt = TY_UNKNOWN;
    if (sp_streq(name, "step") && (rt == TY_INT || rt == TY_FLOAT)) {
      /* a float receiver or float limit/step yields floats */
      int args = nt_ref(nt, id, "arguments");
      int sc = 0; const int *sv = args >= 0 ? nt_arr(nt, args, "arguments", &sc) : NULL;
      int isf = (rt == TY_FLOAT) || (sc >= 1 && infer_type(c, sv[0]) == TY_FLOAT) ||
                (sc >= 2 && infer_type(c, sv[1]) == TY_FLOAT);
      pt = isf ? TY_FLOAT : TY_INT;
    }
    else if (sp_streq(name, "step") && rt == TY_RANGE) {
      /* (range).step(k) { |x| }: float when the step or a literal range bound
         is float; mirrors emit_range_step_array's element type. */
      int args = nt_ref(nt, id, "arguments");
      int sc = 0; const int *sv = args >= 0 ? nt_arr(nt, args, "arguments", &sc) : NULL;
      int isf = sc >= 1 && infer_type(c, sv[0]) == TY_FLOAT;
      int rnn = recv;
      while (rnn >= 0 && nt_type(nt, rnn) && sp_streq(nt_type(nt, rnn), "ParenthesesNode")) {
        int rbody = nt_ref(nt, rnn, "body"); int rbn = 0;
        const int *rbd = rbody >= 0 ? nt_arr(nt, rbody, "body", &rbn) : NULL;
        rnn = rbn == 1 ? rbd[0] : -1;
      }
      if (rnn >= 0 && nt_type(nt, rnn) && sp_streq(nt_type(nt, rnn), "RangeNode")) {
        int lo = nt_ref(nt, rnn, "left"), hi = nt_ref(nt, rnn, "right");
        if ((lo >= 0 && infer_type(c, lo) == TY_FLOAT) ||
            (hi >= 0 && infer_type(c, hi) == TY_FLOAT)) isf = 1;
      }
      pt = isf ? TY_FLOAT : TY_INT;
    }
    else if (sp_streq(name, "step") && rt == TY_RATIONAL)
      pt = TY_POLY;  /* yields boxed Rational/Integer values (#2566) */
    else if ((sp_streq(name, "times") || sp_streq(name, "upto") ||
         sp_streq(name, "downto")) && rt == TY_INT)
      pt = TY_INT;
    else if (rt == TY_POLY && sp_streq(name, "each_line"))
      pt = TY_STRING;  /* File/IO object yielding lines */
    else if (rt == TY_POLY && sp_streq(name, "each_byte"))
      pt = TY_INT;
    else if (rt == TY_STRING && (sp_streq(name, "each_char") || sp_streq(name, "each_line") || sp_streq(name, "upto") ||
                                 sp_streq(name, "chars") || sp_streq(name, "lines") || sp_streq(name, "split")))
      pt = TY_STRING;  /* split { |piece| } yields each substring */
    else if (rt == TY_STRING && (sp_streq(name, "gsub") || sp_streq(name, "sub")))
      pt = TY_STRING;  /* block receives the matched substring */
    else if (rt == TY_STRING && (sp_streq(name, "each_byte") || sp_streq(name, "bytes") || sp_streq(name, "codepoints")))
      pt = TY_INT;
    else if (rt == TY_STRING && sp_streq(name, "scan")) {
      /* scan { |m| } yields each match; m is string (no captures) or str_array (captures) */
      int scan_args_id = nt_ref(nt, id, "arguments");
      int scan_argc = 0;
      const int *scan_argv = scan_args_id >= 0 ? nt_arr(nt, scan_args_id, "arguments", &scan_argc) : NULL;
      int has_cap = 0;
      if (scan_argc == 1 && scan_argv) {
        /* through a name too: `PAT = /(\d)(\w)/; s.scan(PAT) { |a, b| }` must
           destructure the capture row exactly as the inline literal does
           (#3391) */
        const char *src = an_regex_lit_src(c, scan_argv[0]);
        if (src && an_re_has_captures(src)) has_cap = 1;
      }
      /* a capturing scan yields each captures ROW (a boxed-element array);
         multiple params destructure it into strings */
      if (has_cap && block_param_name(c, block, 1)) {
        Scope *scs = comp_scope_of(c, block);
        for (int pk = 0; ; pk++) {
          const char *pn2 = block_param_name(c, block, pk);
          if (!pn2) break;
          LocalVar *plv2 = scope_local_intern(scs, pn2); plv2->is_block_param = 1;
          TyKind pm2 = ty_unify(plv2->type, TY_STRING);
          if (pm2 != plv2->type) { plv2->type = pm2; changed = 1; }
        }
        continue;
      }
      pt = has_cap ? TY_POLY_ARRAY : TY_STRING;
    }
    else if ((sp_streq(name, "each") || ty_iter_shape(name) == TY_ITER_MAP ||
              sp_streq(name, "select") || sp_streq(name, "reject") || sp_streq(name, "filter") ||
              sp_streq(name, "find") || sp_streq(name, "detect") || sp_streq(name, "each_with_index") ||
              sp_streq(name, "sort_by") || sp_streq(name, "find_all") || sp_streq(name, "count") ||
              sp_streq(name, "any?") || sp_streq(name, "all?") || sp_streq(name, "none?") ||
              sp_streq(name, "one?") || sp_streq(name, "sum") || sp_streq(name, "min_by") ||
              sp_streq(name, "max_by") || sp_streq(name, "bsearch") ||
              sp_streq(name, "flat_map") || sp_streq(name, "collect_concat")) &&
             (rt == TY_RANGE || rt == TY_FLOAT_RANGE)) {
      /* a distinct float range binds a FLOAT block element (its bsearch
         bisects the reals; the enumerating forms raise but still bind). */
      if (rt == TY_FLOAT_RANGE) { pt = TY_FLOAT; }
      else {
      /* a float-bounded int range binds a FLOAT element (bsearch bisects the reals) */
      int frn = recv;
      while (frn >= 0 && nt_type(nt, frn) && sp_streq(nt_type(nt, frn), "ParenthesesNode")) {
        int pb = nt_ref(nt, frn, "body"); int pbn = 0;
        const int *pbd = pb >= 0 ? nt_arr(nt, pb, "body", &pbn) : NULL;
        frn = pbn == 1 ? pbd[0] : -1;
      }
      int fl9 = frn >= 0 && nt_type(nt, frn) && sp_streq(nt_type(nt, frn), "RangeNode");
      int fb = fl9 ? nt_ref(nt, frn, "left") : -1, fe = fl9 ? nt_ref(nt, frn, "right") : -1;
      TyKind fbt = fb >= 0 ? infer_type(c, fb) : TY_NIL;
      TyKind fet = fe >= 0 ? infer_type(c, fe) : TY_NIL;
      if (sp_streq(name, "bsearch") && fl9 &&
          (fbt == TY_INT || fbt == TY_FLOAT) && (fet == TY_INT || fet == TY_FLOAT) &&
          (fbt == TY_FLOAT || fet == TY_FLOAT))
        pt = TY_FLOAT;
      /* a string-endpoint range ("a".."c") yields String elements (#3103) */
      else if (fl9 && fbt == TY_STRING && fet == TY_STRING)
        pt = TY_STRING;
      else
        pt = TY_INT;
      }
    }
    /* (range).lazy.select/reject/filter { |x| } : x is an integer range element */
    else if ((sp_streq(name, "select") || sp_streq(name, "reject") || sp_streq(name, "filter")) &&
             rt == TY_UNKNOWN && recv >= 0 &&
             nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
             nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "lazy")) {
      int lsrc = nt_ref(nt, recv, "receiver");
      if (lsrc >= 0 && infer_type(c, lsrc) == TY_RANGE) pt = TY_INT;
      /* an empty `[]` literal source has no element type; the pipeline still
         binds (and assigns) the parameter, so it needs a boxed slot (#3128) */
      else if (lsrc >= 0 && nt_type(nt, lsrc) &&
               sp_streq(nt_type(nt, lsrc), "ArrayNode")) pt = TY_POLY;
    }
    else if ((sp_streq(name, "each") || ty_iter_shape(name) == TY_ITER_MAP ||
              sp_streq(name, "select") || sp_streq(name, "reject") || sp_streq(name, "filter") ||
              sp_streq(name, "find") || sp_streq(name, "detect") ||
              sp_streq(name, "max_by") || sp_streq(name, "min_by") || sp_streq(name, "sort_by") ||
              sp_streq(name, "sort_by!") ||
              sp_streq(name, "take_while") || sp_streq(name, "drop_while") ||
              sp_streq(name, "reverse_each") || sp_streq(name, "each_entry") ||
              sp_streq(name, "sum") || sp_streq(name, "count") ||
              sp_streq(name, "any?") || sp_streq(name, "all?") || sp_streq(name, "none?") ||
              sp_streq(name, "one?") || sp_streq(name, "each_with_index") ||
              sp_streq(name, "find_all") ||
              sp_streq(name, "bsearch") || sp_streq(name, "find_index") ||
              sp_streq(name, "index") || sp_streq(name, "rindex") ||
              sp_streq(name, "map!") || sp_streq(name, "collect!") ||
              sp_streq(name, "select!") || sp_streq(name, "filter!") || sp_streq(name, "reject!") ||
              sp_streq(name, "uniq") || sp_streq(name, "uniq!") ||
              sp_streq(name, "keep_if") || sp_streq(name, "delete_if") ||
              sp_streq(name, "flat_map") || sp_streq(name, "collect_concat") ||
              sp_streq(name, "each_with_object") ||
              sp_streq(name, "chunk") || sp_streq(name, "group_by") ||
              sp_streq(name, "slice_before") || sp_streq(name, "slice_after") ||
              sp_streq(name, "tally_by") || sp_streq(name, "min_by_all") ||
              sp_streq(name, "filter_map") || sp_streq(name, "count_by") ||
              sp_streq(name, "partition") || sp_streq(name, "each_slice") ||
              sp_streq(name, "minmax_by") || sp_streq(name, "bsearch_index") ||
              sp_streq(name, "each_cons") || sp_streq(name, "cycle") ||
              sp_streq(name, "grep") || sp_streq(name, "grep_v") ||
              sp_streq(name, "to_h")) &&
             ty_is_array(rt))
      pt = ty_array_elem(rt);
    /* each_index { |i| } / fill { |i| } bind the index, not the element: always
       int (fill's block form takes the index and returns the value to store). */
    else if ((sp_streq(name, "each_index") || sp_streq(name, "fill") ||
              sp_streq(name, "fetch_values")) && ty_is_array(rt))  /* fetch_values yields the missing INDEX */
      pt = TY_INT;
    /* TY_POLY receiver with iteration methods: element type is TY_POLY */
    else if (rt == TY_POLY &&
             (sp_streq(name, "each") || ty_iter_shape(name) == TY_ITER_MAP ||
              sp_streq(name, "select") || sp_streq(name, "reject") || sp_streq(name, "find") ||
              sp_streq(name, "detect") || sp_streq(name, "any?") || sp_streq(name, "all?") ||
              sp_streq(name, "uniq") || sp_streq(name, "uniq!") || sp_streq(name, "sort_by") ||
              sp_streq(name, "min_by") || sp_streq(name, "max_by")))
      pt = TY_POLY;

    /* array.each_cons(n) / each_slice(n) { |a, b, ...| } -- a single param
       binds the n-element sub-array; multiple params destructure elements.
       Also handles |(a, b)| destructuring: leaves bind to element type. */
    if ((sp_streq(name, "each_cons") || sp_streq(name, "each_slice")) && ty_is_array(rt)) {
      Scope *es = comp_scope_of(c, block);
      int np = 0; while (block_param_name(c, block, np)) np++;
      if (np == 0 && block_param_is_multi(c, block, 0)) {
        TyKind elem = ty_array_elem(rt);
        int lc = block_param_multi_count(c, block, 0);
        for (int li = 0; li < lc; li++) {
          const char *ln = block_param_multi_leaf(c, block, 0, li);
          if (!ln) continue;
          LocalVar *lp = scope_local_intern(es, ln); lp->is_block_param = 1;
          TyKind m = ty_unify(lp->type, elem);
          if (m != lp->type) { lp->type = m; changed = 1; }
        }
      }
      else {
        for (int pj = 0; pj < np; pj++) {
          const char *pn = block_param_name(c, block, pj);
          LocalVar *lp = scope_local_intern(es, pn); lp->is_block_param = 1;
          TyKind want = (np == 1) ? rt : ty_array_elem(rt);
          TyKind m = ty_unify(lp->type, want);
          if (m != lp->type) { lp->type = m; changed = 1; }
        }
      }
      continue;
    }

    /* array.each_slice(n).map/collect { |x, y, ...| } chain: each block param
       gets the element type of the original array (slice elements).
       array.each_cons(n).map { |pair| } chain: block param gets the array type.
       Also handles |(a, b)| destructuring as the first param. */
    if ((ty_iter_shape(name) == TY_ITER_MAP) && (rt == TY_UNKNOWN || rt == TY_ENUMERATOR) &&
        nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
        nt_str(nt, recv, "name") && (sp_streq(nt_str(nt, recv, "name"), "each_slice") ||
                                     sp_streq(nt_str(nt, recv, "name"), "each_cons")) &&
        nt_ref(nt, recv, "block") < 0) {
      int es_recv2 = nt_ref(nt, recv, "receiver");
      TyKind arr_t2 = es_recv2 >= 0 ? infer_type(c, es_recv2) : TY_UNKNOWN;
      /* a Range under the chain types its params as the materialized int array */
      if (arr_t2 == TY_RANGE && range_enum_redispatch(c, recv)) arr_t2 = TY_INT_ARRAY;
      if (ty_is_array(arr_t2)) {
        Scope *es2 = comp_scope_of(c, block);
        int np2 = 0; while (block_param_name(c, block, np2)) np2++;
        /* each_cons and each_slice bind the n-window / slice (an array) for a
           single param `|w|`, or destructure it into elements for several
           params `|a, b|` (matching the codegen, which binds element pj when
           np > 1). A single destructured param `|(a, b)|` splits it likewise. */
        TyKind bp_t2 = (np2 == 1 ? arr_t2 : ty_array_elem(arr_t2));
        if (bp_t2 != TY_UNKNOWN) {
          if (np2 == 0 && block_param_is_multi(c, block, 0)) {
            /* |(a, b)| destructuring: each leaf gets element type */
            TyKind elem2 = ty_array_elem(arr_t2);
            if (elem2 != TY_UNKNOWN) {
              int lc2 = block_param_multi_count(c, block, 0);
              for (int li = 0; li < lc2; li++) {
                const char *ln = block_param_multi_leaf(c, block, 0, li);
                if (!ln) continue;
                LocalVar *lp = scope_local_intern(es2, ln); lp->is_block_param = 1;
                TyKind m2 = ty_unify(lp->type, elem2);
                if (m2 != lp->type) { lp->type = m2; changed = 1; }
              }
            }
          }
          else {
            for (int pj2 = 0; pj2 < np2; pj2++) {
              const char *pn2 = block_param_name(c, block, pj2);
              if (!pn2) break;
              LocalVar *lp2 = scope_local_intern(es2, pn2); lp2->is_block_param = 1;
              TyKind m2 = ty_unify(lp2->type, bp_t2);
              if (m2 != lp2->type) { lp2->type = m2; changed = 1; }
            }
          }
          continue;
        }
      }
    }

    /* array.each_cons(n).with_index(off).map { |pair, i| } or { |(a,b), i| }
       chain. A blockless enum.with_index now infers TY_ENUMERATOR (it used to
       be TY_UNKNOWN), so accept both -- this arm must keep pinning the params'
       concrete types ahead of the generic enumerator surface. */
    if ((ty_iter_shape(name) == TY_ITER_MAP) &&
        (rt == TY_UNKNOWN || rt == TY_ENUMERATOR) &&
        nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "with_index") &&
        nt_ref(nt, recv, "block") < 0) {
      int wi_recv = nt_ref(nt, recv, "receiver");
      if (wi_recv >= 0 && nt_type(nt, wi_recv) && sp_streq(nt_type(nt, wi_recv), "CallNode") &&
          nt_str(nt, wi_recv, "name") && sp_streq(nt_str(nt, wi_recv, "name"), "each_cons") &&
          nt_ref(nt, wi_recv, "block") < 0) {
        int ec_recv = nt_ref(nt, wi_recv, "receiver");
        TyKind ec_arr_t = ec_recv >= 0 ? infer_type(c, ec_recv) : TY_UNKNOWN;
        if (ty_is_array(ec_arr_t)) {
          Scope *wi_es = comp_scope_of(c, block);
          TyKind elem_t = ty_array_elem(ec_arr_t);
          /* p0 is the pair (array) or |(a,b)| multi-target; p1 is the int index */
          const char *idx_p = block_param_name(c, block, 1);
          if (idx_p) {
            LocalVar *ip = scope_local_intern(wi_es, idx_p); ip->is_block_param = 1;
            TyKind im = ty_unify(ip->type, TY_INT);
            if (im != ip->type) { ip->type = im; changed = 1; }
          }
          if (block_param_is_multi(c, block, 0)) {
            /* |(a, b), i|: destructure first multi-target param */
            int lc3 = block_param_multi_count(c, block, 0);
            for (int li = 0; li < lc3; li++) {
              const char *ln = block_param_multi_leaf(c, block, 0, li);
              if (!ln) continue;
              LocalVar *lp = scope_local_intern(wi_es, ln); lp->is_block_param = 1;
              TyKind m3 = ty_unify(lp->type, elem_t);
              if (m3 != lp->type) { lp->type = m3; changed = 1; }
            }
          }
          else {
            /* |pair, i|: pair gets the sub-array type */
            const char *pair_p = block_param_name(c, block, 0);
            if (pair_p) {
              LocalVar *pp = scope_local_intern(wi_es, pair_p); pp->is_block_param = 1;
              TyKind m3 = ty_unify(pp->type, ec_arr_t);
              if (m3 != pp->type) { pp->type = m3; changed = 1; }
            }
          }
          continue;
        }
      }
    }

    /* arr.each.with_index(off).inject(init) { |acc, (v,i)| } / { |acc, pair| }
       and arr.each_with_index.inject{...}: type the fold's params over the
       [elem, index] pair enumerator. (matz/spinel#1481) */
    if ((sp_streq(name, "inject") || sp_streq(name, "reduce")) &&
        nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
        nt_ref(nt, recv, "block") < 0) {
      const char *rn = nt_str(nt, recv, "name");
      int chain_arr = -1;
      if (rn && sp_streq(rn, "each_with_index")) chain_arr = nt_ref(nt, recv, "receiver");
      else if (rn && sp_streq(rn, "with_index")) {
        int wir = nt_ref(nt, recv, "receiver");
        if (wir >= 0 && nt_type(nt, wir) && sp_streq(nt_type(nt, wir), "CallNode") &&
            nt_str(nt, wir, "name") && sp_streq(nt_str(nt, wir, "name"), "each") &&
            nt_ref(nt, wir, "block") < 0)
          chain_arr = nt_ref(nt, wir, "receiver");
      }
      TyKind chain_at = chain_arr >= 0 ? infer_type(c, chain_arr) : TY_UNKNOWN;
      if (ty_is_array(chain_at) && block >= 0) {
        TyKind elem = ty_array_elem(chain_at);
        Scope *bs = comp_scope_of(c, block);
        int rargs = nt_ref(nt, id, "arguments"); int rargc = 0;
        const int *rargv = rargs >= 0 ? nt_arr(nt, rargs, "arguments", &rargc) : NULL;
        TyKind acc_t = (rargc > 0 && rargv) ? infer_type(c, rargv[0]) : elem;
        if (acc_t == TY_UNKNOWN) acc_t = elem;
        if (p0) {
          LocalVar *ap = scope_local_intern(bs, p0); ap->is_block_param = 1;
          TyKind m = ty_unify(ap->type, acc_t); if (m != ap->type) { ap->type = m; changed = 1; }
        }
        if (block_param_is_multi(c, block, 1)) {
          int lc = block_param_multi_count(c, block, 1);
          for (int li = 0; li < lc; li++) {
            const char *ln = block_param_multi_leaf(c, block, 1, li);
            if (!ln) continue;
            LocalVar *lp = scope_local_intern(bs, ln); lp->is_block_param = 1;
            TyKind want = (li == 0) ? elem : TY_INT;
            TyKind m = ty_unify(lp->type, want); if (m != lp->type) { lp->type = m; changed = 1; }
          }
        }
        else {
          const char *pp = block_param_name(c, block, 1);
          if (pp) {
            LocalVar *lp = scope_local_intern(bs, pp); lp->is_block_param = 1;
            TyKind pairt = (elem == TY_INT) ? TY_INT_ARRAY : TY_POLY_ARRAY;
            TyKind m = ty_unify(lp->type, pairt); if (m != lp->type) { lp->type = m; changed = 1; }
          }
        }
        continue;
      }
    }

    /* arr.each.with_index(off).<terminal> { |v, i| } / { |(v,i)| } / { |pair| }
       (map/collect/select/filter/reject/count/any?/all?/none?/each over the
       [elem, index] pair enumerator). (matz/spinel#1483) */
    if (block >= 0 &&
        (ty_iter_shape(name) == TY_ITER_MAP || ty_iter_shape(name) == TY_ITER_SELECT ||
         ty_iter_shape(name) == TY_ITER_REJECT || sp_streq(name, "each") ||
         sp_streq(name, "filter_map") ||
         sp_streq(name, "count") || sp_streq(name, "any?") || sp_streq(name, "all?") ||
         sp_streq(name, "none?")) &&
        nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
        nt_ref(nt, recv, "block") < 0) {
      const char *rn = nt_str(nt, recv, "name");
      int chain_arr = -1;
      if (rn && sp_streq(rn, "each_with_index")) chain_arr = nt_ref(nt, recv, "receiver");
      else if (rn && sp_streq(rn, "with_index")) {
        int wir = nt_ref(nt, recv, "receiver");
        if (wir >= 0 && nt_type(nt, wir) && sp_streq(nt_type(nt, wir), "CallNode") &&
            nt_str(nt, wir, "name") && sp_streq(nt_str(nt, wir, "name"), "each") &&
            nt_ref(nt, wir, "block") < 0)
          chain_arr = nt_ref(nt, wir, "receiver");
      }
      TyKind chain_at = chain_arr >= 0 ? infer_type(c, chain_arr) : TY_UNKNOWN;
      /* Only the |v, i| two-param form (v = element, i = index); single-param
         and destructure forms have method-dependent semantics and are left to
         other rules (the codegen path bails on them too). */
      const char *vp = block_param_name(c, block, 0);
      const char *ip = block_param_name(c, block, 1);
      if (ty_is_array(chain_at) && !block_param_is_multi(c, block, 0) && vp && ip) {
        TyKind elem = ty_array_elem(chain_at);
        Scope *bs = comp_scope_of(c, block);
        LocalVar *lp = scope_local_intern(bs, vp); lp->is_block_param = 1;
        TyKind m = ty_unify(lp->type, elem); if (m != lp->type) { lp->type = m; changed = 1; }
        LocalVar *lp2 = scope_local_intern(bs, ip); lp2->is_block_param = 1;
        TyKind m2 = ty_unify(lp2->type, TY_INT); if (m2 != lp2->type) { lp2->type = m2; changed = 1; }
        continue;
      }
    }

    /* array.{map,collect,each,select,filter,reject}.with_index(off) { |x, i| }:
       a blockless enumerator over an array, indexed -- element + int index. */
    if (sp_streq(name, "with_index") &&
        nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
        nt_ref(nt, recv, "block") < 0) {
      const char *inner = nt_str(nt, recv, "name");
      if (inner && (sp_streq(inner, "map") || sp_streq(inner, "collect") ||
                    sp_streq(inner, "each") || sp_streq(inner, "select") ||
                    sp_streq(inner, "filter") || sp_streq(inner, "reject") ||
                    sp_streq(inner, "map!") || sp_streq(inner, "collect!"))) {
        int arr_recv = nt_ref(nt, recv, "receiver");
        TyKind arr_t = arr_recv >= 0 ? infer_type(c, arr_recv) : TY_UNKNOWN;
        /* an Integer Range source behaves as an int array (the emitter
           materializes it) (#3228) */
        if (arr_t == TY_RANGE) arr_t = TY_INT_ARRAY;
        if (ty_is_array(arr_t)) {
          Scope *wis = comp_scope_of(c, block);
          if (p0) {
            LocalVar *ep = scope_local_intern(wis, p0); ep->is_block_param = 1;
            TyKind em = ty_unify(ep->type, ty_array_elem(arr_t));
            if (em != ep->type) { ep->type = em; changed = 1; }
          }
          const char *idx_p = block_param_name(c, block, 1);
          if (idx_p) {
            LocalVar *ip = scope_local_intern(wis, idx_p); ip->is_block_param = 1;
            TyKind im = ty_unify(ip->type, TY_INT);
            if (im != ip->type) { ip->type = im; changed = 1; }
          }
          continue;
        }
      }
    }

    /* array.combination(k)/permutation(k) { |c| } binds the k-element sub-array */
    if ((sp_streq(name, "combination") || sp_streq(name, "permutation")) && ty_is_array(rt)) {
      LocalVar *lp = scope_local_intern(comp_scope_of(c, block), p0); lp->is_block_param = 1;
      TyKind m = ty_unify(lp->type, rt);
      if (m != lp->type) { lp->type = m; changed = 1; }
      continue;
    }

    /* array.sort/min/max/minmax/slice_when { |a, b| cmp } -- a comparator block
       binds both parameters to the element type */
    if ((sp_streq(name, "sort") || sp_streq(name, "sort!") || sp_streq(name, "min") || sp_streq(name, "max") ||
         sp_streq(name, "minmax") || sp_streq(name, "slice_when") || sp_streq(name, "chunk_while")) && ty_is_array(rt)) {
      Scope *cs = comp_scope_of(c, block);
      for (int pj = 0; pj < 2; pj++) {
        const char *pn = block_param_name(c, block, pj);
        if (!pn) continue;
        LocalVar *lp = scope_local_intern(cs, pn); lp->is_block_param = 1;
        TyKind m = ty_unify(lp->type, ty_array_elem(rt));
        if (m != lp->type) { lp->type = m; changed = 1; }
      }
      continue;
    }

    /* array.reduce(init) { |acc, elem| } or inject: p0=acc type, p1=elem type */
    if ((sp_streq(name, "reduce") || sp_streq(name, "inject")) && ty_is_array(rt)) {
      if (!p0) continue;
      Scope *rs = comp_scope_of(c, block);
      TyKind et2 = ty_array_elem(rt);
      /* `[[ints],...].inject { |a, b| a & b }`: the inner arrays are int arrays,
         so type both fold params as int arrays rather than poly. */
      if (rt == TY_POLY_ARRAY && comp_is_nested_int_array_literal(c, nt_ref(nt, id, "receiver")))
        et2 = TY_INT_ARRAY;
      /* Determine accumulator type from initial value argument (if any) */
      int rargs = nt_ref(nt, id, "arguments");
      int rargc = 0;
      const int *rargv = rargs >= 0 ? nt_arr(nt, rargs, "arguments", &rargc) : NULL;
      TyKind acc_t = (rargc > 0 && rargv) ? infer_type(c, rargv[0]) : et2;
      if (acc_t == TY_UNKNOWN) acc_t = et2;
      /* the accumulator is reassigned to the block's value each step, so a
         boxed block result widens it rather than truncating -- whatever the
         element type (an int-array fold whose OPERAND is poly, e.g. a
         parameter called with Integer and Rational, still folds boxed;
         #2982, #3308) */
      if (acc_t != TY_POLY) {
        int rbody = nt_ref(nt, block, "body");
        int rbn = 0; const int *rbb = rbody >= 0 ? nt_arr(nt, rbody, "body", &rbn) : NULL;
        TyKind bt3 = rbn > 0 ? infer_type(c, rbb[rbn - 1]) : TY_UNKNOWN;
        if (bt3 == TY_POLY || ty_is_object(bt3) || bt3 == TY_RATIONAL ||
            bt3 == TY_COMPLEX || bt3 == TY_BIGINT) acc_t = TY_POLY;
      }
      LocalVar *ap = scope_local_intern(rs, p0); ap->is_block_param = 1;
      TyKind am = ty_unify(ap->type, acc_t);
      if (am != ap->type) { ap->type = am; changed = 1; }
      const char *rp1 = block_param_name(c, block, 1);
      if (rp1) {
        LocalVar *ep2 = scope_local_intern(rs, rp1); ep2->is_block_param = 1;
        TyKind em2 = ty_unify(ep2->type, et2);
        if (em2 != ep2->type) { ep2->type = em2; changed = 1; }
      }
      continue;
    }

    /* array.each_with_index { |x, i| } binds element + int index */
    if (sp_streq(name, "each_with_index") && ty_is_array(rt)) {
      Scope *es = comp_scope_of(c, block);
      if (!p0) continue;
      LocalVar *ep = scope_local_intern(es, p0); ep->is_block_param = 1;
      TyKind em = ty_unify(ep->type, ty_array_elem(rt));
      if (em != ep->type) { ep->type = em; changed = 1; }
      const char *p1 = block_param_name(c, block, 1);
      if (p1) {
        LocalVar *ip = scope_local_intern(es, p1); ip->is_block_param = 1;
        TyKind im = ty_unify(ip->type, TY_INT);
        if (im != ip->type) { ip->type = im; changed = 1; }
      }
      continue;
    }

    /* array.zip(other) { |a, b| } binds element of recv + element of other */
    /* str.match(/re/) { |m| }: the block receives the MatchData */
    if (sp_streq(name, "match") && p0) {
      int margs = nt_ref(nt, id, "arguments");
      int mac = 0; const int *mav = margs >= 0 ? nt_arr(nt, margs, "arguments", &mac) : NULL;
      const char *mrt = nt_type(nt, recv), *mat = mac > 0 ? nt_type(nt, mav[0]) : NULL;
      if ((mrt && sp_streq(mrt, "RegularExpressionNode")) ||
          (mat && sp_streq(mat, "RegularExpressionNode"))) {
        Scope *ms = comp_scope_of(c, block);
        LocalVar *mp = scope_local_intern(ms, p0); mp->is_block_param = 1;
        TyKind mm = ty_unify(mp->type, TY_MATCHDATA);
        if (mm != mp->type) { mp->type = mm; changed = 1; }
        continue;
      }
    }
    if (sp_streq(name, "zip") && ty_is_array(rt)) {
      Scope *zs = comp_scope_of(c, block);
      const char *zp1s = block_param_name(c, block, 1);
      LocalVar *ep0 = scope_local_intern(zs, p0); ep0->is_block_param = 1;
      /* a SOLO param receives the boxed TUPLE ([e1, e2]); two params
         auto-splat it */
      TyKind em0 = ty_unify(ep0->type, zp1s ? ty_array_elem(rt) : TY_POLY);
      if (em0 != ep0->type) { ep0->type = em0; changed = 1; }
      const char *zp1 = zp1s;
      if (zp1) {
        int zargs = nt_ref(nt, id, "arguments");
        int zargc = 0; const int *zargv = zargs >= 0 ? nt_arr(nt, zargs, "arguments", &zargc) : NULL;
        TyKind et2 = (zargc > 0 && zargv && ty_is_array(infer_type(c, zargv[0])))
                     ? ty_array_elem(infer_type(c, zargv[0])) : ty_array_elem(rt);
        LocalVar *ep1 = scope_local_intern(zs, zp1); ep1->is_block_param = 1;
        TyKind em1 = ty_unify(ep1->type, et2);
        if (em1 != ep1->type) { ep1->type = em1; changed = 1; }
      }
      continue;
    }

    /* array.each_with_object(init) { |x, acc| } binds element + accumulator */
    if (sp_streq(name, "each_with_object") && ty_is_array(rt)) {
      Scope *es = comp_scope_of(c, block);
      if (p0) {
        TyKind et = ty_array_elem(rt);
        LocalVar *ep = scope_local_intern(es, p0); ep->is_block_param = 1;
        if (!(ty_is_array(ep->type) && !ty_is_array(et))) {
          TyKind em = ty_unify(ep->type, et);
          if (em != ep->type) { ep->type = em; changed = 1; }
        }
      }
      const char *p1_name = block_param_name(c, block, 1);
      if (p1_name) {
        int ewobj_args = nt_ref(nt, id, "arguments");
        int ewobj_argc = 0;
        const int *ewobj_argv = ewobj_args >= 0 ? nt_arr(nt, ewobj_args, "arguments", &ewobj_argc) : NULL;
        if (ewobj_argc > 0 && ewobj_argv) {
          TyKind at = infer_type(c, ewobj_argv[0]);
          int from_usage = 0;
          if (at == TY_UNKNOWN) {
            const char *a0ty = nt_type(nt, ewobj_argv[0]);
            int an0 = 0;
            if (a0ty && sp_streq(a0ty, "ArrayNode")) nt_arr(nt, ewobj_argv[0], "elements", &an0);
            if (a0ty && sp_streq(a0ty, "ArrayNode") && an0 == 0) {
              /* empty `[]`: the accumulator element type comes from how the memo
                 is filled (`memo << e`), following a forwarded callable's body. */
              TyKind me = ewo_memo_elem_type(c, id);
              if (me != TY_UNKNOWN) { at = ty_array_of(me); from_usage = 1; }
              else at = TY_INT_ARRAY;
            }
          }
          if (at != TY_UNKNOWN) {
            LocalVar *ap = scope_local_intern(es, p1_name); ap->is_block_param = 1;
            /* A usage-derived type overrides the bare int_array guess (the
               no-evidence default) so an early guess can't widen a later
               str/poly memo to poly_array; otherwise unify. */
            TyKind am = (from_usage && (ap->type == TY_UNKNOWN || ap->type == TY_INT_ARRAY))
                        ? at : ty_unify(ap->type, at);
            if (am != ap->type) { ap->type = am; changed = 1; }
          }
        }
      }
      continue;
    }

    /* hash.merge/merge!/update(other) { |k, v1, v2| } binds key + both values */
    if ((sp_streq(name, "merge") || sp_streq(name, "merge!") || sp_streq(name, "update")) &&
        ty_is_hash(rt)) {
      Scope *ms = comp_scope_of(c, block);
      LocalVar *kp = scope_local_intern(ms, p0); kp->is_block_param = 1;
      TyKind km = ty_unify(kp->type, ty_hash_key(rt));
      if (km != kp->type) { kp->type = km; changed = 1; }
      const char *mp1 = block_param_name(c, block, 1);
      const char *mp2 = block_param_name(c, block, 2);
      const char *mps[2]; mps[0] = mp1; mps[1] = mp2;
      for (int mi2 = 0; mi2 < 2; mi2++) {
        if (!mps[mi2]) continue;
        LocalVar *vp = scope_local_intern(ms, mps[mi2]); vp->is_block_param = 1;
        TyKind vm = ty_unify(vp->type, ty_hash_val(rt));
        if (vm != vp->type) { vp->type = vm; changed = 1; }
      }
      continue;
    }

    /* array.product(other) { |pair| } binds the boxed pair array */
    if (sp_streq(name, "product") && ty_is_array(rt) && p0) {
      Scope *aps = comp_scope_of(c, block);
      LocalVar *pp = scope_local_intern(aps, p0); pp->is_block_param = 1;
      TyKind pm = ty_unify(pp->type, TY_POLY);
      if (pm != pp->type) { pp->type = pm; changed = 1; }
      continue;
    }
    /* array.fetch(i) { |i| } binds the (int) index */
    if (sp_streq(name, "fetch") && ty_is_array(rt) && p0) {
      Scope *afs = comp_scope_of(c, block);
      LocalVar *ap = scope_local_intern(afs, p0); ap->is_block_param = 1;
      TyKind am = ty_unify(ap->type, TY_INT);
      if (am != ap->type) { ap->type = am; changed = 1; }
      continue;
    }
    /* hash.fetch(key) { |k| } binds the looked-up key */
    if (sp_streq(name, "fetch") && ty_is_hash(rt)) {
      Scope *fs = comp_scope_of(c, block);
      LocalVar *kp = scope_local_intern(fs, p0); kp->is_block_param = 1;
      TyKind km = ty_unify(kp->type, ty_hash_key(rt));
      if (km != kp->type) { kp->type = km; changed = 1; }
      continue;
    }

    /* hash.transform_keys { |k| } binds key; transform_values { |v| } value */
    if ((sp_streq(name, "transform_keys") || sp_streq(name, "transform_values")) && ty_is_hash(rt)) {
      Scope *hs = comp_scope_of(c, block);
      LocalVar *vp = scope_local_intern(hs, p0); vp->is_block_param = 1;
      TyKind want = sp_streq(name, "transform_keys") ? ty_hash_key(rt) : ty_hash_val(rt);
      TyKind vm = ty_unify(vp->type, want);
      if (vm != vp->type) { vp->type = vm; changed = 1; }
      continue;
    }

    /* hash.each_value { |v| } binds value; each_key { |k| } binds key */
    if ((sp_streq(name, "each_value") || sp_streq(name, "each_key")) && ty_is_hash(rt)) {
      Scope *hs = comp_scope_of(c, block);
      LocalVar *vp = scope_local_intern(hs, p0); vp->is_block_param = 1;
      TyKind want = sp_streq(name, "each_value") ? ty_hash_val(rt) : ty_hash_key(rt);
      TyKind vm = ty_unify(vp->type, want);
      if (vm != vp->type) { vp->type = vm; changed = 1; }
      continue;
    }

    /* hash.each / each_pair { |k, v| } or { |(k,v)| } binds two params.
       Also handles each_with_object { |(k,v), memo| } and mutating
       iteration (delete_if / select! / reject! / keep_if). */
    if ((sp_streq(name, "each") || sp_streq(name, "each_pair") || sp_streq(name, "map") ||
         sp_streq(name, "collect") || sp_streq(name, "flat_map") ||
         sp_streq(name, "collect_concat") || sp_streq(name, "select") ||
         sp_streq(name, "filter") || sp_streq(name, "reject") || sp_streq(name, "find") ||
         sp_streq(name, "detect") || sp_streq(name, "sort_by") || sp_streq(name, "min_by") ||
         sp_streq(name, "max_by") || sp_streq(name, "count") || sp_streq(name, "sum") ||
         sp_streq(name, "filter_map") || sp_streq(name, "partition") || sp_streq(name, "group_by") ||
         sp_streq(name, "collect_concat") || sp_streq(name, "chunk") ||
         sp_streq(name, "any?") || sp_streq(name, "all?") || sp_streq(name, "none?") ||
         sp_streq(name, "delete_if") || sp_streq(name, "select!") || sp_streq(name, "reject!") ||
         sp_streq(name, "filter!") || sp_streq(name, "keep_if") ||
         sp_streq(name, "each_with_index") || sp_streq(name, "each_with_object")) && ty_is_hash(rt)) {
      Scope *hs = comp_scope_of(c, block);
      /* |(k,v)| or |(k,v), memo| destructuring (MultiTargetNode first param) */
      if (block_param_is_multi(c, block, 0)) {
        int lc = block_param_multi_count(c, block, 0);
        if (lc >= 1) {
          const char *kn = block_param_multi_leaf(c, block, 0, 0);
          if (kn) {
            LocalVar *kp2 = scope_local_intern(hs, kn); kp2->is_block_param = 1;
            TyKind km2 = ty_unify(kp2->type, ty_hash_key(rt));
            if (km2 != kp2->type) { kp2->type = km2; changed = 1; }
          }
        }
        if (lc >= 2) {
          const char *vn = block_param_multi_leaf(c, block, 0, 1);
          if (vn) {
            LocalVar *vp2 = scope_local_intern(hs, vn); vp2->is_block_param = 1;
            TyKind vm2 = ty_unify(vp2->type, ty_hash_val(rt));
            if (vm2 != vp2->type) { vp2->type = vm2; changed = 1; }
          }
        }
        /* for each_with_object: bind the memo param (position 1) */
        if (sp_streq(name, "each_with_object")) {
          const char *mp = block_param_name(c, block, 1);
          if (mp) {
            int ewobj_args = nt_ref(nt, id, "arguments");
            int ewobj_argc = 0;
            const int *ewobj_argv = ewobj_args >= 0 ? nt_arr(nt, ewobj_args, "arguments", &ewobj_argc) : NULL;
            if (ewobj_argc > 0 && ewobj_argv) {
              TyKind at2 = infer_type(c, ewobj_argv[0]);
              if (at2 != TY_UNKNOWN) {
                LocalVar *mp_lv = scope_local_intern(hs, mp); mp_lv->is_block_param = 1;
                TyKind mm = ty_unify(mp_lv->type, at2);
                if (mm != mp_lv->type) { mp_lv->type = mm; changed = 1; }
              }
            }
          }
        }
      }
      else if (sp_streq(name, "each_with_object") &&
               block_param_name(c, block, 1) && !block_param_name(c, block, 2)) {
        /* hash.each_with_object(seed) { |element, memo| }: CRuby yields the
           [k,v] pair as the element (the desugared |__destr, memo| shape). */
        if (p0) {
          LocalVar *ep = scope_local_intern(hs, p0); ep->is_block_param = 1;
          TyKind em = ty_unify(ep->type, TY_POLY_ARRAY);
          if (em != ep->type) { ep->type = em; changed = 1; }
        }
        const char *mp = block_param_name(c, block, 1);
        int ewo_args = nt_ref(nt, id, "arguments"); int ewo_argc = 0;
        const int *ewo_argv = ewo_args >= 0 ? nt_arr(nt, ewo_args, "arguments", &ewo_argc) : NULL;
        TyKind accT = (ewo_argc > 0 && ewo_argv) ? infer_type(c, ewo_argv[0]) : TY_UNKNOWN;
        if (mp && accT != TY_UNKNOWN) {
          LocalVar *mlv = scope_local_intern(hs, mp); mlv->is_block_param = 1;
          TyKind mm = ty_unify(mlv->type, accT);
          if (mm != mlv->type) { mlv->type = mm; changed = 1; }
        }
      }
      else {
        /* an Enumerable-flavored method's SOLO param receives the boxed
           [k, v] pair (the emitter's pair mode), not the key */
        const char *p1 = block_param_name(c, block, 1);
        int pair_solo = !p1 &&
                        (sp_streq(name, "flat_map") || sp_streq(name, "collect_concat") ||
                         sp_streq(name, "filter_map") || sp_streq(name, "partition") ||
                         sp_streq(name, "each") || sp_streq(name, "each_pair") ||
                         sp_streq(name, "map") || sp_streq(name, "collect") ||
                         sp_streq(name, "find") ||
                         sp_streq(name, "detect") || sp_streq(name, "sort_by") ||
                         sp_streq(name, "group_by") || sp_streq(name, "sum") ||
                         /* Enumerable predicates/counters: a solo param is the
                            [k, v] pair, not the key (#2339) */
                         sp_streq(name, "any?") || sp_streq(name, "all?") ||
                         sp_streq(name, "none?") || sp_streq(name, "one?") ||
                         sp_streq(name, "count"));
        if (p0) {
          LocalVar *kp = scope_local_intern(hs, p0); kp->is_block_param = 1;
          TyKind km = ty_unify(kp->type, pair_solo ? TY_POLY : ty_hash_key(rt));
          if (km != kp->type) { kp->type = km; changed = 1; }
        }
        if (p1) {
          LocalVar *vp = scope_local_intern(hs, p1); vp->is_block_param = 1;
          TyKind vm = ty_unify(vp->type, ty_hash_val(rt));
          if (vm != vp->type) { vp->type = vm; changed = 1; }
        }
      }
      continue;
    }

    /* array.each/map with 2+ params: auto-destructure sub-array elements.
       Handles `[[1,2],[3,4]].each { |a,b| }` and numbered `{ _1; _2 }`. */
    if (pt != TY_UNKNOWN && ty_is_array(rt)) {
      int np = 0;
      while (block_param_name(c, block, np)) np++;
      if (np >= 2) {
        TyKind inner_elem = TY_UNKNOWN;
        if (ty_is_array(pt)) {
          inner_elem = ty_array_elem(pt);
        }
        else if (pt == TY_POLY && recv >= 0) {
          const char *rty2 = nt_type(nt, recv);
          if (rty2 && sp_streq(rty2, "ArrayNode")) {
            int re_n2 = 0;
            const int *re_els2 = nt_arr(nt, recv, "elements", &re_n2);
            TyKind common_at = TY_UNKNOWN;
            for (int ri = 0; ri < re_n2; ri++)
              common_at = ty_unify(common_at, infer_type(c, re_els2[ri]));
            if (ty_is_array(common_at)) inner_elem = ty_array_elem(common_at);
            else inner_elem = TY_POLY;
          }
          else if (rty2 && sp_streq(rty2, "ConstantReadNode") &&
                   nt_str(nt, recv, "name") &&
                   const_array_elems_all_int_array(c, nt_str(nt, recv, "name"))) {
            /* a poly-array CONSTANT of int-array rows (DIRECTIONS = [[dx,dy],
               ...].freeze) destructures to int params -- otherwise one such
               call site poisons every downstream method's params to poly */
            inner_elem = TY_INT;
          }
          else { inner_elem = TY_POLY; }
        }
        if (inner_elem != TY_UNKNOWN) {
          Scope *ds = comp_scope_of(c, block);
          for (int pj = 0; pj < np; pj++) {
            const char *pname2 = block_param_name(c, block, pj);
            if (!pname2) continue;
            LocalVar *lp2 = scope_local_intern(ds, pname2); lp2->is_block_param = 1;
            TyKind m2 = ty_unify(lp2->type, inner_elem);
            if (m2 != lp2->type) { lp2->type = m2; changed = 1; }
          }
          continue;
        }
      }
    }

    if (pt == TY_UNKNOWN) continue;
    Scope *s = comp_scope_of(c, block);
    /* When iterating a poly receiver (TY_POLY) with 2+ block params, all params
       are poly (auto-splat from the poly element). Assign TY_POLY to all. */
    if (pt == TY_POLY) {
      int npp2 = 0; while (block_param_name(c, block, npp2)) npp2++;
      if (npp2 >= 2) {
        for (int pj2 = 0; pj2 < npp2; pj2++) {
          const char *pnj2 = block_param_name(c, block, pj2);
          if (!pnj2) continue;
          LocalVar *lp2 = scope_local_intern(s, pnj2); lp2->is_block_param = 1;
          /* Don't widen a param already typed as a concrete array (e.g. a
             desugared destructure temp bound to an each_cons/each_slice window)
             down to a poly scalar; that mismatches the array the codegen binds. */
          if (ty_is_array(lp2->type)) continue;
          TyKind m2 = ty_unify(lp2->type, TY_POLY);
          if (m2 != lp2->type) { lp2->type = m2; changed = 1; }
        }
        continue;
      }
    }
    if (!p0) continue;
    LocalVar *lv = scope_local_intern(s, p0); lv->is_block_param = 1;
    /* Don't widen an array-typed variable to a scalar via block-param
       inference.  When the variable already holds an array (set by a write
       site in the same iteration, before infer_block_params runs), widening
       it to the element scalar type collapses the outer array type to TY_POLY.
       Codegen emits a scoped shadow for the block param instead. */
    if (ty_is_array(lv->type) && !ty_is_array(pt))
      continue;
    TyKind merged = ty_unify(lv->type, pt);
    if (merged != lv->type) { lv->type = merged; changed = 1; }
  }
  return changed;
}

/* Value type of an explicit `return expr` (or nil for bare return). */
TyKind return_node_type(Compiler *c, int id) {
  int args = nt_ref(c->nt, id, "arguments");
  if (args < 0) return TY_NIL;
  int n = 0;
  const int *a = nt_arr(c->nt, args, "arguments", &n);
  if (n > 1) return TY_POLY_ARRAY;
  if (n == 0) return TY_NIL;
  /* `return *x` builds an array (`[*x]`): a scalar wraps in a one-element
     array, an array stays itself. Codegen emits sp_splat_to_array, so the
     method's return type is a poly array -- not the splat's element type that
     infer_type would report for a bare SplatNode in array-literal context. */
  const char *aty = nt_type(c->nt, a[0]);
  if (aty && sp_streq(aty, "SplatNode")) return TY_POLY_ARRAY;
  return infer_type(c, a[0]);
}

/* Defined in codegen_fold.c (linked in). */
int is_descendant(Compiler *c, int k, int anc);

/* True when the method body's tail statement unconditionally raises, so the C
   function never reaches a return (used to widen its void type to the override
   return type -- the unreachable "return value" can safely take that type). */
static int scope_tail_raises(Compiler *c, int s) {
  const NodeTable *nt = c->nt;
  int body = c->scopes[s].body;
  if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) return 0;
  int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
  if (bn <= 0) return 0;
  int last = bb[bn - 1];
  const char *ty = nt_type(nt, last);
  return ty && sp_streq(ty, "CallNode") && nt_ref(nt, last, "receiver") < 0 &&
         nt_str(nt, last, "name") && sp_streq(nt_str(nt, last, "name"), "raise");
}

/* name -> named class-method scopes, cached per scope count. The abstract-base
   widening below otherwise rescans every scope per void-returning raising base
   (O(bases * scopes)). Built once per fixpoint run (scope shape is fixed). */
static int rn_nscopes = -1, rn_buckets = 0;
static int *rn_next = NULL, *rn_head = NULL;
static void rn_build(Compiler *c) {
  int ns = c->nscopes;
  free(rn_next); free(rn_head);
  rn_buckets = ns > 0 ? ns : 1;
  rn_next = malloc((size_t)(ns > 0 ? ns : 1) * sizeof(int));
  rn_head = malloc((size_t)rn_buckets * sizeof(int));
  rn_nscopes = ns;
  if (!rn_next || !rn_head) { rn_buckets = 0; return; }
  for (int i = 0; i < rn_buckets; i++) rn_head[i] = -1;
  for (int s = 0; s < ns; s++) {
    if (c->scopes[s].class_id < 0 || !c->scopes[s].name) continue;
    unsigned b = wrn_hash(c->scopes[s].name) % (unsigned)rn_buckets;
    rn_next[s] = rn_head[b]; rn_head[b] = s;
  }
}

/* A `{}` / `Hash.new` / `Hash.new(default)` construct whose element types are
   not witnessed here: infer_type reports it as TY_UNKNOWN (analyze_infer.c),
   deferring the hash variant to key/value usage. `Hash.new { }` is excluded
   (it infers a concrete TY_STR_POLY_HASH). A `::Hash` / namespaced receiver is a
   ConstantPathNode, matching the receiver forms codegen's tail handling accepts. */
static int node_is_empty_hash_construct(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return 0;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_HashNode || k == NK_KeywordHashNode) {
    int n = 0; nt_arr(nt, node, "elements", &n);
    return n == 0;
  }
  if (k == NK_CallNode) {
    const char *nm = nt_str(nt, node, "name");
    if (!nm || !sp_streq(nm, "new")) return 0;
    if (nt_ref(nt, node, "block") >= 0) return 0;   /* Hash.new { } is STR_POLY_HASH */
    int recv = nt_ref(nt, node, "receiver");
    if (recv < 0) return 0;
    NodeKind rk = nt_kind(nt, recv);
    if (rk != NK_ConstantReadNode && rk != NK_ConstantPathNode) return 0;
    const char *cn = nt_str(nt, recv, "name");
    return cn && sp_streq(cn, "Hash");
  }
  return 0;
}

/* If method scope `mi`'s value is an element-less hash -- the body's tail
   expression (or a trailing `return {}`) is an empty `{}` / `Hash.new` -- return
   that tail node, else -1. Such a return infers TY_UNKNOWN with no in-body
   witness, which collapses the C signature to `void` (#1680). */
static int scope_tail_empty_hash(Compiler *c, int mi) {
  const NodeTable *nt = c->nt;
  int b = c->scopes[mi].body;
  if (b < 0) return -1;
  if (nt_kind(nt, b) == NK_StatementsNode) {
    int n = 0; const int *bb = nt_arr(nt, b, "body", &n);
    if (n == 0) return -1;
    b = bb[n - 1];
  }
  if (nt_kind(nt, b) == NK_ReturnNode) {
    int a = nt_ref(nt, b, "arguments"); int an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (an == 1) b = av[0];
  }
  return node_is_empty_hash_construct(c, b) ? b : -1;
}

/* Resolve a `local = call(...)` value node to its callee method scope for the
   subset of shapes empty-hash returns arrive through: a bare self-send, a
   `Const.cmethod`, and an object-receiver instance call. -1 if unresolved. */
int backprop_call_target(Compiler *c, int call_id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, call_id, "name");
  if (!name || sp_streq(name, "new")) return -1;  /* constructors bind elsewhere */
  int recv = nt_ref(nt, call_id, "receiver");
  if (recv < 0) {
    int mi = comp_method_index(c, name);
    if (mi < 0) {
      Scope *self = comp_scope_of(c, call_id);
      if (self && self->class_id >= 0) {
        mi = comp_method_in_chain(c, self->class_id, name, NULL);
        if (mi < 0 && self->is_cmethod)
          mi = comp_cmethod_in_chain(c, self->class_id, name, NULL);
      }
    }
    if (mi < 0) mi = comp_included_method_index(c, name);
    return mi;
  }
  NodeKind rk = nt_kind(nt, recv);
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) {
    const char *cn = nt_str(nt, recv, "name");   /* NULL for a non-flat path */
    int ci = cn ? comp_class_index(c, cn) : -1;
    return ci >= 0 ? comp_cmethod_in_chain(c, ci, name, NULL) : -1;
  }
  TyKind rt = infer_type(c, recv);
  if (ty_is_object(rt))
    return comp_method_in_chain(c, ty_object_class(rt), name, NULL);
  return -1;
}

/* Back-propagate a caller's concrete hash type onto an empty-hash-returning
   method whose return would otherwise collapse to `void` (#1680). A method that
   ends in `{}` / `Hash.new` witnesses no element types, so its return infers
   TY_UNKNOWN and codegen emits a void C function; the caller's `x = mk()` then
   fails to compile. But the caller local `x` is independently pinned to a
   concrete hash by its own use (`x["k"] = "v"` -> StrStrHash); adopt that as the
   method's return so the signature is a real hash pointer. Mirrors the empty-`{}`
   argument reverse-binding in bind_call_params.

   Pin only when every hash-typed caller of a method AGREES on the variant. A
   method compiles to a single C return type, so disagreeing callers (`a=mk;
   a["s"]=1` vs `b=mk; b[o]=o`) can't be served by any one concrete hash; unifying
   to TY_POLY isn't a valid empty-hash return (infer_return_types would re-collapse
   it to void every pass), so a conflict is left UNKNOWN -- the same honest void
   error as before the fix, rather than a mispinned incompatible-pointer type. */
int backprop_hash_return_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int ns = c->nscopes;
  /* want[mi]: TY_UNKNOWN = no hash caller seen; a hash = agreed so far;
     TY_POLY = conflict / unconstructible -> do not pin. */
  TyKind *want = calloc((size_t)(ns > 0 ? ns : 1), sizeof(TyKind));
  if (!want) return 0;
  NT_FOREACH_KIND(nt, NK_LocalVariableWriteNode, id) {
    int val = nt_ref(nt, id, "value");
    if (val < 0 || nt_kind(nt, val) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    Scope *lsc = comp_scope_of(c, id);
    LocalVar *lv = lsc ? scope_local(lsc, nm) : NULL;
    if (!lv || lv->is_param || lv->is_block_param || !ty_is_hash(lv->type)) continue;
    int mi = backprop_call_target(c, val);
    if (mi < 0) continue;
    Scope *m = &c->scopes[mi];
    if (m->ret != TY_UNKNOWN) continue;   /* already settled elsewhere; leave it */
    if (m->ret_rbs_seeded || m->ret_specialized || m->cs_synth || m->is_lowered_yield) continue;
    int tail = scope_tail_empty_hash(c, mi);
    if (tail < 0) continue;
    if (want[mi] == TY_UNKNOWN) want[mi] = lv->type;
    else if (want[mi] != lv->type) want[mi] = TY_POLY;   /* callers disagree */
  }
  int changed = 0;
  for (int mi = 0; mi < ns; mi++) {
    if (!ty_is_hash(want[mi])) continue;   /* TY_UNKNOWN (none) or TY_POLY (conflict) */
    Scope *m = &c->scopes[mi];
    if (m->ret == TY_UNKNOWN) { m->ret = want[mi]; changed = 1; }
  }
  free(want);
  return changed;
}

int infer_return_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  int ns = c->nscopes;
  /* Accumulate each scope's explicit-return type in a single node pass.
     The naive form rescanned every node for every scope (O(scopes*nodes));
     on a large input that dominates. Group ReturnNodes by their owning scope
     once instead. */
  TyKind *ret_acc = (TyKind *)malloc(sizeof(TyKind) * (size_t)ns);
  char *has_ret = (char *)calloc((size_t)ns, 1);
  /* Also chain each scope's ReturnNodes (ret_head[scope] -> id -> ret_next[id]),
     so the proc-return block below walks a scope's returns instead of rescanning
     every node per proc-returning scope. */
  int *ret_head = (int *)malloc((size_t)(ns > 0 ? ns : 1) * sizeof(int));
  int *ret_next = (int *)malloc((size_t)(nt->count > 0 ? nt->count : 1) * sizeof(int));
  if (ret_head) for (int i = 0; i < ns; i++) ret_head[i] = -1;
  /* `return v if v.is_a?(K)` with a user class K: that return can only ever
     yield a K, so type it K instead of v's (wider) static type. Without this
     a String-typed param made the method String|K, and dispatching a user
     method named after a builtin (URI#join) on the union collapsed to the
     builtin String signature (#3259). */
  TyKind *ret_narrow = (TyKind *)calloc((size_t)(nt->count > 0 ? nt->count : 1), sizeof(TyKind));
  if (ret_narrow) {
    for (int id = 0; id < nt->count; id++) {
      if (nt_kind(nt, id) != NK_IfNode) continue;
      int pred = nt_ref(nt, id, "predicate");
      const char *pty = pred >= 0 ? nt_type(nt, pred) : NULL;
      if (!pty || !sp_streq(pty, "CallNode")) continue;
      const char *pn = nt_str(nt, pred, "name");
      if (!pn || (!sp_streq(pn, "is_a?") && !sp_streq(pn, "kind_of?") &&
                  !sp_streq(pn, "instance_of?"))) continue;
      int prec = nt_ref(nt, pred, "receiver");
      if (prec < 0 || !nt_type(nt, prec) ||
          !sp_streq(nt_type(nt, prec), "LocalVariableReadNode")) continue;
      const char *vn = nt_str(nt, prec, "name");
      int pargs = nt_ref(nt, pred, "arguments");
      int pan = 0; const int *pav = pargs >= 0 ? nt_arr(nt, pargs, "arguments", &pan) : NULL;
      if (pan != 1 || !pav) continue;
      const char *aty = nt_type(nt, pav[0]);
      const char *kn = (aty && (sp_streq(aty, "ConstantReadNode") ||
                                sp_streq(aty, "ConstantPathNode")))
                         ? nt_str(nt, pav[0], "name") : NULL;
      int kcid = kn ? comp_class_index(c, kn) : -1;
      if (kcid < 0) continue;
      /* single-statement then-arm returning the SAME variable */
      int stm = nt_ref(nt, id, "statements");
      int sn = 0; const int *sb = stm >= 0 ? nt_arr(nt, stm, "body", &sn) : NULL;
      if (sn != 1 || !sb || nt_kind(nt, sb[0]) != NK_ReturnNode) continue;
      int ra = nt_ref(nt, sb[0], "arguments");
      int ran = 0; const int *rav = ra >= 0 ? nt_arr(nt, ra, "arguments", &ran) : NULL;
      if (ran != 1 || !rav || !nt_type(nt, rav[0]) ||
          !sp_streq(nt_type(nt, rav[0]), "LocalVariableReadNode")) continue;
      const char *rn = nt_str(nt, rav[0], "name");
      if (!vn || !rn || !sp_streq(vn, rn)) continue;
      ret_narrow[sb[0]] = ty_object(kcid);
    }
  }
  if (ret_acc && has_ret) {
    for (int id = 0; id < nt->count; id++) {
      if (nt_kind(nt, id) != NK_ReturnNode) continue;
      Scope *rs = comp_scope_of(c, id);
      if (!rs) continue;
      int si = (int)(rs - c->scopes);
      if (si < 0 || si >= ns) continue;
      TyKind rt = (ret_narrow && ret_narrow[id]) ? ret_narrow[id] : return_node_type(c, id);
      ret_acc[si] = has_ret[si] ? ty_unify(ret_acc[si], rt) : rt;
      has_ret[si] = 1;
      if (ret_head && ret_next) { ret_next[id] = ret_head[si]; ret_head[si] = id; }
    }
  }
  /* implicit return: the body's value */
  for (int s = 1; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    /* Specialized inherited-cls-new copies keep their fixed subclass return
       type (the shared body's bare `new` would otherwise infer the base). */
    if (sc->ret_specialized) continue;
    /* An --rbs-seeded return is pinned, with one exception: a scalar-valued
       str-keyed hash return (Hash[String,String] / Hash[String,Integer]) whose
       body actually builds a poly-valued StrPolyHash (mixed / non-scalar
       values -- the RBS value type is too narrow for what the code returns).
       Emitting the StrPolyHash body through a StrStrHash* signature is a layout
       mismatch that corrupts every read, so let the body widen the return to
       its poly-valued sibling. Every other rbs-seeded return stays pinned. */
    if (sc->ret_rbs_seeded) {
      if (sc->ret == TY_STR_STR_HASH || sc->ret == TY_STR_INT_HASH) {
        TyKind br = sc->body >= 0 ? infer_type(c, sc->body) : TY_UNKNOWN;
        if (has_ret && has_ret[s]) br = ty_unify(br, ret_acc[s]);
        if (br == TY_STR_POLY_HASH) { sc->ret = TY_STR_POLY_HASH; changed = 1; }
      }
      continue;
    }
    /* synthesized compiler_state methods carry a fixed return type (no AST). */
    if (sc->cs_synth) continue;
    /* A lowered self-recursive yield method returns its block's value through
       a raw mrb_int carrier pinned when the lowering rewrites the scope
       (post-fixpoint); re-deriving from the body would break that ABI. */
    if (sc->is_lowered_yield) continue;
    /* An empty method body returns nil; if its value is used at all it must
       be poly (a void C function yields nothing to read). */
    int empty_body = sc->body < 0;
    if (sc->body >= 0 && nt_kind(nt, sc->body) == NK_StatementsNode) {
      int bn = 0; nt_arr(nt, sc->body, "body", &bn); if (bn == 0) empty_body = 1;
    }
    /* A trailing infinite loop (`while true` / `until false`) with no
       top-level break can't fall through, so its nil value is unreachable:
       when explicit returns exist, they alone type the method instead of
       nil-widening it to poly. A breaking or finite loop still contributes
       its nil fall-through, as CRuby does. */
    int tail_unreachable = 0;
    if (!empty_body && has_ret && has_ret[s] &&
        nt_kind(nt, sc->body) == NK_StatementsNode) {
      int bn2 = 0; const int *bb2 = nt_arr(nt, sc->body, "body", &bn2);
      if (bn2 > 0) {
        int last = bb2[bn2 - 1];
        NodeKind lk = nt_kind(nt, last);
        if (lk == NK_WhileNode || lk == NK_UntilNode) {
          int pred = nt_ref(nt, last, "predicate");
          const char *cty = pred >= 0 ? nt_type(nt, pred) : NULL;
          int infinite = cty && ((lk == NK_WhileNode && sp_streq(cty, "TrueNode")) ||
                                 (lk == NK_UntilNode && sp_streq(cty, "FalseNode")));
          int lbody = nt_ref(nt, last, "statements");
          if (infinite && (lbody < 0 || !block_has_top_break(c, lbody)))
            tail_unreachable = 1;
        }
        /* A trailing `raise` is unreachable-fall-through for the same reason:
           the method never returns through it, so its (void) value must not
           be unified with the explicit returns. Unifying void with Integer
           has no rule and lands on poly, which then boxes the return of a
           `return x if cond; raise` guard method and everything downstream
           of its callers. Same rule the branch arms got for raise. */
        else if (lk == NK_CallNode && nt_ref(nt, last, "receiver") < 0) {
          const char *lnm = nt_str(nt, last, "name");
          if (lnm && (sp_streq(lnm, "raise") || sp_streq(lnm, "fail") ||
                      sp_streq(lnm, "throw") || sp_streq(lnm, "exit") ||
                      sp_streq(lnm, "abort") || sp_streq(lnm, "exit!")))
            tail_unreachable = 1;
        }
      }
    }
    TyKind r = empty_body ? TY_POLY
             : tail_unreachable ? ret_acc[s]
             : infer_type(c, sc->body);
    /* explicit returns within this scope (collected above) */
    if (!tail_unreachable && has_ret && has_ret[s]) r = ty_unify(r, ret_acc[s]);
    /* Post-backstop re-runs fill returns whose body only settled after the
       main fixpoint (a `r = expr; r` chain, #1670). Adopting a NEW poly there
       is a net loss: the late-settling chains that matter are scalar, while a
       previously-UNKNOWN return deriving poly is typically a store-style
       method whose value no caller reads -- boxing it puts an sp_RbVal
       return in optcarrot's hottest poke path for ~4% fps. Keep those at
       their pre-pass type; the main fixpoint still widens to poly freely. */
    if (g_ret_no_new_poly && r == TY_POLY && sc->ret != TY_POLY) continue;
    /* An element-less-hash body (`{}` / Hash.new) infers TY_UNKNOWN every pass
       (no witnessed element). Once a caller has pinned it to a concrete hash
       (backprop_hash_return_types), don't collapse it back to UNKNOWN -- that
       would re-emit a void C signature the caller can't assign (#1680). */
    if (r == TY_UNKNOWN && ty_is_hash(sc->ret) && scope_tail_empty_hash(c, s) >= 0) continue;
    /* A void body-recompute must not downgrade an established return: an
       abstract/raising body infers TY_VOID every pass, while the slot's real
       type comes from descendant-override dispatch unification (or a caller
       backprop). Re-deriving VOID here would flip the slot every iteration
       and the fixpoint never converges. */
    if (r == TY_VOID && sc->ret != TY_UNKNOWN && sc->ret != TY_VOID) continue;
    if (r != sc->ret) { sc->ret = r; changed = 1; }
    /* For a method with a &block param, record the value type its block yields
       (unified across all call sites). Blocks passed to it are emitted returning
       this common type so the sp_proc_call ABI is consistent. */
    if (sc->blk_param && sc->blk_param[0] && !sc->yields && !sc->is_lowered_yield) {
      TyKind bvt = yield_value_type(c, (int)(sc - c->scopes));
      if (bvt != TY_UNKNOWN && sc->blk_ret != (int)bvt) { sc->blk_ret = (int)bvt; changed = 1; }
    }
    /* When the method returns a proc, record the proc's body return type so a
       caller's `m.call(...)` resolves its result type (factory pattern). */
    if (r == TY_PROC) {
      TyKind pr = TY_UNKNOWN;
      if (sc->body >= 0) {
        int bn = 0; const int *bb = nt_arr(nt, sc->body, "body", &bn);
        if (bn > 0) pr = proc_ret_of(c, bb[bn - 1]);
      }
      if (ret_head && ret_next) {
        for (int id = ret_head[s]; id >= 0; id = ret_next[id]) {
          int a = nt_ref(nt, id, "arguments"); int an = 0;
          const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
          if (an > 0) pr = ty_unify(pr == TY_UNKNOWN ? TY_UNKNOWN : pr, proc_ret_of(c, av[0]));
        }
      }
      else for (int id = 0; id < nt->count; id++) {
        const char *ty = nt_type(nt, id);
        if (ty && sp_streq(ty, "ReturnNode") && comp_scope_of(c, id) == sc) {
          int a = nt_ref(nt, id, "arguments"); int an = 0;
          const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
          if (an > 0) pr = ty_unify(pr == TY_UNKNOWN ? TY_UNKNOWN : pr, proc_ret_of(c, av[0]));
        }
      }
      if (pr != TY_UNKNOWN && sc->ret_proc_ret != (int)pr) { sc->ret_proc_ret = (int)pr; changed = 1; }
    }
  }

  /* An abstract base method (`def self.table_name; raise; end`) infers a void
     return, but a subclass overrides it with a value-returning version. A call
     bound to the base in value position would then assign void into a temp and
     fail to compile (#1416). Since the base body always raises, its return is
     unreachable -- widen its type to the override return(s), so the call is
     usable. Only raising bases qualify (a genuinely nil-returning void method
     must stay void). */
  for (int s = 1; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (sc->ret != TY_VOID || sc->class_id < 0 || !sc->name) continue;
    if (sc->ret_specialized || sc->ret_rbs_seeded || sc->cs_synth) continue;
    if (!scope_tail_raises(c, s)) continue;
    if (rn_nscopes != c->nscopes) rn_build(c);
    TyKind unified = TY_VOID;
    int use_idx = rn_buckets > 0;
    int t = use_idx ? rn_head[wrn_hash(sc->name) % (unsigned)rn_buckets] : 1;
    for (; use_idx ? (t >= 0) : (t < c->nscopes); t = use_idx ? rn_next[t] : t + 1) {
      Scope *ot = &c->scopes[t];
      if (t == s || !ot->name || !sp_streq(ot->name, sc->name)) continue;
      if (ot->is_cmethod != sc->is_cmethod || ot->class_id < 0) continue;
      if (!is_descendant(c, ot->class_id, sc->class_id)) continue;
      if (ot->ret == TY_VOID || ot->ret == TY_UNKNOWN) continue;
      unified = (unified == TY_VOID) ? ot->ret : ty_unify(unified, ot->ret);
    }
    if (unified != TY_VOID) { sc->ret = unified; changed = 1; }
  }

  free(ret_acc); free(has_ret); free(ret_head); free(ret_next); free(ret_narrow);
  return changed;
}

/* Collect CallNode names in the subtree rooted at `id`, stopping at nested
   DefNodes (which are separate method scopes). `out` / `n` / `cap` are
   the dynamic string array to append to. */
void cr_collect_calls(Compiler *c, const NodeTable *nt, int id,
                              char ***out, int *n, int *cap) {
  if (id < 0) return;
  const char *ty = nt_type(nt, id);
  if (!ty) return;
  if (sp_streq(ty, "DefNode")) return;          /* don't enter nested methods */
  /* `if defined?(UnknownConst) ... end`: the then-branch is compile-time dead
     (and never emitted -- see emit_if), so collecting its calls would mark
     genuinely unemittable methods live. Only walk the live side. */
  if (sp_streq(ty, "IfNode") && comp_defined_guard_false(c, nt_ref(nt, id, "predicate"))) {
    cr_collect_calls(c, nt, nt_ref(nt, id, "subsequent"), out, n, cap);
    return;
  }
  if (sp_streq(ty, "UnlessNode") && comp_defined_guard_false(c, nt_ref(nt, id, "predicate"))) {
    cr_collect_calls(c, nt, nt_ref(nt, id, "statements"), out, n, cap);
    return;
  }
  /* Collect method name from CallNode, or operator name from op-assign nodes
     (e.g. `a += 1` → InstanceVariableOperatorWriteNode with binary_operator "+"). */
  const char *nm = NULL;
  if (sp_streq(ty, "CallNode")) {
    nm = nt_str(nt, id, "name");
    /* `method(:foo)` takes a reference to foo without calling it; the target
       must still be emitted, so treat the symbol arg as a called name. */
    if (nm && (sp_streq(nm, "method") || sp_streq(nm, "instance_method"))) {
      int margs = nt_ref(nt, id, "arguments");
      int man = 0; const int *mav = margs >= 0 ? nt_arr(nt, margs, "arguments", &man) : NULL;
      if (man >= 1) {
        const char *aty = nt_type(nt, mav[0]);
        const char *msym = NULL;
        if (aty && sp_streq(aty, "SymbolNode")) msym = nt_str(nt, mav[0], "value");
        else if (aty && sp_streq(aty, "StringNode")) { msym = nt_str(nt, mav[0], "content"); if (!msym) msym = nt_str(nt, mav[0], "unescaped"); }
        if (msym) {
          int found = 0;
          for (int i = 0; i < *n; i++) if (sp_streq((*out)[i], msym)) { found = 1; break; }
          if (!found) {
            if (*n >= *cap) { *cap = *cap ? *cap * 2 : 8; *out = realloc(*out, sizeof(char *) * (size_t)*cap); }
            (*out)[(*n)++] = strdup(msym);
          }
        }
      }
    }
  }
  else {
    size_t tl = strlen(ty);
    if (tl > 17 && (sp_streq(ty + tl - 17, "OperatorWriteNode")))
      nm = nt_str(nt, id, "binary_operator");
  }
  if (nm) {
    int found = 0;
    for (int i = 0; i < *n; i++) if (sp_streq((*out)[i], nm)) { found = 1; break; }
    if (!found) {
      if (*n >= *cap) { *cap = *cap ? *cap * 2 : 8; *out = realloc(*out, sizeof(char *) * (size_t)*cap); }
      (*out)[(*n)++] = strdup(nm);
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(nt, id, i); if (ch >= 0) cr_collect_calls(c, nt, ch, out, n, cap); }
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) { int nn = 0; const int *ids = nt_arr_at(nt, id, i, &nn); for (int k = 0; k < nn; k++) if (ids[k] >= 0) cr_collect_calls(c, nt, ids[k], out, n, cap); }
}

/* Mark each method scope reachable via transitive call-graph BFS.
   Scope 0 (top level), every `initialize`, and implicitly-called methods
   are roots. Any method reachable from a root (directly or transitively)
   is marked live; others are dead-code-eliminated. */

/* ---- Loop-growth bigint promotion ----
   The legacy compiler's pre_detect_bigint, ported: inside a while loop a
   local rebuilt by self-referential multiplication (x = a * b or x *= y
   where an operand flows back from x through local-to-local assignments)
   or by fibonacci-shaped addition (x = a + b where BOTH operands flow
   back from x) grows without bound; promote it from int to bigint. The
   main inference fixpoint then spreads bigint through arithmetic results
   and assignment chains (ty_unify keeps int+bigint at bigint). */

#define BI_MAX_PAIRS 256

typedef struct { const char *dst, *src; } BiPair;

static const char *bi_local_name(const NodeTable *nt, int id) {
  if (id < 0) return NULL;
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "LocalVariableReadNode")) return NULL;
  return nt_str(nt, id, "name");
}

/* Collect `dst = src` local-to-local assignments in the loop subtree. */
static void bi_collect_assigns(const NodeTable *nt, int id, BiPair *pairs, int *np) {
  if (id < 0) return;
  const char *ty = nt_type(nt, id);
  if (!ty || sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return;
  if (sp_streq(ty, "LocalVariableWriteNode")) {
    const char *src = bi_local_name(nt, nt_ref(nt, id, "value"));
    const char *dst = nt_str(nt, id, "name");
    if (src && dst && *np < BI_MAX_PAIRS) { pairs[*np].dst = dst; pairs[*np].src = src; (*np)++; }
  }
  /* `a, b = c, d` style multi-writes also carry values between locals. */
  if (sp_streq(ty, "MultiWriteNode")) {
    int ln = 0, rn = 0;
    const int *lhs = nt_arr(nt, id, "lefts", &ln);
    int v = nt_ref(nt, id, "value");
    const int *rhs = NULL;
    if (v >= 0 && nt_type(nt, v) && sp_streq(nt_type(nt, v), "ArrayNode"))
      rhs = nt_arr(nt, v, "elements", &rn);
    for (int k = 0; lhs && rhs && k < ln && k < rn; k++) {
      const char *lty = nt_type(nt, lhs[k]);
      if (!lty || !sp_streq(lty, "LocalVariableTargetNode")) continue;
      const char *src = bi_local_name(nt, rhs[k]);
      const char *dst = nt_str(nt, lhs[k], "name");
      if (src && dst && *np < BI_MAX_PAIRS) { pairs[*np].dst = dst; pairs[*np].src = src; (*np)++; }
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) bi_collect_assigns(nt, nt_ref_at(nt, id, i), pairs, np);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++) bi_collect_assigns(nt, ids[j], pairs, np);
  }
}

/* Does `var`'s value flow into `target` through the assignment pairs? */
static int bi_reaches(const BiPair *pairs, int np, const char *var, const char *target, int depth) {
  if (sp_streq(var, target)) return 1;
  if (depth > 10) return 0;
  for (int i = 0; i < np; i++)
    if (sp_streq(pairs[i].src, var) &&
        bi_reaches(pairs, np, pairs[i].dst, target, depth + 1)) return 1;
  return 0;
}

static void bi_promote(Compiler *c, int write_id, const char *lname) {
  Scope *s = comp_scope_of(c, write_id);
  LocalVar *lv = s ? scope_local(s, lname) : NULL;
  if (lv && !lv->rbs_seeded && (lv->type == TY_UNKNOWN || lv->type == TY_INT)) lv->type = TY_BIGINT;
}

static void bi_scan_loop_node(Compiler *c, int id, const BiPair *pairs, int np) {
  const NodeTable *nt = c->nt;
  if (id < 0) return;
  const char *ty = nt_type(nt, id);
  if (!ty || sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return;
  if (sp_streq(ty, "LocalVariableWriteNode")) {
    const char *lname = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    const char *vty = v >= 0 ? nt_type(nt, v) : NULL;
    if (lname && vty && sp_streq(vty, "CallNode")) {
      const char *op = nt_str(nt, v, "name");
      const char *rname = bi_local_name(nt, nt_ref(nt, v, "receiver"));
      const char *aname = NULL;
      int args = nt_ref(nt, v, "arguments");
      int an = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an >= 1) aname = bi_local_name(nt, argv[0]);
      if (op && (sp_streq(op, "*") || sp_streq(op, "**"))) {
        if ((rname && bi_reaches(pairs, np, lname, rname, 0)) ||
            (aname && bi_reaches(pairs, np, lname, aname, 0)))
          bi_promote(c, id, lname);
      }
      else if (op && sp_streq(op, "+")) {
        /* fibonacci shape: BOTH operands flow back from lname; this
           rejects the linear `i = i + 1`. */
        if (rname && aname &&
            bi_reaches(pairs, np, lname, rname, 0) &&
            bi_reaches(pairs, np, lname, aname, 0))
          bi_promote(c, id, lname);
      }
    }
  }
  if (sp_streq(ty, "LocalVariableOperatorWriteNode")) {
    const char *op = nt_str(nt, id, "binary_operator");
    const char *lname = nt_str(nt, id, "name");
    if (op && lname && (sp_streq(op, "*") || sp_streq(op, "**")))
      bi_promote(c, id, lname);
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) bi_scan_loop_node(c, nt_ref_at(nt, id, i), pairs, np);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++) bi_scan_loop_node(c, ids[j], pairs, np);
  }
}

/* Run the self-referential-multiply scan over one loop body subtree. */
static void bi_scan_loop_body(Compiler *c, int body) {
  if (body < 0) return;
  BiPair pairs[BI_MAX_PAIRS];
  int np = 0;
  bi_collect_assigns(c->nt, body, pairs, &np);
  bi_scan_loop_node(c, body, pairs, np);
}

/* An iteration method whose block runs an unbounded number of times, so an
   accumulator multiplied inside it can grow without bound. Only consulted in
   promote mode (the wrap-pinned optcarrot must not pay a block-loop bigint
   widening, which is why the default path stays `while`-only). */
static int bi_is_block_loop_method(const char *name) {
  return sp_streq(name, "times") || sp_streq(name, "each") ||
         sp_streq(name, "upto") || sp_streq(name, "downto") ||
         sp_streq(name, "step") || sp_streq(name, "loop") ||
         sp_streq(name, "each_with_index");
}

void infer_bigint_loop_locals(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (sp_streq(ty, "WhileNode")) {
      bi_scan_loop_body(c, nt_ref(nt, id, "statements"));
      continue;
    }
    /* Promote mode additionally treats block-iteration loops as growth sites:
       `n.times { f = f * x }`, `(a..b).each { ... }`, etc. The block body is a
       BlockNode -> statements; reuse the same self-referential-multiply scan. */
    if (g_promote_mode && sp_streq(ty, "CallNode")) {
      const char *mname = nt_str(nt, id, "name");
      int block = nt_ref(nt, id, "block");
      if (mname && bi_is_block_loop_method(mname) && block >= 0 &&
          nt_type(nt, block) && sp_streq(nt_type(nt, block), "BlockNode"))
        bi_scan_loop_body(c, nt_ref(nt, block, "body"));
    }
  }
}
