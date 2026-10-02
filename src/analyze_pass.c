#include "analyze_internal.h"
int callee_has_kwarg(Compiler *c, Scope *m, const char *name);
int callee_declares_kwargs(Compiler *c, Scope *m);
int callee_param_is_declared_kwarg(Compiler *c, Scope *m, const char *name);
int is_fresh_array(Compiler *c, int v);
static int widen_nested_literals(Compiler *c, int recv, int is_push, int is_splice, TyKind kt, TyKind vt);
int kwh_only_spreads(const NodeTable *nt, int kwh);
#include <stdint.h>
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
/* Bucket key. Local writes are keyed by (name, scope) so a common local name
   (`s`, `result`) written across thousands of methods does not collapse into
   one giant chain that every query must walk; ivar writes are keyed by name
   alone, matching the lookup semantics (an ivar `@x` write anywhere counts). */
static unsigned wrn_key(const char *nm, int scopeidx) {
  unsigned h = sp_strhash(nm);
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
static int recv_has_array_write(Compiler *c, int recv);

/* The class owning the slot an `@x` receiver names. Ivar slots are per-class
   (comp_ivar_index is a flat per-class table, and a subclass interns its own),
   so a write to `@x` in an unrelated class is evidence about a DIFFERENT slot.
   The query below indexes writes by NAME; for a local it then filters by scope,
   but its ivar arm filtered by nothing, so `@threads = 1` in one class answered
   a question asked about `@threads` in another. -1 is a top-level ivar (main's
   own slot), which is still a well-defined owner to compare. */
static int ivar_recv_class_id(Compiler *c, int recv) {
  Scope *s = comp_scope_of(c, recv);
  return s ? s->class_id : -1;
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
  int rcls = is_ivar ? ivar_recv_class_id(c, recv) : -1;
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
    if (is_ivar) {
      Scope *ws = comp_scope_of(c, id);
      if (!ws || ws->class_id != rcls) continue;
    }
    int v = nt_ref(nt, id, "value");
    if (v < 0) continue;
    const char *vty = nt_type(nt, v);
    if (vty && (sp_streq(vty, "IntegerNode") || sp_streq(vty, "FloatNode"))) return 1;
    TyKind vt = infer_type(c, v);
    if (vt == TY_INT || vt == TY_FLOAT || vt == TY_BIGINT) return 1;
  }
  return 0;
}

/* Does the receiver slot have a write that is definitely an array? Used to
   tell a genuine push accumulator (`out = []` ... `out << e`) from a slot
   whose `<<` is a user class's own operator. */
static int recv_has_array_write(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  const char *rty = recv >= 0 ? nt_type(nt, recv) : NULL;
  if (!rty) return 0;
  /* `obj.list << x` -- the receiver is a READER. Its array evidence is the
     backing ivar's, in whichever class owns the reader: without it, any
     program that also contains a user `<<` (a bundled csv or a Set-like class
     is enough) stopped taking element evidence from every such push, and the
     pushed-into slot kept the empty literal's bottom kind (#3781). */
  if (sp_streq(rty, "CallNode")) {
    int rargs = nt_ref(nt, recv, "arguments");
    int rargc = 0;
    if (rargs >= 0) nt_arr(nt, rargs, "arguments", &rargc);
    const char *gname = nt_str(nt, recv, "name");
    if (rargc != 0 || !gname) return 0;
    char ivn[300];
    snprintf(ivn, sizeof ivn, "@%s", gname);
    /* Drive from the WRITES to `@gname` (the name-keyed index above), not from
       every class crossed with every node: the two loops asked one question --
       is there an array write to the backing ivar in the class that defines
       the reader -- and the writes are the small side of it. */
    if (wrn_nt != nt || wrn_ntc != nt->count) wrn_build(c);
    if (!wrn_buckets) return 0;
    unsigned rb = wrn_key(ivn, -1) % (unsigned)wrn_buckets;
    for (int id = wrn_head[rb]; id >= 0; id = wrn_next[id]) {
      if (nt_kind(nt, id) != NK_InstanceVariableWriteNode) continue;
      const char *wnm = nt_str(nt, id, "name");
      if (!wnm || !sp_streq(wnm, ivn)) continue;
      Scope *ws = comp_scope_of(c, id);
      if (!ws) continue;
      int wc = ws->class_id;
      if (wc < 0 || wc >= c->nclasses) continue;
      if (comp_ivar_index(&c->classes[wc], ivn) < 0) continue;
      int v = nt_ref(nt, id, "value");
      if (v < 0) continue;
      if (nt_kind(nt, v) != NK_ArrayNode && !ty_is_array(infer_type(c, v))) continue;
      /* The write only counts when some class really does reach this defining
         class through a reader named `gname` (alias resolution is per-class,
         so this asks every class, exactly as the outer loop did). */
      for (int k = 0; k < c->nclasses; k++) {
        int rdcls = k;
        if (comp_reader_in_chain(c, k, gname, &rdcls) && rdcls == wc) return 1;
      }
    }
    return 0;
  }
  int is_ivar = sp_streq(rty, "InstanceVariableReadNode");
  int is_local = sp_streq(rty, "LocalVariableReadNode");
  if (!is_ivar && !is_local) return 0;
  const char *rnm = nt_str(nt, recv, "name");
  if (!rnm) return 0;
  Scope *rscope = is_local ? comp_scope_of(c, recv) : NULL;
  const char *wkind = is_ivar ? "InstanceVariableWriteNode" : "LocalVariableWriteNode";
  /* Same index, same query shape as recv_has_scalar_numeric_write above. */
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
    if (nt_kind(nt, v) == NK_ArrayNode) return 1;
    if (ty_is_array(infer_type(c, v))) return 1;
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
static unsigned aw_gen = 0;
static int *aw_next = NULL, *aw_head = NULL;
/* The bucket key is the receiver's name AND its owner -- the class for an
   ivar, the scope for a local -- which every consumer filters by: keyed by
   the name alone, a name every unit repeats (`@user`, `h`) put all of them in
   one chain, walked per ask (rubys in #5035). */
static unsigned aw_key(const char *nm, int is_ivar, int owner) {
  return sp_strhash(nm) ^ ((unsigned)(owner + 2) * 2654435761u) ^ (is_ivar ? 0x9e3779b9u : 0);
}
static int aw_owner_of(Compiler *c, int recv, int is_ivar) {
  Scope *s = comp_scope_of(c, recv);
  if (is_ivar) return s ? s->class_id : -1;
  return s ? (int)(s - c->scopes) : -1;
}
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
    int k = nt_kind(nt, id);
    /* `recv[k] = v` plus the read-modify-write forms, which key the same slot:
       `recv[k] ||= v` is as much a key write as `recv[k] = v`, and a variant
       decision that ignores it defaults on incomplete evidence (#3397).
       Their key is arguments[0] and their VALUE is a `value` ref rather than
       arguments[1], so aset_value_type_ex's `an < 2` guard skips them and its
       answer is unchanged. */
    int is_owr = (k == NK_IndexOrWriteNode || k == NK_IndexAndWriteNode ||
                  k == NK_IndexOperatorWriteNode);
    if (k != NK_CallNode && !is_owr) continue;
    if (!is_owr) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || !sp_streq(nm, "[]=")) continue;
    }
    int wr = nt_ref(nt, id, "receiver");
    if (wr < 0) continue;
    int wk = nt_kind(nt, wr);
    if (wk != NK_InstanceVariableReadNode && wk != NK_LocalVariableReadNode) continue;
    const char *wn = nt_str(nt, wr, "name");
    if (!wn) continue;
    int iv = wk == NK_InstanceVariableReadNode;
    unsigned b = aw_key(wn, iv, aw_owner_of(c, wr, iv)) % (unsigned)aw_buckets;
    aw_next[id] = aw_head[b]; aw_head[b] = id;
  }
  aw_gen = comp_scope_index_gen();
}
/* First `[]=` call id chained for receiver name `rnm`, or -1; walk via aw_next.
   Caller must still verify the exact name (hash collisions) and receiver kind. */
static int aw_first(Compiler *c, const char *rnm, int is_ivar, int owner) {
  const NodeTable *nt = c->nt;
  if (aw_nt != nt || aw_ntc != nt->count || aw_gen != comp_scope_index_gen()) aw_build(c);
  if (!aw_buckets) return -1;
  return aw_head[aw_key(rnm, is_ivar, owner) % (unsigned)aw_buckets];
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
  for (int id = aw_first(c, rnm, is_ivar, is_ivar ? rcls : (rsc ? (int)(rsc - c->scopes) : -1)); id >= 0; id = aw_next[id]) {
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
  for (int id = aw_first(c, name, 0, (int)(sc - c->scopes)); id >= 0; id = aw_next[id]) {
    int wrecv = nt_ref(nt, id, "receiver");
    if (wrecv < 0 || nt_kind(nt, wrecv) != NK_LocalVariableReadNode) continue;
    const char *wn = nt_str(nt, wrecv, "name");
    if (!wn || !sp_streq(wn, name)) continue;
    if (comp_scope_of(c, wrecv) != sc) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    /* `[]=` carries key and value in arguments; the read-modify-write forms
       carry only the key there. Either way the key is arguments[0]. */
    int wk = nt_kind(nt, id);
    int min_args = (wk == NK_IndexOrWriteNode || wk == NK_IndexAndWriteNode ||
                    wk == NK_IndexOperatorWriteNode) ? 1 : 2;
    if (an < min_args) continue;
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

/* Whether some call of the method `sc` by name passes a boxed value (not
   an empty `{}`), or one not typed yet, to positional parameter p, as the
   call's layout funds it (a splat's element is boxed). Matched by name, so
   a same-named method elsewhere only makes this more careful. */
static int param_gets_boxed_arg(Compiler *c, Scope *sc, int p) {
  const NodeTable *nt = c->nt;
  if (!sc->name) return 0;
  /* the calls of this name only: asked per parameter of every scope, a walk
     of every call per ask was quadratic (rubys/roundhouse#72) */
  for (int id = an_calls_named_first(c, sc->name); id >= 0; id = an_calls_named_next(id)) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, sc->name)) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    ArgLayout L;
    call_layout(c, sc, av, an, &L);
    ArgFrom from = p < L.n ? L.from[p] : ARG_DEFAULT;
    int a = layout_plain_arg(c, sc, av, &L, p);
    arg_layout_free(&L);
    if (from == ARG_ELEM || from == ARG_GATHERED) return 1;
    if (a < 0) continue;
    NodeKind ak = nt_kind(nt, a);
    if (ak == NK_HashNode) continue;
    TyKind at = infer_type(c, a);
    if (at == TY_POLY) return 1;
    /* not typed yet: wait for it rather than decide the parameter now,
       unless it is a local only ever given an empty `{}` (the container
       the narrowing exists for) */
    if (at == TY_UNKNOWN) {
      if (ak != NK_LocalVariableReadNode) return 1;
      const char *an = nt_str(nt, a, "name");
      Scope *as = an ? comp_scope_of(c, a) : NULL;
      if (!as || !local_all_writes_empty_hash(c, as, an)) return 1;
    }
  }
  return 0;
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
      /* ...and a parameter whose default is an empty `{}`. Such a hash is
         created HERE, not handed in, so narrowing it converts nothing and
         reinterprets nobody. Without this an `acc = {}` default settled on the
         symbol-keyed variant and the body's `acc[key.to_s] = v` wrote a String
         through an sp_sym slot (#3433). Restricted below to a disagreeing KEY
         type: a key mismatch is a wrong slot, not the value-narrowing hazard
         the guard above exists for. */
      int empty_hash_default = 0;
      if (!seedable && ty_is_hash(cur) && p < sc->nparams && sc->pdefault[p] >= 0) {
        int dn = sc->pdefault[p];
        const char *dty = nt_type(nt, dn);
        int den = 0;
        if (dty && (sp_streq(dty, "HashNode") || sp_streq(dty, "KeywordHashNode")))
          { nt_arr(nt, dn, "elements", &den); empty_hash_default = (den == 0); }
      }
      if ((!seedable && !empty_hash_default) || lv->rbs_seeded) continue;
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
      for (int id = aw_first(c, sc->pnames[p], 0, (int)(sc - c->scopes)); id >= 0; id = aw_next[id]) {
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
      /* A boxed parameter some caller hands a boxed value is that caller's
         object, whatever kind it holds at run time: narrowing it cast a
         `Hash.new(0)` (a poly-keyed hash) to the string-keyed struct and the
         counting loop ran off its table (tally(hash) in builtins/). */
      if (seedable && param_gets_boxed_arg(c, sc, p)) continue;
      TyKind hv = ty_hash_of(kt, vt);
      if (hv == TY_UNKNOWN) continue;
      if (empty_hash_default && !seedable) {
        /* only the key mismatch, and keep whatever value type is already
           settled unless the writes are more specific */
        if (ty_hash_key(cur) != kt) {
          TyKind want = ty_hash_of(kt, ty_hash_val(cur) != TY_UNKNOWN ? ty_hash_val(cur) : vt);
          if (want != TY_UNKNOWN && want != cur) { lv->type = want; changed = 1; }
        }
        continue;
      }
      if (hv != cur && ty_hash_val(hv) != TY_POLY) { lv->type = hv; changed = 1; }
    }
  }
  return changed;
}

/* 1 when `v` is a bare `Array.new` (k NK_ArrayNode) or `Hash.new` (k
   NK_HashNode): no argument, no block, the same empty container as the
   literal (#3613) */
static int is_bare_container_new(Compiler *c, int v, NodeKind k) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, v) != NK_CallNode || nt_ref(nt, v, "arguments") >= 0 || nt_ref(nt, v, "block") >= 0) return 0;
  return an_empty_container_kind(c, v) == (k == NK_HashNode ? 2 : 1);
}

/* `bare_new`: a bare constructor (is_bare_container_new) counts as the
   empty literal too */
static int local_all_writes_empty(Compiler *c, Scope *sc, const char *name, NodeKind k, int bare_new) {
  const NodeTable *nt = c->nt;
  int saw = 0;
  int si = (int)(sc - c->scopes);
  for (int r = lw_shared_first(c, name, si); r >= 0; r = lw_shared_next(r)) {
    int id = lw_shared_node(r);
    if (nt_kind(nt, id) != NK_LocalVariableWriteNode) continue;
    const char *wn = nt_str(nt, id, "name");
    if (!wn || !sp_streq(wn, name) || comp_scope_of(c, id) != sc) continue;
    int v = nt_ref(nt, id, "value");
    if (v < 0) return 0;
    if (bare_new && is_bare_container_new(c, v, k)) { saw = 1; continue; }
    if (nt_kind(nt, v) != k) return 0;
    int en = 0; nt_arr(nt, v, "elements", &en);
    if (en != 0) return 0;
    saw = 1;
  }
  return saw;
}

/* 1 if local `name` in scope `sc` has at least one write and every write
   assigns an empty `{}` hash literal -- i.e. it is a hash container whose
   contents come from elsewhere (passed by reference into a callee). Such a
   local can safely adopt a hash type from a parameter it is passed to. */
/* Shared cached write-index accessors (defined after the LWIndex machinery
   below): bucket walk over (scope, name) instead of a whole-table rescan per
   query. */
int local_all_writes_empty_hash(Compiler *c, Scope *sc, const char *name) {
  return local_all_writes_empty(c, sc, name, NK_HashNode, 0);
}

/* local_all_writes_empty_hash, where a bare `Hash.new` is the empty `{}` too */
int local_all_writes_empty_hash_or_new(Compiler *c, Scope *sc, const char *name) {
  return local_all_writes_empty(c, sc, name, NK_HashNode, 1);
}

/* 1 if local `name` in scope `sc` has at least one write and every write
   assigns an empty `[]` array literal -- the array analogue of
   local_all_writes_empty_hash. Such a local carries no element evidence of
   its own, so it can adopt the poly element type a callee's push forced. */
int local_all_writes_empty_array(Compiler *c, Scope *sc, const char *name) {
  return local_all_writes_empty(c, sc, name, NK_ArrayNode, 0);
}

/* 1 iff every write of local `name` in `sc` builds a new array (and there is
   one): a value nothing else holds, so rebuilding it as another array kind
   loses no sharing. */
static int local_all_writes_fresh_array(Compiler *c, Scope *sc, const char *name) {
  const NodeTable *nt = c->nt;
  int saw = 0;
  int si = (int)(sc - c->scopes);
  for (int r = lw_shared_first(c, name, si); r >= 0; r = lw_shared_next(r)) {
    int id = lw_shared_node(r);
    if (comp_scope_of(c, id) != sc) continue;
    const char *wn = nt_str(nt, id, "name");
    if (!wn || !sp_streq(wn, name)) continue;
    if (nt_kind(nt, id) != NK_LocalVariableWriteNode) return 0;
    if (!is_fresh_array(c, nt_ref(nt, id, "value"))) return 0;
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

/* The position of the positional parameter of method scope `sc` that `n`
   reads, when the body never assigns it (so the read is what the call
   passed); -1 otherwise. */
int unassigned_param_read(Compiler *c, Scope *sc, int n) {
  const NodeTable *nt = c->nt;
  if (!sc || n < 0 || nt_kind(nt, n) != NK_LocalVariableReadNode || comp_scope_of(c, n) != sc) return -1;
  const char *nm = nt_str(nt, n, "name");
  if (!nm) return -1;
  int k = 0;
  while (k < sc->nparams && !(sc->pnames[k] && sp_streq(sc->pnames[k], nm))) k++;
  if (k >= sc->nparams || k >= 64) return -1;
  if ((sc->rest_idx >= 0 && k >= sc->rest_idx) || (sc->kwrest_idx >= 0 && k >= sc->kwrest_idx)) return -1;
  if (callee_param_is_declared_kwarg(c, sc, nm)) return -1;
  int si = (int)(sc - c->scopes);
  for (int r = lw_shared_first(c, nm, si); r >= 0; r = lw_shared_next(r)) {
    int w = lw_shared_node(r);
    if (comp_scope_of(c, w) == sc && sp_streq(nt_str(nt, w, "name"), nm)) return -1;
  }
  return k;
}

/* Sets bit `k` of `*mask`; 1 when it was clear. */
static int mask_add(unsigned long long *mask, int k) {
  if (k < 0 || (*mask >> k) & 1ULL) return 0;
  *mask |= 1ULL << k;
  return 1;
}

/* Per-pass index of instance-variable write nodes keyed by ivar name -- the
   ivar analogue of LWIndex. The usage-driven promotion scans below ask "does
   @x have a non-empty-hash / typed write"; without this each query re-scanned
   the whole node table, the dominant cost on ivar-heavy model graphs (the
   #1302 from_hash/to_hash shape). Indexes both plain and `||=` ivar writes;
   callers filter by node kind and class. Reuses LWIndex's layout (scope-less,
   so the bucket key is name only). */
static unsigned ivw_hash(const char *name) {
  unsigned h = 2166136261u;
  for (const char *p = name; p && *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  return h;
}

static const NodeKind ivw_write_kinds[2] = {
  NK_InstanceVariableWriteNode, NK_InstanceVariableOrWriteNode};

static void ivw_index_build(Compiler *c, LWIndex *ix) {
  const NodeTable *nt = c->nt;
  /* Iterate only the ivar-write kinds (kind-grouped id lists) rather than the
     whole table -- the write population is a small fraction of the node count,
     and this build reruns whenever the table grows. Same move as
     lw_index_build; consumers are order-independent existence walks. */
  int n = 0;
  for (int t = 0; t < 2; t++) { int kn; nt_nodes_of_kind(nt, ivw_write_kinds[t], &kn); n += kn; }
  int cap = 16;
  while (cap < n * 2) cap <<= 1;
  ix->cap = cap;
  ix->node = (int *)malloc(sizeof(int) * (n > 0 ? n : 1));
  ix->next = (int *)malloc(sizeof(int) * (n > 0 ? n : 1));
  ix->head = (int *)malloc(sizeof(int) * cap);
  for (int i = 0; i < cap; i++) ix->head[i] = -1;
  int k = 0;
  for (int t = 0; t < 2; t++) {
    int kn; const int *ids = nt_nodes_of_kind(nt, ivw_write_kinds[t], &kn);
    for (int j = 0; j < kn; j++) {
      int id = ids[j];
      const char *nm = nt_str(nt, id, "name");
      unsigned h = ivw_hash(nm) & (unsigned)(cap - 1);
      ix->node[k] = id;
      ix->next[k] = ix->head[h];
      ix->head[h] = k;
      k++;
    }
  }
}

/* First record index in the bucket for ivar `name`; iterate via ix->next.
   Callers must still confirm name (hash collisions), node kind, and class. */
static int ivw_index_first(const LWIndex *ix, const char *name) {
  return ix->head[ivw_hash(name) & (unsigned)(ix->cap - 1)];
}

/* Long-lived ivar-write index behind mark_empty_hash_key_ctx's write-site
   lookups -- the ivar analogue of lw_shared_ix. The bucket key is the ivar
   name, a syntactic property of the write node that is stable across the
   frozen fixpoint (the defining class is confirmed by the caller per hit, so
   scope re-homing needs no re-key). Rebuilt on the same signal lw_shared uses:
   node-table identity/growth, or a scope-index epoch tick. */
static LWIndex ivw_shared_ix;
static const NodeTable *ivw_shared_nt = NULL;
static int ivw_shared_ntc = -1;
static unsigned ivw_shared_gen = 0;
int ivw_shared_first(Compiler *c, const char *name) {
  unsigned gen = comp_scope_index_gen();
  if (!comp_scope_index_is_frozen() ||
      ivw_shared_nt != c->nt || ivw_shared_ntc != c->nt->count || ivw_shared_gen != gen) {
    if (ivw_shared_nt) lw_index_free(&ivw_shared_ix);
    ivw_index_build(c, &ivw_shared_ix);
    ivw_shared_nt = c->nt; ivw_shared_ntc = c->nt->count; ivw_shared_gen = gen;
  }
  return ivw_index_first(&ivw_shared_ix, name);
}
int ivw_shared_node(int rec) { return ivw_shared_ix.node[rec]; }
int ivw_shared_next(int rec) { return ivw_shared_ix.next[rec]; }

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

int multi_return_elem_types(Compiler *c, int value, TyKind *out, int max) {
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
      mi = comp_method_index(c, mn);
      if (mi >= 0 && c->scopes[mi].def_node < 0) mi = -1;
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
/* The array an object scrutinee deconstructs to, from its own #deconstruct.
   An array pattern over an object used to default its bindings to Integer,
   which read a boxed Symbol's bits as a small int and bound 1 and 2 for
   `[:x, :y]` (#3954). The method's return type says what the elements really
   are; TY_UNKNOWN means the class has no #deconstruct to ask. */
static TyKind pm_object_deconstruct_array(Compiler *c, TyKind scrut) {
  if (!ty_is_object(scrut)) return TY_UNKNOWN;
  int cid = ty_object_class(scrut);
  if (cid < 0 || cid >= c->nclasses) return TY_UNKNOWN;
  int defcls = cid;
  int mi = comp_method_in_chain(c, cid, "deconstruct", &defcls);
  if (mi < 0) return TY_UNKNOWN;
  TyKind rt = c->scopes[mi].ret;
  return ty_is_array(rt) ? rt : TY_POLY_ARRAY;
}

static int pm_seed_locals_poly(Compiler *c, Scope *ms, int pat) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  if (pat < 0) return 0;
  const char *pty = nt_type(nt, pat);
  if (!pty) return 0;
  if (sp_streq(pty, "LocalVariableTargetNode")) {
    const char *lnm = nt_str(nt, pat, "name");
    LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
    /* No `changed` here, for the reason on infer_write_types' reset: this runs
       inside that pass, so the slot was cleared to UNKNOWN moments ago and
       seeding it POLY looks like a change every round even when the answer is
       last round's. The end-of-pass sweep reports it. (#4116) */
    if (lv && !lv->is_param && !lv->is_block_param)
      lv->type = ty_unify(lv->type, TY_POLY);
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

/* Both callers run this INSIDE infer_write_types, after its per-round reset of
   every non-param local to UNKNOWN -- so the sites here that type such a local
   must not report `changed` either, for the same reason and with the same
   sweep reporting for them. See the comment on that reset. (#4116) */
static int infer_case_pattern_locals(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  /* CaseMatchNode: `case X; in PATTERN; ...` -- infer locals bound by pattern.
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
        /* in x if guard -- binding is in IfNode.statements body */
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
          if (mg != lv->type) lv->type = mg;   /* reset slot: the sweep reports */
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
          if (mg != lv->type) lv->type = mg;   /* reset slot: the sweep reports */
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
          if (mg != lv->type) lv->type = mg;
        }
      }
      /* Bind simple LV target to scrutinee type */
      if (bind_lv_node >= 0 && scrutinee_t != TY_UNKNOWN) {
        const char *lnm = nt_str(nt, bind_lv_node, "name");
        LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
        if (lv && !lv->is_param && !lv->is_block_param) {
          TyKind mg = ty_unify(lv->type, scrutinee_t);
          if (mg != lv->type) lv->type = mg;
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
             element's bits and produced garbage. An object scrutinee's elements
             come from its own #deconstruct, whose return type says what they
             are; only an object with none left to ask keeps the legacy default. */
          TyKind darr = pm_object_deconstruct_array(c, array_scrutinee);
          TyKind et = (elem_t != TY_UNKNOWN) ? elem_t
                    : (darr != TY_UNKNOWN) ? ty_array_elem(darr)
                    : ty_is_object(array_scrutinee) ? TY_INT : TY_POLY;
          TyKind mg = ty_unify(lv->type, et);
          if (mg != lv->type) lv->type = mg;
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
              TyKind darr2 = pm_object_deconstruct_array(c, array_scrutinee);
              TyKind rest_arr = ty_is_array(array_scrutinee) ? array_scrutinee
                              : (darr2 != TY_UNKNOWN) ? darr2
                              : ty_is_object(array_scrutinee) ? TY_INT_ARRAY : TY_POLY_ARRAY;
              TyKind mg = ty_unify(lv->type, rest_arr);
              if (mg != lv->type) lv->type = mg;
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
/* A local typed as one object class whose write value settled POLY only after
   the post-fixpoint write re-run (a callee's return widens last, once its
   parameter has seen a second class): the emitted assignment hands an sp_RbVal
   to an sp_Foo * and the C build fails. Widen the slot to poly -- the value
   really can be either class. Object slots only: a scalar slot has a coercion
   at the assignment, an object slot has none. (#3964) */
int widen_object_locals_from_poly_writes(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_LocalVariableWriteNode) continue;
    const char *nm = nt_str(nt, id, "name");
    Scope *s = nm ? comp_scope_of(c, id) : NULL;
    LocalVar *lv = s ? scope_local(s, nm) : NULL;
    if (!lv || lv->is_param || lv->is_block_param || lv->rbs_seeded) continue;
    if (!ty_is_object(lv->type)) continue;
    int v = nt_ref(nt, id, "value");
    if (v < 0 || infer_type(c, v) != TY_POLY) continue;
    lv->type = TY_POLY;
    changed = 1;
  }
  return changed;
}
/* `arr.map! { ... }` REPLACES every element with the block's value, so a tail
   the receiver's element type cannot hold widens the receiver itself -- the
   same reasoning as a push of a foreign element, on a mutation that rewrites
   the whole array rather than extending it. Without this the typed setter took
   the tail raw (`sp_IntArray_set(a, i, <a String>)`) and the C build failed.
   Widening only: a receiver whose element type already fits is untouched. */
int widen_arrays_from_map_bang(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "map!") && !sp_streq(nm, "collect!"))) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0) continue;
    int body = nt_ref(nt, blk, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    if (bn <= 0) continue;
    TyKind tt = infer_type(c, bb[bn - 1]);
    if (tt == TY_UNKNOWN) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_LocalVariableReadNode) continue;
    const char *rnm = nt_str(nt, recv, "name");
    Scope *ls = rnm ? comp_scope_of(c, recv) : NULL;
    LocalVar *lv = ls ? scope_local(ls, rnm) : NULL;
    /* A plain local owns its array outright, so widening the slot is the whole
       story. A parameter or an ivar shares the storage with a caller or with
       another method, and the box a boxed receiver writes back through cannot
       change its element representation -- widening those here compiles the
       program and then loses the values (`sp_poly_to_i` of a String), which is
       worse than the build failure. They are left alone. */
    if (!lv || lv->is_param || lv->is_block_param || lv->rbs_seeded) continue;
    if (!ty_is_array(lv->type) || lv->type == TY_POLY_ARRAY) continue;
    if (tt == ty_array_elem(lv->type)) continue;
    lv->type = TY_POLY_ARRAY; changed = 1;
  }
  return changed;
}
/* Every block parameter gets a slot, whether or not anything reads it. The loop
   emitters bind the parameter either way, so a name nothing else interned named
   an undeclared identifier in the generated C -- once for a tap/then parameter
   (#3979) and again for one of a sum over an enumerator (#3988). The slot is
   claimed with no type: what types it, if anything does, is unchanged, and
   codegen gives an untyped block parameter boxed storage. */
void intern_block_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_BlockNode, id) {
    int pn = nt_ref(nt, id, "parameters");
    if (pn < 0) continue;
    const char *pnty = nt_type(nt, pn);
    if (!pnty || !sp_streq(pnty, "BlockParametersNode")) continue;
    int inner = nt_ref(nt, pn, "parameters");
    if (inner < 0) continue;
    Scope *bs = comp_scope_of(c, id);
    if (!bs) continue;
    int rn = 0; const int *reqs = nt_arr(nt, inner, "requireds", &rn);
    for (int k = 0; k < rn && reqs; k++) {
      /* a destructuring parameter `|(a, b)|` is a node of its own, whose
         leaves the massign lowering interns where it binds them */
      if (nt_kind(nt, reqs[k]) != NK_RequiredParameterNode) continue;
      const char *nm = nt_str(nt, reqs[k], "name");
      if (!nm || scope_local(bs, nm)) continue;
      LocalVar *lv = scope_local_intern(bs, nm);
      if (lv) lv->is_block_param = 1;
    }
    int on = 0; const int *opts = nt_arr(nt, inner, "optionals", &on);
    for (int k = 0; k < on && opts; k++) {
      const char *nm = nt_str(nt, opts[k], "name");
      if (!nm || scope_local(bs, nm)) continue;
      LocalVar *lv = scope_local_intern(bs, nm);
      if (lv) lv->is_block_param = 1;
    }
  }
}

static int lv_widen(LocalVar *lv, TyKind t) {
  TyKind m = ty_unify(lv->type, t);
  if (m == lv->type) return 0;
  lv->type = m;
  return 1;
}

int reconcile_locals_reading_ivars(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "LocalVariableWriteNode")) continue;
    int val_id = nt_ref(nt, id, "value");
    if (val_id < 0) continue;
    const char *vty = nt_type(nt, val_id);
    if (!vty) continue;
    int is_iv = sp_streq(vty, "InstanceVariableReadNode");
    /* `x = reader` reads the same slot one call deep: an argument-less,
       block-less attr_reader on self. Its own node type was settled before the
       late widening below reached the slot, so reading it back would answer
       the stale narrower type -- the local then declared the object pointer
       while the field it is assigned from had become boxed (#3938). */
    int is_rd = !is_iv && sp_streq(vty, "CallNode");
    if (!is_iv && !is_rd) continue;
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? scope_local(comp_scope_of(c, id), nm) : NULL;
    if (!lv || lv->is_param || lv->is_block_param || lv->rbs_seeded) continue;
    TyKind ivt = TY_UNKNOWN;
    if (is_iv) ivt = infer_type(c, val_id);
    else {
      int rcv = nt_ref(nt, val_id, "receiver");
      const char *rnm = nt_str(nt, val_id, "name");
      int ra = nt_ref(nt, val_id, "arguments"); int rac = 0;
      if (ra >= 0) nt_arr(nt, ra, "arguments", &rac);
      if (!rnm || rac != 0 || nt_ref(nt, val_id, "block") >= 0) continue;
      if (rcv >= 0 && !(nt_type(nt, rcv) && sp_streq(nt_type(nt, rcv), "SelfNode"))) continue;
      Scope *sc = comp_scope_of(c, val_id);
      int cls = sc ? sc->class_id : -1;
      int rdcls = -1;
      if (cls < 0 || !comp_reader_in_chain(c, cls, rnm, &rdcls)) continue;
      if (rdcls < 0 || rdcls >= c->nclasses) continue;
      const char *rn2 = comp_resolve_alias(c, cls, rnm);
      char ivn2[300]; snprintf(ivn2, sizeof ivn2, "@%s", rn2 ? rn2 : rnm);
      int iv2 = comp_ivar_index(&c->classes[rdcls], ivn2);
      if (iv2 < 0) continue;
      ivt = ivar_value_ty(&c->classes[rdcls], iv2);
    }
    if (ivt == TY_UNKNOWN) continue;
    if (lv_widen(lv, ivt)) changed = 1;
  }
  return changed;
}

/* Element type contributed by a pushed value (see yield_aware_elem_ty). */
/* An empty `[]` / `{}` pushed as an element infers UNKNOWN (its own kind
   comes from its uses, and it has none), which the slot rule reads as "no
   evidence": `@c = []; @c << []` left the ivar at the int array the empty
   literal defaults to, and the push handed it a PolyArray pointer, a C
   error. The literal is a container whatever its kind, so the element is
   poly and the slot the poly array; a later pass may still narrow a table
   of such rows once something decides their kind (#4484). */
static TyKind push_elem_ty(Compiler *c, int node) {
  TyKind t = yield_aware_elem_ty(c, node);
  /* a pushed nil keeps the container boxed, as a nil literal element does:
     the scalar nil joins of ty_unify are for slots, not for typed storage */
  if (t == TY_NIL) return TY_POLY;
  if (t == TY_UNKNOWN && node >= 0) {
    NodeKind k = nt_kind(c->nt, node);
    if (k == NK_ArrayNode || k == NK_HashNode || k == NK_KeywordHashNode) {
      int en = 0; nt_arr(c->nt, node, "elements", &en);
      if (en == 0) return TY_POLY;
    }
  }
  return t;
}

/* ---- "this poly slot can hold a builtin container" ----
   TY_POLY is a top type with no member list, so a call on a poly receiver
   cannot tell a union that really includes an Array/Hash from a user object
   the fixpoint has not pinned down yet. The dispatch needs the difference: a
   user class owning a container method name must not strip the builtin answer
   (#3459), and widening every such call instead poisons classes whose poly
   slots never hold a container. These two passes carry one bit -- "a builtin
   Array or Hash is among the values that flow here" -- along the same edges
   the types travel, and only ever turn it on. */

static int flows_container(Compiler *c, int node, int depth);

/* The tail value of a statements list, which is what a body evaluates to. */
static int stmts_tail(const NodeTable *nt, int stmts) {
  if (stmts < 0) return -1;
  int n = 0;
  const int *b = nt_arr(nt, stmts, "body", &n);
  return (b && n > 0) ? b[n - 1] : -1;
}

/* Resolve a call to the user scope it lands in, or -1. */
static int call_target_scope(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm) return -1;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return comp_self_call_mi(c, id, nm);
  const char *rty = nt_type(nt, recv);
  if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
    int ci = comp_class_index(c, nt_str(nt, recv, "name"));
    return ci >= 0 ? comp_cmethod_in_chain(c, ci, nm, NULL) : -1;
  }
  TyKind rt = infer_type(c, recv);
  if (ty_is_object(rt)) return comp_method_in_chain(c, ty_object_class(rt), nm, NULL);
  return -1;
}

/* Would an element of kind t be a foreign value in an array of `elem`? A boxed
   value is decided at run time and exempt, as it is for a single push. */
static int elem_is_foreign(TyKind t, TyKind elem) {
  return t != TY_UNKNOWN && t != TY_POLY && t != TY_POLY_ARRAY && t != elem;
}

/* Does argument node store an element foreign to an array of `elem`? A
   splatted array stores its elements. One splatted into a rest parameter is
   boxed to match it, so an array literal, or a local every write of which is
   one, is read by its elements. */
static int literal_elems_foreign(Compiler *c, int lit, TyKind elem) {
  int en = 0;
  const int *els = nt_arr(c->nt, lit, "elements", &en);
  for (int e = 0; e < en; e++)
    if (nt_kind(c->nt, els[e]) != NK_SplatNode && elem_is_foreign(push_elem_ty(c, els[e]), elem)) return 1;
  return 0;
}
static int push_arg_foreign(Compiler *c, int node, TyKind elem) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, node) != NK_SplatNode) return elem_is_foreign(push_elem_ty(c, node), elem);
  int ex = nt_ref(nt, node, "expression");
  if (ex < 0) return 0;
  if (nt_kind(nt, ex) == NK_ArrayNode) return literal_elems_foreign(c, ex, elem);
  if (nt_kind(nt, ex) == NK_LocalVariableReadNode) {
    const char *an = nt_str(nt, ex, "name");
    Scope *asc = an ? comp_scope_of(c, ex) : NULL;
    LocalVar *al = asc ? scope_local(asc, an) : NULL;
    if (al && !al->is_param && !al->is_block_param) {
      int si = (int)(asc - c->scopes), saw = 0, foreign = 0;
      for (int r = lw_shared_first(c, an, si); r >= 0; r = lw_shared_next(r)) {
        int w = lw_shared_node(r);
        if (comp_scope_of(c, w) != asc || !sp_streq(nt_str(nt, w, "name"), an)) continue;
        int v = nt_ref(nt, w, "value");
        if (nt_kind(nt, w) != NK_LocalVariableWriteNode || v < 0 || nt_kind(nt, v) != NK_ArrayNode) {
          saw = 0; break;
        }
        saw = 1;
        foreign |= literal_elems_foreign(c, v, elem);
      }
      if (saw) return foreign;
    }
  }
  TyKind at = infer_type(c, ex);
  return ty_is_array(at) && elem_is_foreign(ty_array_elem(at), elem);
}

/* Is argument a of scope sc its rest parameter splatted (answers 0) or one
   element of it, `rest[k]` (answers k)? -1 for anything else. */
static int rest_elem_arg(Compiler *c, Scope *sc, int a) {
  const NodeTable *nt = c->nt;
  const char *rest = sc->rest_idx >= 0 ? sc->pnames[sc->rest_idx] : NULL;
  if (!rest) return -1;
  int ex = -1, k = 0;
  if (nt_kind(nt, a) == NK_SplatNode) ex = nt_ref(nt, a, "expression");
  else if (nt_kind(nt, a) == NK_CallNode && sp_streq(nt_str(nt, a, "name"), "[]")) {
    int ia = nt_ref(nt, a, "arguments");
    int in = 0;
    const int *iv = ia >= 0 ? nt_arr(nt, ia, "arguments", &in) : NULL;
    if (in != 1 || nt_kind(nt, iv[0]) != NK_IntegerNode) return -1;
    k = (int)nt_int(nt, iv[0], "value", -1);
    if (k < 0) return -1;
    ex = nt_ref(nt, a, "receiver");
  }
  if (ex < 0 || nt_kind(nt, ex) != NK_LocalVariableReadNode ||
      nt_int(nt, ex, "depth", 0) != 0 || !sp_streq(nt_str(nt, ex, "name"), rest)) return -1;
  return k;
}

/* The first element of sc's rest parameter the value arguments store, or -1. */
static int rest_push_first(Compiler *c, Scope *sc, const int *argv, int an,
                           int from, int splat_index) {
  int first = -1;
  for (int ai = from; ai < an; ai++) {
    int k = rest_elem_arg(c, sc, argv[ai]);
    if (k < 0) continue;
    if (nt_kind(c->nt, argv[ai]) == NK_SplatNode && splat_index && ai == 0) k = 1;
    if (first < 0 || k < first) first = k;
  }
  return first;
}

/* Do the values a push, unshift or insert through a typed array parameter
   stores include one its elements cannot hold? Each argument is its own
   evidence: unified, a Symbol and a String read as one boxed value and were
   exempt. The rest parameter's elements are the binding's to check
   (param_rest_misfits). */
static int param_push_args_foreign(Compiler *c, Scope *sc, const int *argv, int an,
                                   int from, int splat_index, TyKind elem) {
  if (splat_index) return 0;
  for (int ai = from; ai < an; ai++)
    if (rest_elem_arg(c, sc, argv[ai]) < 0 && push_arg_foreign(c, argv[ai], elem)) return 1;
  return 0;
}

/* 1 when a call passing `argv` to method m has parameter `p` store into an
   array of type `ct` an element of m's rest parameter that `ct` cannot hold
   (p's store_rest_src): the rest collects its elements boxed, but this call's
   arguments for it have types. A splat before the rest's position leaves the
   positions unknown, and every argument from it on is read. */
int param_rest_misfits(Compiler *c, Scope *m, LocalVar *p, TyKind ct, const int *argv, int an) {
  const NodeTable *nt = c->nt;
  if (!p || !argv || p->store_rest_src <= 0 || m->rest_idx < 0) return 0;
  if (!ty_is_array(ct) || ct == TY_POLY_ARRAY) return 0;
  if (an > 0 && nt_kind(nt, argv[an - 1]) == NK_KeywordHashNode &&
      (callee_declares_kwargs(c, m) || m->kwrest_idx >= 0)) an--;
  int at = m->rest_idx + p->store_rest_src - 1;
  int from = at, to = an - m->npost_rest;
  for (int k = 0; k < an && k < at; k++)
    if (nt_kind(nt, argv[k]) == NK_SplatNode) { from = k; to = an; break; }
  for (int k = from; k < to; k++)
    if (push_arg_foreign(c, argv[k], ty_array_elem(ct))) return 1;
  return 0;
}

static int flows_container(Compiler *c, int node, int depth) {
  if (node < 0 || depth > 6) return 0;
  const NodeTable *nt = c->nt;
  TyKind t = infer_type(c, node);
  /* At depth 0 the caller is asking about a POLY receiver, so "this expression
     is itself a container" is not evidence -- it would mean there is no poly
     dispatch to widen. Only the slot evidence counts there. Deeper down we are
     walking the values that flow into such a slot, where a container-typed
     producer is exactly the evidence wanted. A transient array typing of the
     receiver otherwise answered yes for a slot that never holds one, which is
     how Set's `orig.to_a` came to widen (#3459). */
  if (depth > 0 && (ty_is_array(t) || ty_is_hash(t))) return 1;
  /* a settled non-container concrete type carries no container */
  if (t != TY_POLY && t != TY_UNKNOWN && t != TY_NIL) return 0;
  const char *ty = nt_type(nt, node);
  if (!ty) return 0;
  if (sp_streq(ty, "ParenthesesNode")) return flows_container(c, stmts_tail(nt, nt_ref(nt, node, "body")), depth + 1);
  if (sp_streq(ty, "StatementsNode")) return flows_container(c, stmts_tail(nt, node), depth + 1);
  if (sp_streq(ty, "ReturnNode")) {
    int a = nt_ref(nt, node, "arguments");
    int n = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &n) : NULL;
    return (av && n > 0) ? flows_container(c, av[0], depth + 1) : 0;
  }
  if (sp_streq(ty, "IfNode") || sp_streq(ty, "UnlessNode")) {
    if (flows_container(c, stmts_tail(nt, nt_ref(nt, node, "statements")), depth + 1)) return 1;
    int sub = nt_ref(nt, node, "subsequent");
    if (sub < 0) sub = nt_ref(nt, node, "else_clause");
    if (sub < 0) return 0;
    const char *sty = nt_type(nt, sub);
    if (sty && (sp_streq(sty, "ElseNode") || sp_streq(sty, "IfNode") || sp_streq(sty, "UnlessNode")))
      return flows_container(c, sty && sp_streq(sty, "ElseNode")
                                ? stmts_tail(nt, nt_ref(nt, sub, "statements")) : sub, depth + 1);
    return flows_container(c, sub, depth + 1);
  }
  if (sp_streq(ty, "AndNode") || sp_streq(ty, "OrNode"))
    return flows_container(c, nt_ref(nt, node, "left"), depth + 1) ||
           flows_container(c, nt_ref(nt, node, "right"), depth + 1);
  if (sp_streq(ty, "CaseNode")) {
    int n = 0; const int *ws = nt_arr(nt, node, "conditions", &n);
    for (int i = 0; ws && i < n; i++)
      if (flows_container(c, stmts_tail(nt, nt_ref(nt, ws[i], "statements")), depth + 1)) return 1;
    int el = nt_ref(nt, node, "else_clause");
    return el >= 0 && flows_container(c, stmts_tail(nt, nt_ref(nt, el, "statements")), depth + 1);
  }
  if (sp_streq(ty, "LocalVariableReadNode")) {
    const char *nm = nt_str(nt, node, "name");
    LocalVar *lv = nm ? scope_local(comp_scope_of(c, node), nm) : NULL;
    return lv ? lv->poly_ctr : 0;
  }
  if (sp_streq(ty, "CallNode")) {
    int mi = call_target_scope(c, node);
    return mi >= 0 ? c->scopes[mi].ret_poly_ctr : 0;
  }
  return 0;
}

int poly_expr_flows_container(Compiler *c, int node) { return flows_container(c, node, 0); }

int infer_container_flow(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (sp_streq(ty, "LocalVariableWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      LocalVar *lv = nm ? scope_local(comp_scope_of(c, id), nm) : NULL;
      if (!lv || lv->poly_ctr) continue;
      if (flows_container(c, nt_ref(nt, id, "value"), 1)) { lv->poly_ctr = 1; changed = 1; }
    }
  }
  /* explicit returns, in ONE node pass: the per-scope rescan this replaces is
     O(scopes * nodes), which dominates on a large input */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "ReturnNode")) continue;
    int s = c->nscope[id];
    if (s < 0 || s >= c->nscopes || c->scopes[s].ret_poly_ctr) continue;
    if (flows_container(c, id, 1)) { c->scopes[s].ret_poly_ctr = 1; changed = 1; }
  }
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (sc->ret_poly_ctr) continue;
    if (flows_container(c, stmts_tail(nt, sc->body), 1)) { sc->ret_poly_ctr = 1; changed = 1; }
  }
  return changed;
}

/* Does the subtree assign a local anywhere? Gates the recompute pass below:
   only a block whose body introduces locals can have left an enclosing write's
   RHS type derived from still-reset slots. */
static int subtree_writes_local(const NodeTable *nt, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (ty && (sp_streq(ty, "LocalVariableWriteNode") ||
             sp_streq(ty, "LocalVariableOperatorWriteNode") ||
             sp_streq(ty, "LocalVariableOrWriteNode") ||
             sp_streq(ty, "LocalVariableAndWriteNode"))) return 1;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_writes_local(nt, nt_ref_at(nt, id, i))) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++)
      if (subtree_writes_local(nt, ids[k])) return 1;
  }
  return 0;
}

/* Widen every ARRAY ivar read under `node` (a local's write value: a bare
   @read, or a conditional whose arms read ivars) to the poly array. A local
   that aliases ivar arrays and then takes a foreign element widens itself,
   and its reads become a REBUILT copy of the source -- the push lands on
   the copy and the receiver's own array answers unchanged (#4210). Widening
   the sources keeps the local's read a plain pointer. Returns 1 on change. */
static int widen_aliased_array_ivars(Compiler *c, int node, int cls_id) {
  const NodeTable *nt = c->nt;
  if (node < 0 || cls_id < 0 || cls_id >= c->nclasses) return 0;
  NodeKind k = nt_kind(nt, node);
  int changed = 0;
  if (k == NK_InstanceVariableReadNode) {
    const char *nm = nt_str(nt, node, "name");
    ClassInfo *ci = &c->classes[cls_id];
    int iv = nm ? comp_ivar_index(ci, nm) : -1;
    if (iv < 0 || class_ivar_pinned(ci, nm) || ci->ivar_int_table[iv]) return 0;
    if (ty_is_array(ci->ivar_types[iv]) && ci->ivar_types[iv] != TY_POLY_ARRAY) {
      sp_ivwatch(nm, "aliased_local_push", ci->ivar_types[iv], TY_POLY_ARRAY);
      ci->ivar_types[iv] = TY_POLY_ARRAY;
      changed = 1;
    }
    return changed;
  }
  switch (k) {
  case NK_IfNode: case NK_UnlessNode:
    changed |= widen_aliased_array_ivars(c, nt_ref(nt, node, "statements"), cls_id);
    changed |= widen_aliased_array_ivars(c, nt_ref(nt, node,
                 k == NK_UnlessNode ? "else_clause" : "subsequent"), cls_id);
    break;
  case NK_ElseNode:
    changed |= widen_aliased_array_ivars(c, nt_ref(nt, node, "statements"), cls_id); break;
  case NK_ParenthesesNode:
    changed |= widen_aliased_array_ivars(c, nt_ref(nt, node, "body"), cls_id); break;
  case NK_StatementsNode: {
    int bn = 0; const int *bb = nt_arr(nt, node, "body", &bn);
    if (bb && bn > 0) changed |= widen_aliased_array_ivars(c, bb[bn - 1], cls_id);
    break;
  }
  default: break;
  }
  return changed;
}

/* The type slot a global, class-variable or constant read names, or NULL. */
static TyKind *named_array_slot(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, recv, "name");
  if (!nm) return NULL;
  switch (nt_kind(nt, recv)) {
    case NK_GlobalVariableReadNode: {
      const char *rn = comp_resolve_gvar(c, nm + 1);
      LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
      return lv ? &lv->type : NULL;
    }
    case NK_ConstantReadNode: {
      LocalVar *lv = comp_const(c, nm);
      return lv ? &lv->type : NULL;
    }
    case NK_ClassVariableReadNode: {
      Scope *s = comp_scope_of(c, recv);
      int cid = s ? s->class_id : -1;
      if (cid < 0) cid = comp_class_index(c, "Toplevel");
      if (cid < 0) return NULL;
      cid = comp_cvar_owner(c, cid, nm);
      int idx = comp_cvar_index(&c->classes[cid], nm);
      return idx >= 0 ? &c->classes[cid].cvar_types[idx] : NULL;
    }
    default: return NULL;
  }
}

/* `m[i]` over a method returning a fixed tuple (`def pair = [1, "x"]`), at an
   integer-literal position inside it: that element's own type, else
   TY_UNKNOWN. */
TyKind tuple_elem_read_type(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0 || nt_kind(nt, node) != NK_CallNode) return TY_UNKNOWN;
  const char *nm = nt_str(nt, node, "name");
  if (!nm || !sp_streq(nm, "[]") || nt_ref(nt, node, "block") >= 0) return TY_UNKNOWN;
  int args = nt_ref(nt, node, "arguments");
  int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != 1 || nt_kind(nt, av[0]) != NK_IntegerNode || nt_str(nt, av[0], "bigval")) return TY_UNKNOWN;
  int recv = nt_ref(nt, node, "receiver");
  if (recv < 0 || nt_kind(nt, recv) != NK_CallNode) return TY_UNKNOWN;
  TyKind elems[16];
  int en = multi_return_elem_types(c, recv, elems, 16);
  long long pos = nt_int(nt, av[0], "value", 0);
  if (pos < 0) pos += en;
  return (pos >= 0 && pos < en) ? elems[pos] : TY_UNKNOWN;
}

/* The type a `m[i]` read of a boxed tuple is narrowed to and unboxed as: a
   scalar or an object, never a container a converting unbox would copy away
   from the tuple. Else TY_UNKNOWN. */
TyKind tuple_elem_read_unboxed(Compiler *c, int node) {
  TyKind et = tuple_elem_read_type(c, node);
  if (et == TY_INT || et == TY_FLOAT || et == TY_STRING || et == TY_SYMBOL ||
      et == TY_BOOL || ty_is_object(et)) return et;
  return TY_UNKNOWN;
}

/* The type `node` stores when it is `m[i]` over a method returning a fixed
   tuple and reads as the boxed element: that element's own type, which is
   what the store puts in the container. Else `vt`. */
static TyKind tuple_elem_evidence(Compiler *c, int node, TyKind vt) {
  if (vt != TY_POLY) return vt;
  TyKind et = tuple_elem_read_type(c, node);
  return et != TY_UNKNOWN ? et : vt;
}

/* Folds one piece of container evidence into `slot` (see the usage fold in
   infer_write_types). Returns 0 where the evidence does not apply to the
   slot, leaving it untouched. */
static int fold_container_evidence(TyKind *slot, int is_push, int is_splice,
                                   TyKind kt, TyKind vt) {
  if (is_push) {
    /* explicit push/append: definitely array.  A PolyArray stays PolyArray
       regardless of the pushed value type; mixing typed arrays widens to
       PolyArray (ty_unify would return TY_POLY scalar, so use array-aware
       widening instead). */
    if (vt == TY_UNKNOWN) return 0;
    /* If a [] read already promoted this slot to a hash type, the push
       wins: a variable that is pushed to is an array, not a hash.
       Reset the slot so the array promotion below can fire. */
    if (ty_is_hash(*slot)) *slot = TY_UNKNOWN;
    if (*slot != TY_UNKNOWN && !ty_is_array(*slot)) return 0;
    if (*slot == TY_POLY_ARRAY) return 0;  /* already widest array type */
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
  else if ((kt == TY_INT || kt == TY_POLY) && *slot != TY_UNKNOWN && ty_is_array(*slot)) {
    /* int-key element write into a typed array: a value its element type
       CONCRETELY cannot hold widens the slot to a poly array, mirroring
       `a << x` -- the poly emitters then store the value exactly as CRuby
       does (the former bail left e.g. `a[0] = "s"` on an int array to emit
       invalid C through the typed setter). A TY_POLY value is exempt: the
       typed setter's runtime conversion (sp_poly_to_i etc.) is the
       long-standing intended path for it. A poly KEY into an array slot is
       an index all the same -- Array#[]= takes an Integer or raises -- so it
       is the same evidence; without it an object stored at an unpacked
       index was handed to the int setter (#4832). */
    if (vt != TY_UNKNOWN && vt != TY_POLY &&
        *slot != TY_POLY_ARRAY && vt != ty_array_elem(*slot))
      *slot = TY_POLY_ARRAY;
  }
  else if (kt == TY_INT) {
    /* int key []= on a non-array slot: infer an int-keyed hash */
    if (vt == TY_UNKNOWN) return 0;
    if (*slot != TY_UNKNOWN && !ty_is_hash(*slot)) return 0;
    TyKind hv = ty_hash_of(TY_INT, vt);
    if (hv == TY_UNKNOWN) hv = TY_POLY_POLY_HASH;  /* int key + unknown val type */
    if (*slot != TY_UNKNOWN && *slot != hv) {
      /* widen to poly-poly if mismatch */
      if (ty_is_hash(*slot)) { *slot = TY_POLY_POLY_HASH; }
      return 0;
    }
    *slot = hv;
  }
  else if (kt == TY_STRING) {
    if (vt == TY_UNKNOWN) return 0;
    TyKind hv = ty_hash_of(TY_STRING, vt);
    if (hv == TY_UNKNOWN) hv = TY_STR_POLY_HASH;  /* mixed values */
    if (*slot != TY_UNKNOWN && !ty_is_hash(*slot)) return 0;
    /* a str-keyed hash that has seen >1 value type widens to StrPoly */
    if (*slot != TY_UNKNOWN && *slot != hv &&
        (*slot == TY_STR_INT_HASH || *slot == TY_STR_STR_HASH || *slot == TY_STR_POLY_HASH))
      hv = TY_STR_POLY_HASH;
    *slot = hv;
  }
  else if (kt == TY_SYMBOL) {
    /* symbol key -> SymPolyHash (boxed values) */
    if (vt == TY_UNKNOWN) return 0;
    if (*slot != TY_UNKNOWN && *slot != TY_SYM_POLY_HASH) return 0;
    *slot = TY_SYM_POLY_HASH;
  }
  else if (kt != TY_UNKNOWN) {
    /* non-standard key type (array, object, etc.): heterogeneous hash */
    if (vt == TY_UNKNOWN) return 0;
    if (*slot != TY_UNKNOWN && !ty_is_hash(*slot)) return 0;
    *slot = TY_POLY_POLY_HASH;
  }
  return 1;
}

/* The ivar slot `inm` of class `ivar_cls_id` that container evidence folds
   into, or NULL where the ivar takes none: the guards of the usage fold in
   infer_write_types for a receiver that is the ivar. May widen *vt. */
static TyKind *ivar_evidence_slot(Compiler *c, const LWIndex *ivw, int ivar_cls_id,
                                  const char *inm, int is_push, TyKind *vt) {
  const NodeTable *nt = c->nt;
  ClassInfo *ci = &c->classes[ivar_cls_id];
  int iv = inm ? comp_ivar_index(ci, inm) : -1;
  if (iv < 0) return NULL;
  if (class_ivar_pinned(ci, inm)) return NULL;  /* --rbs seed pins are authoritative */
  /* A narrowed int table is pinned: its own write still reads
     TY_POLY_ARRAY, and re-deriving from that would unify two array kinds
     into the plain poly scalar -- strictly worse than what it replaced. */
  if (ci->ivar_int_table[iv]) return NULL;
  /* An UNKNOWN slot here is a fixpoint ORDERING gap, not absent evidence:
     a push whose value type is already settled (a literal) runs before
     the ivar's own writes have merged, and seeding the slot with the
     pushed element's array kind made the later merge unify two typed
     kinds into the scalar poly box, for good (#4210) -- while a push on
     a NON-array attribute (`@q = Queue.new; @q.push(x)`) seeded an array
     the merge then destroyed the Queue with (#4211). Consult the ivar's
     own direct writes first: a settled non-array write means this push
     is no array evidence at all, and an array write of another element
     kind means the slot is the poly array from the start. */
  if (is_push && ci->ivar_types[iv] == TY_UNKNOWN && inm) {
    int nonarray_write = 0, other_kind_write = 0;
    TyKind want0 = ty_array_of(*vt);
    for (int _r = ivw_index_first(ivw, inm); _r >= 0; _r = ivw->next[_r]) {
      int _wi = ivw->node[_r];
      if (nt_kind(nt, _wi) != NK_InstanceVariableWriteNode) continue;
      const char *_wnm = nt_str(nt, _wi, "name");
      if (!_wnm || !sp_streq(_wnm, inm)) continue;
      Scope *_ws = comp_scope_of(c, _wi);
      int _wcls = _ws ? _ws->class_id : -1;
      if (_wcls < 0) _wcls = comp_class_index(c, "Toplevel");
      if (_wcls != ivar_cls_id) continue;
      int _wv = nt_ref(nt, _wi, "value");
      if (_wv < 0) continue;
      TyKind _wt = infer_type(c, _wv);
      if (_wt == TY_UNKNOWN || _wt == TY_NIL) continue;
      if (!ty_is_array(_wt)) { nonarray_write = 1; break; }
      if (_wt != want0 && _wt != TY_POLY_ARRAY) other_kind_write = 1;
    }
    if (nonarray_write) return NULL;
    if (other_kind_write) *vt = TY_POLY;   /* ty_array_of => the poly array */
  }
  TyKind *slot = &ci->ivar_types[iv];
  /* If the slot is TY_UNKNOWN but has a direct InstanceVariableWriteNode
     that assigns a typed value OR an empty array/hash literal (e.g.
     @buf = [nil]*7 or @free = []), skip usage-driven hash promotion
     (but allow push-driven array promotion through). Without this guard,
     @free[0] read promotes @free to poly_poly_hash before @free = []
     has been processed as an array. */
  /* A typed (non-nil) construction write -- `@a = [x]*n`, `@a = arr.map{}`,
     or an `@a = []` literal -- means this ivar is an array filled by index,
     not a hash. Skip usage-driven hash promotion for both plain reads and
     `@a[k]=v` index-writes. A genuine hash (`@h = {}`) infers UNKNOWN from
     its empty literal and is unaffected. */
  if (!is_push && *slot == TY_UNKNOWN && inm) {
    int has_typed_write = 0;
    for (int _r = ivw_index_first(ivw, inm); _r >= 0 && !has_typed_write; _r = ivw->next[_r]) {
      int _wi = ivw->node[_r];
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
         block promotion -- the hash type is determined by key/value usage. */
      const char *_wvty = nt_type(nt, _wval);
      if (_wvty && sp_streq(_wvty, "ArrayNode"))
        has_typed_write = 1;
    }
    if (has_typed_write) return NULL;
  }
  /* `@s << x` on an ivar with a STRING write anywhere is a string
     append, not an array push: without this the push promoted the
     still-UNKNOWN slot to str_array before the write merge saw the
     string, and the union settled poly (#3227 P4). */
  if (is_push && (*slot == TY_UNKNOWN || *slot == TY_STRING ||
                  *slot == TY_STRBUF) && inm) {
    int has_string_write = 0;
    for (int _r = ivw_index_first(ivw, inm); _r >= 0 && !has_string_write; _r = ivw->next[_r]) {
      int _wi = ivw->node[_r];
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
    if (has_string_write) return NULL;
  }
  return slot;
}

/* The ivar a getter's exit expression hands back, or NULL: a read of it,
   `@x ||= v`, `@x = v`, or one of those parenthesized or returned. */
static const char *exit_ivar_name(const NodeTable *nt, int e) {
  for (int depth = 0; e >= 0 && depth < 8; depth++) {
    switch (nt_kind(nt, e)) {
    case NK_InstanceVariableReadNode:
    case NK_InstanceVariableOrWriteNode:
    case NK_InstanceVariableWriteNode:
      return nt_str(nt, e, "name");
    case NK_ParenthesesNode:
      e = stmts_tail(nt, nt_ref(nt, e, "body"));
      break;
    case NK_StatementsNode:
      e = stmts_tail(nt, e);
      break;
    case NK_ReturnNode: {
      int a = nt_ref(nt, e, "arguments");
      int n = 0;
      const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &n) : NULL;
      if (!av || n != 1) return NULL;
      e = av[0];
      break;
    }
    default:
      return NULL;
    }
  }
  return NULL;
}

/* Every `return` under `node` (not crossing a nested def, class or lambda)
   hands back an ivar: adds each name to names[]. 0 when one does not. */
static int returns_are_ivars(const NodeTable *nt, int node, const char **names, int *n, int max) {
  if (node < 0) return 1;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode ||
      k == NK_SingletonClassNode || k == NK_LambdaNode) return 1;
  if (k == NK_ReturnNode) {
    const char *nm = exit_ivar_name(nt, node);
    if (!nm) return 0;
    for (int i = 0; i < *n; i++) if (sp_streq(names[i], nm)) return 1;
    if (*n >= max) return 0;
    names[(*n)++] = nm;
    return 1;
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++)
    if (!returns_are_ivars(nt, nt_ref_at(nt, node, i), names, n, max)) return 0;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int cnt = 0;
    const int *ids = nt_arr_at(nt, node, i, &cnt);
    for (int j = 0; j < cnt; j++)
      if (!returns_are_ivars(nt, ids[j], names, n, max)) return 0;
  }
  return 1;
}

/* The ivars a zero-argument getter can return: its tail and every `return`
   must be one. Returns the count, 0 when some exit is anything else. */
static int getter_exit_ivars(Compiler *c, int mi, const char **names, int max) {
  const NodeTable *nt = c->nt;
  int body = c->scopes[mi].body;
  const char *tail = exit_ivar_name(nt, stmts_tail(nt, body));
  if (!tail || max < 1) return 0;
  int n = 0;
  names[n++] = tail;
  if (!returns_are_ivars(nt, body, names, &n, max)) return 0;
  return n;
}

/* `super` or `super()` with no block: in a zero-argument method, the
   ancestor's same-named method called with the same (no) arguments. */
static int is_bare_super(Compiler *c, int e) {
  const NodeTable *nt = c->nt;
  if (e < 0) return 0;
  NodeKind k = nt_kind(nt, e);
  if (k == NK_ForwardingSuperNode) return nt_ref(nt, e, "block") < 0;
  if (k != NK_SuperNode || nt_ref(nt, e, "block") >= 0) return 0;
  int a = nt_ref(nt, e, "arguments");
  int n = 0;
  if (a >= 0) nt_arr(nt, a, "arguments", &n);
  return n == 0;
}

/* The (class, ivar) pairs an index write or push through the zero-argument
   call `mname` can reach: for `cid` and each descendant, the method the call
   dispatches to there (class side when `cside`), taken when it is an
   attr_reader or a getter whose every exit is an ivar. The ivar is the
   defining class's, as a direct `@x` in that method reads it. Returns the
   count, or -1 when there are more than `max`. */
static int getter_ivar_targets(Compiler *c, int cid, int cside, const char *mname,
                               int *tcls, const char **tiv, int max) {
  int n = 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (k != cid && !is_descendant(c, k, cid)) continue;
    const char *ivs[8];
    int niv = 0, defcls = -1, supered = 0;
    char rbuf[300];
    /* `def cache = super` hands back whatever the ancestor's getter does:
       follow it up the chain to the method or reader that names the ivar */
    for (int from = k, hop = 0; from >= 0 && hop < 8; hop++) {
      int mi = -1;
      defcls = -1;
      if (cside) {
        mi = comp_cmethod_in_chain(c, from, mname, &defcls);
        if (mi < 0) break;
        niv = getter_exit_ivars(c, mi, ivs, 8);
      }
      else {
        int mdef = -1, rdef = -1;
        mi = comp_method_in_chain(c, from, mname, &mdef);
        int rd = comp_reader_in_chain(c, from, mname, &rdef);
        /* a class carries its parent's readers as its own, so a method there
           over an inherited reader is the override, not a tie */
        int pdef = mdef >= 0 ? c->classes[mdef].parent : -1;
        int over = mi >= 0 && rd && mdef == rdef && pdef >= 0 &&
                   comp_reader_in_chain(c, pdef, mname, NULL);
        if (mi >= 0 && (!rd || over || (mdef != rdef && is_descendant(c, mdef, rdef)))) {
          defcls = mdef;
          niv = getter_exit_ivars(c, mi, ivs, 8);
        }
        else if (rd && (mi < 0 || (mdef != rdef && is_descendant(c, rdef, mdef)))) {
          snprintf(rbuf, sizeof rbuf, "@%s", comp_resolve_alias(c, from, mname));
          defcls = rdef;
          ivs[0] = rbuf;
          niv = 1;
          break;
        }
        else mi = -1;
      }
      if (mi < 0 || niv > 0 || defcls < 0) break;
      if (!is_bare_super(c, scope_body_last(c, mi))) break;
      from = c->classes[defcls].parent;
      defcls = -1;
      supered = 1;
    }
    if (defcls < 0) continue;
    /* Every class keeps its own copy of an inherited ivar's type, and the
       ancestors' methods run on this object too: credit the copies from the
       defining class up (from `k`, whose object it is, through a `super`
       getter). Crediting the defining class alone left a base class that
       writes the ivar with its narrower copy, which the up-merge after the
       fixpoint unified with the widened one into a plain box. */
    for (int cc = supered ? k : defcls; cc >= 0; cc = c->classes[cc].parent) {
      ClassInfo *dci = &c->classes[cc];
      for (int i = 0; i < niv; i++) {
        int ivx = comp_ivar_index(dci, ivs[i]);
        if (ivx < 0) continue;
        int seen = 0;
        for (int j = 0; j < n; j++)
          if (tcls[j] == cc && sp_streq(tiv[j], ivs[i])) seen = 1;
        if (seen) continue;
        if (n >= max) return -1;
        tcls[n] = cc;
        tiv[n] = dci->ivars[ivx];
        n++;
      }
    }
  }
  return n;
}

/* Does class `k` itself define the instance getter `mname`, as a method or
   an attr_reader (not one it inherits)? */
static int class_owns_getter(Compiler *c, int k, const char *mname) {
  int kd = -1;
  if (comp_reader_in_chain(c, k, mname, &kd)) return kd == k;
  return comp_method_in_class(c, k, mname) >= 0;
}

/* The getter call or ivar read a hash local aliases: `x` read at `recv`,
   whose only write in its scope is `x = @c` or `x = recv.getter`, where the
   getter answers an ivar (getter_ivar_targets). -1 otherwise. */
static int local_hash_alias_source(Compiler *c, const LWIndex *lw, int recv) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, recv, "name");
  Scope *sc = nm ? comp_scope_of(c, recv) : NULL;
  LocalVar *lv = sc ? scope_local(sc, nm) : NULL;
  if (!lv || lv->is_param || lv->is_block_param || !ty_is_hash(lv->type)) return -1;
  int sid = (int)(sc - c->scopes), src = -1, nw = 0;
  for (int r = lw_index_first(lw, nm, sid); r >= 0; r = lw->next[r]) {
    int w = lw->node[r];
    const char *wn = nt_str(nt, w, "name");
    if (!wn || !sp_streq(wn, nm) || comp_scope_of(c, w) != sc) continue;
    if (nt_kind(nt, w) != NK_LocalVariableWriteNode || ++nw > 1) return -1;
    src = unwrap_parens(c, nt_ref(nt, w, "value"));
  }
  if (src < 0) return -1;
  if (nt_kind(nt, src) == NK_InstanceVariableReadNode) return src;
  if (nt_kind(nt, src) != NK_CallNode || nt_ref(nt, src, "block") >= 0 ||
      nt_ref(nt, src, "arguments") >= 0) return -1;
  const char *mname = nt_str(nt, src, "name");
  if (!mname) return -1;
  int gr = nt_ref(nt, src, "receiver"), gcid = -1, cside = 0;
  if (gr >= 0 && nt_kind(nt, gr) == NK_ConstantReadNode) {
    const char *cnm = nt_str(nt, gr, "name");
    gcid = cnm ? comp_class_index(c, cnm) : -1;
    cside = 1;
  }
  else if (gr >= 0 && nt_kind(nt, gr) != NK_SelfNode) {
    TyKind grt = infer_type(c, gr);
    gcid = ty_is_object(grt) ? ty_object_class(grt) : -1;
  }
  else {
    Scope *cs = comp_scope_of(c, src);
    gcid = cs ? cs->class_id : -1;
    cside = cs ? cs->is_cmethod : 0;
  }
  if (gcid < 0 || gcid >= c->nclasses) return -1;
  int tcls[16]; const char *tiv[16];
  return getter_ivar_targets(c, gcid, cside, mname, tcls, tiv, 16) > 0 ? src : -1;
}

/* Does class `cls` write `inm` with an array (literal or typed)? */
static int ivar_has_array_write(Compiler *c, const LWIndex *ivw, int cls, const char *inm) {
  const NodeTable *nt = c->nt;
  for (int r = ivw_index_first(ivw, inm); r >= 0; r = ivw->next[r]) {
    int wi = ivw->node[r];
    const char *wnm = nt_str(nt, wi, "name");
    if (!wnm || !sp_streq(wnm, inm)) continue;
    Scope *ws = comp_scope_of(c, wi);
    if (!ws || ws->class_id != cls) continue;
    int wv = nt_ref(nt, wi, "value");
    if (wv >= 0 && (nt_kind(nt, wv) == NK_ArrayNode || ty_is_array(infer_type(c, wv)))) return 1;
  }
  return 0;
}

static int widen_arg_hash(Compiler *c, int arg);
static int widen_arg_array(Compiler *c, int arg);
static int widen_boxed_array_sources(Compiler *c, int v, TyKind elem, int depth);
static int widen_proc_call_hash_args(Compiler *c, int lit, const char *pn, TyKind hk, TyKind hv);

/* Widens the parameters class `cls` assigns `inm` (`@c = a`) to the
   general Array, as a push through the parameter itself would, with its
   callers' arrays. */
static int widen_ivar_array_params(Compiler *c, const LWIndex *ivw, int cls, const char *inm) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int r = ivw_index_first(ivw, inm); r >= 0; r = ivw->next[r]) {
    int wi = ivw->node[r];
    const char *wnm = nt_str(nt, wi, "name");
    if (!wnm || !sp_streq(wnm, inm) || nt_kind(nt, wi) != NK_InstanceVariableWriteNode) continue;
    Scope *ws = comp_scope_of(c, wi);
    if (!ws || ws->class_id != cls) continue;
    int wv = nt_ref(nt, wi, "value");
    if (unassigned_param_read(c, ws, wv) >= 0) changed |= widen_arg_array(c, wv);
  }
  return changed;
}

/* 1 when class `cls` assigns `inm` one of its methods' own parameters
   (`@c = h`): the ivar holds the caller's hash. */
static int ivar_has_param_write(Compiler *c, const LWIndex *ivw, int cls, const char *inm) {
  const NodeTable *nt = c->nt;
  for (int r = ivw_index_first(ivw, inm); r >= 0; r = ivw->next[r]) {
    int wi = ivw->node[r];
    const char *wnm = nt_str(nt, wi, "name");
    if (!wnm || !sp_streq(wnm, inm) || nt_kind(nt, wi) != NK_InstanceVariableWriteNode) continue;
    Scope *ws = comp_scope_of(c, wi);
    if (!ws || ws->class_id != cls) continue;
    if (unassigned_param_read(c, ws, nt_ref(nt, wi, "value")) >= 0) return 1;
  }
  return 0;
}

/* Marks every hash literal class `cls` assigns to `inm` (`@c = {}`,
   `@c ||= {}`) the poly-keyed variant. A parameter it is assigned
   (`@c = h`) is its caller's hash, and widens as a store through the
   parameter itself would, with its callers' hashes. */
static void widen_ivar_hash_literals(Compiler *c, const LWIndex *ivw, int cls, const char *inm) {
  const NodeTable *nt = c->nt;
  if (!c->hash_want) return;
  for (int r = ivw_index_first(ivw, inm); r >= 0; r = ivw->next[r]) {
    int wi = ivw->node[r];
    const char *wnm = nt_str(nt, wi, "name");
    if (!wnm || !sp_streq(wnm, inm)) continue;
    Scope *ws = comp_scope_of(c, wi);
    if (!ws || ws->class_id != cls) continue;
    int wv = nt_ref(nt, wi, "value");
    if (wv >= 0 && wv < c->node_cap && nt_kind(nt, wv) == NK_HashNode)
      c->hash_want[wv] = TY_POLY_POLY_HASH;
    else if (unassigned_param_read(c, ws, wv) >= 0)
      widen_arg_hash(c, wv);
  }
}

int a_proc_params_node(Compiler *c, int create);

static int proc_literal_escapes_as_arg(Compiler *c, int lit);

/* The names that run a Proc: `.call`, `.()` (which is `call`), `[]` and
   `.yield`. */
static int proc_call_name(const char *cn) {
  return cn && (sp_streq(cn, "call") || sp_streq(cn, "[]") || sp_streq(cn, "yield"));
}

/* An index of the proc and lambda literals, built once per node table (as
   ple_build is): which literal written in a scope has a parameter of a name,
   and whether each literal's calls are all in sight. The push-evidence pass
   asks it at every push or element write into an unassigned local, every
   round, where a walk of the whole table each time made a program of many
   such literals quadratic. A (scope, name) key holds the first literal with
   that parameter, a local's reads and its reads that call a Proc. */
typedef struct { const Scope *s; const char *nm; int lit, reads, calls; } PplEnt;
static PplEnt *ppl_tab = NULL; static int ppl_cap = 0;
static signed char *ppl_sight = NULL;   /* per literal: 1 when its calls are all in sight */
static const NodeTable *ppl_nt = NULL; static int ppl_ntc = -1;

/* The entry of (s, nm): made when `add`, else NULL when there is none. The
   build adds at most one entry per node, into a table twice that size. */
static PplEnt *ppl_slot_at(const Scope *s, const char *nm, int add) {
  unsigned h = 2166136261u;
  for (const char *q = nm; *q; q++) h = (h ^ (unsigned char)*q) * 16777619u;
  h ^= (unsigned)((uintptr_t)s >> 4);
  int j = (int)(h & (unsigned)(ppl_cap - 1));
  while (ppl_tab[j].nm && !(ppl_tab[j].s == s && sp_streq(ppl_tab[j].nm, nm))) j = (j + 1) & (ppl_cap - 1);
  if (!ppl_tab[j].nm) {
    if (!add) return NULL;
    ppl_tab[j].s = s; ppl_tab[j].nm = nm; ppl_tab[j].lit = -1;
  }
  return &ppl_tab[j];
}
static PplEnt *ppl_slot(const Scope *s, const char *nm) { return ppl_slot_at(s, nm, 1); }

static void ppl_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  int n = nt->count;
  free(ppl_tab); free(ppl_sight);
  ppl_cap = 64;
  while (ppl_cap < 2 * n + 2) ppl_cap *= 2;
  ppl_tab = calloc((size_t)ppl_cap, sizeof *ppl_tab);
  ppl_sight = calloc((size_t)(n > 0 ? n : 1), 1);
  int *recv_call = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
  int *nw = calloc((size_t)(n > 0 ? n : 1), sizeof(int));
  PplEnt **wl = calloc((size_t)(n > 0 ? n : 1), sizeof(PplEnt *));
  if (!ppl_tab || !ppl_sight || !recv_call || !nw || !wl) {
    free(recv_call); free(nw); free(wl); ppl_nt = NULL; return;
  }
  for (int i = 0; i < n; i++) recv_call[i] = -1;
  for (int id = 0; id < n; id++) {
    if (!is_proc_create(c, id)) continue;
    int pn = a_proc_params_node(c, id);
    if (pn >= 0 && nt_kind(nt, pn) == NK_BlockParametersNode) pn = nt_ref(nt, pn, "parameters");
    if (pn < 0) continue;
    Scope *sc = comp_scope_of(c, id);
    static const char *const lists[] = { "requireds", "optionals", "posts" };
    for (int l = 0; l < 3; l++) {
      int pc = 0; const int *ps = nt_arr(nt, pn, lists[l], &pc);
      for (int k = 0; k < pc; k++) {
        const char *p = nt_str(nt, ps[k], "name");
        if (!p) continue;
        PplEnt *e = ppl_slot(sc, p);
        if (e->lit < 0) e->lit = id;
      }
    }
  }
  NT_FOREACH_KIND(nt, NK_LocalVariableReadNode, r) {
    const char *rn = nt_str(nt, r, "name");
    if (rn) ppl_slot(comp_scope_of(c, r), rn)->reads++;
  }
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    int r = nt_ref(nt, id, "receiver");
    if (r < 0 || r >= n) continue;
    const char *cn = nt_str(nt, id, "name");
    if (recv_call[r] < 0) recv_call[r] = proc_call_name(cn);
    if (nt_kind(nt, r) != NK_LocalVariableReadNode || !proc_call_name(cn)) continue;
    const char *rn = nt_str(nt, r, "name");
    if (rn) ppl_slot(comp_scope_of(c, r), rn)->calls++;
  }
  NT_FOREACH_KIND(nt, NK_LocalVariableWriteNode, w) {
    int v = nt_ref(nt, w, "value");
    const char *wn = nt_str(nt, w, "name");
    if (v < 0 || v >= n || !wn || !is_proc_create(c, v)) continue;
    nw[v]++;
    wl[v] = ppl_slot(comp_scope_of(c, w), wn);
  }
  /* in sight: the literal is the receiver of its one call and that call runs
     it, or it is the value of one local every read of which runs it */
  for (int id = 0; id < n; id++) {
    if (!is_proc_create(c, id) || proc_literal_escapes_as_arg(c, id)) continue;
    if (recv_call[id] >= 0) ppl_sight[id] = (signed char)recv_call[id];
    else ppl_sight[id] = nw[id] == 1 && wl[id]->reads == wl[id]->calls;
  }
  free(recv_call); free(nw); free(wl);
  ppl_nt = nt; ppl_ntc = n;
}

static int ppl_ready(Compiler *c) {
  if (ppl_nt != c->nt || ppl_ntc != c->nt->count) ppl_build(c);
  return ppl_nt != NULL;
}

/* The proc or lambda literal written in scope `sc` (`proc { |a| }`,
   `lambda { |a| }`, `Proc.new { |a| }`, `->(a) { }`) that has a parameter
   `nm`, held as a local of that scope; -1 if none. */
static int local_proc_literal_param_of(Compiler *c, Scope *sc, const char *nm) {
  PplEnt *e = ppl_ready(c) && nm ? ppl_slot_at(sc, nm, 0) : NULL;
  return e ? e->lit : -1;
}

/* Whether every call of proc literal `lit` is in sight of the binders: the
   literal is the receiver of its one call (`->(a) { }.call(x)`), or the value
   of one local every read of which calls it (`f.call(x)`, `f.(x)`, `f[x]`,
   `f.yield(x)`: proc_call_name). Anything else -- passed on, returned,
   stored, read into another local, curried -- is called where the binders
   cannot look. A call on the literal that does not run it (`.curry`,
   `.itself`) answers a Proc its caller calls out of sight. */
static int proc_literal_calls_in_sight(Compiler *c, int lit) {
  return ppl_ready(c) && lit >= 0 && lit < ppl_ntc && ppl_sight[lit];
}

/* The `@h ||= {}` / `h ||= {}` a container write's receiver evaluates to:
   the receiver itself, parenthesized or not, or the tail of a zero-argument
   self-getter (`def tbl = (@h ||= {})`; `tbl[k] = v`). -1 otherwise. */
/* Whether `nm` is a parameter of a block written in scope `sc` that its
   callee keeps as a Proc rather than yields to: every method of the call's
   name takes it as a named `&blk` and none yields (`Agg.new { |ctx| }` into
   `def initialize(&blk) = @blk = blk`). Whoever calls that Proc later is out
   of sight here. */
static int local_is_kept_block_param(Compiler *c, Scope *sc, const char *nm) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_CallNode, call) {
    int id = nt_ref(nt, call, "block");
    if (id < 0 || nt_kind(nt, id) != NK_BlockNode || comp_scope_of(c, id) != sc) continue;
    int pn = nt_ref(nt, id, "parameters");
    if (pn < 0 || nt_kind(nt, pn) != NK_BlockParametersNode) continue;
    int inner = nt_ref(nt, pn, "parameters");
    if (inner < 0) continue;
    int hit = 0;
    int rn = 0; const int *reqs = nt_arr(nt, inner, "requireds", &rn);
    for (int k = 0; k < rn && !hit; k++) {
      const char *p = nt_str(nt, reqs[k], "name");
      if (p && sp_streq(p, nm)) hit = 1;
    }
    int on = 0; const int *opts = nt_arr(nt, inner, "optionals", &on);
    for (int k = 0; k < on && !hit; k++) {
      const char *p = nt_str(nt, opts[k], "name");
      if (p && sp_streq(p, nm)) hit = 1;
    }
    if (!hit) continue;
    const char *mn = nt_str(nt, call, "name");
    if (!mn) return 0;
    /* `Klass.new { }`: the block goes to Klass's initialize */
    int recv = nt_ref(nt, call, "receiver");
    if (sp_streq(mn, "new")) {
      const char *cn = recv >= 0 && nt_kind(nt, recv) == NK_ConstantReadNode ? nt_str(nt, recv, "name") : NULL;
      int ci = cn ? comp_class_index(c, cn) : -1;
      int si = ci >= 0 ? comp_method_in_chain(c, ci, "initialize", NULL) : -1;
      if (si < 0) return 0;
      Scope *d = &c->scopes[si];
      return !d->yields && d->blk_param && d->blk_param[0];
    }
    int nkept = 0;
    for (int s = 0; s < c->nscopes; s++) {
      Scope *d = &c->scopes[s];
      if (!d->name || !sp_streq(d->name, mn)) continue;
      if (d->yields || !d->blk_param || !d->blk_param[0]) return 0;
      nkept++;
    }
    return nkept > 0;
  }
  return 0;
}

/* Whether `nm` is a parameter of a `Thread.new` or `Fiber.new` block written
   in scope `sc`. The body runs as its own function and declares each such
   parameter an sp_RbVal (emit_fiber_new): the runtime hands it the thread's
   arguments or the resume value boxed, whatever they are. */
static int local_is_fiber_block_param(Compiler *c, const int *fb, int nfb, Scope *sc, const char *nm) {
  const NodeTable *nt = c->nt;
  for (int f = 0; f < nfb; f++) {
    int blk = fb[f];
    if (comp_scope_of(c, blk) != sc) continue;
    int pn = nt_ref(nt, blk, "parameters");
    int inner = pn >= 0 ? nt_ref(nt, pn, "parameters") : -1;
    int rn2 = 0; const int *reqs = inner >= 0 ? nt_arr(nt, inner, "requireds", &rn2) : NULL;
    for (int k = 0; k < rn2; k++) {
      const char *p = nt_str(nt, reqs[k], "name");
      if (p && sp_streq(p, nm)) return 1;
    }
  }
  return 0;
}
/* The blocks of the program's `Thread.new { }` / `Fiber.new { }` calls, into
   a malloc'd array (NULL when there are none): collected once per pass, so
   the push sites ask a short list rather than every `new` in the program. */
static int *fiber_new_blocks(Compiler *c, int *out_n) {
  const NodeTable *nt = c->nt;
  int n = 0, cap = 0, *v = NULL;
  for (int call = an_calls_named_first(c, "new"); call >= 0; call = an_calls_named_next(call)) {
    int blk = nt_ref(nt, call, "block"), recv = nt_ref(nt, call, "receiver");
    if (nt_kind(nt, call) != NK_CallNode || blk < 0 || nt_kind(nt, blk) != NK_BlockNode || recv < 0) continue;
    if (nt_kind(nt, recv) != NK_ConstantReadNode && nt_kind(nt, recv) != NK_ConstantPathNode) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || (!sp_streq(rn, "Thread") && !sp_streq(rn, "Fiber"))) continue;
    if (n == cap) {
      cap = cap ? cap * 2 : 8;
      v = (int *)realloc(v, sizeof(int) * (size_t)cap);
      if (!v) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    v[n++] = blk;
  }
  *out_n = n;
  return v;
}

static int recv_hash_or_write(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  int n = unwrap_parens(c, recv);
  if (n >= 0 && nt_kind(nt, n) == NK_CallNode) {
    int gr = nt_ref(nt, n, "receiver");
    int ga = nt_ref(nt, n, "arguments");
    const char *gm = nt_str(nt, n, "name");
    Scope *s = comp_scope_of(c, n);
    /* A class-method caller's self is the class, whose getter is not the
       instance chain's: leave it boxed rather than credit the wrong slot. */
    if (ga >= 0 || nt_ref(nt, n, "block") >= 0 || !gm || !s || s->class_id < 0 ||
        s->is_cmethod || (gr >= 0 && nt_kind(nt, gr) != NK_SelfNode)) return -1;
    int mi = comp_method_in_chain(c, s->class_id, gm, NULL);
    if (mi < 0) return -1;
    /* self may be a subclass whose override answers another slot, as in
       multi_return_elem_types: only a getter no descendant replaces */
    for (int cj = 0; cj < c->nclasses; cj++) {
      int an = c->classes[cj].parent;
      while (an >= 0 && an != s->class_id) an = c->classes[an].parent;
      if (an == s->class_id && comp_method_in_chain(c, cj, gm, NULL) != mi) return -1;
    }
    /* only a getter whose sole exit is its tail: an earlier `return` could
       hand back a different slot, as in multi_return_elem_types (#4889) */
    int last = scope_body_last(c, mi);
    int def = c->scopes[mi].def_node;
    if (def >= 0 && subtree_has_return(nt, nt_ref(nt, def, "body"), last)) return -1;
    n = unwrap_parens(c, last);
  }
  if (n < 0) return -1;
  NodeKind k = nt_kind(nt, n);
  if (k != NK_InstanceVariableOrWriteNode && k != NK_LocalVariableOrWriteNode) return -1;
  int v = nt_ref(nt, n, "value");
  if (v < 0 || (nt_kind(nt, v) != NK_HashNode && !ty_is_hash(infer_type(c, v)))) return -1;
  return n;
}

/* Is a multi-write's RHS a tuple -- a literal whose elements line up one to
   one with the targets? One holding a splat (`a, b = *xs, "s"`) is not: it
   is typed as the array it builds. */
int masgn_tuple_rhs(const NodeTable *nt, int value) {
  if (value < 0 || nt_kind(nt, value) != NK_ArrayNode) return 0;
  int n = 0;
  const int *els = nt_arr(nt, value, "elements", &n);
  for (int i = 0; i < n; i++)
    if (nt_kind(nt, els[i]) == NK_SplatNode) return 0;
  return 1;
}

/* Unify elem into each local / ivar / constant target of a multi-write's
   lefts or rights. Returns 1 when an ivar or constant slot moved. */
static int masgn_unify_elem(Compiler *c, Scope *ms, const int *tgts, int n, TyKind elem) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int i = 0; i < n; i++) {
    const char *lty_ms = nt_type(nt, tgts[i]) ? nt_type(nt, tgts[i]) : "";
    if (sp_streq(lty_ms, "LocalVariableTargetNode")) {
      const char *lnm = nt_str(nt, tgts[i], "name");
      LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
      if (!lv || lv->is_param || lv->is_block_param) continue;
      lv->type = ty_unify(lv->type, elem);
    }
    else if (sp_streq(lty_ms, "InstanceVariableTargetNode") && ms) {
      /* outside a method: the class body's slot, or the top level's */
      int icid = ms->class_id;
      if (icid < 0 && c->node_cbody && tgts[i] < c->node_cap) icid = c->node_cbody[tgts[i]];
      if (icid < 0) icid = comp_class_index(c, "Toplevel");
      if (icid < 0) continue;
      const char *ivnm = nt_str(nt, tgts[i], "name");
      int iv_ms = ivnm ? comp_ivar_index(&c->classes[icid], ivnm) : -1;
      if (iv_ms < 0 || class_ivar_pinned(&c->classes[icid], ivnm)) continue;
      TyKind mg = ty_unify(c->classes[icid].ivar_types[iv_ms], elem);
      if (mg != c->classes[icid].ivar_types[iv_ms]) {
        c->classes[icid].ivar_types[iv_ms] = mg; changed = 1;
      }
    }
    else if (sp_streq(lty_ms, "GlobalVariableTargetNode")) {
      const char *gnm = nt_str(nt, tgts[i], "name");
      const char *grn = gnm ? comp_resolve_gvar(c, gnm + 1) : NULL;
      LocalVar *glv = grn ? comp_gvar(c, grn) : NULL;
      if (!glv) continue;
      if (lv_widen(glv, elem)) changed = 1;
    }
    else if (sp_streq(lty_ms, "ClassVariableTargetNode")) {
      /* in a method, its class; in a class body, the body's */
      int cc = ms && ms->class_id >= 0 ? ms->class_id
             : tgts[i] < c->node_cap ? c->node_cbody[tgts[i]] : -1;
      const char *cvnm = nt_str(nt, tgts[i], "name");
      if (cc >= 0 && cvnm) cc = comp_cvar_owner(c, cc, cvnm);
      int cvx = cc >= 0 && cvnm ? comp_cvar_index(&c->classes[cc], cvnm) : -1;
      if (cvx < 0) continue;
      TyKind mg = ty_unify(c->classes[cc].cvar_types[cvx], elem);
      if (mg != c->classes[cc].cvar_types[cvx]) {
        c->classes[cc].cvar_types[cvx] = mg; changed = 1;
      }
    }
    else if (sp_streq(lty_ms, "ConstantTargetNode") || sp_streq(lty_ms, "ConstantPathTargetNode")) {
      const char *cnm_ms = nt_str(nt, tgts[i], "name");
      LocalVar *cv_ms = cnm_ms ? comp_const(c, cnm_ms) : NULL;
      if (!cv_ms) continue;
      if (lv_widen(cv_ms, elem)) changed = 1;
    }
  }
  return changed;
}

/* An instance variable target of a multiple assignment from a boxed value
   whose slot nothing else has typed yet (unknown, or only nil): it takes the
   boxed elements. A slot another write already types (optcarrot's int
   @io_addr) keeps its type, so a boxed right side alone does not widen it. */
static int masgn_ivar_untyped(Compiler *c, Scope *ms, int tgt) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, tgt) != NK_InstanceVariableTargetNode || !ms || ms->class_id < 0) return 0;
  const char *ivnm = nt_str(nt, tgt, "name");
  int iv = ivnm ? comp_ivar_index(&c->classes[ms->class_id], ivnm) : -1;
  if (iv < 0) return 0;
  TyKind t = c->classes[ms->class_id].ivar_types[iv];
  return t == TY_UNKNOWN || t == TY_NIL || t == TY_POLY;
}

/* The slots under a nested (a, *b, c) target of a multiple assignment take
   elements of a boxed value (emit_massign_poly_target): each target widens
   to poly, and a splat target to a poly array. */
static int masgn_nested_poly(Compiler *c, Scope *ms, int tgt) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, tgt) != NK_MultiTargetNode) return 0;
  int changed = 0;
  const char *sides[2] = { "lefts", "rights" };
  for (int s = 0; s < 2; s++) {
    int n = 0;
    const int *ts = nt_arr(nt, tgt, sides[s], &n);
    for (int i = 0; i < n; i++) {
      if (nt_kind(nt, ts[i]) == NK_MultiTargetNode) changed |= masgn_nested_poly(c, ms, ts[i]);
      else changed |= masgn_unify_elem(c, ms, &ts[i], 1, TY_POLY);
    }
  }
  int rest = nt_ref(nt, tgt, "rest");
  int inner = rest >= 0 && nt_kind(nt, rest) == NK_SplatNode ? nt_ref(nt, rest, "expression") : -1;
  if (inner >= 0) changed |= masgn_unify_elem(c, ms, &inner, 1, TY_POLY_ARRAY);
  return changed;
}

/* Whether local `nm` of scope `sc` names a row of a narrowed table: one of
   its writes reads an element (`[]`, `at`) of an Integer or Float table, or
   copies a local that does (`row = @t[k]; al = row`). A copy is the same
   row, and the direct-alias propagation carries its type back to `row`, so
   widening the copy made `row` a converted copy as well. */
static int table_row_alias(Compiler *c, const LWIndex *lw, const char *nm, Scope *sc, int depth) {
  const NodeTable *nt = c->nt;
  if (depth > 8) return 0;
  for (int r = lw_index_first(lw, nm, (int)(sc - c->scopes)); r >= 0; r = lw->next[r]) {
    int w = lw->node[r];
    const char *wn = nt_str(nt, w, "name");
    if (nt_kind(nt, w) != NK_LocalVariableWriteNode || !wn || !sp_streq(wn, nm) ||
        comp_scope_of(c, w) != sc) continue;
    int wv = nt_ref(nt, w, "value");
    if (wv >= 0 && nt_kind(nt, wv) == NK_LocalVariableReadNode) {
      const char *src = nt_str(nt, wv, "name");
      if (src && !sp_streq(src, nm) && table_row_alias(c, lw, src, sc, depth + 1)) return 1;
      continue;
    }
    const char *wcn = wv >= 0 && nt_kind(nt, wv) == NK_CallNode ? nt_str(nt, wv, "name") : NULL;
    int wr = wcn ? nt_ref(nt, wv, "receiver") : -1;
    TyKind wrt = wr >= 0 ? infer_type(c, wr) : TY_UNKNOWN;
    if (wcn && (sp_streq(wcn, "[]") || sp_streq(wcn, "at")) &&
        (wrt == TY_INT_ARRAY_ARRAY || wrt == TY_FLOAT_ARRAY_ARRAY)) return 1;
  }
  return 0;
}

int infer_write_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  g_infer_write_round = 1;
  int nfb = 0, *fb = fiber_new_blocks(c, &nfb);

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
  /* Because of that reset, a site below that types a non-param local must NOT
     report `changed` itself: it is comparing against UNKNOWN, so it answers
     "changed" every round even when it re-derives exactly last round's type,
     and the fixpoint never converges. The sweep at the end of this function
     is the one that reports, by comparing against the type stashed above.
     Eight sites did report, and between them they were why 70 of the repo's
     own test programs ran the fixpoint to its 128-round cap (#4116). Ivars,
     class variables, constants and parameters are NOT reset, so those sites
     report normally. */
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
          /* ...unless that type is not a container of the literal's kind:
             `x = 1; x = {}` holds an Integer and a Hash, so it boxes. Kept,
             the Integer slot was assigned the hash pointer. */
          if (an_empty_container_disagrees(an_empty_container_kind(c, val_id), newt))
            newt = TY_POLY;
        }
        else if (an_empty_container_kind(c, val_id)) {
          LocalVar *lv2 = scope_local(comp_scope_of(c, id), nm);
          TyKind gr = lv2 ? (TyKind)lv2->gc_root : TY_UNKNOWN;
          if (gr == TY_POLY || an_empty_container_disagrees(an_empty_container_kind(c, val_id), gr))
            newt = TY_POLY;
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
      if (newt != TY_UNKNOWN && !lv->rbs_seeded)
        changed |= slot_take(c, lv, newt, nt_ref(nt, id, "value"));
      continue;
    }
    slot_take(c, lv, newt, nt_ref(nt, id, "value"));
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
      int tramp = ty_is_object(vrt) &&
                  comp_trampoline_kind(c, ty_object_class(vrt), vnm, NULL);
      /* An iterator block that assigns a local has the same shape: the block
         body's locals sit at higher node ids than this write, so the main loop
         derived the element type with them still reset to UNKNOWN and the slot
         came out narrower than the value the block actually yields
         (`r = [0].map { |i| w = ...; w ? w[0] : 9 }` -> an Integer array
         holding boxed values) (#3463). */
      if (!tramp && !subtree_writes_local(nt, nt_ref(nt, val_id, "block"))) continue;
    }
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? scope_local(comp_scope_of(c, id), nm) : NULL;
    if (!lv || lv->is_param || lv->is_block_param) continue;
    TyKind newt = infer_type(c, val_id);
    if (newt == TY_NIL) newt = TY_POLY;
    /* see the note at the multi-write arm below: no `changed` for a plain
       local this pass reset. */
    slot_take(c, lv, newt, val_id);
  }

  /* Multiple assignment `a, b = e0, e1`: each target gets its element's
     type (the RHS ArrayNode is a tuple here, not an array value). */
  for (int id = 0; id < nt->count; id++) {
    if (!sp_streq(nt_type(nt, id) ? nt_type(nt, id) : "", "MultiWriteNode")) continue;
    int ln = 0;
    const int *lefts = nt_arr(nt, id, "lefts", &ln);
    int value = nt_ref(nt, id, "value");
    {
      int rn_n = 0;
      const int *rights_n = nt_arr(nt, id, "rights", &rn_n);
      for (int i = 0; i < ln; i++) changed |= masgn_nested_poly(c, comp_scope_of(c, id), lefts[i]);
      for (int j = 0; j < rn_n; j++) changed |= masgn_nested_poly(c, comp_scope_of(c, id), rights_n[j]);
    }
    /* an empty `{}` on the right, whole or as one of the values, has no
       slot to take its variant from: it is the general boxed-key hash */
    {
      int vn = 1;
      const int *vs = &value;
      if (masgn_tuple_rhs(nt, value)) vs = nt_arr(nt, value, "elements", &vn);
      for (int i = 0; i < vn; i++) {
        int hv = vs[i], hen = 0;
        if (hv < 0 || hv >= c->node_cap || nt_kind(nt, hv) != NK_HashNode || !c->hash_want) continue;
        nt_arr(nt, hv, "elements", &hen);
        if (hen == 0 && !ty_is_hash(c->hash_want[hv])) { c->hash_want[hv] = TY_POLY_POLY_HASH; changed = 1; }
      }
    }
    const char *vty = nt_type(nt, value);
    /* `r, w = IO.pipe` / `a, b = Socket.pair(...)` -> both targets are IO
       handles. The general path below reads a USER method's multi-value
       return; a builtin class method has no scope to read, so the few that
       answer a fixed pair are named here. Without it both targets settle poly,
       and every method gated on a typed receiver -- recv_nonblock is gated on
       TY_IO -- cannot reach them, which reads as NoMethodError on a perfectly
       good socket. The runtime kind is what answers #class, so a Socket pair
       still says Socket. */
    if (ln == 2 && vty && sp_streq(vty, "CallNode")) {
      const char *vnm = nt_str(nt, value, "name");
      int vrecv = nt_ref(nt, value, "receiver");
      const char *vcn = (vrecv >= 0 && nt_type(nt, vrecv) &&
                         sp_streq(nt_type(nt, vrecv), "ConstantReadNode"))
                        ? nt_str(nt, vrecv, "name") : NULL;
      int is_io_pair = vnm && vcn &&
        ((sp_streq(vcn, "IO") && sp_streq(vnm, "pipe")) ||
         (sp_streq(vcn, "Socket") &&
          (sp_streq(vnm, "pair") || sp_streq(vnm, "socketpair"))));
      /* UNIXSocket.pair is deliberately absent: the call itself has no arm, so
         naming it here would claim a typing for something that cannot compile. */
      if (is_io_pair) {
        for (int i = 0; i < 2; i++) {
          const char *lty_io = nt_type(nt, lefts[i]) ? nt_type(nt, lefts[i]) : "";
          if (sp_streq(lty_io, "ConstantTargetNode") || sp_streq(lty_io, "ConstantPathTargetNode")) {
            /* `R, W = IO.pipe`: an untyped constant gets no slot to assign */
            const char *cnm_io = nt_str(nt, lefts[i], "name");
            LocalVar *cv_io = cnm_io ? comp_const(c, cnm_io) : NULL;
            if (cv_io && cv_io->type != TY_IO) {
              cv_io->type = ty_unify(cv_io->type, TY_IO);
              changed = 1;
            }
            continue;
          }
          if (sp_streq(lty_io, "GlobalVariableTargetNode"))
            changed |= masgn_unify_elem(c, comp_scope_of(c, id), &lefts[i], 1, TY_IO);
          if (!sp_streq(lty_io, "LocalVariableTargetNode")) continue;
          const char *lnm = nt_str(nt, lefts[i], "name");
          LocalVar *lv = lnm ? scope_local_intern(comp_scope_of(c, id), lnm) : NULL;
          if (lv) lv->type = TY_IO;   /* the end-of-pass sweep reports it */
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
          else if (sp_streq(lty_mr, "GlobalVariableTargetNode") || sp_streq(lty_mr, "ClassVariableTargetNode"))
            changed |= masgn_unify_elem(c, ms_mr, &lefts[i], 1, elems[i]);
          else if (sp_streq(lty_mr, "ConstantTargetNode") || sp_streq(lty_mr, "ConstantPathTargetNode")) {
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
    if (!masgn_tuple_rhs(nt, value)) {
      /* scalar RHS (`a, b = 1`): the first target gets the scalar, the rest
         their slot default. Type every target as the scalar's kind. Array /
         hash RHS would splat and is handled elsewhere, so skip those. */
      int multi_src = vty && (sp_streq(vty, "CallNode") || sp_streq(vty, "SuperNode") ||
                              sp_streq(vty, "ForwardingSuperNode") || sp_streq(vty, "YieldNode"));
      if (vty && value >= 0 && !multi_src) {
        TyKind st = infer_type(c, value);
        if (st != TY_UNKNOWN && st != TY_NIL && !ty_is_array(st) && !ty_is_hash(st)) {
          for (int i = 0; i < ln; i++) {
            const char *lty_s = nt_type(nt, lefts[i]) ? nt_type(nt, lefts[i]) : "";
            if (sp_streq(lty_s, "GlobalVariableTargetNode") || sp_streq(lty_s, "ClassVariableTargetNode"))
              changed |= masgn_unify_elem(c, comp_scope_of(c, id), &lefts[i], 1, st);
            /* a constant has no slot until a write types it: the first takes
               the scalar, the rest nil */
            if (sp_streq(lty_s, "ConstantTargetNode") || sp_streq(lty_s, "ConstantPathTargetNode"))
              changed |= masgn_unify_elem(c, comp_scope_of(c, id), &lefts[i], 1, i == 0 ? st : TY_POLY);
            if (!sp_streq(lty_s, "LocalVariableTargetNode")) continue;
            const char *lnm = nt_str(nt, lefts[i], "name");
            LocalVar *lv = lnm ? scope_local(comp_scope_of(c, id), lnm) : NULL;
            if (!lv || lv->is_param || lv->is_block_param) continue;
            lv->type = ty_unify(lv->type, st);
          }
          /* a target after the splat takes the scalar when no target before
             it did (`*r, C = 1`), else nil */
          int rn_s = 0;
          const int *rights_s = nt_arr(nt, id, "rights", &rn_s);
          for (int j = 0; j < rn_s; j++) {
            const char *rty_s = nt_type(nt, rights_s[j]) ? nt_type(nt, rights_s[j]) : "";
            if (sp_streq(rty_s, "ConstantTargetNode") || sp_streq(rty_s, "ConstantPathTargetNode") ||
                sp_streq(rty_s, "ClassVariableTargetNode"))
              changed |= masgn_unify_elem(c, comp_scope_of(c, id), &rights_s[j], 1,
                                          j == 0 && ln == 0 ? st : TY_POLY);
          }
          /* the rest target under a scalar RHS collects [scalar] (or stays
             empty when fixed targets consumed it): an ARRAY of the scalar. */
          int rest_ms = nt_ref(nt, id, "rest");
          if (rest_ms >= 0 && nt_type(nt, rest_ms) && sp_streq(nt_type(nt, rest_ms), "SplatNode")) {
            int rin_ms = nt_ref(nt, rest_ms, "expression");
            if (rin_ms >= 0 && nt_type(nt, rin_ms) &&
                !sp_streq(nt_type(nt, rin_ms), "LocalVariableTargetNode")) {
              TyKind rat = ty_array_of(st);
              changed |= masgn_unify_elem(c, comp_scope_of(c, id), &rin_ms, 1,
                                          rat == TY_UNKNOWN ? TY_POLY_ARRAY : rat);
            }
            if (rin_ms >= 0 && nt_type(nt, rin_ms) &&
                sp_streq(nt_type(nt, rin_ms), "LocalVariableTargetNode")) {
              const char *rnm_ms = nt_str(nt, rin_ms, "name");
              LocalVar *rlv_ms = rnm_ms ? scope_local(comp_scope_of(c, id), rnm_ms) : NULL;
              if (rlv_ms && !rlv_ms->is_param && !rlv_ms->is_block_param) {
                TyKind rat = ty_array_of(st);
                if (rat == TY_UNKNOWN) rat = TY_POLY_ARRAY;
                TyKind mg_r = ty_unify(rlv_ms->type, rat);
                rlv_ms->type = mg_r;
              }
            }
          }
        }
      }
      /* any expression returning a typed array: assign element types to targets */
      if (value >= 0) {
        TyKind st = infer_type(c, value);
        /* poly RHS: destructure gives poly elements. So does a hash, or a
           scalar from a call (which could have answered an array): codegen
           boxes it and destructures it at run time as itself. */
        if (st == TY_POLY || st == TY_POLY_ARRAY ||
            (st != TY_UNKNOWN && !ty_is_array(st) && (ty_is_hash(st) || multi_src))) {
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
            else if (sp_streq(lty_p, "GlobalVariableTargetNode") || sp_streq(lty_p, "ClassVariableTargetNode") ||
                     masgn_ivar_untyped(c, ms_poly, lefts[i]))
              changed |= masgn_unify_elem(c, ms_poly, &lefts[i], 1, TY_POLY);
          }
          int rn_p = 0;
          const int *rights_p = nt_arr(nt, id, "rights", &rn_p);
          for (int j = 0; j < rn_p; j++) {
            const char *rty_p = nt_type(nt, rights_p[j]) ? nt_type(nt, rights_p[j]) : "";
            if (sp_streq(rty_p, "GlobalVariableTargetNode") || sp_streq(rty_p, "ClassVariableTargetNode") ||
                masgn_ivar_untyped(c, ms_poly, rights_p[j]))
              changed |= masgn_unify_elem(c, ms_poly, &rights_p[j], 1, TY_POLY);
          }
          int rest_p = nt_ref(nt, id, "rest");
          int rin_p = (rest_p >= 0 && nt_type(nt, rest_p) && sp_streq(nt_type(nt, rest_p), "SplatNode"))
                      ? nt_ref(nt, rest_p, "expression") : -1;
          if (st == TY_POLY && rin_p >= 0 && nt_type(nt, rin_p) &&
              !sp_streq(nt_type(nt, rin_p), "LocalVariableTargetNode"))
            changed |= masgn_unify_elem(c, ms_poly, &rin_p, 1, TY_POLY_ARRAY);
        }
        if (ty_is_array(st)) {
          TyKind elem = ty_array_elem(st);
          int rn2 = 0;
          const int *rights2 = nt_arr(nt, id, "rights", &rn2);
          Scope *ms_arr = comp_scope_of(c, id);
          changed |= masgn_unify_elem(c, ms_arr, lefts, ln, elem);
          changed |= masgn_unify_elem(c, ms_arr, rights2, rn2, elem);
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
            else if (inner2 >= 0 && nt_type(nt, inner2))
              changed |= masgn_unify_elem(c, ms_arr, &inner2, 1, st);
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
        /* a nil element widens its target, as `x = nil` does */
        if (et == TY_NIL) et = TY_POLY;
        LocalVar *lv = lnm ? scope_local(comp_scope_of(c, id), lnm) : NULL;
        if (!lv || lv->is_param || lv->is_block_param) continue;
        lv->type = ty_unify(lv->type, et);
      }
      else if (sp_streq(lty, "ConstantTargetNode") || sp_streq(lty, "ConstantPathTargetNode")) {
        const char *cnm = nt_str(nt, lefts[i], "name");
        LocalVar *cv = cnm ? comp_const(c, cnm) : NULL;
        if (!cv) continue;
        TyKind et = infer_type(c, els[i]);
        if (et == TY_NIL || nt_kind(nt, els[i]) == NK_SplatNode) et = TY_POLY;
        if (lv_widen(cv, et)) changed = 1;
      }
      else if (sp_streq(lty, "InstanceVariableTargetNode")) {
        Scope *iv_sc = comp_scope_of(c, id);
        int iv_cid = iv_sc ? iv_sc->class_id : -1;
        if (iv_cid < 0) continue;
        const char *ivnm = nt_str(nt, lefts[i], "name");
        int iv_idx = ivnm ? comp_ivar_index(&c->classes[iv_cid], ivnm) : -1;
        if (iv_idx < 0 || class_ivar_pinned(&c->classes[iv_cid], ivnm)) continue;
        TyKind et = infer_type(c, els[i]);
        if (et == TY_NIL) et = TY_POLY;
        TyKind mg = ty_unify(c->classes[iv_cid].ivar_types[iv_idx], et);
        if (mg != c->classes[iv_cid].ivar_types[iv_idx]) {
          c->classes[iv_cid].ivar_types[iv_idx] = mg; changed = 1;
        }
      }
      else if (sp_streq(lty, "GlobalVariableTargetNode") || sp_streq(lty, "ClassVariableTargetNode")) {
        TyKind et = infer_type(c, els[i]);
        if (et == TY_NIL) et = TY_POLY;
        changed |= masgn_unify_elem(c, comp_scope_of(c, id), &lefts[i], 1, et);
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
       elements land nil, so widen them to poly like a plain `x = nil`. */
    Scope *usc = comp_scope_of(c, id);
    for (int i = en; i < ln; i++) {
      const char *lty = nt_type(nt, lefts[i]);
      if (lty && !sp_streq(lty, "LocalVariableTargetNode")) {
        changed |= masgn_unify_elem(c, usc, &lefts[i], 1, TY_POLY);
        continue;
      }
      if (!lty) continue;
      const char *lnm = nt_str(nt, lefts[i], "name");
      LocalVar *lv = lnm ? scope_local(usc, lnm) : NULL;
      if (!lv || lv->is_param || lv->is_block_param) continue;
      TyKind mg = ty_unify(lv->type, TY_POLY);
      if (mg != lv->type) lv->type = mg;
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
        if (et == TY_NIL) et = TY_POLY;
      }
      if (sp_streq(rty3, "LocalVariableTargetNode")) {
        const char *rnm2 = nt_str(nt, rights[j], "name");
        LocalVar *lv = rnm2 ? scope_local(comp_scope_of(c, id), rnm2) : NULL;
        if (!lv || lv->is_param || lv->is_block_param) continue;
        lv->type = ty_unify(lv->type, et);
      }
      else if (sp_streq(rty3, "ConstantTargetNode") || sp_streq(rty3, "ConstantPathTargetNode")) {
        const char *cnm2 = nt_str(nt, rights[j], "name");
        LocalVar *cv2 = cnm2 ? comp_const(c, cnm2) : NULL;
        if (!cv2) continue;
        if (lv_widen(cv2, et)) changed = 1;
      }
      else if (sp_streq(rty3, "GlobalVariableTargetNode") || sp_streq(rty3, "ClassVariableTargetNode"))
        changed |= masgn_unify_elem(c, comp_scope_of(c, id), &rights[j], 1, et);
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
        /* a literal holding a splat (`g, *r = *xs`) has no fixed positions:
           the rest slices the array it builds */
        for (int i = 0; i < en; i++)
          if (nt_kind(nt, els[i]) == NK_SplatNode) { rest_arr = infer_type(c, value); break; }
        LocalVar *lv = rnm ? scope_local(comp_scope_of(c, id), rnm) : NULL;
        if (lv && !lv->is_param && !lv->is_block_param)
          lv->type = ty_unify(lv->type, rest_arr);
      }
      /* an instance or class variable or a constant takes the same array */
      else if (inner >= 0 && nt_type(nt, inner) &&
               !sp_streq(nt_type(nt, inner), "GlobalVariableTargetNode")) {
        int rstart = ln, rend = en - rn;
        if (rend < rstart) rend = rstart;
        TyKind rest_elem = TY_UNKNOWN;
        for (int i = rstart; i < rend; i++)
          rest_elem = ty_unify(rest_elem, infer_type(c, els[i]));
        TyKind rest_arr = rest_elem != TY_UNKNOWN ? ty_array_of(rest_elem) : TY_POLY_ARRAY;
        for (int i = 0; i < en; i++)
          if (nt_kind(nt, els[i]) == NK_SplatNode) { rest_arr = infer_type(c, value); break; }
        changed |= masgn_unify_elem(c, comp_scope_of(c, id), &inner, 1, rest_arr);
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
    if (!ty || nt_int(nt, id, "dyn_arm", 0)) continue;
    int recv, kt = TY_UNKNOWN, vt = TY_UNKNOWN, is_push = 0, is_idx_write = 0, is_splice = 0;
    int is_fill = 0;  /* a fill's span: one value, the element evidence a store's is */
    int concat_scalar = 0;  /* a concat of no array, which raises rather than stores */
    int splice_arr = 0;     /* a splice's source is a typed array: its kind is the elements' */
    int is_merge = 0;  /* merge!/update: kt and vt are the merged hashes' */
    /* the value arguments of a push/unshift/insert, each its own evidence */
    const int *elem_argv = NULL;
    int elem_from = 0, elem_an = 0, elem_splat_index = 0;
    int knode = -1, vnode = -1;  /* the stored key and value, for a plain store */
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
        /* `<<` is ambiguous a third way: a user class can own it. A slot whose
           value comes from a container -- a fused loop variable over an array
           of such objects -- has no array evidence of its own, and reading its
           `<<` as a push typed it an int array, so the element assignment and
           every later call went to the wrong type (#3502). Only decline where
           the reading is a guess: a slot the program does write an array into
           keeps the push promotion. */
        if (sp_streq(name, "<<") && recv >= 0 &&
            (an_user_defines_method(c, "<<") || an_native_defines_method(c, "<<")) &&
            !recv_has_array_write(c, recv)) continue;
        is_push = 1; elem_argv = argv; elem_an = an; vt = push_elem_ty(c, argv[0]);
        for (int ai = 1; ai < an; ai++) vt = ty_unify(vt, push_elem_ty(c, argv[ai]));
        if (an == 1) vnode = argv[0];
      }
      else if (name && (sp_streq(name, "unshift") || sp_streq(name, "prepend")) && an >= 1) {
        /* unshift(v, ...): every argument is element evidence, like push
           (a foreign value used to store its raw bits into the typed slots) */
        is_push = 1; elem_argv = argv; elem_an = an; vt = push_elem_ty(c, argv[0]);
        for (int ai = 1; ai < an; ai++) vt = ty_unify(vt, push_elem_ty(c, argv[ai]));
      }
      else if (name && sp_streq(name, "insert") && an >= 2) {
        /* insert(i, v, ...): the values from position 1 on are evidence */
        is_push = 1; elem_argv = argv; elem_an = an; elem_from = 1; vt = push_elem_ty(c, argv[1]);
        for (int ai = 2; ai < an; ai++) vt = ty_unify(vt, push_elem_ty(c, argv[ai]));
      }
      else if (name && sp_streq(name, "insert") && an == 1 &&
               nt_kind(nt, argv[0]) == NK_SplatNode) {
        /* insert(*a): the index rides in the splat, ahead of the values; only
           a container parameter reads this, through the call sites */
        elem_argv = argv; elem_an = an; elem_splat_index = 1;
      }
      else if (name && sp_streq(name, "concat") && an == 1) {
        /* concat(other): the other array's elements splice in */
        is_push = 1; vt = splice_incoming_elem(c, argv[0]);
        TyKind cat = infer_type(c, argv[0]);
        concat_scalar = cat != TY_UNKNOWN && cat != TY_POLY && !ty_is_array(cat);
      }
      else if (name && sp_streq(name, "replace") && an == 1 && recv >= 0 &&
               ty_is_array(infer_type(c, argv[0])) &&
               recv_has_array_write(c, recv)) {
        /* replace(other) makes the other's elements the receiver's WHOLE
           contents, which is the same evidence about what it holds -- and a
           local's static type is an upper bound, so the union answers here as
           it does for concat. Without it `[1, 2].replace(["x"])` left the
           receiver an IntArray with no arm that could take a StrArray, and the
           call answered NoMethodError (#4339).

           Unlike concat, `replace` is also HASH's and String's, so BOTH sides
           have to look like an array before this reads as array evidence: the
           argument by its type, and the receiver by having an array written
           into it somewhere. Without the first, `h1.replace(h2)` widened a
           hash local into an array; without the second, `h.replace([1, 2])`
           did -- and each stopped compiling. */
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
          rplv->type = TY_POLY_POLY_HASH;   /* a reset local: the sweep reports */
        }
        continue;
      }
      else if (name && sp_streq(name, "[]=") && an == 2) {
        is_idx_write = 1; kt = infer_type(c, argv[0]);
        vt = tuple_elem_evidence(c, argv[1], infer_type(c, argv[1]));
        knode = argv[0]; vnode = argv[1];
        /* a range key is a splice: the RHS contributes element evidence */
        if (kt == TY_RANGE) {
          is_splice = 1; vt = splice_incoming_elem(c, argv[1]);
          splice_arr = ty_is_array(infer_type(c, argv[1])) && vt != TY_POLY_ARRAY;
        }
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
        knode = argv[0]; vnode = argv[1];
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
        /* a conflict block's value is stored under the colliding key */
        TyKind bvt = hash_merge_block_value_ty(c, id);
        if (bvt != TY_UNKNOWN) vt = ty_unify(vt, bvt);
        is_merge = 1;
      }
      else if (name && sp_streq(name, "[]=") && an == 3) {
        /* a[start, len] = rhs: a splice over the (start, len) span */
        is_idx_write = 1; is_splice = 1; vt = splice_incoming_elem(c, argv[2]);
        splice_arr = ty_is_array(infer_type(c, argv[2])) && vt != TY_POLY_ARRAY;
      }
      else if (name && sp_streq(name, "fill") && an >= 1 && an <= 3 &&
               nt_ref(nt, id, "block") < 0) {
        /* arr.fill(v[, start[, len]]) writes v into every element slot of the
           span: the value is element evidence exactly like a splice, so a typed
           array whose elements cannot hold the value widens to a poly array
           (previously the raw bits were stored: [1,2,3].fill(:a) filled the int
           array with the symbol id). The block form is the next arm. */
        is_idx_write = 1; is_splice = 1; is_fill = 1; vt = infer_type(c, argv[0]);
        kt = TY_INT;  /* a positional span, never hash evidence */
      }
      else if (name && sp_streq(name, "fill") && an <= 2 && nt_ref(nt, id, "block") >= 0 &&
               nt_kind(nt, nt_ref(nt, id, "block")) == NK_BlockNode) {
        /* the block form, fill([start[, len]]) { |i| v }: the block's value
           is what goes into each slot of the span, the same evidence */
        int fblk = nt_ref(nt, id, "block");
        int fbody = nt_ref(nt, fblk, "body");
        int fbn = 0; const int *fbs = fbody >= 0 ? nt_arr(nt, fbody, "body", &fbn) : NULL;
        vt = fbn > 0 ? infer_type(c, fbs[fbn - 1]) : TY_NIL;
        TyKind fnx = block_next_value_ty(c, fbody);
        if (fnx != TY_UNKNOWN) vt = vt == TY_UNKNOWN ? fnx : ty_unify(vt, fnx);
        if (vt == TY_VOID) vt = TY_NIL;
        is_idx_write = 1; is_splice = 1; is_fill = 1;
        kt = TY_INT;
      }
      else if (name && (sp_streq(name, "fetch") ||
                        (sp_streq(name, "[]") && an == 1)) && an >= 1) {
        /* hash.fetch(key,..) / hash[key]: promote TY_UNKNOWN local to a typed hash.
           Only fires when the slot is currently TY_UNKNOWN (empty hash).
           A 2-arg [] is a string/array slice, never a hash read -- only the
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
             (the actual empty-hash case) -- a syntactic test that is stable
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
      knode = argv[0];
      if (!sp_streq(ty, "IndexOperatorWriteNode")) vnode = nt_ref(nt, id, "value");
    }
    else {
      continue;
    }
    if (recv < 0) continue;
    if (elem_splat_index && nt_kind(nt, recv) != NK_LocalVariableReadNode) continue;
    const char *rty = nt_type(nt, recv);
    /* `(@h ||= {})[k] = v` fills @h exactly as `@h ||= {}; @h[k] = v` does,
       and so does a write through a getter whose value is that or-write.
       Without this the write was no evidence, the empty literal left @h
       boxed, and the boxed receiver's `[]=` bound its arguments to every
       user-defined `[]=` in the program. Once a getter's ivar is a hash, the
       getter branch below takes the write instead: it widens a hash the write
       does not fit, and the literals the ivar is assigned, where folding it
       here left an Integer-keyed hash that dropped a String key (#4902). */
    {
      int orw = recv_hash_or_write(c, recv);
      if (orw >= 0 && !(nt_kind(nt, unwrap_parens(c, recv)) == NK_CallNode &&
                        ty_is_hash(infer_type(c, orw)))) {
        recv = orw;
        rty = nt_kind(nt, orw) == NK_InstanceVariableOrWriteNode ? "InstanceVariableReadNode"
                                                                 : "LocalVariableReadNode";
      }
    }
    /* `x = c.cache; x[k] = v` writes into the hash the getter answers, not
       into a copy: a hash local whose one write is a getter call (or an ivar
       read) takes the write as that source's evidence, through the getter
       branch below. Folding it into the local alone typed the local apart
       from the ivar it aliases, and the C build refused the assignment. */
    if (is_idx_write && !is_splice && rty && sp_streq(rty, "LocalVariableReadNode")) {
      int src = local_hash_alias_source(c, &lw_ix, recv);
      if (src >= 0) { recv = src; rty = nt_type(nt, src); }
    }
    if ((is_push || is_idx_write) && !elem_splat_index) {
      /* each pushed value is its own evidence (`a[0].push(2, "x")`) */
      if (elem_argv && elem_an - elem_from > 1)
        for (int ai = elem_from; ai < elem_an; ai++)
          changed |= widen_nested_literals(c, recv, is_push, is_splice, (TyKind)kt, push_elem_ty(c, elem_argv[ai]));
      else
        changed |= widen_nested_literals(c, recv, is_push, is_splice, (TyKind)kt, (TyKind)vt);
    }
    /* fold into a local's type or an ivar's type (an empty `@buf=[]` filled by
       `@buf << x` infers its element type the same way a local does) */
    TyKind *slot = NULL;
    int slot_reset = 0;  /* slot is a plain local, reset+recomputed per iteration:
                            net change is the stash compare's job, not this site's */
    const char *watch_nm = NULL;  /* ivar name for SP_IVWATCH, NULL for locals */
    int watch_cls = -1;           /* the ivar's class, with watch_nm */
    if (rty && sp_streq(rty, "LocalVariableReadNode")) {
      const char *rnm = nt_str(nt, recv, "name");
      Scope *lsc = rnm ? comp_scope_of(c, recv) : NULL;
      LocalVar *lv = lsc ? scope_local(lsc, rnm) : NULL;
      /* A boxed local or block parameter holds its array the way a boxed
         parameter does: a store of another kind promotes a copy into this
         one slot, and wherever else the array is held keeps it typed. Its
         arrays widen where they are built instead. A value whose kind is
         decided at run time is exempt, as it is for a typed parameter
         (#4481): the boxed store checks it, and raises for a foreign one. */
      if (lv && (!lv->is_param || lv->is_block_param) && lv->type == TY_POLY && (is_push || (is_idx_write && (is_splice || kt == TY_INT))) &&
          vt != TY_UNKNOWN && vt != TY_POLY && !concat_scalar && (!is_splice || is_fill || splice_arr))
        changed |= widen_boxed_array_sources(c, recv, (TyKind)vt, 0);
      /* A proc or lambda literal's boxed parameter written as a Hash
         (`proc { |t| t[:k] = 2 }`): the Hashes its calls hand it take the
         key and value, as a method's boxed parameter's callers do */
      if (lv && lv->is_block_param && lv->type == TY_POLY && is_idx_write && !is_push && !is_splice &&
          kt != TY_UNKNOWN && kt != TY_POLY && kt != TY_INT && vt != TY_UNKNOWN && !g_infer_optimistic) {
        int plit = local_proc_literal_param_of(c, lsc, rnm);
        if (plit >= 0) changed |= widen_proc_call_hash_args(c, plit, rnm, (TyKind)kt, (TyKind)vt);
      }
      if (!lv || lv->is_block_param) continue;
      /* A parameter is typed from its call sites, not from its uses -- except
         that a push through it MUTATES the caller's own array, so an element
         the bound type cannot hold has to widen it (and, through the reverse
         binding in bind_call_params, the caller's local too). Widening only:
         an UNKNOWN parameter still takes its type from the call site (#2989). */
      if (lv->is_param) {
        if ((!is_push && !is_idx_write && !elem_splat_index) || lv->rbs_seeded) continue;
        /* A boxed key or value is exempt below. When it is another parameter
           of the method, boxed because its callers disagree, each call's
           argument for it is still in hand at the binding: record which
           parameter it is, for the binding to check (param_src_misfits). */
        if (!is_splice && vt == TY_POLY)
          changed |= mask_add(&lv->store_val_src, unassigned_param_read(c, lsc, vnode));
        if (!is_splice && !is_push && kt == TY_POLY)
          changed |= mask_add(&lv->store_key_src, unassigned_param_read(c, lsc, knode));
        /* Each value a push, unshift or insert stores is its own evidence,
           and a store of the rest parameter's elements is checked at the
           binding against each call's own rest arguments. */
        if (elem_argv) {
          for (int ai = elem_from; ai < elem_an && !elem_splat_index; ai++)
            if (push_elem_ty(c, elem_argv[ai]) == TY_POLY)
              changed |= mask_add(&lv->store_val_src, unassigned_param_read(c, lsc, elem_argv[ai]));
          int rf = rest_push_first(c, lsc, elem_argv, elem_an, elem_from, elem_splat_index);
          if (rf >= 0 && (lv->store_rest_src == 0 || rf + 1 < lv->store_rest_src)) {
            lv->store_rest_src = rf + 1; changed = 1;
          }
        }
        /* A boxed parameter records what an element write stores, for the
           binding to check each caller's container against; an Integer key
           indexes an array as a push appends to one. A boxed key or value
           is exempt, as it is for a typed parameter. */
        /* A merged hash's boxed keys or values are of kinds nothing here
           knows, not one boxed value the setter converts: evidence. */
        if (is_idx_write && !is_push && lv->type == TY_POLY) {
          if (vt == TY_UNKNOWN || (vt == TY_POLY && !is_merge) || (is_splice && !is_fill && !splice_arr)) continue;
          /* a fill's value and a typed array's spliced elements are
             elements, as a push's are */
          int hkey = !is_splice && kt != TY_UNKNOWN && (kt != TY_POLY || is_merge);
          int elem_ev = is_splice || kt == TY_INT || kt == TY_POLY;
          TyKind *ev[4] = { hkey ? &lv->boxed_store_key : NULL, hkey ? &lv->boxed_store_val : NULL,
                            elem_ev ? &lv->boxed_push_elem : NULL,
                            elem_ev && vt != TY_POLY ? &lv->boxed_known_elem : NULL };
          TyKind got[4] = { kt, vt, vt, vt };
          for (int e = 0; e < 4; e++) {
            if (!ev[e]) continue;
            TyKind was = *ev[e];
            TyKind now = was == TY_UNKNOWN ? got[e] : (was == got[e] ? was : TY_POLY);
            if (now != was) { *ev[e] = now; changed = 1; }
          }
          continue;
        }
        /* A typed hash parameter is the caller's hash: a key or a value its
           variant cannot hold widens it to the poly-keyed variant, and the
           binding widens the caller's hash with it. A boxed key or value is
           exempt, as it is for an ivar's hash: the typed setter converts it. */
        if (is_idx_write && !is_push && !is_splice && ty_is_hash(lv->type) &&
            lv->type != TY_POLY_POLY_HASH) {
          TyKind hkt = kt == TY_POLY && !is_merge ? ty_hash_key(lv->type) : kt;
          TyKind hvt = vt == TY_POLY && !is_merge ? ty_hash_val(lv->type) : vt;
          if (hkt == TY_UNKNOWN || hvt == TY_UNKNOWN) continue;
          TyKind folded = lv->type;
          int fits = fold_container_evidence(&folded, 0, 0, hkt, hvt);
          if (fits && folded == lv->type) continue;
          lv->type = TY_POLY_POLY_HASH; lv->push_widened = 1; changed = 1;
          continue;
        }
        /* An element write through it stores into the caller's array the
           same way: `arr[i] = v` is the push's evidence when the key indexes
           an array, and so is `arr.fill(v)`, whose one value lands in every
           slot of its span, and a splice's of a typed array (`arr[0, 1] =
           ["s"]`), whose elements do: skipped, a String filled into an
           Array[Integer] parameter went to a copy and the caller's array
           never changed, and a splice of another kind was refused. A general
           Array's elements are checked as they land, as a boxed value is. */
        if (is_idx_write && !is_push) {
          if (!ty_is_array(lv->type) || lv->type == TY_POLY_ARRAY) continue;
          if (is_splice ? !is_fill && !splice_arr : kt != TY_INT && kt != TY_POLY) continue;
          if (vt == TY_UNKNOWN || vt == TY_POLY || vt == ty_array_elem(lv->type)) continue;
          lv->type = TY_POLY_ARRAY; lv->push_widened = 1; changed = 1;
          continue;
        }
        /* A BOXED parameter is the same hazard with the container hidden:
           the callee pushes through it into the caller's own array, and the
           container is not visible here to compare against. Record WHAT is
           pushed and let the ivar-widening pass, which can see both sides,
           decide -- an `@rows = []` kept the empty-array default (an int
           array) while a helper pushed objects into it, and the push failed at
           run time. Recording the element rather than widening outright is
           what keeps an ivar that really does hold ints in its typed
           representation. */
        if (lv->type == TY_POLY) {
          if (vt != TY_UNKNOWN) {
            TyKind was = lv->boxed_push_elem;
            TyKind now = (was == TY_UNKNOWN) ? vt : (was == vt ? was : TY_POLY);
            if (now != was) { lv->boxed_push_elem = now; changed = 1; }
          }
          if (vt != TY_UNKNOWN && vt != TY_POLY && !concat_scalar) {
            TyKind was = lv->boxed_known_elem;
            TyKind now = (was == TY_UNKNOWN) ? vt : (was == vt ? was : TY_POLY);
            if (now != was) { lv->boxed_known_elem = now; changed = 1; }
          }
          continue;
        }
        if (!ty_is_array(lv->type) || lv->type == TY_POLY_ARRAY) continue;
        /* A POLY value is exempt, as it is for `[]=` below: it is decided at
           run time and usually the element kind, and the typed push refuses
           a foreign one with TypeError (#4481). Widening the parameter to the
           general Array instead made every typed caller copy its array into
           the call, which is the copy #4480 refuses. */
        if (!(elem_argv && param_push_args_foreign(c, lsc, elem_argv, elem_an, elem_from,
                                                   elem_splat_index, ty_array_elem(lv->type))) &&
            (vt == TY_UNKNOWN || vt == TY_POLY || vt == TY_POLY_ARRAY ||
             vt == ty_array_elem(lv->type))) continue;
        lv->type = TY_POLY_ARRAY; lv->push_widened = 1; changed = 1;
        continue;
      }
      if (elem_splat_index) continue;
      /* A Thread.new or Fiber.new block's parameter is boxed in the body
         (local_is_fiber_block_param), so a push or an element write there
         says only that the value answers `<<` or `[]=`, which a String does
         as well as an Array. Read as "t is a String array", `Thread.new(s)
         { |t| t << x }` pushed into the boxed value as an sp_StrArray *,
         and the C did not compile; an Integer array's push did not either. */
      if ((is_push || is_idx_write) && nfb && local_is_fiber_block_param(c, fb, nfb, lsc, rnm)) continue;
      /* A bare `x[i]` read OR an `x[i] = v` element assignment must not promote
         `x` to a hash if `x` elsewhere gets an array-typed write (`x = a.split`
         etc.): it is an array indexed/assigned by position, and the hash type
         would otherwise collide with it. Mirrors the ivar guard. */
      if (!is_push && lv->type == TY_UNKNOWN) {
        int lsc_sid = (int)(lsc - c->scopes);
        int has_array_write = 0, has_unsettled_write = 0, nwrites = 0;
        for (int _r = lw_index_first(&lw_ix, rnm, lsc_sid); _r >= 0; _r = lw_ix.next[_r]) {
          int w = lw_ix.node[_r];
          if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
          const char *wn = nt_str(nt, w, "name");
          if (!wn || !sp_streq(wn, rnm) || comp_scope_of(c, w) != lsc) continue;
          int wv = nt_ref(nt, w, "value");
          if (wv < 0) continue;
          nwrites++;
          if (ty_is_array(infer_type(c, wv))) { has_array_write = 1; break; }
          /* `xs = src.map { ... }` is an ARRAY once the block's own return
             settles, but it reads UNKNOWN until then -- and an int-keyed
             `xs[i] = v` in between would pin xs to an int-keyed hash. That
             guess is committed the moment xs is passed anywhere, because
             parameters only widen: when the map result later settles on an
             Integer array the two unify to poly and every method the value
             reaches boxes for good. Only the array-producing iterators count
             here; a write that cannot become an array (a hash literal, a
             Hash.new call) is left to the rules that already type it. */
          if (infer_type(c, wv) == TY_UNKNOWN && nt_kind(nt, wv) == NK_CallNode &&
              nt_ref(nt, wv, "block") >= 0 &&
              ty_iter_shape(nt_str(nt, wv, "name")) != TY_ITER_NONE)
            has_unsettled_write = 1;
        }
        if (has_array_write) continue;
        /* A block parameter nothing in the body assigns holds what the
           block's caller passes. For a block its callee keeps as a Proc, that
           caller is out of sight, and a `p[:k] = v` in the body says nothing
           about what it is: the block of `Agg.new { |ctx, v| ctx[:n] = 1 }`
           may well be handed an object with its own `[]=`, and a hash
           guessed here read that object as a hash's table. */
        if (nwrites == 0 && local_is_kept_block_param(c, lsc, rnm)) continue;
        /* while the fixpoint runs; the second stage (g_infer_optimistic
           cleared) still types a slot whose array evidence never arrived */
        if (g_infer_optimistic && has_unsettled_write) continue;
      }
      /* A proc or lambda literal's parameter holds what the Proc's caller
         passes, and that caller can be out of sight: the Proc handed to a
         method that calls it (`def fw(f, s) = f.call(s)`), or called through
         another local. A push or an element write in the body then says
         only that the value answers `<<` or `[]=`, which a String does as
         well as an Array. Read as "a is a String array", `proc { |a| a <<
         "x" }` laundered the String such a call passed out of the slot as an
         array, and the push aborted; `a[0] = "z"` crashed the same way. Boxed,
         the operator takes whichever arrives. A literal whose every call is in
         sight keeps the evidence: those calls bind the parameter as well.
         Set outright, as a plain local is reset each round and the stash
         compare sees the change. */
      if (is_push || is_idx_write) {
        int assigned = 0;
        for (int _r = lw_index_first(&lw_ix, rnm, (int)(lsc - c->scopes)); _r >= 0 && !assigned; _r = lw_ix.next[_r]) {
          int w = lw_ix.node[_r];
          const char *wn = nt_str(nt, w, "name");
          assigned = nt_kind(nt, w) == NK_LocalVariableWriteNode && wn && sp_streq(wn, rnm) &&
                     comp_scope_of(c, w) == lsc;
        }
        int lit = assigned ? -1 : local_proc_literal_param_of(c, lsc, rnm);
        if (lit >= 0 && !proc_literal_calls_in_sight(c, lit)) { lv->type = TY_POLY; continue; }
      }
      /* A row read out of a narrowed table (`row = @t[k]` over an Integer or
         Float table), or a local copied from one (`al = row`, table_row_alias),
         is that row, not a copy: a boxed value pushed into it is
         unboxed as the typed push unboxes one, raising on a foreign element,
         and a local narrowed from the table keeps its kind the same way (its
         oa_pin). Widened to the general Array, the read became a converted
         copy and the pushes never reached the table: under
         --int-overflow=promote a computed Integer is boxed, so `row << k * 10`
         filled nothing. */
      if (is_push && vt == TY_POLY && ty_is_array(lv->type) && lv->type != TY_POLY_ARRAY) {
        if (table_row_alias(c, &lw_ix, rnm, lsc, 0)) continue;
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
      TyKind ivt = (TyKind)vt;
      slot = ivar_evidence_slot(c, &ivw_ix, ivar_cls_id, inm, is_push, &ivt);
      if (!slot) continue;
      vt = ivt;
      watch_nm = inm;
      watch_cls = ivar_cls_id;
    }
    else if ((is_push || is_idx_write) && rty &&
             (sp_streq(rty, "GlobalVariableReadNode") || sp_streq(rty, "ClassVariableReadNode") ||
              sp_streq(rty, "ConstantReadNode"))) {
      /* `$g[i] = v`, `@@a << v`, `TABLE[i] = v`: the slot is typed from its
         writes alone, so an element its array kind cannot hold widens it to
         the general Array here, as it does a local's. Only the widening is
         taken: seeding or re-keying these slots stays with their write
         passes, which keep a general Array against a typed array write. */
      TyKind *nslot = named_array_slot(c, recv);
      if (!nslot || !ty_is_array(*nslot) || *nslot == TY_POLY_ARRAY) continue;
      TyKind nt2 = *nslot;
      fold_container_evidence(&nt2, is_push, is_splice, (TyKind)kt, (TyKind)vt);
      if (nt2 == TY_POLY_ARRAY) { *nslot = TY_POLY_ARRAY; changed = 1; }
      continue;
    }
    else if ((is_push || is_idx_write) && rty && sp_streq(rty, "CallNode") &&
             nt_ref(nt, recv, "block") < 0) {
      /* `getter_method << x` / `getter_method[k] = v` where the getter
         returns @ivar: trace through to that ivar so the container it holds
         takes the evidence, as `@ivar << x` / `@ivar[k] = v` would. */
      int recv_args = nt_ref(nt, recv, "arguments");
      int recv_argc = 0;
      if (recv_args >= 0) nt_arr(nt, recv_args, "arguments", &recv_argc);
      if (recv_argc != 0) continue;
      const char *mname = nt_str(nt, recv, "name");
      if (!mname) continue;
      /* The class the getter belongs to: the RECEIVER's when the push names one
         (`dest.synapses_in << s` from outside), else the enclosing class for
         the implicit-self form (`list << msg`). Without the receiver case an
         ivar array filled only from outside kept its empty literal's default
         and every element read back as an Integer (#3781). */
      int gcid = -1, cside = 0;
      int all_owners = 0;
      int grecv = nt_ref(nt, recv, "receiver");
      if (grecv >= 0 && nt_kind(nt, grecv) == NK_ConstantReadNode) {
        /* `Tbl.cache[k] = v`: the class-side getter */
        const char *cnm = nt_str(nt, grecv, "name");
        gcid = cnm ? comp_class_index(c, cnm) : -1;
        if (gcid < 0) continue;
        cside = 1;
      }
      else if (grecv >= 0 && !(nt_type(nt, grecv) && sp_streq(nt_type(nt, grecv), "SelfNode"))) {
        TyKind grt = infer_type(c, grecv);
        gcid = ty_is_object(grt) ? ty_object_class(grt) : -1;
        /* An index write through a receiver of a known non-object type is
           no user getter's; a boxed or still-settling one may be any class
           that owns the getter. */
        if (gcid < 0 && !is_push && grt != TY_POLY && grt != TY_UNKNOWN) continue;
        /* The receiver's own type may still be settling (a block parameter over
           an array whose element type is what this evidence decides). Fall back
           to the class that owns this getter when exactly one does -- with no
           ambiguity there is nothing else it could be. */
        if (gcid < 0) {
          int owner = -1, nown = 0;
          for (int k = 0; k < c->nclasses && (nown < 2 || !is_push); k++) {
            if (!class_owns_getter(c, k, mname)) continue;
            owner = k;
            nown++;
          }
          /* `[a, b].each { |o| o.cache[k] = v }`: the receiver is any of the
             owners, so each owner's getter ivar takes the write, as a write
             through a typed receiver would (the targets below). */
          if (!is_push && nown != 1) {
            if (nown == 0) continue;
            all_owners = 1;
          }
          else if (nown != 1) {
            /* Several classes own this getter and the receiver's class is only
               known at run time: apply the evidence to EVERY owner. Leaving
               them narrow stored a boxed object into an int-array slot, and the
               elements read back as Integers (#3781). Widening is monotone, so
               the fixpoint still settles. */
            TyKind pvt = (vt == TY_STRBUF) ? TY_STRING : vt;
            if (pvt == TY_UNKNOWN) continue;
            for (int k = 0; k < c->nclasses; k++) {
              int kd = k;
              const char *ivn = NULL;
              char rbuf[300];
              int gm = comp_method_in_chain(c, k, mname, &kd);
              if (gm >= 0) {
                int last = scope_body_last(c, gm);
                if (last < 0 || nt_kind(nt, last) != NK_InstanceVariableReadNode) continue;
                ivn = nt_str(nt, last, "name");
              }
              else if (comp_reader_in_chain(c, k, mname, &kd)) {
                snprintf(rbuf, sizeof rbuf, "@%s", mname);
                ivn = rbuf;
              }
              else continue;
              if (!ivn || kd < 0 || kd >= c->nclasses) continue;
              ClassInfo *ck = &c->classes[kd];
              int ivi = comp_ivar_index(ck, ivn);
              if (ivi < 0 || class_ivar_pinned(ck, ivn)) continue;
              if (!ty_is_array(ck->ivar_types[ivi])) continue;   /* an established array slot only */
              if (ck->ivar_types[ivi] == TY_POLY_ARRAY) continue;
              if (ck->ivar_types[ivi] != ty_array_of(pvt)) {
                ck->ivar_types[ivi] = TY_POLY_ARRAY;
                changed = 1;
              }
            }
            continue;
          }
          gcid = owner;
        }
      }
      else {
        Scope *caller = comp_scope_of(c, recv);
        if (!caller || caller->class_id < 0) continue;
        gcid = caller->class_id;
        cside = caller->is_cmethod;
      }
      if (!all_owners && (gcid < 0 || gcid >= c->nclasses)) continue;
      /* The write lands in whichever ivar the getter returns, and a subclass
         override can return another one: credit the ivar of every class the
         call can dispatch to, the same evidence `@c[k] = v` in that getter's
         class would be. A class whose getter is not a plain ivar read (or
         `||=`) keeps its container to itself and takes none. Sized for every
         (class, ivar) pair, so no target is dropped. */
      int cap = 1;
      for (int k = 0; k < c->nclasses; k++) cap += c->classes[k].nivars;
      int *tcls = malloc(sizeof(int) * cap * 2);
      const char **tiv = malloc(sizeof(const char *) * cap * 2);
      int *gcls = tcls + cap;
      const char **giv = tiv + cap;
      int ntg = 0;
      for (int k = all_owners ? 0 : gcid; k < (all_owners ? c->nclasses : gcid + 1); k++) {
        if (all_owners && !class_owns_getter(c, k, mname)) continue;
        int ng = getter_ivar_targets(c, k, cside, mname, gcls, giv, cap);
        for (int j = 0; j < ng; j++) {
          int seen = 0;
          for (int t = 0; t < ntg && !seen; t++)
            seen = tcls[t] == gcls[j] && sp_streq(tiv[t], giv[j]);
          if (!seen) { tcls[ntg] = gcls[j]; tiv[ntg] = giv[j]; ntg++; }
        }
      }
      if (ntg <= 0) { free(tcls); free(tiv); continue; }
      /* a shared-mutable string spends its handle at a typed array's boundary,
         so the element slot stays a plain string (#3227) */
      if (is_push && vt == TY_STRBUF) vt = TY_STRING;
      for (int ti = 0; ti < ntg; ti++) {
        ClassInfo *ci2 = &c->classes[tcls[ti]];
        int iv2 = comp_ivar_index(ci2, tiv[ti]);
        if (iv2 < 0) continue;
        TyKind tvt = (TyKind)vt;
        if (is_push) {
          /* Only an ARRAY slot takes element evidence from a push: `q.push(1)`
             on a Queue attribute is not an array fill. The slot must say it is
             one -- already typed as an array, or written with one -- since a
             slot that is still settling (`@q = nil` before `@q = Queue.new`)
             reads UNKNOWN. */
          if (!ty_is_array(ci2->ivar_types[iv2]) &&
              !ivar_has_array_write(c, &ivw_ix, tcls[ti], tiv[ti])) {
            /* a subclass's slot reads UNKNOWN when only its superclass
               writes the ivar: that class's writes say what it holds */
            int arr = 0;
            for (int pk = c->classes[tcls[ti]].parent; pk >= 0 && !arr; pk = c->classes[pk].parent) {
              int piv = comp_ivar_index(&c->classes[pk], tiv[ti]);
              arr = piv >= 0 && (ty_is_array(c->classes[pk].ivar_types[piv]) ||
                                 ivar_has_array_write(c, &ivw_ix, pk, tiv[ti]));
            }
            if (!arr) continue;
          }
        }
        /* An index write only reshapes a container the slot already is: the
           ivar's own writes decide what it holds, and a typeless slot keeps
           whatever they decide. */
        else if (!ty_is_hash(ci2->ivar_types[iv2]) && !ty_is_array(ci2->ivar_types[iv2])) continue;
        TyKind *tslot = ivar_evidence_slot(c, &ivw_ix, tcls[ti], tiv[ti], is_push, &tvt);
        if (!tslot) continue;
        TyKind tbefore = *tslot;
        /* A boxed value is exempt, as it is for an array element: the typed
           setter converts it at run time (#651). */
        if (!is_push && tvt == TY_POLY && ty_is_hash(tbefore)) tvt = ty_hash_val(tbefore);
        int fits = fold_container_evidence(tslot, is_push, is_splice, (TyKind)kt, tvt);
        /* A hash the evidence does not fit widens to the poly-keyed variant,
           and so do the hash literals the ivar is assigned: they take their
           variant from the ivar's own `@c[k] = v` sites, which never see this
           write, and a literal narrower than its slot unifies with it to a
           plain boxed value that drops the foreign key. The fold refusing a
           settled key and value is a misfit too: it leaves an Integer-keyed
           slot as it is for a Symbol key. */
        if (!is_push && ty_is_hash(tbefore) &&
            (*tslot != tbefore || (!fits && kt != TY_UNKNOWN && tvt != TY_UNKNOWN))) {
          *tslot = TY_POLY_POLY_HASH;
          widen_ivar_hash_literals(c, &ivw_ix, tcls[ti], tiv[ti]);
        }
        sp_ivwatch(tiv[ti], is_push ? "getter_push" : "getter_idxwrite", tbefore, *tslot);
        if (*tslot != tbefore) changed = 1;
        if (tbefore != TY_POLY_ARRAY && *tslot == TY_POLY_ARRAY)
          changed |= widen_ivar_array_params(c, &ivw_ix, tcls[ti], tiv[ti]);
      }
      free(tcls); free(tiv);
      continue;
    }
    else continue;

    /* The slot's own `Hash.new(default)`: the default is a value the hash
       answers, so it is part of the value type the `[]=` writes establish.
       Without it `h = Hash.new("none"); h["b"] = 1` made the slot a StrInt
       hash and the C build stopped on the string handed to its Integer
       constructor; with it the value widens to poly and the default is
       carried boxed, which is what the global and constant slots already do.
       A push or a splice is array evidence and has no default to consult. */
    if (!is_push && !is_splice) {
      int dn = recv_hash_new_default_arg(c, recv);
      if (dn >= 0) vt = ty_unify(vt, hash_default_value_ty(c, dn));
    }
    TyKind before = *slot;
    int fits = fold_container_evidence(slot, is_push, is_splice, kt, vt);
    /* An ivar hash the write does not fit widens to the poly-keyed variant,
       with the literals the ivar is assigned, as a write through its getter
       does: other sites (a getter's `c[1] = 2`) settled the key, and the fold
       refusing a Symbol key into an Integer-keyed slot -- or trading the key
       for a String one -- left the direct write to a hash that cannot hold
       it. So does any other variant change of an ivar assigned a parameter:
       the parameter is typed by its callers, and can only follow to the
       poly-keyed variant. */
    if (watch_nm && !is_push && !is_splice && ty_is_hash(before) &&
        before != TY_POLY_POLY_HASH &&
        ((!fits && kt != TY_UNKNOWN && vt != TY_UNKNOWN) ||
         (ty_is_hash(*slot) && ty_hash_key(*slot) != ty_hash_key(before)) ||
         (*slot != before && ivar_has_param_write(c, &ivw_ix, watch_cls, watch_nm)))) {
      *slot = TY_POLY_POLY_HASH;
      widen_ivar_hash_literals(c, &ivw_ix, watch_cls, watch_nm);
      sp_ivwatch(watch_nm, "usage_idxwrite_misfit", before, *slot);
      changed = 1;
      continue;
    }
    if (!fits) continue;
    sp_ivwatch(watch_nm, is_push ? "usage_push" : (is_idx_write ? "usage_idxwrite" : "usage_read"), before, *slot);
    if (*slot != before && !slot_reset) changed = 1;
    if (watch_nm && before != TY_POLY_ARRAY && *slot == TY_POLY_ARRAY)
      changed |= widen_ivar_array_params(c, &ivw_ix, watch_cls, watch_nm);
    /* A LOCAL that widened to the poly array under a push and whose writes
       read ivar arrays (directly, or through a conditional's arms) is an
       ALIAS of those arrays: widen the sources too, or the local's read
       becomes a rebuilt copy and the push mutates the copy while the
       receiver's own array answers unchanged (#4210's conditional shape). */
    if (is_push && *slot == TY_POLY_ARRAY &&
        rty && sp_streq(rty, "LocalVariableReadNode")) {
      const char *bn2 = nt_str(nt, recv, "name");
      Scope *bs2 = bn2 ? comp_scope_of(c, recv) : NULL;
      int bcls = bs2 ? bs2->class_id : -1;
      if (bcls < 0) bcls = comp_class_index(c, "Toplevel");
      if (bn2 && bs2 && bcls >= 0) {
        int bsid = (int)(bs2 - c->scopes);
        for (int _r = lw_index_first(&lw_ix, bn2, bsid); _r >= 0; _r = lw_ix.next[_r]) {
          int _wi = lw_ix.node[_r];
          if (nt_kind(nt, _wi) != NK_LocalVariableWriteNode) continue;
          const char *_wn = nt_str(nt, _wi, "name");
          if (!_wn || !sp_streq(_wn, bn2) || comp_scope_of(c, _wi) != bs2) continue;
          if (widen_aliased_array_ivars(c, nt_ref(nt, _wi, "value"), bcls)) changed = 1;
        }
      }
    }
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
        /* Both are reset locals, so the sweep reports; `prop` still drives
           this loop's own re-run to a fixed point within the round. */
        if (dst->type == TY_POLY_ARRAY) { src->type = TY_POLY_ARRAY; prop = 1; }
        else if (src->type == TY_POLY_ARRAY) { dst->type = TY_POLY_ARRAY; prop = 1; }
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
    if (merged != lv->type) lv->type = merged;
  }

  /* Second pass: re-compute proc_ret for proc-typed locals after body-internal
     locals have been typed. The first pass resets all locals to TY_UNKNOWN, so
     computing proc_ret there would see stale TY_UNKNOWN for variables assigned
     inside the proc body. Running after the first pass ensures those locals
     have their correct types (e.g. `x = 10` -> TY_INT) before proc_node_ret
     evaluates the body's return type.
     A local written with two procs of different return types takes the
     last write's, as before; what counts as a change is the value the pass
     leaves against the one it found. Comparing write by write reported both
     writes of `sh = ->(x) { x }; sh = ->(a, b, c) { a + b + c }` as changes
     every round, and the fixpoint ran to its cap (#4962). */
  {
    int npr = 0, cappr = 0;
    LocalVar **pr_lv = NULL; int *pr_old = NULL;
    NT_FOREACH_KIND(nt, NK_LocalVariableWriteNode, id) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm) continue;
      LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
      if (!lv || lv->type != TY_PROC) continue;
      int vnode = nt_ref(nt, id, "value");
      TyKind pr = vnode >= 0 ? proc_ret_of(c, vnode) : TY_UNKNOWN;
      if (pr == TY_UNKNOWN || (TyKind)lv->proc_ret == pr) continue;
      int seen = 0;
      for (int k = 0; k < npr && !seen; k++) if (pr_lv[k] == lv) seen = 1;
      if (!seen) {
        if (npr == cappr) {
          cappr = cappr ? cappr * 2 : 8;
          pr_lv = (LocalVar **)realloc(pr_lv, sizeof(LocalVar *) * (size_t)cappr);
          pr_old = (int *)realloc(pr_old, sizeof(int) * (size_t)cappr);
          if (!pr_lv || !pr_old) { fprintf(stderr, "oom\n"); exit(1); }
        }
        pr_lv[npr] = lv; pr_old[npr] = lv->proc_ret; npr++;
      }
      lv->proc_ret = (int)pr;
    }
    for (int k = 0; k < npr; k++) if (pr_lv[k]->proc_ret != pr_old[k]) changed = 1;
    free(pr_lv); free(pr_old);
  }

  /* A slot already promoted to the append handle keeps that REPRESENTATION
     across the recompute frame. The reset above re-derives `out = +""` as a
     plain TY_STRING, and promote_append_accumulators -- which fires on a
     TY_STRING slot -- then re-promotes it, so the two passes trade the slot
     back and forth and the fixpoint never converges: every compile ran to the
     128-iteration cap. TY_STRBUF *is* a String; only the storage differs, so
     re-deriving the Ruby-level type must not clobber the choice. Same reason
     infer_bigint_loop_locals re-seeds inside this frame. If the re-derived
     type is neither String nor the handle, the promotion's precondition is
     genuinely gone and the flag goes with it. */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++) {
      LocalVar *lv = &c->scopes[s].locals[i];
      if (!lv->str_append) continue;
      if (lv->type == TY_STRING) lv->type = TY_STRBUF;
      else if (lv->type != TY_STRBUF) lv->str_append = 0;
    }

  /* The same contract for a slot narrowed to a pointer array. The reset
     re-derives it from its writes, which still read the poly array, and the
     two array KINDS unify to the plain poly SCALAR -- strictly worse than
     either. The narrowing is a decision about the slot, not about any one
     write, so re-assert it inside the frame; a slot that no longer even
     derives as a poly array has lost the precondition and the pin with it.
     Without this the two passes trade the slot back and forth and every
     compile ran to the 128-iteration cap. */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++) {
      LocalVar *lv = &c->scopes[s].locals[i];
      if (lv->oa_pin == TY_UNKNOWN) continue;
      /* TY_POLY too: a local narrowed to a container's ELEMENT type re-derives
         as the plain poly scalar the read hands back, exactly as a narrowed
         container re-derives as the poly array. */
      if (lv->type == TY_POLY_ARRAY || lv->type == TY_POLY || lv->type == lv->oa_pin)
        lv->type = lv->oa_pin;
      else lv->oa_pin = TY_UNKNOWN;
    }

  /* The same again for the empty-`{}` argument a TY_POLY parameter widened to
     the PolyPoly hash (#3158). The caller's own element writes (`h["k"] = v`)
     re-derive the slot as the narrower StrStr kind every round, and the
     reverse binding widened it back on the next -- to the cap, and the
     callee's writes through the reference were dropped, which is the very
     bug the binding exists to fix. A slot that no longer derives as a hash
     at all has lost the precondition and the pin with it. */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++) {
      LocalVar *lv = &c->scopes[s].locals[i];
      if (!lv->poly_hash_pin) continue;
      if (lv->type == TY_UNKNOWN || lv->type == TY_POLY || ty_is_hash(lv->type))
        lv->type = TY_POLY_POLY_HASH;
      else lv->poly_hash_pin = 0;
    }

  /* And for the array-literal local a widened parameter took (poly_array_pin):
     its literals re-derive a typed array kind every round. */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++) {
      LocalVar *lv = &c->scopes[s].locals[i];
      if (!lv->poly_array_pin) continue;
      if (lv->type == TY_UNKNOWN || lv->type == TY_POLY || ty_is_array(lv->type))
        lv->type = TY_POLY_ARRAY;
      else lv->poly_array_pin = 0;
    }

  /* A slot this round could not derive AT ALL keeps what it had. The reset at
     the top exists so a slot can narrow when better evidence arrives, and a
     narrowed slot is concrete -- so restoring the stash where the answer came
     out UNKNOWN cannot block a narrowing, and it stops the round from throwing
     away what the rest of the program already established. Without it a write
     the pass has no rule for (`g = Hash.new(99)`) dropped its slot every round,
     another pass settled it again, and the fixpoint ran to its cap (#4116).
     The empty-collection write above already does this for its own case; this
     is the same rule without the special case. */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++) {
      LocalVar *lv = &c->scopes[s].locals[i];
      if (lv->is_param || lv->is_block_param || lv->rbs_seeded) continue;
      if (lv->type == TY_UNKNOWN && (TyKind)lv->gc_root != TY_UNKNOWN)
        lv->type = (TyKind)lv->gc_root;
    }

  /* The shared-mutable / append-accumulator promotion is durable -- the mark
     is, and analyze re-asserts the slot type from it after the fixpoint. Do it
     HERE too, before the comparison below: the reset at the top of this pass
     wipes the promotion, the writes re-derive TY_STRING, and
     promote_shared_stored_strings puts TY_STRBUF back later in the same round.
     Neither yielded, so the fixpoint ran to its cap on every program with a
     shared-mutable string (#3227, #4116). Same treatment oa_pin gets just
     above, and for the same reason. */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++) {
      LocalVar *lv = &c->scopes[s].locals[i];
      if ((lv->str_shared || lv->str_append) &&
          (lv->type == TY_STRING || lv->type == TY_STR_ARRAY)) lv->type = TY_STRBUF;
    }

  /* Detect change vs the stashed old types -- over EXACTLY the slots the reset
     above stashed. It used to skip only params and block params, so a slot the
     reset skips for the third reason, rbs_seeded, was compared against a
     gc_root nothing ever wrote: 0, which reads as UNKNOWN, against whatever
     type the slot really has. That answered "changed" every round for every
     desugar-synthesized temp (`__ie_*` for instance_eval, `__cd_sav_*` for
     Dir.chdir), and the fixpoint ran to its cap on any program with one
     (#4116). */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++) {
      LocalVar *lv = &c->scopes[s].locals[i];
      if (lv->is_param || lv->is_block_param || lv->rbs_seeded) continue;
      if ((TyKind)lv->gc_root != lv->type) changed = 1;
    }
  lw_index_free(&lw_ix);
  lw_index_free(&ivw_ix);
  g_infer_write_round = 0;
  free(fb);
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
  /* a variadic wrapper's rest holds the arguments boxed (bam_call_argc) */
  for (int k = 0; k < argc && k + shift < m->nparams && k + shift != m->rest_idx; k++) {
    LocalVar *p = scope_local(m, m->pnames[k + shift]);
    if (!p || p->rbs_seeded) continue;
    TyKind at = infer_type(c, argv[k]);
    if (at == TY_VOID || at == TY_NIL) at = TY_POLY;
    if (lv_widen(p, at)) changed = 1;
  }
  return changed;
}

static void mark_body_self(const NodeTable *nt, int id, int owner, int *body_self, int depth) {
  if (id < 0 || depth > 400) return;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode) return;
  if (k == NK_SelfNode) { body_self[id] = owner; return; }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) mark_body_self(nt, nt_ref_at(nt, id, i), owner, body_self, depth + 1);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++) mark_body_self(nt, ids[j], owner, body_self, depth + 1);
  }
}

static int name_in(const char *n, const char *const *set) {
  for (int i = 0; n && set[i]; i++) if (sp_streq(n, set[i])) return 1;
  return 0;
}

/* Is the value of node `u` handed on -- stored, passed, returned -- rather
   than only consumed where it stands? Consumed: the receiver of a call that
   does not hand its receiver on, an interpolated part, the argument of an
   output, comparison or type-test call, the parent of a `K::X` path, a class
   definition's name or superclass, a `when` or `rescue` clause, a statement
   whose value is dropped. A conditional, `case` arm, `and`/`or`,
   parentheses or a block of statements passes its last value up to be asked
   the same; a value chosen by a conditional, `case` or `and`/`or` is handed
   on to the receiver it becomes, as that receiver is no longer one class. */
static int value_handed_on(const NodeTable *nt, const int *parent, int u) {
  static const char *const passes_recv[] = {
    "method", "public_method", "send", "public_send", "__send__", "then", "tap",
    "itself", "yield_self", "freeze", "dup", "clone", "instance_exec",
    "instance_eval", "class_exec", "class_eval", "module_eval", NULL };
  static const char *const consumes_args[] = {
    "puts", "print", "p", "pp", "warn", "raise", "format", "sprintf", "printf",
    "is_a?", "kind_of?", "instance_of?", "include", "extend", "prepend",
    "include?", "==", "!=", "===", "equal?", "eql?", "<", "<=", ">", ">=", "<=>", NULL };
  int chosen = 0;
  for (int depth = 0; depth < 400; depth++) {
    int p = parent[u];
    if (p < 0) return 0;
    NodeKind pk = nt_kind(nt, p);
    const char *pt = nt_type(nt, p);
    if (pk == NK_StatementsNode) {
      int n = 0; const int *body = nt_arr(nt, p, "body", &n);
      if (n > 0 && body[n - 1] != u) return 0;
      u = p; continue;
    }
    if (pk == NK_ParenthesesNode || pk == NK_BeginNode ||
        (pt && sp_streq(pt, "EmbeddedStatementsNode"))) {
      u = p; continue;
    }
    if (pk == NK_IfNode || pk == NK_UnlessNode || pk == NK_AndNode || pk == NK_OrNode ||
        (pt && sp_streq(pt, "ElseNode"))) {
      if ((pk == NK_IfNode || pk == NK_UnlessNode) && nt_ref(nt, p, "predicate") == u) return 0;
      chosen = 1; u = p; continue;
    }
    if (pt && (sp_streq(pt, "WhenNode") || sp_streq(pt, "InNode"))) {
      if (nt_ref(nt, p, "statements") != u) return 0;
      chosen = 1; u = p; continue;
    }
    if (pt && (sp_streq(pt, "CaseNode") || sp_streq(pt, "CaseMatchNode"))) {
      if (nt_ref(nt, p, "predicate") == u) return 0;
      chosen = 1; u = p; continue;
    }
    if (pt && (sp_streq(pt, "InterpolatedStringNode") || sp_streq(pt, "InterpolatedSymbolNode") ||
               sp_streq(pt, "InterpolatedXStringNode") ||
               sp_streq(pt, "InterpolatedRegularExpressionNode") ||
               sp_streq(pt, "ProgramNode")))
      return 0;
    if (pk == NK_ClassNode || pk == NK_ModuleNode || pk == NK_ConstantPathNode ||
        pk == NK_RescueNode)
      return 0;
    /* the `self` of `def self.m` names where m is defined; it is no value
       (taken as one, every class with a class method escaped, #5272) */
    if (pk == NK_DefNode && nt_ref(nt, p, "receiver") == u) return 0;
    if (pk == NK_CallNode) {
      if (nt_ref(nt, p, "receiver") == u) return chosen || name_in(nt_str(nt, p, "name"), passes_recv);
      return 1;
    }
    if (pt && sp_streq(pt, "ArgumentsNode")) {
      int call = parent[p];
      if (call >= 0 && nt_kind(nt, call) == NK_CallNode &&
          name_in(nt_str(nt, call, "name"), consumes_args)) return 0;
      return 1;
    }
    return 1;
  }
  return 1;
}

/* Can a Class value of class `cid` reach a receiver the analysis cannot pin?
   Only if some expression producing it is handed on (value_handed_on): a
   constant naming it, `self` in its class body or in a class method of it or
   an ancestor. `.class`, `superclass` or `singleton_class` handed on, and
   the reflective readers (`const_get`, `subclasses`, an `inherited` hook),
   are taken to hand out any class. */
int class_value_escapes(Compiler *c, int cid) {
  const NodeTable *nt = c->nt;
  static char *esc = NULL;
  static int esc_n = -1, esc_count = -1, esc_round = -1;
  extern int g_fixpoint_rounds;
  if (cid < 0 || cid >= c->nclasses) return 1;
  /* recomputed once per fixpoint round: the receiver types the `.class`
     narrowing below reads settle as the rounds go */
  if (esc && esc_n == c->nclasses && esc_count == nt->count && esc_round == g_fixpoint_rounds)
    return esc[cid];
  free(esc);
  esc = (char *)calloc((size_t)(c->nclasses > 0 ? c->nclasses : 1), 1);
  size_t nn = (size_t)(nt->count > 0 ? nt->count : 1);
  int *parent = (int *)malloc(sizeof(int) * nn);
  int *body_self = (int *)malloc(sizeof(int) * nn);
  if (!esc || !parent || !body_self) {
    free(parent); free(body_self); free(esc); esc = NULL; return 1;
  }
  esc_n = c->nclasses; esc_count = nt->count; esc_round = g_fixpoint_rounds;
  for (int u = 0; u < nt->count; u++) parent[u] = body_self[u] = -1;
  /* parents come from a walk down from the root: a desugar that repoints a
     call's receiver past its parentheses leaves the old ParenthesesNode
     holding the same child, and read off every node that child's parent
     was the orphan, so `(c ? A : B).new` never reached the call and A stayed
     unconstructed (#5272) */
  {
    int *stack = (int *)malloc(sizeof(int) * nn);
    int sp = 0;
    if (!stack) { free(parent); free(body_self); free(esc); esc = NULL; return 1; }
    if (nt->root_id >= 0 && nt->root_id < nt->count) stack[sp++] = nt->root_id;
    while (sp > 0) {
      int u = stack[--sp];
      int nr = nt_num_refs(nt, u);
      for (int i = 0; i < nr; i++) {
        int ch = nt_ref_at(nt, u, i);
        if (ch >= 0 && ch < nt->count && parent[ch] < 0 && ch != nt->root_id) { parent[ch] = u; stack[sp++] = ch; }
      }
      int na = nt_num_arrs(nt, u);
      for (int i = 0; i < na; i++) {
        int n = 0; const int *ids = nt_arr_at(nt, u, i, &n);
        for (int j = 0; j < n; j++)
          if (ids[j] >= 0 && ids[j] < nt->count && parent[ids[j]] < 0 && ids[j] != nt->root_id) { parent[ids[j]] = u; stack[sp++] = ids[j]; }
      }
    }
    free(stack);
  }
  for (int u = 0; u < nt->count; u++) {
    NodeKind k = nt_kind(nt, u);
    if (k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode) {
      int owner = -2;   /* a singleton class body's self: any class */
      if (k != NK_SingletonClassNode) {
        int cp = nt_ref(nt, u, "constant_path");
        owner = cp >= 0 ? comp_class_index(c, nt_str(nt, cp, "name")) : -1;
        if (owner < 0) owner = -2;
      }
      mark_body_self(nt, nt_ref(nt, u, "body"), owner, body_self, 0);
    }
  }
  /* A node a rewrite unwrapped (`(c ? A : B).new` taking the conditional as
     its receiver) still has the parent it had inside the node left behind,
     which leads nowhere: the tree hanging from the program gives the parent
     the code has. */
  int *stack = (int *)malloc(sizeof(int) * nn);
  char *seen = (char *)calloc(nn, 1);
  if (stack && seen && nt->root_id >= 0 && nt->root_id < nt->count) {
    int sp = 0;
    stack[sp++] = nt->root_id; seen[nt->root_id] = 1;
    while (sp > 0) {
      int u = stack[--sp];
      int nr = nt_num_refs(nt, u);
      for (int i = 0; i < nr; i++) {
        int ch = nt_ref_at(nt, u, i);
        if (ch < 0 || ch >= nt->count || seen[ch]) continue;
        seen[ch] = 1; parent[ch] = u; stack[sp++] = ch;
      }
      int na = nt_num_arrs(nt, u);
      for (int i = 0; i < na; i++) {
        int n = 0; const int *ids = nt_arr_at(nt, u, i, &n);
        for (int j = 0; j < n; j++) {
          int ch = ids[j];
          if (ch < 0 || ch >= nt->count || seen[ch]) continue;
          seen[ch] = 1; parent[ch] = u; stack[sp++] = ch;
        }
      }
    }
  }
  free(stack); free(seen);
  static const char *const reflective[] = {
    "inherited", "const_get", "subclasses", "descendants", "each_object", NULL };
  int all = 0;
  for (int u = 0; u < nt->count && !all; u++) {
    NodeKind k = nt_kind(nt, u);
    if (k == NK_CallNode && name_in(nt_str(nt, u, "name"), reflective)) {
      /* `const_get(:Get)` / `const_get("A::Get")` names the one class it
         hands out: only that class escapes. Taken as any class, one
         literal lookup anywhere bound every `k.new(x)` into every class's
         initialize, and Campfire's BCrypt::Password took an untyped URL
         (#5217). A name computed at run time can be any class. */
      if (sp_streq(nt_str(nt, u, "name"), "const_get")) {
        int an = 0, ag = nt_ref(nt, u, "arguments");
        const int *av = ag >= 0 ? nt_arr(nt, ag, "arguments", &an) : NULL;
        NodeKind ak = an >= 1 ? nt_kind(nt, av[0]) : NK_NilNode;
        const char *lit = NULL;
        if (an >= 1 && ak == NK_SymbolNode) lit = nt_str(nt, av[0], "value");
        else if (an >= 1 && ak == NK_StringNode) lit = nt_str(nt, av[0], "content");
        if (lit) {
          const char *leaf = strrchr(lit, ':');
          leaf = leaf ? leaf + 1 : lit;
          for (int d = 0; d < c->nclasses; d++)
            if (c->classes[d].name && sp_streq(c->classes[d].name, leaf)) esc[d] = 1;
          continue;
        }
      }
      all = 1; break;
    }
    if (k != NK_ConstantReadNode && k != NK_ConstantPathNode && k != NK_SelfNode &&
        k != NK_CallNode) continue;
    if (k == NK_CallNode) {
      const char *un = nt_str(nt, u, "name");
      int rc = nt_ref(nt, u, "receiver");
      const char *rn = rc >= 0 && nt_kind(nt, rc) == NK_ConstantReadNode ? nt_str(nt, rc, "name") : NULL;
      /* an anonymous class, unless a constant names it */
      int anon = un && rn && ((sp_streq(un, "new") && (sp_streq(rn, "Class") || sp_streq(rn, "Struct"))) ||
                              (sp_streq(un, "define") && sp_streq(rn, "Data")));
      if (anon && parent[u] >= 0 && nt_type(nt, parent[u]) &&
          sp_streq(nt_type(nt, parent[u]), "ConstantWriteNode")) continue;
      if (!anon && (!un || !(sp_streq(un, "class") || sp_streq(un, "superclass") ||
                             sp_streq(un, "singleton_class")))) continue;
    }
    if (!value_handed_on(nt, parent, u)) continue;
    if (k == NK_CallNode) {
      /* `x.class` handed on names x's class or a subclass of it, when x is
         known: `Sanitizer.new.class` returned from a method let every class
         escape, and Campfire's BCrypt::Password took an untyped URL from an
         unrelated `k.new(url)` (#5271). `K.superclass` names K's ancestors.
         A receiver of no known class still hands out any class. */
      const char *un = nt_str(nt, u, "name");
      int rc = nt_ref(nt, u, "receiver");
      int from = -1;
      if (un && rc >= 0 && sp_streq(un, "class")) {
        if (nt_kind(nt, rc) == NK_CallNode && nt_str(nt, rc, "name") &&
            sp_streq(nt_str(nt, rc, "name"), "new")) {
          int rr = nt_ref(nt, rc, "receiver");
          if (rr >= 0 && (nt_kind(nt, rr) == NK_ConstantReadNode || nt_kind(nt, rr) == NK_ConstantPathNode))
            from = comp_class_index(c, nt_str(nt, rr, "name"));
        }
        if (from < 0) { TyKind rt = infer_type(c, rc); if (ty_is_object(rt)) from = ty_object_class(rt); }
        if (from >= 0) {
          for (int d = 0; d < c->nclasses; d++)
            if (d == from || is_descendant(c, d, from)) esc[d] = 1;
          continue;
        }
      }
      else if (un && rc >= 0 && sp_streq(un, "superclass") &&
               (nt_kind(nt, rc) == NK_ConstantReadNode || nt_kind(nt, rc) == NK_ConstantPathNode)) {
        int ci = comp_class_index(c, nt_str(nt, rc, "name"));
        if (ci >= 0) {
          for (int a = c->classes[ci].parent; a >= 0; a = c->classes[a].parent) esc[a] = 1;
          continue;
        }
      }
      all = 1; break;
    }
    if (k != NK_SelfNode) {
      const char *cn = nt_str(nt, u, "name");
      for (int d = 0; cn && d < c->nclasses; d++)
        if (c->classes[d].name && sp_streq(c->classes[d].name, cn)) esc[d] = 1;
      continue;
    }
    if (body_self[u] >= 0) { esc[body_self[u]] = 1; continue; }
    if (body_self[u] == -2) { all = 1; break; }
    Scope *s = comp_scope_of(c, u);
    if (!s || !s->is_cmethod) continue;   /* an instance, or main */
    if (s->class_id < 0) { all = 1; break; }
    for (int d = 0; d < c->nclasses; d++)
      if (d == s->class_id || is_descendant(c, d, s->class_id)) esc[d] = 1;
  }
  if (all) memset(esc, 1, (size_t)c->nclasses);
  free(parent); free(body_self);
  return esc[cid];
}

/* Mark in `set` each class the value of `v` can be, when every value it can
   take is a constant naming a class: through parentheses, the branches of
   an if, unless or case, and either side of `||` or `&&`. 0 when some value
   can be anything else. */
static int value_class_set(Compiler *c, int v, char *set, int depth) {
  const NodeTable *nt = c->nt;
  if (v < 0 || depth > 64) return 0;
  NodeKind k = nt_kind(nt, v);
  const char *t = nt_type(nt, v);
  if (k == NK_ConstantReadNode || k == NK_ConstantPathNode) {
    int ci = comp_class_index(c, nt_str(nt, v, "name"));
    if (ci < 0) return 0;
    set[ci] = 1;
    return 1;
  }
  if (k == NK_ParenthesesNode) return value_class_set(c, nt_ref(nt, v, "body"), set, depth + 1);
  if (k == NK_StatementsNode) {
    int n = 0; const int *body = nt_arr(nt, v, "body", &n);
    return n > 0 && value_class_set(c, body[n - 1], set, depth + 1);
  }
  if (k == NK_OrNode || k == NK_AndNode)
    return value_class_set(c, nt_ref(nt, v, "left"), set, depth + 1) &&
           value_class_set(c, nt_ref(nt, v, "right"), set, depth + 1);
  if (k == NK_IfNode || k == NK_UnlessNode) {
    int alt = nt_ref(nt, v, k == NK_IfNode ? "subsequent" : "else_clause");
    if (alt < 0 && k == NK_IfNode) alt = nt_ref(nt, v, "consequent");
    return value_class_set(c, nt_ref(nt, v, "statements"), set, depth + 1) &&
           value_class_set(c, alt, set, depth + 1);
  }
  if (t && sp_streq(t, "ElseNode")) return value_class_set(c, nt_ref(nt, v, "statements"), set, depth + 1);
  if (t && sp_streq(t, "CaseNode")) {
    int n = 0; const int *arms = nt_arr(nt, v, "conditions", &n);
    for (int i = 0; i < n; i++)
      if (!value_class_set(c, nt_ref(nt, arms[i], "statements"), set, depth + 1)) return 0;
    return value_class_set(c, nt_ref(nt, v, "else_clause"), set, depth + 1);
  }
  return 0;
}

/* The classes a `new` receiver can be when that is a known set of constants:
   the receiver itself, or a local of the method whose every write assigns
   one (`k = c ? A : B`). 0 when it is not. */
static int recv_class_set(Compiler *c, int recv, char *set) {
  const NodeTable *nt = c->nt;
  memset(set, 0, (size_t)c->nclasses);
  if (recv < 0) return 0;
  if (nt_kind(nt, recv) != NK_LocalVariableReadNode) return value_class_set(c, recv, set, 0);
  const char *vn = nt_str(nt, recv, "name");
  Scope *sc = comp_scope_of(c, recv);
  if (!vn || !sc) return 0;
  for (int i = 0; i < sc->nparams; i++)
    if (sc->pnames[i] && sp_streq(sc->pnames[i], vn)) return 0;
  int nw = 0;
  for (int u = 0; u < nt->count; u++) {
    const char *ut = nt_type(nt, u);
    if (!ut || strncmp(ut, "LocalVariable", 13) != 0 || sp_streq(ut, "LocalVariableReadNode")) continue;
    const char *un = nt_str(nt, u, "name");
    if (!un || !sp_streq(un, vn)) continue;
    if (comp_scope_of(c, u) != sc) continue;
    if (!sp_streq(ut, "LocalVariableWriteNode")) return 0;
    if (!value_class_set(c, nt_ref(nt, u, "value"), set, 0)) return 0;
    nw++;
  }
  return nw > 0;
}

/* Can `new` at `call_id`, on a Class value the analysis cannot pin,
   construct class `cid`? `new(...)` or `self.new(...)` in a class method,
   and `self.class.new(...)` in an instance method, reach that class and its
   descendants; `x.class.new(...)` and `x.superclass.new(...)` any class;
   any other receiver a class whose value is handed on somewhere
   (class_value_escapes). */
int dynamic_new_may_reach(Compiler *c, int call_id, int cid) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, call_id, "receiver");
  Scope *s = comp_scope_of(c, call_id);
  int self_cid = -1;
  if (recv < 0 || nt_kind(nt, recv) == NK_SelfNode) {
    if (s && s->is_cmethod) self_cid = s->class_id;
  }
  else if (nt_kind(nt, recv) == NK_CallNode && nt_str(nt, recv, "name") &&
           (sp_streq(nt_str(nt, recv, "name"), "class") ||
            sp_streq(nt_str(nt, recv, "name"), "superclass"))) {
    int of = nt_ref(nt, recv, "receiver");
    if (sp_streq(nt_str(nt, recv, "name"), "class") &&
        (of < 0 || nt_kind(nt, of) == NK_SelfNode) && s && !s->is_cmethod && s->class_id >= 0)
      self_cid = s->class_id;
    /* `x.class.new` with x of a known class: that class or a subclass */
    else if (sp_streq(nt_str(nt, recv, "name"), "class") && of >= 0 && ty_is_object(infer_type(c, of)))
      self_cid = ty_object_class(infer_type(c, of));
    /* `K.superclass.new`: an ancestor of K (#5271) */
    else if (sp_streq(nt_str(nt, recv, "name"), "superclass") && of >= 0 &&
             (nt_kind(nt, of) == NK_ConstantReadNode || nt_kind(nt, of) == NK_ConstantPathNode) &&
             comp_class_index(c, nt_str(nt, of, "name")) >= 0) {
      for (int a = c->classes[comp_class_index(c, nt_str(nt, of, "name"))].parent; a >= 0; a = c->classes[a].parent)
        if (a == cid) return 1;
      return 0;
    }
    else return 1;
  }
  if (self_cid >= 0) return cid == self_cid || is_descendant(c, cid, self_cid);
  /* a receiver that is one of a known set of constants reaches just those */
  static char *set = NULL;
  static int set_call = -1, set_ok = 0, set_n = -1, set_count = -1;
  if (set_call != call_id || set_n != c->nclasses || set_count != nt->count) {
    free(set);
    set = (char *)calloc((size_t)(c->nclasses > 0 ? c->nclasses : 1), 1);
    set_ok = set && recv_class_set(c, recv, set);
    set_call = call_id; set_n = c->nclasses; set_count = nt->count;
  }
  if (set_ok && cid >= 0 && cid < c->nclasses) return set[cid];
  return class_value_escapes(c, cid);
}

/* `klass.new(a, b)` on a Class value the analysis cannot pin: the codegen
   switch has an arm for every class whose initialize takes this many
   positionals, and each class the call can reach is constructed by it. Its
   initialize's parameters take the arguments' types as a static
   `K.new(a, b)` would give them. */
static int bind_dynamic_new_initializers(Compiler *c, int call_id) {
  const NodeTable *nt = c->nt;
  int an = nt_ref(nt, call_id, "arguments");
  int argc = 0;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &argc) : NULL;
  int npos = 0, kwh = 0;
  for (int k = 0; k < argc; k++) {
    NodeKind ak = nt_kind(nt, av[k]);
    if (ak == NK_KeywordHashNode) {
      int ne = 0; const int *els = nt_arr(nt, av[k], "elements", &ne);
      kwh = ne > 0;
      for (int e = 0; e < ne; e++) if (nt_kind(nt, els[e]) == NK_AssocSplatNode) kwh = 0;
    }
    else if (ak != NK_BlockArgumentNode) npos++;
  }
  int changed = 0;
  int *seen = (int *)calloc((size_t)(c->nscopes > 0 ? c->nscopes : 1), sizeof(int));
  for (int k = 0; k < c->nclasses; k++) {
    if (!dynamic_new_may_reach(c, call_id, k)) continue;
    int imi = comp_method_in_chain(c, k, "initialize", NULL);
    /* A Struct or Data built by its generated constructor from `k.new(a: 1)`:
       a member a keyword names with a value of another type than the one
       the member has is boxed, as the arm would read the value as the
       member's type. A keyword_init: false Struct takes the Hash as its
       first member. */
    ClassInfo *sk = &c->classes[k];
    if (imi < 0 && sk->is_struct && kwh && npos == 0) {
      int kh = -1;
      for (int a = 0; a < argc; a++) if (nt_kind(nt, av[a]) == NK_KeywordHashNode) kh = av[a];
      int ne = 0; const int *els = nt_arr(nt, kh, "elements", &ne);
      for (int a = 0; a < sk->nmembers; a++) {
        if (class_ivar_pinned(sk, sk->ivars[a])) continue;
        TyKind mt = sk->ivar_types[a];
        if (mt == TY_UNKNOWN || mt == TY_POLY) continue;
        TyKind at = TY_UNKNOWN;
        if (sk->kw_init >= 0 || sk->is_data) {
          for (int e = 0; e < ne; e++) {
            int key = nt_ref(nt, els[e], "key");
            const char *kn = key >= 0 && nt_kind(nt, key) == NK_SymbolNode ? nt_str(nt, key, "value") : NULL;
            if (kn && sp_streq(kn, sk->ivars[a] + 1)) at = infer_type(c, nt_ref(nt, els[e], "value"));
          }
        }
        else if (a == 0) at = TY_POLY;
        if (at == TY_UNKNOWN || at == mt) continue;
        sk->ivar_types[a] = TY_POLY; changed = 1;
      }
      continue;
    }
    if (imi < 0 || imi >= c->nscopes || seen[imi]) continue;
    seen[imi] = 1;
    int pn = c->scopes[imi].def_node >= 0 ? nt_ref(nt, c->scopes[imi].def_node, "parameters") : -1;
    int nreq = 0, nopt = 0, npost = 0, nkw = 0, rest = -1, kwrest = -1;
    if (pn >= 0) {
      nt_arr(nt, pn, "requireds", &nreq);
      nt_arr(nt, pn, "optionals", &nopt);
      nt_arr(nt, pn, "posts", &npost);
      nt_arr(nt, pn, "keywords", &nkw);
      rest = nt_ref(nt, pn, "rest");
      kwrest = nt_ref(nt, pn, "keyword_rest");
    }
    nreq += npost;
    /* an initialize taking no keywords takes literal ones as one more
       positional */
    int n = npos + (kwh && nkw == 0 && kwrest < 0);
    if (n < nreq || (rest < 0 && n > nreq + nopt)) continue;
    changed |= bind_call_params(c, call_id, imi);
  }
  free(seen);
  return changed;
}

/* Unify a call's argument types into method scope `mi`'s parameters. */
/* 1 iff `v` builds a new hash: a literal or `Hash.new`. */
static int is_fresh_hash(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (v < 0) return 0;
  if (nt_kind(nt, v) == NK_HashNode) return 1;
  if (nt_kind(nt, v) != NK_CallNode || !sp_streq(nt_str(nt, v, "name"), "new")) return 0;
  int r = nt_ref(nt, v, "receiver");
  return r >= 0 && nt_kind(nt, r) == NK_ConstantReadNode && sp_streq(nt_str(nt, r, "name"), "Hash");
}

/* Marks hash literal `v` the poly-keyed variant; 1 on a change. */
static int want_poly_hash(Compiler *c, int v) {
  if (!c->hash_want || v < 0 || v >= c->node_cap || nt_kind(c->nt, v) != NK_HashNode) return 0;
  if (c->hash_want[v] == TY_POLY_POLY_HASH) return 0;
  c->hash_want[v] = TY_POLY_POLY_HASH;
  return 1;
}

/* A callee stores into the hash argument `arg` names what its variant cannot
   hold: the caller's own hash has to be the poly-keyed variant. Widens the
   hashes the argument's slot is built from, and the slot, where every write
   of it builds a new hash; a parameter widens as the callee's did, for its
   own callers. Returns 1 on a change. */
static int widen_arg_hash(Compiler *c, int arg) {
  const NodeTable *nt = c->nt;
  arg = unwrap_parens(c, arg);
  NodeKind ak = nt_kind(nt, arg);
  if (ak == NK_HashNode) return want_poly_hash(c, arg);
  const char *an = nt_str(nt, arg, "name");
  Scope *asc = an ? comp_scope_of(c, arg) : NULL;
  if (!asc) return 0;
  NodeKind wk;
  TyKind *slot = NULL;
  int cid = -1;
  if (ak == NK_LocalVariableReadNode) {
    LocalVar *al = scope_local(asc, an);
    if (!al || al->is_block_param || !ty_is_hash(al->type) || al->type == TY_POLY_POLY_HASH) return 0;
    if (al->is_param) {
      if (al->rbs_seeded) return 0;
      al->type = TY_POLY_POLY_HASH; al->push_widened = 1;
      return 1;
    }
    int si = (int)(asc - c->scopes), saw = 0;
    for (int r = lw_shared_first(c, an, si); r >= 0; r = lw_shared_next(r)) {
      int w = lw_shared_node(r);
      if (comp_scope_of(c, w) != asc || !sp_streq(nt_str(nt, w, "name"), an)) continue;
      if (nt_kind(nt, w) != NK_LocalVariableWriteNode || !is_fresh_hash(c, nt_ref(nt, w, "value"))) return 0;
      saw = 1;
    }
    if (!saw) return 0;
    for (int r = lw_shared_first(c, an, si); r >= 0; r = lw_shared_next(r)) {
      int w = lw_shared_node(r);
      if (comp_scope_of(c, w) == asc && sp_streq(nt_str(nt, w, "name"), an))
        want_poly_hash(c, nt_ref(nt, w, "value"));
    }
    al->type = TY_POLY_POLY_HASH; al->poly_hash_pin = 1;
    return 1;
  }
  if (ak == NK_InstanceVariableReadNode) {
    cid = asc->class_id >= 0 ? asc->class_id : comp_class_index(c, "Toplevel");
    int ivi = cid >= 0 ? comp_ivar_index(&c->classes[cid], an) : -1;
    if (ivi < 0) return 0;
    slot = &c->classes[cid].ivar_types[ivi];
    wk = NK_InstanceVariableWriteNode;
  }
  else if (ak == NK_GlobalVariableReadNode || ak == NK_ClassVariableReadNode ||
           ak == NK_ConstantReadNode) {
    slot = named_array_slot(c, arg);
    wk = ak == NK_GlobalVariableReadNode ? NK_GlobalVariableWriteNode
       : ak == NK_ClassVariableReadNode ? NK_ClassVariableWriteNode : NK_ConstantWriteNode;
    if (ak == NK_ClassVariableReadNode) {
      cid = asc->class_id >= 0 ? asc->class_id : comp_class_index(c, "Toplevel");
      cid = comp_cvar_owner(c, cid, an);
    }
  }
  else return 0;
  if (!slot || !ty_is_hash(*slot) || *slot == TY_POLY_POLY_HASH) return 0;
  int saw = 0;
  for (int pass = 0; pass < 2; pass++) {
    NT_FOREACH_KIND(nt, wk, w) {
      if (!sp_streq(nt_str(nt, w, "name"), an)) continue;
      if (cid >= 0) {
        Scope *ws = comp_scope_of(c, w);
        int wc = ws && ws->class_id >= 0 ? ws->class_id : comp_class_index(c, "Toplevel");
        if (ak == NK_ClassVariableReadNode) wc = comp_cvar_owner(c, wc, an);
        if (wc != cid) continue;
      }
      int v = nt_ref(nt, w, "value");
      if (pass == 0) { if (!is_fresh_hash(c, v)) return 0; saw = 1; }
      else want_poly_hash(c, v);
    }
    if (!saw) return 0;
  }
  *slot = TY_POLY_POLY_HASH;
  return 1;
}

/* 1 when `v` builds a new array nothing else holds: a literal, `Array.new`,
   or a builtin that answers a new array. */
int is_fresh_array(Compiler *c, int v) {
  static const char *const fresh[] = {
    "map", "collect", "flat_map", "collect_concat", "filter_map", "select", "filter",
    "reject", "sort", "sort_by", "reverse", "uniq", "compact", "flatten", "zip",
    "split", "chars", "bytes", "lines", "scan", "keys", "values", "dup",
    "take", "drop", "take_while", "drop_while", "rotate", "shuffle", "each_slice",
    "each_cons", "+", "-", "*", "&", "|", NULL };
  const NodeTable *nt = c->nt;
  v = unwrap_parens(c, v);
  if (v < 0) return 0;
  if (nt_kind(nt, v) == NK_ArrayNode) {
    int en = 0; const int *ev = nt_arr(nt, v, "elements", &en);
    for (int k = 0; k < en; k++)
      if (nt_kind(nt, ev[k]) == NK_SplatNode) return 0;
    return 1;
  }
  if (nt_kind(nt, v) != NK_CallNode) return 0;
  const char *nm = nt_str(nt, v, "name");
  int r = nt_ref(nt, v, "receiver");
  if (!nm || r < 0 || !ty_is_array(infer_type(c, v))) return 0;
  if (nt_kind(nt, r) == NK_ConstantReadNode)
    return sp_streq(nm, "new") && sp_streq(nt_str(nt, r, "name"), "Array");
  TyKind rt = infer_type(c, r);
  if (rt == TY_UNKNOWN || rt == TY_POLY || ty_is_object(rt)) return 0;
  if (sp_streq(nm, "to_a")) return !ty_is_array(rt);
  return str_in(nm, fresh);
}

/* Collects into out[] the container literals `n` can evaluate to, as far
   as the program shows: a literal itself, the literals a variable is
   assigned, and, for an element read `x[k]` (`fetch`, `first`, `last`),
   the container-literal elements of those `x` can be. Returns the count. */
static int container_literals(Compiler *c, int n, int *out, int nout, int cap, int depth) {
  const NodeTable *nt = c->nt;
  n = unwrap_parens(c, n);
  if (n < 0 || depth > 4 || nout >= cap) return nout;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_ArrayNode || k == NK_HashNode) {
    for (int q = 0; q < nout; q++) if (out[q] == n) return nout;
    out[nout++] = n;
    return nout;
  }
  const char *nm = nt_str(nt, n, "name");
  if (!nm) return nout;
  if (k == NK_CallNode) {
    int r = nt_ref(nt, n, "receiver"), a = nt_ref(nt, n, "arguments"), an = 0;
    if (a >= 0) nt_arr(nt, a, "arguments", &an);
    int elem_read = (sp_streq(nm, "[]") && an == 1) || (sp_streq(nm, "fetch") && an >= 1) ||
                    ((sp_streq(nm, "first") || sp_streq(nm, "last")) && an == 0);
    if (r < 0 || !elem_read || nt_ref(nt, n, "block") >= 0) return nout;
    int outer[32];
    int no = container_literals(c, r, outer, 0, 32, depth + 1);
    for (int q = 0; q < no; q++) {
      int en = 0; const int *ev = nt_arr(nt, outer[q], "elements", &en);
      for (int e = 0; e < en; e++) {
        int el = nt_kind(nt, ev[e]) == NK_AssocNode ? nt_ref(nt, ev[e], "value") : ev[e];
        NodeKind ek = nt_kind(nt, el);
        if (ek == NK_ArrayNode || ek == NK_HashNode) nout = container_literals(c, el, out, nout, cap, depth + 1);
      }
    }
    return nout;
  }
  Scope *sc = comp_scope_of(c, n);
  if (!sc) return nout;
  if (k == NK_LocalVariableReadNode) {
    int si = (int)(sc - c->scopes);
    for (int r = lw_shared_first(c, nm, si); r >= 0; r = lw_shared_next(r)) {
      int w = lw_shared_node(r);
      if (nt_kind(nt, w) != NK_LocalVariableWriteNode || comp_scope_of(c, w) != sc ||
          !sp_streq(nt_str(nt, w, "name"), nm)) continue;
      nout = container_literals(c, nt_ref(nt, w, "value"), out, nout, cap, depth + 1);
    }
    return nout;
  }
  NodeKind wk = k == NK_InstanceVariableReadNode ? NK_InstanceVariableWriteNode
              : k == NK_GlobalVariableReadNode ? NK_GlobalVariableWriteNode
              : k == NK_ClassVariableReadNode ? NK_ClassVariableWriteNode
              : k == NK_ConstantReadNode ? NK_ConstantWriteNode : NK_NONE;
  if (wk == NK_NONE) return nout;
  int cid = -1;
  if (k == NK_InstanceVariableReadNode || k == NK_ClassVariableReadNode)
    cid = sc->class_id >= 0 ? sc->class_id : comp_class_index(c, "Toplevel");
  if (k == NK_ClassVariableReadNode) cid = comp_cvar_owner(c, cid, nm);
  NT_FOREACH_KIND(nt, wk, w) {
    if (!sp_streq(nt_str(nt, w, "name"), nm)) continue;
    if (cid >= 0) {
      Scope *ws = comp_scope_of(c, w);
      int wc = ws && ws->class_id >= 0 ? ws->class_id : comp_class_index(c, "Toplevel");
      if (k == NK_ClassVariableReadNode) wc = comp_cvar_owner(c, wc, nm);
      if (wc != cid) continue;
    }
    nout = container_literals(c, nt_ref(nt, w, "value"), out, nout, cap, depth + 1);
  }
  return nout;
}

/* An element write or push whose receiver is an element read (`y[k][j] = v`,
   `a[0] << v`) stores into a container literal nested in another: widens
   each such literal the evidence does not fit, as a store through a
   variable holding it would. A boxed value is exempt, as it is there.
   Returns 1 on a change. */
static int widen_nested_literals(Compiler *c, int recv, int is_push, int is_splice, TyKind kt, TyKind vt) {
  const NodeTable *nt = c->nt;
  int r = unwrap_parens(c, recv);
  if (r < 0 || nt_kind(nt, r) != NK_CallNode) return 0;
  int lits[32];
  int nl = container_literals(c, r, lits, 0, 32, 0);
  int changed = 0;
  for (int q = 0; q < nl; q++) {
    int l = lits[q];
    TyKind lt = infer_type(c, l);
    if (nt_kind(nt, l) == NK_HashNode) {
      if (is_push || is_splice || lt == TY_POLY_POLY_HASH || kt == TY_UNKNOWN || vt == TY_UNKNOWN) continue;
      TyKind hvt = vt == TY_POLY && ty_is_hash(lt) ? ty_hash_val(lt) : vt;
      TyKind folded = ty_is_hash(lt) ? lt : TY_UNKNOWN;
      int fits = fold_container_evidence(&folded, 0, 0, kt, hvt);
      if (ty_is_hash(lt) && fits && folded == lt) continue;
      changed |= want_poly_hash(c, l);
    }
    else {
      if (lt == TY_POLY_ARRAY || vt == TY_UNKNOWN || vt == TY_POLY) continue;
      if (!is_push && kt != TY_INT && kt != TY_POLY) continue;
      if (ty_is_array(lt) && vt == ty_array_elem(lt)) continue;
      if (!c->arr_want || l >= c->node_cap || c->arr_want[l] == TY_POLY_ARRAY) continue;
      c->arr_want[l] = TY_POLY_ARRAY;
      changed = 1;
    }
  }
  return changed;
}

/* Collects into out[] the expressions whose value is the value of `n`:
   through parentheses, the last statement, and each arm of an `if`,
   `unless` or `case`. Answers the new count, or -1 when a path has no
   such expression (an arm left out answers nil) or out[] is full. */
static int value_leaves(Compiler *c, int n, int *out, int nout, int cap) {
  const NodeTable *nt = c->nt;
  n = unwrap_parens(c, n);
  if (n < 0 || nout < 0) return -1;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_StatementsNode) {
    int bn = 0; const int *bb = nt_arr(nt, n, "body", &bn);
    return bn > 0 ? value_leaves(c, bb[bn - 1], out, nout, cap) : -1;
  }
  if (k == NK_ParenthesesNode) return value_leaves(c, nt_ref(nt, n, "body"), out, nout, cap);
  if (k == NK_ElseNode) return value_leaves(c, nt_ref(nt, n, "statements"), out, nout, cap);
  if (k == NK_IfNode || k == NK_UnlessNode) {
    int alt = nt_ref(nt, n, k == NK_IfNode ? "subsequent" : "else_clause");
    nout = value_leaves(c, nt_ref(nt, n, "statements"), out, nout, cap);
    return value_leaves(c, alt, out, nout, cap);
  }
  if (k == NK_CaseNode) {
    int an = 0; const int *arms = nt_arr(nt, n, "conditions", &an);
    for (int i = 0; i < an; i++) nout = value_leaves(c, nt_ref(nt, arms[i], "statements"), out, nout, cap);
    return value_leaves(c, nt_ref(nt, n, "else_clause"), out, nout, cap);
  }
  if (k == NK_ReturnNode) {
    int a = nt_ref(nt, n, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (an != 1 || nt_kind(nt, av[0]) == NK_SplatNode) return -1;
    return value_leaves(c, av[0], out, nout, cap);
  }
  if (nout >= cap) return -1;
  out[nout++] = n;
  return nout;
}

/* The values method scope `mi` answers: its body's, and each `return`'s. */
static int method_value_leaves(Compiler *c, int mi, int *out, int cap) {
  Scope *m = &c->scopes[mi];
  int n = m->body >= 0 ? value_leaves(c, m->body, out, 0, cap) : -1;
  NT_FOREACH_KIND(c->nt, NK_ReturnNode, r) {
    if (n < 0) break;
    if (comp_scope_of(c, r) == m) n = value_leaves(c, r, out, n, cap);
  }
  return n;
}

/* 1 when a call bound to method scope `mi` can reach another definition: an
   override in a subclass, or the same method defined again. */
static int method_has_other_body(Compiler *c, int mi) {
  Scope *m = &c->scopes[mi];
  for (int t = 1; t < c->nscopes; t++) {
    Scope *o = &c->scopes[t];
    if (t == mi || !o->name || !m->name || !sp_streq(o->name, m->name) || o->is_cmethod != m->is_cmethod) continue;
    if (o->class_id == m->class_id || (o->class_id >= 0 && m->class_id >= 0 && is_descendant(c, o->class_id, m->class_id)))
      return 1;
  }
  return 0;
}

/* 1 when local `name` of `sc` is also bound as a target (`a, b = ...`,
   `rescue => e`, a `for` variable), which the write index does not list. */
static int local_has_target_write(Compiler *c, Scope *sc, const char *name) {
  NT_FOREACH_KIND(c->nt, NK_LocalVariableTargetNode, t)
    if (comp_scope_of(c, t) == sc && sp_streq(nt_str(c->nt, t, "name"), name)) return 1;
  return 0;
}

/* The value a multiple assignment (`a, b = x, y`) binds its target `t`
   to: the element of its literal right side at the target's place. -1 when
   the right side is no such literal, or the target no plain left one. */
static int masgn_target_value(Compiler *c, int t) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_MultiWriteNode, mw) {
    int ln = 0, rn = 0, en = 0;
    const int *lefts = nt_arr(nt, mw, "lefts", &ln);
    int i = 0;
    while (i < ln && lefts[i] != t) i++;
    if (i == ln) continue;
    nt_arr(nt, mw, "rights", &rn);
    int val = unwrap_parens(c, nt_ref(nt, mw, "value"));
    if (nt_ref(nt, mw, "rest") >= 0 || rn > 0 || val < 0 || nt_kind(nt, val) != NK_ArrayNode) return -1;
    const int *ev = nt_arr(nt, val, "elements", &en);
    if (en != ln) return -1;
    for (int e = 0; e < en; e++) if (nt_kind(nt, ev[e]) == NK_SplatNode) return -1;
    return ev[i];
  }
  return -1;
}

/* 1 when the call `v` is an Array method that answers its receiver itself,
   not a new array or nil. */
static int array_answers_receiver(Compiler *c, int v) {
  static const char *const self_ret[] = {
    "<<", "push", "append", "unshift", "prepend", "insert", "concat", "fill", "replace",
    "sort!", "sort_by!", "reverse!", "rotate!", "shuffle!", "map!", "collect!", "tap",
    "freeze", "itself", NULL };
  const char *nm = nt_str(c->nt, v, "name");
  int r = nt_ref(c->nt, v, "receiver");
  if (!nm || r < 0 || !ty_is_array(infer_type(c, r))) return 0;
  return str_in(nm, self_ret);
}

/* The class whose ivar the value `v` reads, with the ivar's name in *ivn:
   an ivar read in a method, or an attr_reader call (`obj.items`, a bare
   `items` in the class). -1 when `v` is neither. */
int ivar_src_slot(Compiler *c, int v, const char **ivn) {
  const NodeTable *nt = c->nt;
  static char ivb[128];
  Scope *sc = comp_scope_of(c, v);
  if (!sc) return -1;
  if (nt_kind(nt, v) == NK_InstanceVariableReadNode) {
    *ivn = nt_str(nt, v, "name");
    return *ivn && sc->class_id >= 0 ? sc->class_id : -1;
  }
  if (nt_kind(nt, v) != NK_CallNode || nt_ref(nt, v, "block") >= 0) return -1;
  int a = nt_ref(nt, v, "arguments"), an = 0;
  if (a >= 0) nt_arr(nt, a, "arguments", &an);
  const char *nm = nt_str(nt, v, "name");
  if (an > 0 || !nm || strlen(nm) >= sizeof ivb - 1) return -1;
  int r = nt_ref(nt, v, "receiver"), cls = -1;
  if (r < 0) cls = sc->is_cmethod ? -1 : sc->class_id;
  else {
    TyKind rt = infer_type(c, r);
    if (ty_is_object(rt)) cls = ty_object_class(rt);
  }
  int def_cls = -1, mix = -1;
  if (cls < 0 || comp_resolve_member(c, cls, nm, 0, &def_cls, &mix) != SP_MEMBER_ATTR) return -1;
  ivb[0] = '@'; strcpy(ivb + 1, nm);
  *ivn = ivb;
  return cls;
}

/* 1 when every write of the global, class variable or constant `v` reads
   stores nil or a new array, as ivar_writes_fresh asks of an ivar. */
static int named_writes_fresh(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, v);
  NodeKind wk[4] = { NK_NONE, NK_NONE, NK_NONE, NK_NONE };
  if (k == NK_GlobalVariableReadNode) {
    wk[0] = NK_GlobalVariableWriteNode; wk[1] = NK_GlobalVariableOrWriteNode;
    wk[2] = NK_GlobalVariableAndWriteNode; wk[3] = NK_GlobalVariableOperatorWriteNode;
  }
  else if (k == NK_ClassVariableReadNode) {
    wk[0] = NK_ClassVariableWriteNode; wk[1] = NK_ClassVariableOrWriteNode;
    wk[2] = NK_ClassVariableAndWriteNode; wk[3] = NK_ClassVariableOperatorWriteNode;
  }
  else wk[0] = NK_ConstantWriteNode;
  Scope *vs = comp_scope_of(c, v);
  int vcls = vs && vs->class_id >= 0 ? vs->class_id : comp_class_index(c, "Toplevel");
  if (k == NK_ClassVariableReadNode) vcls = comp_cvar_owner(c, vcls, nt_str(nt, v, "name"));
  int saw = 0;
  for (int t = 0; t < 4 && wk[t] != NK_NONE; t++)
    NT_FOREACH_KIND(nt, wk[t], w) {
      if (!sp_streq(nt_str(nt, w, "name"), nt_str(nt, v, "name"))) continue;
      /* a class variable of another class is another slot, unless that class
         inherits it */
      if (k == NK_ClassVariableReadNode) {
        Scope *ws = comp_scope_of(c, w);
        int wc = ws && ws->class_id >= 0 ? ws->class_id : c->node_cbody[w];
        if (wc < 0) wc = comp_class_index(c, "Toplevel");
        if (comp_cvar_owner(c, wc, nt_str(nt, v, "name")) != vcls) continue;
      }
      if (t >= 2) return 0;
      int wv = unwrap_parens(c, nt_ref(nt, w, "value"));
      if (wv < 0 || (nt_kind(nt, wv) != NK_NilNode && !is_fresh_array(c, wv))) return 0;
      saw = 1;
    }
  return saw;
}

/* 1 when every write of ivar `ivn` of class `cls` (or of a class it shares
   the slot with) stores nil, a new array nothing else holds, or a parameter
   of the method writing it, so the slot can be the general Array with no
   copy the program could see: a parameter it keeps (`@a = a`) is its
   callers' array, and widens for them as one a store goes through does.
   With `apply` widens those parameters, setting *ch on a change. */
static int ivar_writes_fresh(Compiler *c, int cls, const char *ivn, int apply, int *ch) {
  const NodeTable *nt = c->nt;
  static const NodeKind wk[4] = { NK_InstanceVariableWriteNode, NK_InstanceVariableOrWriteNode,
                                  NK_InstanceVariableAndWriteNode, NK_InstanceVariableOperatorWriteNode };
  int saw = 0;
  for (int t = 0; t < 4; t++)
    NT_FOREACH_KIND(nt, wk[t], w) {
      if (!sp_streq(nt_str(nt, w, "name"), ivn)) continue;
      Scope *ws = comp_scope_of(c, w);
      int wc = ws && ws->class_id >= 0 ? ws->class_id : c->node_cbody[w];
      if (wc < 0 || (wc != cls && !is_descendant(c, wc, cls) && !is_descendant(c, cls, wc))) continue;
      if (t >= 2) return 0;
      int wv = unwrap_parens(c, nt_ref(nt, w, "value"));
      if (wv < 0) return 0;
      saw = 1;
      if (nt_kind(nt, wv) == NK_NilNode || is_fresh_array(c, wv)) continue;
      LocalVar *pl = unassigned_param_read(c, ws, wv) >= 0 ? scope_local(ws, nt_str(nt, wv, "name")) : NULL;
      if (!pl || pl->rbs_seeded || !(ty_is_array(pl->type) || pl->type == TY_UNKNOWN) || ty_is_ptr_array(pl->type))
        return 0;
      if (apply && pl->type != TY_POLY_ARRAY) *ch |= widen_arg_array(c, wv);
    }
  return saw;
}

/* 1 when a read of the local `r` names is anything but the receiver of an
   element read, an iteration or a size, or an argument printed: the local
   may be stored into (`r << v`), aliased or handed on, so its elements may
   be arrays none of its literals shows. */
static int local_rows_escape(Compiler *c, int r) {
  static const char *const reads[] = { "[]", "first", "last", "fetch", "each", "each_with_index",
                                       "size", "length", "empty?", "inspect", "to_s", NULL };
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, r, "name");
  Scope *sc = comp_scope_of(c, r);
  int nread = 0, nok = 0;
  NT_FOREACH_KIND(nt, NK_LocalVariableReadNode, u)
    if (comp_scope_of(c, u) == sc && sp_streq(nt_str(nt, u, "name"), nm)) nread++;
  NT_FOREACH_KIND(nt, NK_CallNode, call) {
    const char *cn = nt_str(nt, call, "name");
    int rc = unwrap_parens(c, nt_ref(nt, call, "receiver"));
    if (!cn) continue;
    if (rc >= 0 && nt_kind(nt, rc) == NK_LocalVariableReadNode && comp_scope_of(c, rc) == sc &&
        sp_streq(nt_str(nt, rc, "name"), nm)) {
      for (int k = 0; reads[k]; k++) if (sp_streq(cn, reads[k])) { nok++; break; }
      continue;
    }
    if (rc >= 0 || !(sp_streq(cn, "p") || sp_streq(cn, "puts") || sp_streq(cn, "print") || sp_streq(cn, "pp")))
      continue;
    int a = nt_ref(nt, call, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    for (int k = 0; k < an; k++)
      if (nt_kind(nt, av[k]) == NK_LocalVariableReadNode && comp_scope_of(c, av[k]) == sc &&
          sp_streq(nt_str(nt, av[k], "name"), nm)) nok++;
  }
  return nok < nread;
}

/* An element read (`o[i]`, `first`, `last`, `fetch`) out of a local every
   write of which is an array literal of array literals, which nothing stores
   into or hands on: the rows are the arrays the read can answer. With
   `apply` 0 answers 1 when that is so; with 1 builds each row as the general
   Array, answering 1 on a change. -1 when `v` is no such read. */
static int literal_rows_read(Compiler *c, int v, int apply) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, v, "name");
  int r = unwrap_parens(c, nt_ref(nt, v, "receiver")), a = nt_ref(nt, v, "arguments"), an = 0;
  if (a >= 0) nt_arr(nt, a, "arguments", &an);
  if (!nm || r < 0 || nt_ref(nt, v, "block") >= 0 ||
      !((sp_streq(nm, "[]") && an == 1) || (sp_streq(nm, "fetch") && an == 1) ||
        ((sp_streq(nm, "first") || sp_streq(nm, "last")) && an == 0))) return -1;
  if (nt_kind(nt, r) != NK_LocalVariableReadNode) return -1;
  const char *rn = nt_str(nt, r, "name");
  Scope *sc = rn ? comp_scope_of(c, r) : NULL;
  LocalVar *lv = sc ? scope_local(sc, rn) : NULL;
  if (!lv || lv->is_param || lv->is_block_param || lv->rbs_seeded || local_has_target_write(c, sc, rn) ||
      local_rows_escape(c, r)) return -1;
  int saw = 0, ch = 0;
  for (int w0 = lw_shared_first(c, rn, (int)(sc - c->scopes)); w0 >= 0; w0 = lw_shared_next(w0)) {
    int w = lw_shared_node(w0);
    if (comp_scope_of(c, w) != sc || !sp_streq(nt_str(nt, w, "name"), rn)) continue;
    int lit = unwrap_parens(c, nt_ref(nt, w, "value"));
    if (nt_kind(nt, w) != NK_LocalVariableWriteNode || nt_kind(nt, lit) != NK_ArrayNode) return -1;
    int en = 0; const int *ev = nt_arr(nt, lit, "elements", &en);
    for (int e = 0; e < en; e++) {
      int el = unwrap_parens(c, ev[e]);
      TyKind et = infer_type(c, el);
      if (nt_kind(nt, el) != NK_ArrayNode || !is_fresh_array(c, el) || ty_is_ptr_array(et) ||
          (et != TY_UNKNOWN && !ty_is_array(et)) || !c->arr_want || el >= c->node_cap) return -1;
      if (apply && c->arr_want[el] != TY_POLY_ARRAY) { c->arr_want[el] = TY_POLY_ARRAY; ch = 1; }
    }
    saw = 1;
  }
  if (!saw) return -1;
  return apply ? ch : 1;
}

/* The arrays the value `v` can be, followed back to where each is built or
   held: through a local's writes, a method's values (and a method answering
   its block's value, the block at this call), a builtin answering its
   receiver, to an array literal, a new array, a parameter, an ivar, a
   global, class variable or constant. With `apply` 0 it answers whether
   every one of them can be the general Array; with 1 it makes them so,
   answering 1 on a change. A literal is built as one; a parameter widens
   for its own callers, as a push through it widens it; a local and a
   method's value are pinned to it, and a new array of a typed kind
   (`Array.new(n, 0)`, a `map`) is converted where it reaches the pinned
   slot, which is only right because nothing else holds it (`pinned`: the
   slot `v` is written to is one). A slot that may keep an array from
   elsewhere (an ivar a parameter is stored into), a block parameter, an
   element read, a call nothing resolves or one an override may answer is
   none of these, and answers 0: the call site refuses it. The methods being
   walked are on `stack`, and a call back into one of them answers what the
   method does. */
static int array_src_walk(Compiler *c, int v, int pinned, int apply, int *stack, int depth) {
  const NodeTable *nt = c->nt;
  v = unwrap_parens(c, v);
  if (v < 0 || depth > 6) return 0;
  stack[depth] = -1;
  TyKind vt = infer_type(c, v);
  if (vt == TY_POLY_ARRAY) return !apply;
  NodeKind k = nt_kind(nt, v);
  /* a literal, an empty one too, whose kind its uses decide */
  if (k == NK_ArrayNode) {
    if ((vt != TY_UNKNOWN && !ty_is_array(vt)) || ty_is_ptr_array(vt) || !is_fresh_array(c, v) ||
        !c->arr_want || v >= c->node_cap) return 0;
    if (!apply) return 1;
    if (c->arr_want[v] == TY_POLY_ARRAY) return 0;
    c->arr_want[v] = TY_POLY_ARRAY;
    return 1;
  }
  if (!ty_is_array(vt) || ty_is_ptr_array(vt)) return 0;
  if (is_fresh_array(c, v)) return pinned && !apply;
  /* an element read out of a local every write of which is a literal of
     array literals, and nothing stores into: those rows are the value */
  if (k == NK_CallNode) {
    int rows = literal_rows_read(c, v, apply);
    if (rows >= 0) return rows;
  }
  /* a builtin that answers its receiver: the receiver's array is the value */
  if (k == NK_CallNode && array_answers_receiver(c, v))
    return array_src_walk(c, nt_ref(nt, v, "receiver"), pinned, apply, stack, depth + 1);
  /* `x = y = v`: the inner write's local is the value */
  if (k == NK_LocalVariableReadNode || k == NK_LocalVariableWriteNode) {
    const char *nm = nt_str(nt, v, "name");
    Scope *sc = nm ? comp_scope_of(c, v) : NULL;
    LocalVar *lv = sc ? scope_local(sc, nm) : NULL;
    if (!lv || lv->is_block_param) return 0;
    if (lv->is_param) {
      if (lv->rbs_seeded) return 0;
      if (!apply) return 1;
      lv->type = TY_POLY_ARRAY; lv->push_widened = 1;
      return 1;
    }
    int saw = 0, ch = 0;
    /* a target of a multiple assignment takes its element of the right
       side, which it does not convert: not pinned */
    NT_FOREACH_KIND(nt, NK_LocalVariableTargetNode, t) {
      if (comp_scope_of(c, t) != sc || !sp_streq(nt_str(nt, t, "name"), nm)) continue;
      int tv = masgn_target_value(c, t);
      if (tv < 0) return 0;
      int got = array_src_walk(c, tv, 0, apply, stack, depth + 1);
      if (!apply && !got) return 0;
      ch |= got; saw = 1;
    }
    for (int r = lw_shared_first(c, nm, (int)(sc - c->scopes)); r >= 0; r = lw_shared_next(r)) {
      int w = lw_shared_node(r);
      if (comp_scope_of(c, w) != sc || !sp_streq(nt_str(nt, w, "name"), nm)) continue;
      if (nt_kind(nt, w) != NK_LocalVariableWriteNode && nt_kind(nt, w) != NK_LocalVariableOrWriteNode) return 0;
      /* each arm of a branch it is written; one that reads the local back
         (`x = m(x) ? x : y`) is this slot's own array */
      int wl[16];
      int nl = value_leaves(c, nt_ref(nt, w, "value"), wl, 0, 16);
      if (nl <= 0) return 0;
      for (int i = 0; i < nl; i++) {
        if (nt_kind(nt, wl[i]) == NK_LocalVariableReadNode && comp_scope_of(c, wl[i]) == sc &&
            sp_streq(nt_str(nt, wl[i], "name"), nm)) continue;
        int got = array_src_walk(c, wl[i], 1, apply, stack, depth + 1);
        if (!apply && !got) return 0;
        ch |= got;
      }
      saw = 1;
    }
    if (!saw || !apply) return saw;
    if (!lv->poly_array_pin || lv->type != TY_POLY_ARRAY) { lv->type = TY_POLY_ARRAY; lv->poly_array_pin = 1; ch = 1; }
    return ch;
  }
  if (k == NK_GlobalVariableReadNode || k == NK_ClassVariableReadNode || k == NK_ConstantReadNode) {
    TyKind *ns = named_array_slot(c, v);
    if (!ns || !ty_is_array(*ns) || ty_is_ptr_array(*ns) || !named_writes_fresh(c, v)) return 0;
    if (!apply) return 1;
    if (*ns == TY_POLY_ARRAY) return 0;
    *ns = TY_POLY_ARRAY;
    return 1;
  }
  /* An ivar, read here or through an attr_reader, widens after the
     fixpoint (widen_ivars_from_pushed_params), where the write passes cannot
     narrow it back; the locals and method values on the way are pinned now.
     Only one every write of which builds a new array or keeps a parameter,
     which widens with it for its callers (ivar_writes_fresh): a parameter
     left typed would be copied into the widened slot, away from its caller. */
  const char *ivn = NULL;
  int icls = ivar_src_slot(c, v, &ivn);
  if (icls >= 0) {
    ClassInfo *ci = &c->classes[icls];
    int ivi = comp_ivar_index(ci, ivn);
    int wdc = -1, wmi = -1;
    if (ivi < 0 || class_ivar_pinned(ci, ivn) || !c->ivar_widen_src || v >= c->node_cap ||
        comp_resolve_member(c, icls, ivn + 1, 1, &wdc, &wmi) == SP_MEMBER_ATTR ||
        !ivar_writes_fresh(c, icls, ivn, 0, NULL)) return 0;
    if (!apply) return 1;
    int ch = 0;
    ivar_writes_fresh(c, icls, ivn, 1, &ch);
    if (!c->ivar_widen_src[v]) { c->ivar_widen_src[v] = 1; ch = 1; }
    return ch;
  }
  if (k != NK_CallNode) return 0;
  int mi = backprop_call_target(c, v);
  /* `v.then { ... }`: the block's value */
  const char *cn = nt_str(nt, v, "name");
  int tblk = nt_ref(nt, v, "block");
  if (mi < 0 && cn && (sp_streq(cn, "then") || sp_streq(cn, "yield_self")) && nt_ref(nt, v, "receiver") >= 0 &&
      tblk >= 0 && nt_kind(nt, tblk) == NK_BlockNode &&
      ie_block_break_next_ty(c, nt_ref(nt, tblk, "body")) == TY_UNKNOWN) {
    int bl[16];
    int n = value_leaves(c, nt_ref(nt, tblk, "body"), bl, 0, 16), ch = 0;
    if (n <= 0) return 0;
    for (int i = 0; i < n; i++) {
      int got = array_src_walk(c, bl[i], pinned, apply, stack, depth + 1);
      if (!apply && !got) return 0;
      ch |= got;
    }
    return apply ? ch : 1;
  }
  if (mi < 0) return 0;
  for (int d = 0; d < depth; d++) if (stack[d] == mi) return !apply;
  Scope *m = &c->scopes[mi];
  if (m->ret_rbs_seeded || m->ret_specialized || m->cs_synth || m->is_lowered_yield ||
      m->ret_oa_pin != TY_UNKNOWN) return 0;
  /* A method a subclass overrides, or one defined again, answers any of
     its bodies' values: each is followed, and each body's value pinned. */
  if (method_has_other_body(c, mi)) {
    int ch = 0;
    stack[depth] = mi;
    for (int t = 1; t < c->nscopes; t++) {
      Scope *o = &c->scopes[t];
      if (t != mi && !(o->name && sp_streq(o->name, m->name) && o->is_cmethod == m->is_cmethod &&
                       (o->class_id == m->class_id ||
                        (o->class_id >= 0 && m->class_id >= 0 && is_descendant(c, o->class_id, m->class_id)))))
        continue;
      int ol[16];
      int on = method_value_leaves(c, t, ol, 16);
      if (o->ret_rbs_seeded || o->ret_specialized || o->cs_synth || o->is_lowered_yield ||
          o->ret_oa_pin != TY_UNKNOWN || on <= 0) return 0;
      for (int i = 0; i < on; i++) {
        if (nt_kind(nt, ol[i]) == NK_YieldNode) return 0;
        int got = array_src_walk(c, ol[i], 1, apply, stack, depth + 1);
        if (!apply && !got) return 0;
        ch |= got;
      }
      if (apply && !o->ret_poly_array_pin) { o->ret_poly_array_pin = 1; ch = 1; }
    }
    return apply ? ch : 1;
  }
  int lv[16];
  int n = method_value_leaves(c, mi, lv, 16);
  if (n <= 0) return 0;
  stack[depth] = mi;
  /* A method answering its block's value (`def build = yield`) answers, at
     this call, what this call's block does. */
  int yields = 0;
  for (int i = 0; i < n; i++) yields += nt_kind(nt, lv[i]) == NK_YieldNode;
  if (yields) {
    int blk = nt_ref(nt, v, "block");
    if (yields != n || blk < 0 || nt_kind(nt, blk) != NK_BlockNode ||
        ie_block_break_next_ty(c, nt_ref(nt, blk, "body")) != TY_UNKNOWN) return 0;
    n = value_leaves(c, nt_ref(nt, blk, "body"), lv, 0, 16);
    if (n <= 0) return 0;
  }
  int ch = 0;
  for (int i = 0; i < n; i++) {
    int got = array_src_walk(c, lv[i], !yields, apply, stack, depth + 1);
    if (!apply && !got) return 0;
    ch |= got;
  }
  if (!apply) return 1;
  if (!yields && !m->ret_poly_array_pin) { m->ret_poly_array_pin = 1; ch = 1; }
  return ch;
}

/* Widens every array the value `v` can be to the general Array, when all
   of them can be (array_src_walk). Returns 1 on a change. */
static int widen_array_sources(Compiler *c, int v) {
  int stack[8];
  if (!array_src_walk(c, v, 0, 0, stack, 0)) return 0;
  return array_src_walk(c, v, 0, 1, stack, 0);
}

/* A callee stores into the array argument `arg` names an element its kind
   cannot hold: the caller's own array has to be the general Array. Widens
   a local every write of which is an array literal (pinned, since the
   literals re-derive the narrow kind), or a global, class variable or
   constant, whose write passes keep a general Array once it is one. A
   parameter widens as the callee's did, for its own callers. Anything else
   is followed back to where its arrays are built (widen_array_sources): a
   local assigned a method's value, the value of a method, a chain of them.
   Returns 1 on a change. */
static int widen_arg_array(Compiler *c, int arg) {
  const NodeTable *nt = c->nt;
  arg = unwrap_parens(c, arg);
  NodeKind ak = nt_kind(nt, arg);
  if (ak == NK_ArrayNode) {
    if (!c->arr_want || arg >= c->node_cap || !is_fresh_array(c, arg) || c->arr_want[arg] == TY_POLY_ARRAY) return 0;
    c->arr_want[arg] = TY_POLY_ARRAY;
    return 1;
  }
  if (ak == NK_LocalVariableReadNode) {
    const char *an = nt_str(nt, arg, "name");
    Scope *asc = an ? comp_scope_of(c, arg) : NULL;
    LocalVar *al = asc ? scope_local(asc, an) : NULL;
    if (!al || !ty_is_array(al->type) || al->type == TY_POLY_ARRAY || al->is_block_param) return 0;
    if (al->is_param) {
      if (al->rbs_seeded) return 0;
      al->type = TY_POLY_ARRAY; al->push_widened = 1;
      return 1;
    }
    if (local_all_writes_empty_array(c, asc, an)) { al->type = TY_POLY_ARRAY; return 1; }
    if (local_all_writes_fresh_array(c, asc, an)) {
      /* the pin is re-asserted each round, where the local re-derives from
         its writes: only a new pin is a change, or a widening asked every
         round never settles */
      int was = al->poly_array_pin;
      al->type = TY_POLY_ARRAY; al->poly_array_pin = 1; return !was;
    }
    return widen_array_sources(c, arg);
  }
  if (ak == NK_GlobalVariableReadNode || ak == NK_ClassVariableReadNode || ak == NK_ConstantReadNode) {
    TyKind *ns = named_array_slot(c, arg);
    if (ns && ty_is_array(*ns) && *ns != TY_POLY_ARRAY) { *ns = TY_POLY_ARRAY; return 1; }
    return 0;
  }
  return widen_array_sources(c, arg);
}

const char *block_param_name(Compiler *c, int block, int idx);
static int widen_boxed_elem_sources(Compiler *c, int r, TyKind elem, int depth);

/* The calls whose literal block names a positional parameter, keyed by the
   name and the block's scope: the calls a block parameter is bound by.
   Rebuilt, as lw_shared_ix is, when the node table or the scope shape
   moves; a scan of every call per lookup was quadratic in a program of many
   blocks. */
static struct { int cap, n; int *head, *next, *call, *idx, *scope; const char **name; } bp_ix;
static const NodeTable *bp_ix_nt = NULL;
static int bp_ix_ntc = -1;
static unsigned bp_ix_gen = 0;
static int bp_ix_first(Compiler *c, const char *name, int scope) {
  const NodeTable *nt = c->nt;
  unsigned gen = comp_scope_index_gen();
  if (!comp_scope_index_is_frozen() || bp_ix_nt != nt || bp_ix_ntc != nt->count || bp_ix_gen != gen) {
    free(bp_ix.head); free(bp_ix.next); free(bp_ix.call); free(bp_ix.idx); free(bp_ix.scope); free(bp_ix.name);
    int n = 0, cap = 16;
    NT_FOREACH_KIND(nt, NK_CallNode, call) {
      int blk = nt_ref(nt, call, "block");
      if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) continue;
      for (int bi = 0; block_param_name(c, blk, bi); bi++) n++;
    }
    while (cap < n * 2) cap <<= 1;
    bp_ix.cap = cap; bp_ix.n = 0;
    bp_ix.head = malloc(sizeof(int) * cap);
    for (int i = 0; i < cap; i++) bp_ix.head[i] = -1;
    bp_ix.next = malloc(sizeof(int) * (n + 1)); bp_ix.call = malloc(sizeof(int) * (n + 1));
    bp_ix.idx = malloc(sizeof(int) * (n + 1)); bp_ix.scope = malloc(sizeof(int) * (n + 1));
    bp_ix.name = malloc(sizeof(char *) * (n + 1));
    NT_FOREACH_KIND(nt, NK_CallNode, call) {
      int blk = nt_ref(nt, call, "block");
      if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) continue;
      int sc = (int)(comp_scope_of(c, blk) - c->scopes);
      const char *bp;
      for (int bi = 0; (bp = block_param_name(c, blk, bi)) && bp_ix.n < n; bi++) {
        int k = bp_ix.n++;
        unsigned h = lw_hash(bp, sc) & (unsigned)(cap - 1);
        bp_ix.call[k] = call; bp_ix.idx[k] = bi; bp_ix.scope[k] = sc; bp_ix.name[k] = bp;
        bp_ix.next[k] = bp_ix.head[h]; bp_ix.head[h] = k;
      }
    }
    bp_ix_nt = nt; bp_ix_ntc = nt->count; bp_ix_gen = gen;
  }
  return bp_ix.head[lw_hash(name, scope) & (unsigned)(bp_ix.cap - 1)];
}

/* 1 when one of the values lv[0..n) is a typed array a store of kind
   `elem` cannot fit and the walk cannot build as the general Array on its
   own (a new array a builtin answers, which only a pinned slot converts):
   only then is the slot they are the values of pinned. */
static int leaves_need_pin(Compiler *c, const int *lv, int n, TyKind elem) {
  int stack[8];
  for (int i = 0; i < n; i++) {
    TyKind t = infer_type(c, lv[i]);
    if (!ty_is_array(t) || t == TY_POLY_ARRAY || ty_is_ptr_array(t) || elem == ty_array_elem(t)) continue;
    if (!array_src_walk(c, lv[i], 0, 0, stack, 0)) return 1;
  }
  return 0;
}

/* 1 when every value in lv[0..n) is the general Array or an array the
   walk can build as one where it reaches a pinned slot (array_src_walk):
   the slot they are the values of can then be pinned to it, and a new
   array of a typed kind converts there. With `apply` builds them so,
   answering 1 on a change. */
static int leaves_widen_to_poly_array(Compiler *c, const int *lv, int n, int apply) {
  int stack[8], ch = 0;
  for (int i = 0; i < n; i++) {
    if (infer_type(c, lv[i]) == TY_POLY_ARRAY) continue;
    int got = array_src_walk(c, lv[i], 1, apply, stack, 0);
    if (!apply && !got) return 0;
    ch |= got;
  }
  return apply ? ch : n > 0;
}

/* A store of kind `elem` (TY_POLY: one decided at run time) goes into the
   array `v` is, where the program holds `v` boxed: a local written arrays of
   two kinds, an element read out of a general container, a block parameter,
   a boxed parameter. A typed array cannot take a foreign element in place:
   the store through the box promotes a copy and writes it back to the one
   slot it names (sp_poly_arr_widen_and_set), or refuses the element with
   TypeError, and every other slot holding the array -- the caller's, the
   local it was read from, the container it came out of -- kept the typed
   array without it. So each typed array `v` can be that cannot hold `elem`
   is widened where the program builds it, as an argument is
   (widen_arg_array): followed through a local's writes and `||=`, the arms
   of a branch, a method's values, and the elements of the containers an
   element read, an element iterator's block parameter or a `for` variable
   takes it from. A boxed parameter records the element for its callers'
   binding, as a store through it does (boxed_push_elem). What the walk
   cannot follow keeps the run time's answer. Returns 1 on a change. */
/* The arguments the calls of proc or lambda literal `lit` pass its
   parameter `pn`, followed as widen_boxed_array_sources follows a value: a
   call on the literal itself, on a local some write of which is it, or on a
   method's parameter that a call of the method passes such a local or the
   literal in (`def fw(f, x) = f.call(x)` with `fw(l, a)`), where the
   argument is the method's own parameter in turn, whose callers the
   binding checks (boxed_push_elem). */
static int proc_lit_carrier(Compiler *c, int v, int lit) {
  const NodeTable *nt = c->nt;
  v = unwrap_parens(c, v);
  if (v == lit) return 1;
  if (v < 0 || nt_kind(nt, v) != NK_LocalVariableReadNode) return 0;
  const char *vn = nt_str(nt, v, "name");
  Scope *vs = vn ? comp_scope_of(c, v) : NULL;
  int si = vs ? (int)(vs - c->scopes) : -1;
  /* a local any write of which is the literal may hold it at the call */
  for (int w = si >= 0 ? comp_lvw_first_sc(c, si, vn) : -1; w >= 0; w = comp_lvw_next_sc(c, w)) {
    if (comp_scope_of(c, w) != vs || !sp_streq(nt_str(nt, w, "name"), vn)) continue;
    if (nt_kind(nt, w) == NK_LocalVariableWriteNode && unwrap_parens(c, nt_ref(nt, w, "value")) == lit) return 1;
  }
  return 0;
}
/* What a Hash handed a parameter must hold: a key `hk` and a value `hv`
   stored through it. A typed Hash that cannot widens to the poly-keyed
   one; a method's parameter records the store for its own callers'
   binding (boxed_store_key/val), or widens as a typed one does. */
static int widen_hash_arg_for_store(Compiler *c, int arg, TyKind hk, TyKind hv) {
  const NodeTable *nt = c->nt;
  arg = unwrap_parens(c, arg);
  TyKind at = infer_type(c, arg);
  if (nt_kind(nt, arg) == NK_LocalVariableReadNode) {
    const char *an = nt_str(nt, arg, "name");
    Scope *asc = an ? comp_scope_of(c, arg) : NULL;
    LocalVar *al = asc ? scope_local(asc, an) : NULL;
    if (al && al->is_param && !al->is_block_param && !al->rbs_seeded) {
      if (al->type == TY_POLY) {
        int ch = 0;
        TyKind *ev[2] = { &al->boxed_store_key, &al->boxed_store_val };
        TyKind got[2] = { hk, hv };
        for (int e = 0; e < 2; e++) {
          TyKind was = *ev[e];
          TyKind now = was == TY_UNKNOWN ? got[e] : (was == got[e] ? was : TY_POLY);
          if (now != was) { *ev[e] = now; ch = 1; }
        }
        return ch;
      }
      if (ty_is_hash(al->type) && al->type != TY_POLY_POLY_HASH) {
        TyKind folded = al->type;
        if (fold_container_evidence(&folded, 0, 0, hk, hv) && folded == al->type) return 0;
        al->type = TY_POLY_POLY_HASH; al->push_widened = 1;
        return 1;
      }
      return 0;
    }
  }
  if (!ty_is_hash(at) || at == TY_POLY_POLY_HASH) return 0;
  TyKind folded = at;
  if (fold_container_evidence(&folded, 0, 0, hk, hv) && folded == at) return 0;
  return widen_arg_hash(c, arg);
}
static int widen_proc_call_args_m(Compiler *c, int lit, const char *pn, TyKind elem, int depth,
                                  int hash, TyKind hk, TyKind hv);
static int widen_proc_call_args(Compiler *c, int lit, const char *pn, TyKind elem, int depth) {
  return widen_proc_call_args_m(c, lit, pn, elem, depth, 0, TY_UNKNOWN, TY_UNKNOWN);
}
static int widen_proc_call_hash_args(Compiler *c, int lit, const char *pn, TyKind hk, TyKind hv) {
  return widen_proc_call_args_m(c, lit, pn, TY_UNKNOWN, 0, 1, hk, hv);
}
static int widen_proc_call_args_m(Compiler *c, int lit, const char *pn, TyKind elem, int depth,
                                  int hash, TyKind hk, TyKind hv) {
  const NodeTable *nt = c->nt;
  int ppn = a_proc_params_node(c, lit), rn = 0;
  const int *rq = ppn >= 0 ? nt_arr(nt, ppn, "requireds", &rn) : NULL;
  int k = -1;
  for (int i = 0; i < rn && k < 0; i++) if (sp_streq(nt_str(nt, rq[i], "name"), pn)) k = i;
  if (k < 0 || depth > 6) return 0;
  int ch = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, u) {
    const char *un = nt_str(nt, u, "name");
    int r = nt_ref(nt, u, "receiver");
    if (r < 0 || !proc_call_name(un)) continue;
    int a = nt_ref(nt, u, "arguments"), ac = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
    if (k >= ac) continue;
    int plain = 1;
    for (int q = 0; q <= k && plain; q++)
      plain = nt_kind(nt, av[q]) != NK_SplatNode && nt_kind(nt, av[q]) != NK_KeywordHashNode;
    if (!plain) continue;
    if (proc_lit_carrier(c, r, lit)) {
      ch |= hash ? widen_hash_arg_for_store(c, av[k], hk, hv) : widen_boxed_array_sources(c, av[k], elem, depth + 1);
      continue;
    }
    /* a method's parameter its callers hand the literal in */
    if (nt_kind(nt, r) != NK_LocalVariableReadNode) continue;
    const char *rnm = nt_str(nt, r, "name");
    Scope *ms = comp_scope_of(c, r);
    int pj = -1;
    for (int i = 0; ms && ms->name && i < ms->nparams && pj < 0; i++)
      if (ms->pnames[i] && sp_streq(ms->pnames[i], rnm)) pj = i;
    if (pj < 0) continue;
    int hands = 0;
    NT_FOREACH_KIND(nt, NK_CallNode, u2) {
      if (hands || backprop_call_target(c, u2) != (int)(ms - c->scopes)) continue;
      int a2 = nt_ref(nt, u2, "arguments"), ac2 = 0;
      const int *av2 = a2 >= 0 ? nt_arr(nt, a2, "arguments", &ac2) : NULL;
      int arg = call_param_arg(c, ms, av2, ac2, pj);
      hands = arg >= 0 && proc_lit_carrier(c, arg, lit);
    }
    if (hands)
      ch |= hash ? widen_hash_arg_for_store(c, av[k], hk, hv) : widen_boxed_array_sources(c, av[k], elem, depth + 1);
  }
  return ch;
}
static int widen_boxed_array_sources(Compiler *c, int v, TyKind elem, int depth) {
  const NodeTable *nt = c->nt;
  v = unwrap_parens(c, v);
  /* Only once the optimistic rounds have settled: a slot is boxed for a
     round or two while its evidence arrives, and a widening is for good. */
  if (v < 0 || depth > 6 || elem == TY_UNKNOWN || g_infer_optimistic) return 0;
  TyKind vt = infer_type(c, v);
  /* an empty literal's kind is what its uses store, this one among them,
     and so is a local's that only empty literals are written */
  int en = 0;
  if (nt_kind(nt, v) == NK_ArrayNode && (nt_arr(nt, v, "elements", &en), en == 0)) return 0;
  if (nt_kind(nt, v) == NK_LocalVariableReadNode &&
      local_all_writes_empty_array(c, comp_scope_of(c, v), nt_str(nt, v, "name"))) return 0;
  if (ty_is_array(vt)) {
    if (vt == TY_POLY_ARRAY || ty_is_ptr_array(vt) || elem == ty_array_elem(vt)) return 0;
    return widen_arg_array(c, v);
  }
  if (vt != TY_POLY) return 0;
  NodeKind k = nt_kind(nt, v);
  if (k == NK_LocalVariableWriteNode || k == NK_LocalVariableOrWriteNode)
    return widen_boxed_array_sources(c, nt_ref(nt, v, "value"), elem, depth + 1);
  if (k == NK_IfNode || k == NK_UnlessNode || k == NK_CaseNode) {
    int lv[16];
    int n = value_leaves(c, v, lv, 0, 16), ch = 0;
    for (int i = 0; i < n; i++) ch |= widen_boxed_array_sources(c, lv[i], elem, depth + 1);
    return ch;
  }
  if (k == NK_LocalVariableReadNode) {
    const char *nm = nt_str(nt, v, "name");
    Scope *sc = nm ? comp_scope_of(c, v) : NULL;
    LocalVar *lv = sc ? scope_local(sc, nm) : NULL;
    if (!lv || lv->rbs_seeded) return 0;
    int ch = 0;
    if (lv->is_block_param) {
      /* what an element iterator hands its block: an element of the
         receiver, or a Hash's value */
      int si = (int)(sc - c->scopes);
      for (int e = bp_ix_first(c, nm, si); e >= 0; e = bp_ix.next[e]) {
        if (bp_ix.scope[e] != si || !sp_streq(bp_ix.name[e], nm)) continue;
        int call = bp_ix.call[e], bi = bp_ix.idx[e];
        int r = nt_ref(nt, call, "receiver");
        const char *cn = nt_str(nt, call, "name");
        if (r < 0 || !cn) continue;
        TyKind rt = infer_type(c, r), yt[2];
        int elem_at = -1;
        if (ty_is_hash(rt))
          elem_at = sp_streq(cn, "each_value") ? 0 : sp_streq(cn, "each") || sp_streq(cn, "each_pair") ? 1 : -1;
        else if ((ty_is_array(rt) || rt == TY_POLY) && ty_block_yield(TY_POLY_ARRAY, cn, yt, 2) > 0)
          elem_at = 0;
        if (bi == elem_at) ch |= widen_boxed_elem_sources(c, r, elem, depth + 1);
      }
      /* a proc or lambda literal's parameter: what its calls pass there */
      int lit = local_proc_literal_param_of(c, sc, nm);
      if (lit >= 0) ch |= widen_proc_call_args(c, lit, nm, elem, depth + 1);
    }
    /* a `for` variable, bound as a block parameter is */
    NT_FOREACH_KIND(nt, NK_ForNode, f) {
      int ix = nt_ref(nt, f, "index");
      if (ix < 0 || nt_kind(nt, ix) != NK_LocalVariableTargetNode || comp_scope_of(c, f) != sc ||
          !sp_streq(nt_str(nt, ix, "name"), nm)) continue;
      ch |= widen_boxed_elem_sources(c, nt_ref(nt, f, "collection"), elem, depth + 1);
    }
    if (lv->is_block_param) return ch;
    if (lv->is_param) {
      if (lv->type != TY_POLY) return 0;
      TyKind *ev[2] = { &lv->boxed_push_elem, &lv->boxed_known_elem };
      for (int e = 0; e < 2; e++) {
        TyKind was = *ev[e];
        TyKind now = was == TY_UNKNOWN ? elem : (was == elem ? was : TY_POLY);
        if (now != was) { *ev[e] = now; ch = 1; }
      }
      return ch;
    }
    /* every value it is written, each followed on its own; where one the
       store cannot fit can only be converted at the slot, and all of them
       are arrays, the local is pinned to the general Array */
    int wl[32], nl = 0, all = !local_has_target_write(c, sc, nm);
    for (int r = lw_shared_first(c, nm, (int)(sc - c->scopes)); r >= 0; r = lw_shared_next(r)) {
      int w = lw_shared_node(r);
      NodeKind wk = nt_kind(nt, w);
      if (comp_scope_of(c, w) != sc || !sp_streq(nt_str(nt, w, "name"), nm)) continue;
      if (wk != NK_LocalVariableWriteNode && wk != NK_LocalVariableOrWriteNode) { all = 0; continue; }
      ch |= widen_boxed_array_sources(c, nt_ref(nt, w, "value"), elem, depth + 1);
      int got = all ? value_leaves(c, nt_ref(nt, w, "value"), wl, nl, 32) : -1;
      if (got < 0) { all = 0; continue; }
      for (int i = nl; i < got; i++)
        if (!(nt_kind(nt, wl[i]) == NK_LocalVariableReadNode && comp_scope_of(c, wl[i]) == sc &&
              sp_streq(nt_str(nt, wl[i], "name"), nm))) wl[nl++] = wl[i];
    }
    /* The pin is re-asserted at the end of every round's write pass, where
       the local re-derives from its writes: a change is only a new pin. */
    if (all && leaves_need_pin(c, wl, nl, elem) && leaves_widen_to_poly_array(c, wl, nl, 0)) {
      ch |= leaves_widen_to_poly_array(c, wl, nl, 1);
      if (!lv->poly_array_pin) { lv->poly_array_pin = 1; ch = 1; }
      lv->type = TY_POLY_ARRAY;
    }
    return ch;
  }
  if (k != NK_CallNode) return 0;
  const char *nm = nt_str(nt, v, "name");
  int r = nt_ref(nt, v, "receiver"), a = nt_ref(nt, v, "arguments"), an = 0;
  if (a >= 0) nt_arr(nt, a, "arguments", &an);
  if (!nm || nt_ref(nt, v, "block") >= 0) return 0;
  if (r >= 0 && ((sp_streq(nm, "[]") && an == 1) || (sp_streq(nm, "fetch") && an >= 1) ||
                 ((sp_streq(nm, "first") || sp_streq(nm, "last")) && an == 0)))
    return widen_boxed_elem_sources(c, r, elem, depth + 1);
  int mi = backprop_call_target(c, v);
  if (mi < 0) return 0;
  Scope *m = &c->scopes[mi];
  if (m->ret_rbs_seeded || m->cs_synth || m->is_lowered_yield || method_has_other_body(c, mi)) return 0;
  int lv[16];
  int n = method_value_leaves(c, mi, lv, 16), ch = 0;
  /* A value that is one of the method's parameters is, at this call, this
     call's argument for it (`def id(a) = a`), not every caller's. */
  int aa = nt_ref(nt, v, "arguments"), aargc = 0;
  const int *aargv = aa >= 0 ? nt_arr(nt, aa, "arguments", &aargc) : NULL;
  for (int i = 0; i < n; i++) {
    int pk = unassigned_param_read(c, m, lv[i]);
    int arg = pk >= 0 ? call_param_arg(c, m, aargv, aargc, pk) : -1;
    if (arg >= 0) lv[i] = arg;
  }
  if (n > 0 && leaves_need_pin(c, lv, n, elem) && leaves_widen_to_poly_array(c, lv, n, 0)) {
    ch |= leaves_widen_to_poly_array(c, lv, n, 1);
    if (!m->ret_poly_array_pin) { m->ret_poly_array_pin = 1; ch = 1; }
    return ch;
  }
  for (int i = 0; i < n; i++) ch |= widen_boxed_array_sources(c, lv[i], elem, depth + 1);
  return ch;
}

/* The same for the arrays that are elements of the container `r` (the
   values of a Hash), as far as its literals show them (container_literals). */
static int widen_boxed_elem_sources(Compiler *c, int r, TyKind elem, int depth) {
  const NodeTable *nt = c->nt;
  int lits[32], ch = 0;
  if (depth > 6) return 0;
  int nl = container_literals(c, r, lits, 0, 32, 0);
  for (int q = 0; q < nl; q++) {
    int en = 0; const int *ev = nt_arr(nt, lits[q], "elements", &en);
    for (int e = 0; e < en; e++) {
      int el = nt_kind(nt, ev[e]) == NK_AssocNode ? nt_ref(nt, ev[e], "value") : ev[e];
      if (el >= 0 && nt_kind(nt, el) != NK_SplatNode && nt_kind(nt, el) != NK_AssocSplatNode)
        ch |= widen_boxed_array_sources(c, el, elem, depth + 1);
    }
  }
  return ch;
}

/* 1 when a call passing `argv` has parameter `p` store into a container of
   type `ct` a key or value that `ct` cannot hold, where the method takes that
   key or value from another of its parameters (p's store_key_src and
   store_val_src): boxed, since its callers disagree, but this call's
   argument for it, as its layout `L` hands it, has a type. One spread from a
   splat or gathered has its elements' type only, and is not asked. */
int param_src_misfits(Compiler *c, Scope *m, LocalVar *p, TyKind ct, const int *argv,
                      const ArgLayout *L) {
  if (!p || !argv || !(p->store_key_src | p->store_val_src)) return 0;
  int arr = ty_is_array(ct) && ct != TY_POLY_ARRAY;
  if (!arr && !(ty_is_hash(ct) && ct != TY_POLY_POLY_HASH)) return 0;
  for (int j = 0; j < L->n && j < 64; j++) {
    int isk = (int)((p->store_key_src >> j) & 1ULL), isv = (int)((p->store_val_src >> j) & 1ULL);
    int a = isk || isv ? layout_plain_arg(c, m, argv, L, j) : -1;
    if (a < 0) continue;
    TyKind t = infer_type(c, a);
    if (t == TY_STRBUF) t = TY_STRING;
    if (t == TY_UNKNOWN || t == TY_POLY || t == TY_VOID) continue;
    if (arr) {
      if (isv && t != ty_array_elem(ct)) return 1;
      continue;
    }
    TyKind f = ct;
    if (isk && (!fold_container_evidence(&f, 0, 0, t, ty_hash_val(ct)) || f != ct)) return 1;
    f = ct;
    if (isv && (!fold_container_evidence(&f, 0, 0, ty_hash_key(ct), t) || f != ct)) return 1;
  }
  return 0;
}

/* Can the keyword hash `kwh` of a call bring a key that is no Symbol: a
   String or other literal key, a computed one answering something else, or
   a `**` of a hash keyed by another class, of one known only at run time,
   or of an enclosing `**kwrest` that takes any key already? */
static int kwh_brings_other_key(Compiler *c, int kwh) {
  const NodeTable *nt = c->nt;
  int en = 0; const int *el = nt_arr(nt, kwh, "elements", &en);
  for (int e = 0; e < en; e++) {
    if (nt_kind(nt, el[e]) == NK_AssocNode) {
      int key = nt_ref(nt, el[e], "key");
      if (key < 0 || nt_kind(nt, key) == NK_SymbolNode) continue;
      /* A computed key whose class is not settled yet waits for it while
         inference is optimistic; widening is never undone, so taking it then
         would widen every rest a Symbol-valued call reaches. Settled and
         still unknown (`$g = nil; rr($g => 1)`), it may be anything. */
      TyKind kt = infer_type(c, key);
      if (kt == TY_UNKNOWN ? !g_infer_optimistic : kt != TY_SYMBOL) return 1;
      continue;
    }
    if (nt_kind(nt, el[e]) != NK_AssocSplatNode) continue;
    int v = nt_ref(nt, el[e], "value");
    TyKind t = TY_UNKNOWN;
    if (v >= 0) t = infer_type(c, v);
    else {
      /* an anonymous `**` forwards the enclosing method's `**` */
      Scope *esc = comp_scope_of(c, el[e]);
      LocalVar *ek = esc && esc->kwrest_idx >= 0 && esc->pnames[esc->kwrest_idx]
                     ? scope_local(esc, esc->pnames[esc->kwrest_idx]) : NULL;
      if (ek) t = ek->type;
    }
    if (t == TY_POLY || (ty_is_hash(t) && ty_hash_key(t) != TY_SYMBOL)) return 1;
  }
  return 0;
}

/* A `**kwrest` parameter is a Symbol-keyed hash (sp_SymPolyHash), the fast
   path every keyword the call names by a Symbol fills. A key of another
   class reaches it too -- `def rr(**k); rr("s" => 1)` answers `{"s" => 1}`
   -- and the Symbol-keyed hash dropped such a key or raised on it, so a
   call that may bring one widens the parameter to a hash taking any key
   (TY_POLY_POLY_HASH), which every collector then fills. Only widened,
   never back: a method one call reaches with a String key takes any key
   from every call. */
static int widen_kwrest_any_key(Compiler *c, Scope *m, int node) {
  if (m->kwrest_idx < 0 || !m->pnames[m->kwrest_idx]) return 0;
  LocalVar *p = scope_local(m, m->pnames[m->kwrest_idx]);
  if (!p || p->rbs_seeded || p->type != TY_SYM_POLY_HASH) return 0;
  return slot_set(c, p, TY_POLY_POLY_HASH, TY_POLY_POLY_HASH, node);
}

/* May source `s` of a gathered call -- argument s, or at pos_argc the
   keyword hash the gather carries (gather_kwh) -- reach parameter i of `m`?
   Ruby funds the parameters ahead of a rest, or ahead of the requireds a
   leading optional leaves last, from the front of the arguments, and the
   others from the end. The count is the run time's, so a source's place is
   known only up to the splats around it: from the front it is its own index
   when no splat comes before it, and no less than the sources before it
   when one does, from the end the same; and a count below the required
   parameters is refused, so an argument no splat follows is never a front
   parameter the required ones after it take (`f(1, *s)` on
   `def f(a, *r, b)` gives b an element, never the 1), nor one the
   parameters funded from the end always take (`f(*s, 1)` on the same gives
   a an element). A splat, and a `**` hash that may be nothing
   (gather_kwh 1), reach every place from theirs on. A literal hash the
   gather carries (gather_kwh 2) is its last element, placed by the same
   rules: the last post when there is one. Taking it to reach every
   parameter from the first splat ARGUMENT's index on compared an argument
   index with a parameter's, so `m(1, 1, *v, k: 3)` on `def m(*r, q)` left
   q, parameter 1, Integer, and read the hash's pointer as one.
   kwh_rides_gather asks each parameter this reaches to hold the hash. */
int gather_reaches(Compiler *c, Scope *m, const int *argv, int pos_argc, int gather_kwh, int s, int i) {
  const NodeTable *nt = c->nt;
  int nsrc = pos_argc + (gather_kwh ? 1 : 0);
  int nb = 0, sb = 0, na = 0, sa = 0, var = 0;
  for (int k = 0; k < nsrc; k++) {
    if (k < pos_argc && nt_kind(nt, argv[k]) == NK_BlockArgumentNode) continue;
    int v = k < pos_argc ? nt_kind(nt, argv[k]) == NK_SplatNode : gather_kwh == 1;
    if (k == s) var = v;
    else if (k < s) { if (v) sb = 1; else nb++; }
    else { if (v) sa = 1; else na++; }
  }
  /* the positional parameters: every one but the rest and those bound by
     name (arg_layout's ARG_BY_NAME) */
  int last = -1, nreq = 0;
  for (int k = 0; k < m->nparams; k++) {
    if (k == m->rest_idx || k == m->kwrest_idx ||
        (m->pnames[k] && callee_param_is_declared_kwarg(c, m, m->pnames[k]))) continue;
    last = k;
    if (!m->pdefault || m->pdefault[k] < 0) nreq++;
  }
  int end_from = last + 1;
  if (m->rest_idx >= 0) end_from = m->rest_idx + 1;
  else if (opt_before_required(c, m))
    for (int k = 0; k <= last; k++)
      if (m->pdefault && m->pdefault[k] >= 0) end_from = k + 1;
  /* the fewest arguments the call runs with, less one */
  int least = (nb + na + !var > nreq ? nb + na + !var : nreq) - 1;
  if (i < end_from) {
    /* na from the end, with no splat after it: always one of the
       parameters funded from the end, which every count fills */
    if (!var && !sa && na <= last - end_from) return 0;
    if (var || (sb && sa)) return nb <= i;
    if (!sb) return nb == i;
    return i >= nb && i >= least - na;
  }
  int e = last - i;
  if (var || (sb && sa)) return na <= e;
  if (!sa) return na == e;
  return e >= na && e >= least - nb;
}

/* The positional layout of a call's arguments `argv` into method `m`, as
   inference asks it (arg_layout_untyped): a trailing keyword hash is the
   hash. Every pass that pairs a call's arguments with the callee's
   parameters asks it, not argument k for parameter k, which a leading
   optional, a splat, the posts or a hash taken as a positional put
   elsewhere. */
void call_layout(Compiler *c, Scope *m, const int *argv, int argc, ArgLayout *L) {
  int kwh = argv && argc > 0 && nt_kind(c->nt, argv[argc - 1]) == NK_KeywordHashNode ? argv[argc - 1] : -1;
  arg_layout_untyped(c, m, argv, kwh >= 0 ? argc - 1 : argc, kwh, L);
}

/* The argument a call's layout `L` into `m` hands parameter i as written,
   or -1: none, a splat's element, the keyword hash, or a gathered value more
   than one source may be (gather_reaches). */
int layout_plain_arg(Compiler *c, Scope *m, const int *argv, const ArgLayout *L, int i) {
  const NodeTable *nt = c->nt;
  if (!argv || i < 0 || i >= L->n || i == m->rest_idx) return -1;
  int a = -1;
  if (L->from[i] == ARG_NODE) a = argv[L->arg[i]];
  else if (L->gather && (L->from[i] == ARG_ELEM || L->from[i] == ARG_GATHERED))
    for (int s = 0; s < L->pos_argc + (L->gather_kwh ? 1 : 0); s++) {
      if (s < L->pos_argc && nt_kind(nt, argv[s]) == NK_BlockArgumentNode) continue;
      if (!gather_reaches(c, m, argv, L->pos_argc, L->gather_kwh, s, i)) continue;
      if (a >= 0 || s == L->pos_argc) return -1;
      a = argv[s];
    }
  return a < 0 || nt_kind(nt, a) == NK_SplatNode || nt_kind(nt, a) == NK_KeywordHashNode ? -1 : a;
}

/* The same for one parameter of one call. */
int call_param_arg(Compiler *c, Scope *m, const int *argv, int argc, int i) {
  ArgLayout L;
  call_layout(c, m, argv, argc, &L);
  int a = layout_plain_arg(c, m, argv, &L, i);
  arg_layout_free(&L);
  return a;
}

/* The type a parameter takes from a source its layout spreads, gathers or
   hands whole: a splat's elements (boxed when the operand is no typed
   array), the argument's, or the hash a braceless keyword hash is. A key
   that is no Symbol is no keyword, so `f('m' => 1)` passes a String-keyed
   Hash, which a Symbol-keyed parameter read as one empty key (#3487); the
   Symbol-keyed one stays for a hash whose type has not settled. A nil the
   parameter's slot cannot carry, or a value that never arrives, widens it
   to the boxed value. */
static TyKind bound_source_type(Compiler *c, int src, const LocalVar *p) {
  const NodeTable *nt = c->nt;
  TyKind t;
  if (nt_kind(nt, src) == NK_SplatNode) {
    int inner = nt_ref(nt, src, "expression");
    TyKind arr = inner >= 0 ? infer_type(c, inner) : TY_UNKNOWN;
    t = ty_is_array(arr) ? ty_array_elem(arr) : TY_POLY;
    if (t == TY_NIL) t = TY_POLY;
  }
  else if (nt_kind(nt, src) == NK_KeywordHashNode) {
    t = infer_type(c, src);
    if (!ty_is_hash(t)) t = TY_SYM_POLY_HASH;
  }
  else t = infer_type(c, src);
  if (t == TY_VOID) t = TY_POLY;
  if (t == TY_NIL && p->type != TY_UNKNOWN && p->type != TY_NIL && !ty_is_object(p->type)) t = TY_POLY;
  return t;
}

/* Type method mi's parameters from the arguments `argv` of call_id. */
static int bind_args_params(Compiler *c, int call_id, int mi, const int *argv, int argc) {
  if (mi < 0) return 0;
  const NodeTable *nt = c->nt;
  Scope *m = &c->scopes[mi];
  int changed = 0;
  /* `callee(...)`: the arg list is a single ForwardingArgumentsNode. Bind the
     callee's params from the enclosing `def foo(...)` method's synthesized
     __fwd_* params, positionally, so the callee's return type resolves (#1288).
     A `def foo(a, ...)` forwards only what follows its own leading params, so
     the binding starts at the first __fwd_ slot, where the call's emission
     (emit_args_filled_argv) starts: counted from the first param, the
     callee's first param took `a`'s type -- a splat param the method name's
     in `method_missing(name, ...)`, typing it apart from the Array its
     callers pass. */
  if (argc == 1 && argv && nt_type(nt, argv[0]) &&
      sp_streq(nt_type(nt, argv[0]), "ForwardingArgumentsNode")) {
    Scope *encl = comp_scope_of(c, argv[0]);
    if (!encl) return 0;
    int lead = 0;
    while (lead < encl->nparams &&
           (!encl->pnames[lead] || strncmp(encl->pnames[lead], "__fwd_", 6) != 0)) lead++;
    if (lead >= encl->nparams) lead = 0;  /* no __fwd_ slot: forward all */
    int n = m->nparams < encl->nparams - lead ? m->nparams : encl->nparams - lead;
    for (int k = 0; k < n; k++) {
      /* a splat param gathers the rest, an Array by construction, which no
         one forwarded value types */
      if (k == m->rest_idx) break;
      LocalVar *p = scope_local(m, m->pnames[k]);
      LocalVar *ep = scope_local(encl, encl->pnames[lead + k]);
      if (!p || p->rbs_seeded || !ep || ep->type == TY_UNKNOWN) continue;
      changed |= slot_take(c, p, ep->type, ep->why.node >= 0 ? ep->why.node : argv[0]);
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
  if (kwh >= 0 && m->kwrest_idx >= 0 && kwh_brings_other_key(c, kwh))
    changed |= widen_kwrest_any_key(c, m, kwh);
  /* Each parameter takes what the call's positional layout funds it from
     (arg_layout, the plan codegen renders): the argument it names, an
     element of the splat spread in place, the keyword hash as one more
     positional, or, where the count is the run time's, whichever gathered
     source may land there (gather_reaches). Typing them by their own index
     rules spread a splat from its own index and stopped, and took the
     posts from the positionals as written, so `f(1, *["x", :y])` on
     `def f(a, *r, b, c)` typed b from the 1, and a splat's elements typed
     the keyword parameters after it. The rest stays the untyped array, a
     `**kwrest` and the keywords bind by name (below; a positional typing
     the `**kwrest` broke the C build, #4395), and a default types its
     parameter where the callee reads it. */
  ArgLayout L;
  call_layout(c, m, argv, argc, &L);
  for (int i = 0; i < m->nparams; i++) {
    ArgFrom from = L.from[i];
    if (from == ARG_BY_NAME || from == ARG_DEFAULT || from == ARG_REST || i == m->rest_idx) continue;
    LocalVar *p = m->pnames[i] ? scope_local(m, m->pnames[i]) : NULL;
    if (!p || p->rbs_seeded) continue;
    int anode = layout_plain_arg(c, m, argv, &L, i);
    if (anode < 0 && !L.gather) {
      int src = from == ARG_KWH ? kwh : from == ARG_ELEM ? argv[L.splat] : argv[L.arg[i]];
      changed |= slot_take(c, p, bound_source_type(c, src, p), src);
      continue;
    }
    if (anode < 0) {
      for (int s = 0; s < pos_argc + (L.gather_kwh ? 1 : 0); s++) {
        int src = s < pos_argc ? argv[s] : kwh;
        if (nt_kind(nt, src) == NK_BlockArgumentNode || !gather_reaches(c, m, argv, pos_argc, L.gather_kwh, s, i)) continue;
        changed |= slot_take(c, p, bound_source_type(c, src, p), src);
      }
      continue;
    }
    const char *apty = nt_type(nt, anode);
    TyKind at = infer_type(c, anode);
    /* Post-convergence backstop only: an empty array-literal arg fills a
       parameter that no other call site typed, as an (empty) poly array so
       the callee's array methods dispatch. Never during the fixpoint -- a
       concrete kind from another call site must win the unification. */
    if (g_final_bind_pass && p->type == TY_UNKNOWN && at == TY_UNKNOWN &&
        apty && sp_streq(apty, "ArrayNode")) {
      int en0 = 0; nt_arr(nt, anode, "elements", &en0);
      if (en0 == 0) { slot_rule(c, p, TY_POLY_ARRAY, anode, "an empty `[]` argument and no other call site typing it: the parameter is the untyped array"); changed = 1; continue; }
    }
    /* An empty `{}` / `[]` literal carries no type of its own, so it is skipped
       by the unification below. When ANOTHER call site typed the parameter as
       the other container, though, the two cannot share one slot: the empty
       literal still emits as its own kind and the C assignment is ill-typed.
       Widen to poly, which holds either. */
    if (at == TY_UNKNOWN && apty && p->type != TY_UNKNOWN && p->type != TY_POLY) {
      int cross = (sp_streq(apty, "HashNode") && !ty_is_hash(p->type)) ||
                  (sp_streq(apty, "ArrayNode") && !ty_is_array(p->type));
      if (cross) {
        int en1 = 0; nt_arr(nt, anode, "elements", &en1);
        if (en1 == 0) { slot_rule(c, p, TY_POLY, anode, "an empty literal argument of one container kind where another call site passed the other: only the boxed slot holds both"); changed = 1; continue; }
      }
    }
    /* A void arg (`sink(always_raising_method)`) is nil-ish in value position:
       bind the param poly so it is declarable; the arg is emitted via
       emit_boxed (it diverges and yields nil). */
    if (at == TY_VOID) at = TY_POLY;
    /* A nil arg narrows against an object param (NULL encodes nil), and
       against a String, Integer or Float one, whose slots carry nil the same
       way (NULL, SP_INT_NIL, the float sentinel: ty_unify's nil joins). Any
       other non-object param widens to poly. */
    if (at == TY_NIL && p->type != TY_UNKNOWN && p->type != TY_NIL && !ty_is_object(p->type) &&
        p->type != TY_STRING && p->type != TY_INT && p->type != TY_FLOAT) at = TY_POLY;
    /* Two array parameters meet as the poly ARRAY, not the poly SCALAR: a
       param typed as one array kind at one call site and a different array
       kind at another is still a container, so its array methods (pop, <<)
       stay dispatchable (#2989, #3137). A push-widened param is the special
       case of this where the widening came from a use rather than a call. */
    TyKind merged;
    /* A push-widened param takes any array argument as the poly array, and
       decides that first: tested after the two-kinds rule, a param already on
       the poly array met an int-array argument as two kinds (-> poly), and
       the next round met it from poly (-> poly array), every round until the
       fixpoint's cap (#4962). One a boxed argument already made boxed stays
       boxed: taken back to the poly array by the next array argument, it met
       the boxed one again, and the two calls flipped it every round. */
    if (p->push_widened && ty_is_array(at))
      merged = p->type == TY_POLY ? TY_POLY : TY_POLY_ARRAY;
    else if (p->push_widened && p->type == TY_POLY_POLY_HASH && ty_is_hash(at))
      merged = TY_POLY_POLY_HASH;
    else if (ty_is_array(p->type) && ty_is_array(at) && p->type != at)
      /* Two array kinds meet as the poly SCALAR, not the poly ARRAY: the
         boxed value keeps its concrete array class, so the callee's array
         methods dispatch through the poly runtime by cls_id, and every call
         site pays an O(1) box instead of the O(n) element-boxing rebuild a
         poly-ARRAY parameter forces on a concrete typed-array argument
         (which regressed optcarrot 22% -- the empty-`[]`-seeded ivar arg
         types POLY_ARRAY for the whole fixpoint and only re-narrows to the
         int array after it, so the conflict is usually transient, not real). */
      merged = TY_POLY;
    else
      merged = ty_unify(p->type, at);
    changed |= slot_set(c, p, merged, at, anode);
    /* Reverse binding: an empty-`{}`-only local passed to a hash parameter is
       that hash container, filled inside the callee through the reference.
       Type the local as the param's hash so it is constructed (sp_<H>Hash_new)
       rather than passed as a NULL-deref'ing poly nil. */
    if (ty_is_hash(p->type) && apty && sp_streq(apty, "LocalVariableReadNode")) {
      const char *an = nt_str(nt, anode, "name");
      Scope *asc = an ? comp_scope_of(c, anode) : NULL;
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
      const char *an = nt_str(nt, anode, "name");
      Scope *asc = an ? comp_scope_of(c, anode) : NULL;
      LocalVar *al = asc ? scope_local(asc, an) : NULL;
      if (al && !al->is_param && !al->is_block_param &&
          (al->type == TY_UNKNOWN || al->type == TY_POLY || ty_is_hash(al->type)) &&
          al->type != TY_POLY_POLY_HASH &&
          local_all_writes_empty_hash(c, asc, an)) {
        al->type = TY_POLY_POLY_HASH; al->poly_hash_pin = 1; changed = 1;
      }
    }
    /* A key or value the method stores through a typed container parameter
       from another of its parameters is checked against this call's
       argument for it: one the container cannot hold widens it, as a
       literal of that kind does in infer_write_types. */
    int src_misfit = param_src_misfits(c, m, p, p->type == TY_POLY ? at : p->type, argv, &L) ||
                     param_rest_misfits(c, m, p, p->type == TY_POLY ? at : p->type, argv, argc);
    /* A caller that hands its own parameters on, as the container and as
       the key or value, stores them the same way: record it on the caller's
       container parameter, for its own callers' binding to check. */
    if (p->store_key_src | p->store_val_src) {
      Scope *cs = comp_scope_of(c, call_id);
      int ck = unassigned_param_read(c, cs, anode);
      LocalVar *cp = ck >= 0 ? scope_local(cs, cs->pnames[ck]) : NULL;
      for (int j = 0; cp && !cp->rbs_seeded && j < L.n && j < 64; j++) {
        int aj = layout_plain_arg(c, m, argv, &L, j);
        if (aj < 0) continue;
        int cj = unassigned_param_read(c, cs, aj);
        if ((p->store_key_src >> j) & 1ULL) changed |= mask_add(&cp->store_key_src, cj);
        if ((p->store_val_src >> j) & 1ULL) changed |= mask_add(&cp->store_val_src, cj);
      }
    }
    if (src_misfit && p->type != TY_POLY) {
      p->type = ty_is_array(p->type) ? TY_POLY_ARRAY : TY_POLY_POLY_HASH;
      p->push_widened = 1; changed = 1;
    }
    /* Reverse binding for an ARRAY argument: the callee mutates the very array
       the caller holds, so a push inside the method that widened the parameter
       to a poly array widens the caller's local too. Without this the caller
       kept its narrow element type and the in-method push emitted an
       ill-typed sp_StrArray_push of a symbol (#2989). Only the widening
       direction, only to the poly array, so it stays monotonic. A hash
       parameter a foreign store widened widens the caller's hash the same
       way. */
    if (p->push_widened && p->type != TY_POLY_POLY_HASH) changed |= widen_arg_array(c, anode);
    if (p->push_widened && p->type == TY_POLY_POLY_HASH && ty_is_hash(at))
      changed |= widen_arg_hash(c, anode);
    /* A BOXED parameter hides the container from its callee, so the element
       writes through it are checked here, against each caller's own. An
       array the binding cannot widen, where a store of a known kind cannot
       fit it, is marked for codegen, which refuses it: the store would
       promote a copy the caller never sees. A boxed store is left to run
       time, where it may well fit. */
    if (p->type == TY_POLY && ty_is_array(at) && at != TY_POLY_ARRAY &&
        ((p->boxed_push_elem != TY_UNKNOWN && p->boxed_push_elem != ty_array_elem(at)) || src_misfit)) {
      int w = widen_arg_array(c, anode);
      changed |= w;
      if (!w && (src_misfit || p->boxed_push_elem != TY_POLY) && c->store_misfit_arg && anode < c->node_cap)
        c->store_misfit_arg[anode] = 1;
    }
    /* A boxed argument hides its arrays the same way, one step further:
       they are followed back to where they are built, and each one the
       store cannot fit widens there (widen_boxed_array_sources). */
    if (p->type == TY_POLY && at == TY_POLY && p->boxed_known_elem != TY_UNKNOWN)
      changed |= widen_boxed_array_sources(c, anode, p->boxed_known_elem, 0);
    if (p->type == TY_POLY && ty_is_hash(at) && at != TY_POLY_POLY_HASH &&
        (p->boxed_store_key != TY_UNKNOWN || src_misfit)) {
      TyKind folded = at;
      int fits = p->boxed_store_key == TY_UNKNOWN ||
                 fold_container_evidence(&folded, 0, 0, p->boxed_store_key, p->boxed_store_val);
      if (src_misfit || !fits || folded != at) changed |= widen_arg_hash(c, anode);
    }
    if (merged == TY_PROC) {
      /* every call's proc: the parameter answers what any of them returns
         (each call overwriting it flipped the slot round after round) */
      TyKind pr = proc_ret_of(c, anode);
      if (pr != TY_UNKNOWN && p->proc_ret != TY_UNKNOWN) pr = ty_unify((TyKind)p->proc_ret, pr);
      if (pr != TY_UNKNOWN && p->proc_ret != (int)pr) { p->proc_ret = (int)pr; changed = 1; }
    }
  }
  arg_layout_free(&L);
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
        changed |= slot_take(c, p, at, kwh);
      }
    }
    /* The keywords the call names beside a `**` bind by name as well: the
       `**`'s value type alone typed `m(k1: "a", **h)` with `h = {"s" => 3}`
       an Integer keyword, and the String was read out of the merged hash as
       one, its pointer printed as a number. */
    {
      for (int e = 0; e < en; e++) {
        int key = nt_ref(nt, elems[e], "key");
        int val = nt_ref(nt, elems[e], "value");
        if (key < 0 || val < 0) continue;
        const char *kty = nt_type(nt, key);
        const char *kname = (kty && sp_streq(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
        if (!kname && nt_kind(nt, key) != NK_StringNode && callee_declares_kwargs(c, m)) {
          /* a computed key (`f(key(1) => v)`) may name any keyword: each
             takes its value's type, or a String bound into an Integer one
             read its pointer bits as a number */
          /* the value is stored boxed: one whose type is not known yet (an
             empty `[]`) holds any class, so the keyword does too */
          TyKind at = infer_type(c, val);
          if (at == TY_UNKNOWN || at == TY_NIL) at = TY_POLY;
          for (int i = 0; i < m->nparams; i++) {
            if (i == m->kwrest_idx || !m->pnames[i] || !callee_has_kwarg(c, m, m->pnames[i])) continue;
            LocalVar *p = scope_local(m, m->pnames[i]);
            if (!p || p->rbs_seeded) continue;
            changed |= slot_take(c, p, at, val);
          }
          continue;
        }
        if (!kname) continue;
        /* A key binds by name only to a parameter that IS a keyword. Looking
           the name up among ALL locals typed a POSITIONAL parameter from the
           key's value -- `def f(x); f(x: 9)` typed x from the 9 and then
           passed it nothing, answering 0 where Ruby answers `{x: 9}`. A
           positional parameter sharing the name takes the whole hash
           positionally, as the layout above says. */
        if (!callee_has_kwarg(c, m, kname)) continue;
        LocalVar *p = scope_local(m, kname);
        if (!p || p->rbs_seeded) continue;
        TyKind at = infer_type(c, val);
        changed |= slot_take(c, p, at, val);
      }
    }
  }
  return changed;
}

static int bind_zsuper_params(Compiler *c, int id, Scope *s, Scope *pm);

int bind_call_params(Compiler *c, int call_id, int mi) {
  int args = mi >= 0 ? nt_ref(c->nt, call_id, "arguments") : -1;
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(c->nt, args, "arguments", &argc) : NULL;
  return bind_args_params(c, call_id, mi, argv, argc);
}

/* Propagate param types from each prep-chain source scope (the transplanted
   module method) to the shadow scope it calls via super. The shadow scope has
   no AST call site, so bind_call_params never runs for it: each super in the
   source binds it as codegen lays that super out (emit_super), a bare one by
   the zsuper layout and one with arguments as a call. Typed slot by slot,
   `def m(a, b) = super` into `def m(a, *r)` gave the rest b's Integer. */
int propagate_prep_params(Compiler *c) {
  int changed = 0;
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    for (int k = 0; k < cls->nprep_chain; k++) {
      const char *from_name = cls->prep_from[k];
      const char *to_name   = cls->prep_to[k];
      /* a class method's chain is keyed `self.<name>` (comp_super_shadow) */
      int cm = strncmp(from_name, "self.", 5) == 0;
      int from_mi = cm ? comp_cmethod_in_class(c, ci, from_name + 5) : comp_method_in_class(c, ci, from_name);
      int to_mi = -1;
      for (int s = 0; s < c->nscopes; s++) {
        if (c->scopes[s].class_id == ci && c->scopes[s].is_cmethod == cm &&
            c->scopes[s].name && sp_streq(c->scopes[s].name, to_name)) {
          to_mi = s; break;
        }
      }
      if (from_mi < 0 || to_mi < 0) continue;
      Scope *fs = &c->scopes[from_mi];
      Scope *ts = &c->scopes[to_mi];
      int nsup = 0;
      NT_FOREACH_KIND(c->nt, NK_ForwardingSuperNode, id) {
        if (c->nscope[id] != from_mi) continue;
        changed |= bind_zsuper_params(c, id, fs, ts);
        nsup++;
      }
      NT_FOREACH_KIND(c->nt, NK_SuperNode, id) {
        if (c->nscope[id] != from_mi) continue;
        changed |= bind_call_params(c, id, to_mi);
        nsup++;
      }
      if (nsup > 0) continue;
      int n = fs->nparams < ts->nparams ? fs->nparams : ts->nparams;
      for (int i = 0; i < n; i++) {
        LocalVar *fp = scope_local(fs, fs->pnames[i]);
        LocalVar *tp = scope_local(ts, ts->pnames[i]);
        if (!fp || !tp || fp->type == TY_UNKNOWN || tp->rbs_seeded) continue;
        if (lv_widen(tp, fp->type)) changed = 1;
      }
    }
  }
  return changed;
}

/* Optional parameters get a type from their default value too. */
/* Does any call site supply a positional argument at index `pi` of method
   `sc`? By name, receiver-blind: a call of that name (or `new`, for
   initialize) with more than `pi` positional arguments, or one whose
   arguments cannot be counted (a splat, forwarding), or a reference that can
   call it any way (`method(:x)`, `send`, `super`). Unsure answers 1. */
/* Is parameter `pi` of `sc` a keyword parameter? Its default node is the
   value of one of the def's OptionalKeywordParameterNodes. */
static int param_is_keyword(Compiler *c, Scope *sc, int pi) {
  const NodeTable *nt = c->nt;
  if (sc->def_node < 0 || sc->pdefault[pi] < 0) return 0;
  int pn = nt_ref(nt, sc->def_node, "parameters");
  int kn = 0; const int *kws = pn >= 0 ? nt_arr(nt, pn, "keywords", &kn) : NULL;
  for (int i = 0; i < kn; i++)
    if (nt_ref(nt, kws[i], "value") == sc->pdefault[pi]) return 1;
  return 0;
}
static int param_supplied_anywhere(Compiler *c, Scope *sc, int pi) {
  const NodeTable *nt = c->nt;
  if (!sc->name) return 1;
  int is_init = sp_streq(sc->name, "initialize");
  int is_kw = param_is_keyword(c, sc, pi);
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    /* a dynamic reference can call it any way -- unless the name is a user
       method of the program's own (an LSP's `send(obj)`), which is a call
       like any other, or the reference names a literal other method */
    if (sp_streq(nm, "send") || sp_streq(nm, "__send__") || sp_streq(nm, "public_send") ||
        sp_streq(nm, "method") || sp_streq(nm, "instance_method") || sp_streq(nm, "define_method")) {
      if (an_user_defines_method(c, nm)) { if (!sp_streq(nm, sc->name)) continue; }
      else {
        int dargs = nt_ref(nt, id, "arguments"); int dn = 0;
        const int *dav = dargs >= 0 ? nt_arr(nt, dargs, "arguments", &dn) : NULL;
        const char *lit = NULL;
        if (dav && dn > 0) {
          NodeKind dk = nt_kind(nt, dav[0]);
          if (dk == NK_SymbolNode) lit = nt_str(nt, dav[0], "unescaped");
          else if (dk == NK_StringNode) lit = nt_str(nt, dav[0], "unescaped");
        }
        if (!lit || sp_streq(lit, sc->name)) return 1;
        continue;
      }
    }
    if (!sp_streq(nm, sc->name) && !(is_init && sp_streq(nm, "new"))) continue;
    /* `K.new(...)` reaches this initialize only from K or a class under it;
       a `new` on a constant naming another class is that class's. A `new`
       on anything but a written constant could be anyone's: unsure. */
    if (is_init && sp_streq(nm, "new")) {
      int recv = nt_ref(nt, id, "receiver");
      NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_CallNode;
      if (recv >= 0 && (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode)) {
        const char *rn = nt_str(nt, recv, "name");
        int rci = rn ? comp_class_index(c, rn) : -1;
        if (rci >= 0 && sc->class_id >= 0) {
          int under = 0;
          for (int k = rci; k >= 0; k = c->classes[k].parent) if (k == sc->class_id) { under = 1; break; }
          if (!under) continue;
        }
        /* a builtin's `new` (Hash.new(0), Array.new(n)) is not ours either */
        else if (rci < 0 && rn && is_builtin_class_name(rn)) continue;
      }
    }
    int args = nt_ref(nt, id, "arguments");
    int n = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &n) : NULL;
    for (int k = 0; k < n; k++) {
      NodeKind ak = nt_kind(nt, av[k]);
      const char *aty = nt_type(nt, av[k]);
      if (ak == NK_SplatNode || (aty && sp_streq(aty, "ForwardingArgumentsNode"))) return 1;
      /* a keyword parameter is supplied by a `name:` pair (or by a `**`
         spread, which may carry anything) */
      if (ak == NK_KeywordHashNode && is_kw) {
        int en = 0; const int *els = nt_arr(nt, av[k], "elements", &en);
        for (int e = 0; e < en; e++) {
          if (nt_kind(nt, els[e]) == NK_AssocSplatNode) return 1;
          int key = nt_ref(nt, els[e], "key");
          const char *kn = key >= 0 && nt_kind(nt, key) == NK_SymbolNode ? nt_str(nt, key, "value") : NULL;
          if (kn && sc->pnames[pi] && sp_streq(kn, sc->pnames[pi])) return 1;
        }
      }
    }
    /* a positional one takes what the call's layout funds it from, a
       braceless hash no keyword parameter claims among them (#4436) */
    if (!is_kw) {
      ArgLayout L;
      call_layout(c, sc, av, n, &L);
      int supplied = L.from[pi] != ARG_DEFAULT;
      arg_layout_free(&L);
      if (supplied) return 1;
    }
  }
  NT_FOREACH_KIND(nt, NK_SuperNode, id) { (void)id; return 1; }
  NT_FOREACH_KIND(nt, NK_ForwardingSuperNode, id) { (void)id; return 1; }
  return 0;
}
/* A `= nil` default no call site ever supplies a value for is the
   parameter's whole type, and the post-fixpoint backstop makes it poly (a
   boxed nil). Deciding that only after the fixpoint left the body's joins
   built on the dropped unknown: `spinel || ENV[...] || "x"` had typed
   String, the literal holding the ivar Array[String] and the callee's
   parameter with it, then the ivar re-derived untyped and the C had an
   sp_PolyArray * meeting an sp_StrArray * (#4583). Decided once, BEFORE the
   fixpoint: the first round's parameter binding already reads the ivar the
   parameter feeds, so a rule inside the round came too late for it. */
/* The fixpoint re-clears these parameters and so asks this every round, and
   each ask walks every call in the program. The answer reads only the
   program's shape (the calls, their names and argument lists), so it holds
   until the node table or the scopes change. */
static int *psa_memo;   /* per scope: [asked bits, supplied bits] */
static int psa_ns = -1, psa_nn = -1;
static int param_supplied_memo(Compiler *c, int s, int i) {
  if (i >= 31) return param_supplied_anywhere(c, &c->scopes[s], i);
  if (!psa_memo || psa_ns != c->nscopes || psa_nn != c->nt->count) {
    free(psa_memo);
    psa_memo = calloc((size_t)(c->nscopes > 0 ? c->nscopes : 1) * 2, sizeof(int));
    if (!psa_memo) return param_supplied_anywhere(c, &c->scopes[s], i);
    psa_ns = c->nscopes; psa_nn = c->nt->count;
  }
  int *asked = &psa_memo[2 * s], *yes = &psa_memo[2 * s + 1];
  if (!(*asked & (1 << i))) {
    if (param_supplied_anywhere(c, &c->scopes[s], i)) *yes |= 1 << i;
    *asked |= 1 << i;
  }
  return (*yes >> i) & 1;
}

void seed_unsupplied_nil_defaults(Compiler *c) {
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int i = 0; i < sc->nparams; i++) {
      if (sc->pdefault[i] < 0 || nt_kind(c->nt, sc->pdefault[i]) != NK_NilNode) continue;
      LocalVar *p = scope_local(sc, sc->pnames[i]);
      if (!p || p->rbs_seeded || p->type != TY_UNKNOWN) continue;
      if (param_supplied_memo(c, s, i)) continue;
      slot_rule(c, p, TY_POLY, sc->pdefault[i], "a `= nil` default and no call site typing it: the parameter holds nil or a value, untyped");
    }
  }
}
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
      /* A default that cannot complete (`for_user: (raise "...")`, the Rails
         shape for a keyword that must be passed) gives the slot no value, so
         it says nothing about the type: typed from it the parameter was void,
         which no C slot holds, and the method refused to compile. */
      if (dt == TY_NIL || dt == TY_UNKNOWN || dt == TY_VOID) continue;
      LocalVar *p = scope_local(sc, sc->pnames[i]);
      if (!p || p->rbs_seeded) continue;
      /* an empty literal default is untyped by the rule above, not by any
         value: say so; another default's value speaks for itself */
      TyKind merged = ty_unify(p->type, dt);
      if (merged != p->type && ty_degraded(merged) && !ty_degraded(p->type) && p->why.node < 0 &&
          (int)infer_type(c, sc->pdefault[i]) == TY_UNKNOWN)
        { slot_rule(c, p, merged, sc->pdefault[i], "an empty literal default, which has no element type: the parameter is the untyped container"); changed = 1; }
      else
        changed |= slot_set(c, p, merged, dt, sc->pdefault[i]);
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
  return str_in(m, set);
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
    /* a caller's boxed hash, or one not typed yet, is not this body's to
       decide: typed here, the call converted a copy and the callee's
       writes never reached the caller's hash */
    { int pi = -1;
      for (int q = 0; q < s->nparams; q++)
        if (s->pnames && s->pnames[q] && sp_streq(s->pnames[q], nt_str(nt, recv, "name"))) { pi = q; break; }
      if (pi >= 0 && param_gets_boxed_arg(c, s, pi)) continue; }
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

/* `3 + money` reaches the user operator through #coerce: Ruby asks
   money.coerce(3), which by convention answers [Money(3), money], and then
   calls `+` on the first element with the SECOND as its argument. That
   argument is the user object itself, so the operator's parameter has to be
   able to hold one. The direct call site (`money + 3`) is the only one with a
   call node, so the parameter settled on Integer and the coerce path then read
   a Money pointer out of the pair as a raw integer -- a pointer-sized number
   that changed between runs (#3491). */
int bind_coerce_operator_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  /* `coerce` is the numeric protocol's entry point, so its parameter holds a
     value of whatever type the other operand had. Bind it poly for every
     definition, not only the ones a `3 + obj` in this program reaches: with no
     such call site the slot stayed unknown for the whole fixpoint and only a
     late backstop lifted it to poly, too late for the factory it feeds (the
     `Q.scalar(other)` -> `Q.new` chain then took an sp_Rational parameter and
     the build failed). */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (!sc->name || !sp_streq(sc->name, "coerce")) continue;
    if (sc->class_id < 0 || sc->nparams < 1 || !sc->pnames[0]) continue;
    LocalVar *cp = scope_local(sc, sc->pnames[0]);
    if (!cp || cp->rbs_seeded) continue;
    if (lv_widen(cp, TY_POLY)) changed = 1;
  }
  /* A user `<=>` has a caller with no call node of its own:
     sp_obj_cmp_dispatch, the boxed hook the runtime comparator installs for
     sort / min / max / clamp and for every Comparable operator on a boxed
     receiver. That caller hands the operand over as an sp_RbVal and cannot
     know what is in it -- `money.clamp(1..5)` arrives at the very same `<=>`
     with an Integer.

     Typed from the statically resolved call sites alone, the parameter
     settled on whatever those passed: one `a <=> b` between two Money objects
     anywhere in the program pinned it to Money. Two things then went wrong at
     once. The body's own `other.is_a?(Integer)` branch folded away as dead,
     so the class stopped implementing the comparison it wrote; and the hook
     guarded its arm on a Money operand (it shapes the arm from this very
     type), so an Integer operand fell through to not-comparable and
     `money.clamp(1..5)` raised "comparison of Money with 1 failed" for a
     class that compares them perfectly well. Removing either the bare `<=>`
     or the clamp left the rest correct, which is the signature of a parameter
     type that did not account for every reachable caller.

     Bound poly only where the BODY asks whether the operand is something
     OTHER than the method's own class. Two narrower shapes must not be
     widened, and both are common. A `<=>` written as `@x <=> other.x` names
     one kind of operand and nothing else, so the hook's class-guarded arm is
     exactly right for it. So does `return nil unless other.is_a?(Set)`, which
     is Set's own: the guard and the hook's arm say the same thing, and a
     narrowed parameter answers not-comparable for everything else just as the
     body would. Widening those two as well was measured at 61 of 3819 corpus
     files changing, Set's whole specialization among them, for no change in
     any answer.

     A test against a DIFFERENT class is the author saying the method takes
     more than one kind of operand -- Money's `other.is_a?(Integer)` arm --
     and that is the case, and the only case, that narrowing destroys.
     `respond_to?` names no class at all, so it counts as asking about
     something else. */
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    int isa = nm && (sp_streq(nm, "is_a?") || sp_streq(nm, "kind_of?") ||
                     sp_streq(nm, "instance_of?"));
    int rtq = nm && sp_streq(nm, "respond_to?");
    if (!isa && !rtq) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || !sp_streq(rty, "LocalVariableReadNode")) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn) continue;
    Scope *sc = comp_scope_of(c, id);
    if (!sc || !sc->name || !sp_streq(sc->name, "<=>")) continue;
    if (sc->class_id < 0 || sc->nparams != 1 || !sc->pnames[0]) continue;
    if (sc->rest_idx >= 0 || !sp_streq(rn, sc->pnames[0])) continue;
    if (isa) {
      /* the class asked about: a bare constant, or the last segment of a
         path. An expression names no class we can compare, so it counts as
         asking about something else. */
      int ca = nt_ref(nt, id, "arguments");
      int nargs = 0; const int *av = ca >= 0 ? nt_arr(nt, ca, "arguments", &nargs) : NULL;
      if (av && nargs == 1) {
        const char *aty = nt_type(nt, av[0]);
        const char *cn = NULL;
        if (aty && (sp_streq(aty, "ConstantReadNode") || sp_streq(aty, "ConstantPathNode")))
          cn = nt_str(nt, av[0], "name");
        if (cn && sc->class_id < c->nclasses && c->classes[sc->class_id].name &&
            sp_streq(cn, c->classes[sc->class_id].name)) continue;   /* its own class */
      }
    }
    LocalVar *p = scope_local(sc, sc->pnames[0]);
    if (!p || p->rbs_seeded) continue;
    if (lv_widen(p, TY_POLY)) changed = 1;
  }
  /* An operator reached through a POLY receiver goes out to the runtime's
     user-binop dispatch, which hands the argument over boxed -- it cannot know
     what the argument is. So the operator's parameter has to be able to hold
     anything: typed from the one call site the compiler could resolve, the
     dispatch arm was guarded on that argument's class and a call with any
     other argument fell through to NoMethodError on a method the class
     defines (#3511). */
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !is_arith_op(nm) || nt_ref(nt, id, "block") >= 0) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || infer_type(c, recv) != TY_POLY) continue;
    int ca = nt_ref(nt, id, "arguments");
    int argc = 0; if (ca >= 0) nt_arr(nt, ca, "arguments", &argc);
    if (argc != 1) continue;
    for (int k = 0; k < c->nclasses; k++) {
      int mi = comp_method_in_chain(c, k, nm, NULL);
      if (mi < 0) continue;
      Scope *m = &c->scopes[mi];
      if (m->nparams < 1 || !m->pnames[0]) continue;
      LocalVar *p = scope_local(m, m->pnames[0]);
      if (!p || p->rbs_seeded || p->type == TY_POLY) continue;
      p->type = TY_POLY; changed = 1;
    }
  }
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !is_numeric_coerce_op(nm) || nt_ref(nt, id, "block") >= 0) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    TyKind rt = infer_type(c, recv);
    if (rt != TY_INT && rt != TY_FLOAT && rt != TY_RATIONAL && rt != TY_BIGINT) continue;
    int ca = nt_ref(nt, id, "arguments");
    int argc = 0; const int *argv = ca >= 0 ? nt_arr(nt, ca, "arguments", &argc) : NULL;
    if (!argv || argc != 1) continue;
    TyKind a0 = infer_type(c, argv[0]);
    if (!ty_is_object(a0)) continue;
    int acls = ty_object_class(a0);
    int coerce_mi = comp_method_in_chain(c, acls, "coerce", NULL);
    if (coerce_mi < 0) continue;
    /* The coerce call this route emits has no node of its own either, and it
       passes the numeric receiver BOXED. Nothing else binds that parameter:
       left alone it stayed unknown until a late backstop -- too late to widen
       the factory it feeds, so `Q.scalar(other)` took an sp_Q* and read a
       boxed 10 as a pointer (#3497) -- or, where an explicit `m.coerce(4)`
       did bind it, the two disagreed and the build failed (#3499). */
    {
      Scope *cm = &c->scopes[coerce_mi];
      if (cm->nparams >= 1 && cm->pnames[0]) {
        LocalVar *cp = scope_local(cm, cm->pnames[0]);
        if (cp && !cp->rbs_seeded) {
          if (lv_widen(cp, TY_POLY)) changed = 1;
        }
      }
    }
    int op_mi = comp_method_in_chain(c, acls, nm, NULL);
    if (op_mi < 0) continue;
    Scope *m = &c->scopes[op_mi];
    if (m->nparams < 1 || !m->pnames[0]) continue;
    LocalVar *p = scope_local(m, m->pnames[0]);
    if (!p || p->rbs_seeded) continue;
    /* Every operation reaches the class through the boxed hook now, which
       hands the second pair element over as an sp_RbVal -- and which VALUE
       that is depends on the idiom: `[Klass.new(v), self]` makes it the
       object, `[Klass.new, v]` makes it the NUMBER. Narrowing the parameter
       to the object's class guards the dispatch arm on that class, so the
       second idiom's arm falls through and `5 + obj` raises NoMethodError on
       a `+` the class defines (found by matz reviewing #4265). The parameter
       has to be poly. */
    if (lv_widen(p, TY_POLY)) changed = 1;
  }
  return changed;
}

int struct_zsuper_param(Compiler *c, Scope *s, int a, const char *member, int *rest_off) {
  *rest_off = -1;
  int pos = 0, after_rest = 0;
  for (int k = 0; k < s->nparams; k++) {
    const char *pn = s->pnames[k];
    if (!pn || k == s->kwrest_idx) continue;
    if (k == s->rest_idx) { after_rest = 1; continue; }
    if (callee_param_is_declared_kwarg(c, s, pn)) {
      if (sp_streq(pn, member)) return k;
      continue;
    }
    if (after_rest) continue;
    if (pos++ == a) return k;
  }
  if (s->rest_idx >= 0 && s->rest_idx < s->nparams && s->pnames[s->rest_idx]) {
    *rest_off = a - pos;
    return s->rest_idx;
  }
  return -1;
}

int struct_super_spreads(Compiler *c, int args) {
  const NodeTable *nt = c->nt;
  int an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  for (int a = 0; a < an; a++) {
    NodeKind k = nt_kind(nt, av[a]);
    if (k == NK_SplatNode) return 1;
    if (k != NK_KeywordHashNode) continue;
    int kn = 0; const int *ke = nt_arr(nt, av[a], "elements", &kn);
    for (int e = 0; e < kn; e++)
      if (nt_kind(nt, ke[e]) == NK_AssocSplatNode) return 1;
  }
  return 0;
}

static int param_defaults_to_nil(Compiler *c, Scope *s, const char *name) {
  for (int k = 0; name && s->pdefault && k < s->nparams; k++)
    if (s->pnames[k] && sp_streq(s->pnames[k], name))
      return s->pdefault[k] >= 0 && nt_kind(c->nt, s->pdefault[k]) == NK_NilNode;
  return 0;
}

/* `super` in a Struct or Data's own initialize is what sets the members, so
   the values it passes type them: a forwarded parameter's type, which
   includes a `nil` default, or an explicit argument's. Typed only from the
   `.new` arguments, a member read the nil a default left as that type's
   zero. */
static int struct_super_types_members(Compiler *c, int id, Scope *s) {
  const NodeTable *nt = c->nt;
  ClassInfo *cls = &c->classes[s->class_id];
  int changed = 0;
  int fwd = nt_kind(nt, id) == NK_ForwardingSuperNode;
  int args = fwd ? -1 : nt_ref(nt, id, "arguments");
  int an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  int kwh = (an == 1 && nt_kind(nt, av[0]) == NK_KeywordHashNode) ? av[0] : -1;
  /* A `*a` or `**h` decides only at run time which value reaches which
     member, so every member takes a boxed value. */
  int spreads = !fwd && struct_super_spreads(c, args);
  for (int a = 0; a < cls->nmembers; a++) {
    if (class_ivar_pinned(cls, cls->ivars[a])) continue;
    if (spreads) {
      TyKind sm = ty_unify(cls->ivar_types[a], TY_POLY);
      if (sm != cls->ivar_types[a]) { cls->ivar_types[a] = sm; changed = 1; }
      continue;
    }
    const char *mname = cls->ivars[a] + 1;
    TyKind at = TY_UNKNOWN;
    int nilable = 0;
    if (fwd) {
      int roff;
      int k = struct_zsuper_param(c, s, a, mname, &roff);
      LocalVar *lv = k >= 0 ? scope_local(s, s->pnames[k]) : NULL;
      if (lv && roff >= 0) at = TY_POLY;
      else if (lv) { at = lv->type; nilable = lv->nullable_int || param_defaults_to_nil(c, s, s->pnames[k]); }
    }
    else {
      int vnode = -1;
      if (kwh >= 0) {
        int kn = 0; const int *ke = nt_arr(nt, kwh, "elements", &kn);
        for (int e = 0; e < kn; e++) {
          if (nt_kind(nt, ke[e]) != NK_AssocNode) continue;
          int key = nt_ref(nt, ke[e], "key");
          if (key >= 0 && nt_kind(nt, key) == NK_SymbolNode &&
              nt_str(nt, key, "value") && sp_streq(nt_str(nt, key, "value"), mname)) {
            vnode = nt_ref(nt, ke[e], "value"); break;
          }
        }
      }
      else if (a < an && nt_kind(nt, av[a]) != NK_SplatNode) vnode = av[a];
      if (vnode >= 0) {
        at = infer_type(c, vnode);
        /* an initialize parameter that is the handle (#6179), handed on as
           the bare `super` hands it: the member takes the handle, so an
           append after the `super` is the member's too */
        if (nt_kind(nt, vnode) == NK_LocalVariableReadNode) {
          LocalVar *hv = scope_local(s, nt_str(nt, vnode, "name"));
          if (hv && hv->dyn_handle && hv->type == TY_STRBUF && hv->str_shared) at = TY_STRBUF;
        }
        nilable = nullable_int_value(c, vnode) ||
                  (nt_kind(nt, vnode) == NK_LocalVariableReadNode &&
                   param_defaults_to_nil(c, s, nt_str(nt, vnode, "name")));
      }
    }
    if (at == TY_UNKNOWN) continue;
    TyKind m = ty_unify(cls->ivar_types[a], at);
    /* A Float member boxes without the nil check an Integer one has, so one
       that can be nil is kept boxed. */
    if (m == TY_FLOAT && (nilable || at == TY_NIL)) m = TY_POLY;
    if (m != cls->ivar_types[a]) { cls->ivar_types[a] = m; changed = 1; }
  }
  return changed;
}

/* The type a Struct constructor's argument `v` gives its member. An empty
   container literal (or `Array.new` / `Hash.new`) has no type of its own
   until a use fills it in, and a member has no write of its own to be
   filled through: it stayed UNKNOWN to the backstop and read back boxed.
   Take the empty container's kind, the way an `@ivar = []` write does
   (#4460). */
static TyKind struct_member_arg_type(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  TyKind at = infer_type(c, v);
  if (at != TY_UNKNOWN) return at;
  NodeKind vk = nt_kind(nt, v);
  if (vk == NK_ArrayNode) return TY_POLY_ARRAY;
  if (vk == NK_HashNode) return TY_POLY_POLY_HASH;
  if (vk == NK_CallNode) {
    const char *vn = nt_str(nt, v, "name"); int vr = nt_ref(nt, v, "receiver");
    int va = nt_ref(nt, v, "arguments"); int van = 0; if (va >= 0) nt_arr(nt, va, "arguments", &van);
    const char *vrn = vr >= 0 && nt_kind(nt, vr) == NK_ConstantReadNode ? nt_str(nt, vr, "name") : NULL;
    if (vn && vrn && sp_streq(vn, "new") && van == 0 && nt_ref(nt, v, "block") < 0) {
      if (sp_streq(vrn, "Array")) return TY_POLY_ARRAY;
      if (sp_streq(vrn, "Hash")) return TY_POLY_POLY_HASH;
    }
  }
  return at;
}

/* Struct construction: positional (or keyword) args set the member ivars
   in order. Shared by every spelling that constructs the struct: the
   constant, a qualified path, a local holding an anonymous struct, and a
   bare `new` / `self.new` in one of the struct's own class methods, which
   typed nothing and left every member boxed (#4842). */
static int struct_new_types_members(Compiler *c, int id, int ci) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  ClassInfo *cls = &c->classes[ci];
  /* A user initialize receives the arguments; its `super` sets the members
     (struct_super_types_members). */
  int imi = comp_method_in_chain(c, ci, "initialize", NULL);
  if (imi >= 0) return bind_call_params(c, id, imi);
  int args = nt_ref(nt, id, "arguments");
  int an = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  int kwh = (an == 1 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) ? argv[0] : -1;
  /* `S.new(*args)`: from the splat on, a member takes whatever element lands
     there, or nil when a short array leaves it unset -- a run-time shape, so
     the member is boxed. Reading the SplatNode as one positional argument
     pinned the members to the other construction sites' types: a String
     element arriving in an Integer member was unboxed as garbage, and the
     nil fill as 0. */
  int splat_at = -1;
  for (int a = 0; a < an && kwh < 0; a++)
    if (nt_kind(nt, argv[a]) == NK_SplatNode) { splat_at = a; break; }
  /* `S.new(k: v, **h)`: likewise, a member no literal key names takes
     whatever h holds under its name, or nil -- boxed, as a `**h` binds a
     method's keyword parameters. Typing it nil left it to the other
     construction sites, and the boxed value pulled out of h did not fit a
     member they had made a String. So is a member a literal key names
     ahead of the `**`, whose value h replaces when it carries the key; a
     literal key after the last `**` is the member's. */
  int kw_splat = 0;
  if (kwh >= 0) {
    int kn = 0; const int *ke = nt_arr(nt, kwh, "elements", &kn);
    for (int e = 0; e < kn; e++)
      if (nt_kind(nt, ke[e]) == NK_AssocSplatNode) kw_splat = 1;
  }
  /* A keyword_init: false Struct takes the keywords as one positional Hash,
     its first member. With a `**` that Hash is nil when no keyword came, so
     the member is boxed, and the members after it are left nil. */
  int kwf = cls->kw_init == -1;
  if (kwf && !kw_splat) kwh = -1;
  for (int a = 0; a < cls->nmembers; a++) {
    /* a member not supplied at this construction can be nil */
    const char *mname = cls->ivars[a] + 1;
    int kn = 0;
    const int *ke = kwh >= 0 ? nt_arr(nt, kwh, "elements", &kn) : NULL;
    int vnode = -1, splat_after = 0;
    if (kwh >= 0) {
      for (int e = 0; e < kn && !kwf; e++) {
        if (nt_kind(nt, ke[e]) == NK_AssocSplatNode) { splat_after = 1; continue; }
        int key = nt_ref(nt, ke[e], "key");
        if (key >= 0 && nt_type(nt, key) && sp_streq(nt_type(nt, key), "SymbolNode") &&
            nt_str(nt, key, "value") && sp_streq(nt_str(nt, key, "value"), mname)) {
          if (vnode < 0) vnode = nt_ref(nt, ke[e], "value");
          splat_after = 0;
        }
      }
    }
    else if (a < an) vnode = argv[a];
    if (class_ivar_pinned(cls, cls->ivars[a])) continue;
    if ((splat_at >= 0 && a >= splat_at) || (kw_splat && (vnode < 0 || splat_after) && (!kwf || a == 0))) {
      TyKind sm = ty_unify(cls->ivar_types[a], TY_POLY);
      if (sm != cls->ivar_types[a]) { cls->ivar_types[a] = sm; changed = 1; }
      continue;
    }
    TyKind at = vnode >= 0 ? struct_member_arg_type(c, vnode) : TY_NIL;
    TyKind m = ty_unify(cls->ivar_types[a], at);
    /* Any other literal key may bind the member too: its name written
       again, whose last value CRuby binds, or a key that is not a Symbol,
       which names a member only at run time, a String by name and a
       Struct's other key by index. */
    for (int e = 0; e < kn && !kwf; e++) {
      int key = nt_ref(nt, ke[e], "key"), v = nt_ref(nt, ke[e], "value");
      if (nt_kind(nt, ke[e]) != NK_AssocNode || v == vnode) continue;
      const char *sn = nt_kind(nt, key) == NK_SymbolNode ? nt_str(nt, key, "value") : NULL;
      if (!sn || sp_streq(sn, mname))
        m = ty_unify(m, struct_member_arg_type(c, v));
    }
    if (m != cls->ivar_types[a]) { cls->ivar_types[a] = m; changed = 1; }
    /* An empty `{}` argument has no keys of its own: build it as the variant
       the member settles on, which writes through the member's reader can
       widen past what the literal alone says. Built as its own default it
       was a different hash type from the member it initializes. */
    if (vnode >= 0 && c->hash_want && vnode < c->node_cap && ty_is_hash(m) &&
        nt_kind(nt, vnode) == NK_HashNode && c->hash_want[vnode] != m) {
      int hen = 0; nt_arr(nt, vnode, "elements", &hen);
      if (hen == 0) { c->hash_want[vnode] = m; changed = 1; }
    }
  }
  return changed;
}

/* See analyze.h. */
int zsuper_kw_positional(Compiler *c, Scope *s, Scope *pm) {
  const NodeTable *nt = c->nt;
  if (!s || !pm || pm->kwrest_idx >= 0 || pm->def_node < 0) return 0;
  int pn = nt_ref(nt, pm->def_node, "parameters");
  int kn = 0;
  if (pn >= 0) nt_arr(nt, pn, "keywords", &kn);
  if (kn > 0 || (pn >= 0 && nt_ref(nt, pn, "keyword_rest") >= 0)) return 0;
  for (int i = 0; i < s->nparams; i++)
    if (s->pnames[i] && (i == s->kwrest_idx || callee_param_is_declared_kwarg(c, s, s->pnames[i])))
      return 1;
  return 0;
}

/* The parameters of the parent a bare `super` in `s` reaches, typed as it
   binds them (codegen's zsuper_begin): with a rest in `s` the positionals
   gather, a parameter the gather funds from the front taking the element
   at its index, one as far from the end as a post of `s` that post, and any
   other it funds from the end (a post after the parent's rest, a required
   after its leading optionals) any of them; without, they
   are laid out as a call of `s`'s positional count (arg_layout), each
   parent parameter typed from the one of `s` it takes. A keyword takes
   `s`'s like-named keyword, or a boxed value from `s`'s `**` (as a call's
   `**h` gives one), which codegen reads it from (zsuper_kw_begin). Keywords
   a parent without any take as a positional Hash (zsuper_kw_positional)
   may land in any of its positionals, which are boxed for it. */
static int bind_zsuper_params(Compiler *c, int id, Scope *s, Scope *pm) {
  int changed = 0;
  if (zsuper_kw_positional(c, s, pm))
    for (int i = 0; i < pm->nparams; i++) {
      if (i == pm->rest_idx || !pm->pnames[i]) continue;
      LocalVar *p = scope_local(pm, pm->pnames[i]);
      if (p && !p->rbs_seeded) changed |= slot_take(c, p, TY_POLY, id);
    }
  /* the parent's `**` takes this method's own */
  if (pm->kwrest_idx >= 0 && s->kwrest_idx >= 0) {
    LocalVar *src = scope_local(s, s->pnames[s->kwrest_idx]);
    LocalVar *dst = scope_local(pm, pm->pnames[pm->kwrest_idx]);
    if (src && dst && !dst->rbs_seeded && src->type != TY_UNKNOWN) {
      if (lv_widen(dst, src->type)) changed = 1;
    }
  }
  for (int i = 0; i < pm->nparams; i++) {
    if (i == pm->kwrest_idx || !callee_param_is_declared_kwarg(c, pm, pm->pnames[i])) continue;
    LocalVar *dst = scope_local(pm, pm->pnames[i]);
    if (!dst || dst->rbs_seeded) continue;
    if (!callee_param_is_declared_kwarg(c, s, pm->pnames[i])) {
      if (s->kwrest_idx >= 0) changed |= slot_take(c, dst, TY_POLY, id);
      continue;
    }
    LocalVar *src = scope_local(s, pm->pnames[i]);
    if (!src || src->type == TY_UNKNOWN) continue;
    if (lv_widen(dst, src->type)) changed = 1;
  }
  int srest = s->rest_idx;
  if (srest < 0 || !s->pnames[srest]) {
    int npos = 0;
    while (npos < s->nparams && npos != s->kwrest_idx &&
           !callee_param_is_declared_kwarg(c, s, s->pnames[npos])) npos++;
    for (int i = 0; i < pm->nparams; i++) {
      int a = arg_layout_plain_arg(c, pm, npos, i);
      if (a < 0) continue;
      LocalVar *src = scope_local(s, s->pnames[a]);
      LocalVar *dst = scope_local(pm, pm->pnames[i]);
      if (!src || !dst || dst->rbs_seeded || src->type == TY_UNKNOWN) continue;
      if (lv_widen(dst, src->type)) changed = 1;
    }
    return changed;
  }
  /* gathered: the parameters ahead of the rest in order, any element after */
  LocalVar *rv = scope_local(s, s->pnames[srest]);
  TyKind rt = rv ? rv->type : TY_UNKNOWN;
  TyKind at = ty_is_array(rt) ? ty_array_elem(rt) : TY_POLY;
  if (at == TY_VOID || at == TY_NIL || at == TY_UNKNOWN) at = TY_POLY;
  int opt_seen = 0;
  for (int pk = 0; pk < pm->nparams; pk++) {
    if (pk == pm->rest_idx || pk == pm->kwrest_idx) continue;
    LocalVar *p = pm->pnames[pk] ? scope_local(pm, pm->pnames[pk]) : NULL;
    if (!p || p->rbs_seeded || callee_param_is_declared_kwarg(c, pm, pm->pnames[pk])) continue;
    int optional = pm->pdefault && pm->pdefault[pk] >= 0;
    /* one as far from the end as a post of this method is that post, whatever
       the count: the gather ends with them (zsuper_param_source) */
    int own = zsuper_param_source(c, s, pm, pk);
    if (own > srest) {
      LocalVar *src = scope_local(s, s->pnames[own]);
      if (src && src->type != TY_UNKNOWN) {
        if (lv_widen(p, src->type)) changed = 1;
      }
      opt_seen |= optional;
      continue;
    }
    /* counted from the end of the gather: any of this method's positionals
       or the rest's elements, as the count is the run time's */
    if ((pm->rest_idx >= 0 && pk > pm->rest_idx) || (opt_seen && !optional)) {
      for (int j = 0; j < srest; j++) {
        LocalVar *src = scope_local(s, s->pnames[j]);
        if (!src || src->type == TY_UNKNOWN) continue;
        if (lv_widen(p, src->type)) changed = 1;
      }
      if (rt != TY_UNKNOWN) changed |= slot_take(c, p, at, id);
      continue;
    }
    opt_seen |= optional;
    if (pk < srest && (pm->rest_idx < 0 || pk < pm->rest_idx)) {
      LocalVar *src = scope_local(s, s->pnames[pk]);
      if (!src || src->type == TY_UNKNOWN) continue;
      if (lv_widen(p, src->type)) changed = 1;
    }
    else if (rt != TY_UNKNOWN && pk >= srest && (pm->rest_idx < 0 || pk < pm->rest_idx))
      changed |= slot_take(c, p, at, id);
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
      /* super in `self.new` is Class#new: its arguments reach the
         initialize of whichever class receives it */
      if (comp_super_is_class_new(c, id)) {
        for (int k = 0; k < c->nclasses; k++) {
          if (k != s->class_id && !is_descendant(c, k, s->class_id)) continue;
          int imi = comp_method_in_chain(c, k, "initialize", NULL);
          if (imi < 0) continue;
          if (sp_streq(ty, "SuperNode")) { changed |= bind_call_params(c, id, imi); continue; }
          Scope *im = &c->scopes[imi];
          /* A forwarder (`def self.new(*a, **k, &blk) ... super end`) passes
             the CONTENTS of its rest array and keyword-rest hash on, not the
             containers: mapping parameter q to parameter q typed the
             constructor's first parameter as the array itself. What arrives
             is decided at run time, so every parameter takes a boxed value. */
          if (s->rest_idx >= 0 || s->kwrest_idx >= 0) {
            for (int q = 0; q < im->nparams; q++) {
              LocalVar *dst = scope_local(im, im->pnames[q]);
              if (!dst || dst->rbs_seeded) continue;
              if (lv_widen(dst, TY_POLY)) changed = 1;
            }
            continue;
          }
          for (int q = 0; q < s->nparams && q < im->nparams; q++) {
            LocalVar *src = scope_local(s, s->pnames[q]);
            LocalVar *dst = scope_local(im, im->pnames[q]);
            if (!src || !dst || dst->rbs_seeded || src->type == TY_UNKNOWN) continue;
            if (lv_widen(dst, src->type)) changed = 1;
          }
        }
        continue;
      }
      if (c->classes[s->class_id].is_struct && sp_streq(s->name, "initialize")) {
        changed |= struct_super_types_members(c, id, s);
        continue;
      }
      int p = comp_super_parent(c, s->class_id, 0);
      if (p < 0) continue;
      int pmi = comp_method_in_chain(c, p, s->name, NULL);
      if (pmi < 0) continue;
      if (sp_streq(ty, "ForwardingSuperNode")) changed |= bind_zsuper_params(c, id, s, &c->scopes[pmi]);
      else changed |= bind_call_params(c, id, pmi);
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
      changed |= slot_take(c, pp, at2, val);
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
          /* the message is `Cls.new(arg)`'s one argument, into the
             parameter its layout funds: `initialize(a = 5, b)` takes it in b */
          Scope *im = imi >= 0 ? &c->scopes[imi] : NULL;
          for (int i = 0; im && i < im->nparams; i++) {
            if (call_param_arg(c, im, &rav[1], 1, i) != rav[1]) continue;
            LocalVar *ip = scope_local(im, im->pnames[i]);
            TyKind at = infer_type(c, rav[1]);
            if (ip && !ip->rbs_seeded && at != TY_UNKNOWN) changed |= slot_take(c, ip, at, rav[1]);
          }
        }
      }
      /* `raise k, arg` with the class in a variable: every exception class
         whose value can reach k is constructed from arg the same way */
      else if (ran >= 2 && nt_kind(nt, rav[1]) != NK_KeywordHashNode) {
        TyKind kt = infer_type(c, rav[0]);
        TyKind at = kt == TY_CLASS || kt == TY_POLY ? infer_type(c, rav[1]) : TY_UNKNOWN;
        for (int rci = 0; at != TY_UNKNOWN && rci < c->nclasses; rci++) {
          if (!class_is_exc_subclass(c, rci) || !class_value_escapes(c, rci)) continue;
          int imi = comp_method_in_chain(c, rci, "initialize", NULL);
          Scope *im = imi >= 0 ? &c->scopes[imi] : NULL;
          for (int i = 0; im && i < im->nparams; i++) {
            if (call_param_arg(c, im, &rav[1], 1, i) != rav[1]) continue;
            LocalVar *ip = scope_local(im, im->pnames[i]);
            if (ip && !ip->rbs_seeded) changed |= slot_take(c, ip, at, rav[1]);
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
            if (lv_widen(dp, ty_object(dcid))) changed = 1;
          }
        }
      }
    }

    /* <method>.call(args): bind the call-site arg types to the target
       method's params (the Method ABI is the only call site for a method
       reached solely via method(:sym)). Method#=== is a call too: left
       out, `method(:len) === "abc"` handed a String to an Integer slot. */
    if (recv >= 0 && name && (sp_streq(name, "call") || sp_streq(name, "[]") || sp_streq(name, "()") ||
                              sp_streq(name, "===")) &&
        infer_type(c, recv) == TY_METHOD) {
      /* a local re-written to several Methods calls whichever it holds:
         each target takes the arguments */
      int *mns;
      int nmn = method_recv_nodes(c, recv, &mns);
      for (int j = 0; j < nmn; j++) {
        int mn = mns[j];
        int tmi = method_obj_target_mi(c, mn);
        if (tmi < 0) continue;
        int shift = method_call_param_shift(c, mn, tmi);
        if (shift) changed |= bind_call_args_shifted(c, id, tmi, shift);
        else changed |= bind_call_params(c, id, tmi);
      }
      free(mns);
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
        if (ban > 0) changed |= bind_args_params(c, id, tmi, bav + 1, ban - 1);
        continue;
      }
    }
    /* The same on a Class value known only at run time: every class's own
       target may be the one bound, so each takes the arguments. */
    if (recv >= 0 && name && sp_streq(name, "bind_call")) {
      const char *cvsym = class_value_instance_method_sym(c, recv);
      int bargs = cvsym ? nt_ref(nt, id, "arguments") : -1;
      int ban = 0; const int *bav = bargs >= 0 ? nt_arr(nt, bargs, "arguments", &ban) : NULL;
      for (int k = 0; ban > 0 && k < c->nclasses; k++) {
        int tmi = class_value_bind_call_target(c, k, cvsym, NULL);
        if (tmi >= 0) changed |= bind_args_params(c, id, tmi, bav + 1, ban - 1);
      }
      if (cvsym) continue;
    }

    /* <method>.to_proc stored as a Proc: its .call sites are likewise the
       only way the target method is reached, so bind their arg types to the
       target's params (the emitted trampoline calls the real C signature). */
    if (recv >= 0 && name && (sp_streq(name, "call") || sp_streq(name, "[]") || sp_streq(name, "()")) &&
        infer_type(c, recv) == TY_PROC) {
      int *mns, bound = 0;
      int nmn = proc_to_proc_method_nodes(c, recv, &mns);
      for (int j = 0; j < nmn; j++) {
        int mn = mns[j];
        int tmi = method_obj_target_mi(c, mn);
        if (tmi < 0) continue;
        int shift = method_call_param_shift(c, mn, tmi);
        if (shift) changed |= bind_call_args_shifted(c, id, tmi, shift);
        else changed |= bind_call_params(c, id, tmi);
        bound = 1;
      }
      free(mns);
      if (bound) continue;
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

    /* `m(&pr)` where whether there IS a proc is decided at run time: the
       value is poly (a proc or nil), so nothing at the call site can bind
       the literal's parameters to what `m` yields, and the yield hands them
       over through the boxed side-channel. Typed from nothing, the literal
       read its parameter as the default scalar -- `proc { |x| x.path }`
       passed an object asked it for a method Integer has, at run time, with
       nothing raised at compile time. The same widening the composition and
       curry sites above do, for the same reason. A proc whose presence is
       STATIC keeps its bound parameter types: it is spliced, not boxed. */
    { int bnode = nt_ref(nt, id, "block");
      if (bnode >= 0 && nt_kind(nt, bnode) == NK_BlockArgumentNode) {
        int bexpr = nt_ref(nt, bnode, "expression");
        if (bexpr >= 0 && infer_type(c, bexpr) == TY_POLY)
          changed |= widen_proc_params_poly(c, bexpr);
      } }

    if (recv < 0) {
      /* bare `new(args)` inside a class method constructs the enclosing
         (possibly specialized) class -> bind args to that class's
         initialize, so the subclass constructor's params get typed. */
      if (name && sp_streq(name, "new")) {
        Scope *s = comp_scope_of(c, id);
        if (s && s->is_cmethod && s->class_id >= 0) {
          if (c->classes[s->class_id].is_struct)
            changed |= struct_new_types_members(c, id, s->class_id);
          else {
            int initmi = comp_method_in_chain(c, s->class_id, "initialize", NULL);
            if (initmi >= 0) changed |= bind_call_params(c, id, initmi);
          }
        }
        continue;
      }
      int mi = -1;
      int caller_cid = -1;
      /* bare call inside an instance_eval/exec block: dispatch on the
         receiver's class so its params get the call-site arg types. */
      int iec = ie_class_of(c, id);
      if (iec >= 0) {
        int def_cid = -1;
        mi = comp_method_in_chain(c, iec, name, &def_cid);
        if (mi >= 0) caller_cid = def_cid >= 0 ? def_cid : iec;
      }
      { int pk[64], npk = ie_poly_classes_at(c, id, pk, 64);
        for (int i = 0; i < npk; i++) changed |= bind_call_params(c, id, comp_method_in_chain(c, pk[i], name, NULL)); }
      /* Otherwise the call is on the enclosing definition's self: a class
         method resolves against the singleton chain, an instance method
         against the instance chain. Both come before a top-level def, which
         is a private method on Object and so sits at the bottom of every
         ancestry. Asking for the free functions first bound this call's
         argument types to a same-named top-level method and left the real
         callee's parameters to be typed by its own body instead (#4106). */
      if (mi < 0) {
        Scope *self = comp_scope_of(c, id);
        if (self->class_id >= 0) {
          caller_cid = self->class_id;
          int def_cid = -1;
          if (self->is_cmethod) {
            mi = comp_cmethod_in_chain(c, self->class_id, name, &def_cid);
            if (mi >= 0 && def_cid >= 0) caller_cid = def_cid;
          }
          if (mi < 0) {
            def_cid = -1;
            mi = comp_method_in_chain(c, self->class_id, name, &def_cid);
            if (mi >= 0 && def_cid >= 0) caller_cid = def_cid;
          }
        }
        else mi = comp_cbody_call_mi(c, id, name);
      }
      if (mi < 0) mi = comp_method_index(c, name);
      if (mi < 0) mi = comp_included_method_index(c, name, id);
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
        int nd = 0; const int *ds = comp_descendants(c, caller_cid, &nd);
        for (int di = 0; di < nd; di++) {
          int k = ds[di];
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
    /* `target.seen(x)` where `target` is a Class VALUE the analysis cannot pin
       to one class -- a parameter, an element, an untyped slot. At run time the
       dispatch reaches every class method of that name, so every one of them
       has this call as a caller. Binding none of them let the OTHER callers
       settle the parameter alone, and this call was then read at that type:
       an object arrived as a String (answering with its bytes) and an Integer
       segfaulted, with nothing said at compile time (#4066). */
    {
      const char *rty0 = nt_type(nt, recv);
      int static_cls = rty0 && (sp_streq(rty0, "ConstantReadNode") ||
                                sp_streq(rty0, "ConstantPathNode"));
      if (!static_cls && class_var_static_ci(c, recv) >= 0) static_cls = 1;
      int splat_new = 0;
      /* `klass.new(*args)` on such a value: every initialize the dispatch
         can land on takes the array's elements, which no other site may
         pass it -- an optional typed from its default alone was handed an
         element of any class. */
      if (!static_cls && sp_streq(name, "new")) {
        int nargs = 0, splat = 0;
        int an = nt_ref(nt, id, "arguments");
        const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &nargs) : NULL;
        for (int k = 0; k < nargs; k++)
          if (nt_kind(nt, av[k]) == NK_SplatNode) splat = 1;
        TyKind rt0 = splat ? infer_type(c, recv) : TY_UNKNOWN;
        if (rt0 == TY_CLASS || rt0 == TY_POLY) {
          int first_splat = 0;
          while (first_splat < nargs && nt_kind(nt, av[first_splat]) != NK_SplatNode) first_splat++;
          for (int k = 0; k < c->nclasses; k++) {
            int imi = comp_method_in_chain(c, k, "initialize", NULL);
            if (imi >= 0) {
              changed |= bind_call_params(c, id, imi);
              /* which element lands in which parameter differs per class and
                 per call, so each one the splat can reach holds any of them */
              Scope *im = &c->scopes[imi];
              for (int pk = first_splat; pk < im->nparams; pk++) {
                if (pk == im->rest_idx || pk == im->kwrest_idx || !im->pnames[pk] ||
                    callee_has_kwarg(c, im, im->pnames[pk])) continue;
                LocalVar *p = scope_local(im, im->pnames[pk]);
                if (p && !p->rbs_seeded) changed |= slot_take(c, p, TY_POLY, av[first_splat]);
              }
              continue;
            }
            /* a Struct's members, which a sole splat spreads into */
            ClassInfo *sk = &c->classes[k];
            if (!sk->is_struct || nargs != 1) continue;
            for (int a = 0; a < sk->nmembers; a++) {
              if (class_ivar_pinned(sk, sk->ivars[a])) continue;
              TyKind m = ty_unify(sk->ivar_types[a], TY_POLY);
              if (m != sk->ivar_types[a]) { sk->ivar_types[a] = m; changed = 1; }
            }
          }
          splat_new = 1;
        }
      }
      /* the same without a splat: an argument that reaches an initialize only
         through this call left its parameter typed by the static
         constructions alone, and the arm then read it as their type */
      if (!static_cls && !splat_new && sp_streq(name, "new")) {
        TyKind rt1 = infer_type(c, recv);
        if (rt1 == TY_CLASS || rt1 == TY_POLY) changed |= bind_dynamic_new_initializers(c, id);
      }
      if (!static_cls && infer_type(c, recv) == TY_CLASS) {
        int bound_any = 0;
        for (int k = 0; k < c->nclasses; k++) {
          int cmi = comp_cmethod_in_class(c, k, name);
          if (cmi >= 0) { changed |= bind_call_params(c, id, cmi); bound_any = 1; }
        }
        if (bound_any) continue;
      }
      if (splat_new) continue;
    }
    /* Class.new -> initialize params; Class.cmethod -> cmethod params */
    {
      const char *rty = nt_type(nt, recv);
      int rmi = class_reopen_cmethod(c, recv, name);
      if (rmi >= 0) { changed |= bind_call_params(c, id, rmi); continue; }
      /* M::Sub.new(...) -- resolve by the final path component */
      if (rty && sp_streq(rty, "ConstantPathNode")) {
        const char *cn = nt_str(nt, recv, "name");
        int ci = cn ? comp_class_index(c, cn) : -1;
        /* A Struct or Data is constructed, not called: its arguments type the
           MEMBERS, and that is done once for every receiver spelling by the
           branch below. Binding ctor params here instead left every member of
           a struct only ever built through its qualified name untyped, so a
           method reading one answered untyped too (#4185). */
        int cpath_struct = ci >= 0 && sp_streq(name, "new") && c->classes[ci].is_struct;
        if (cpath_struct) { /* handled below, with the short-name spelling */ }
        else if (ci >= 0 && sp_streq(name, "new")) {
          int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
          if (ucnew >= 0)
            changed |= bind_call_params(c, id, ucnew);
          else
            changed |= bind_call_params(c, id, comp_method_in_chain(c, ci, "initialize", NULL));
        }
        else if (ci >= 0)
          changed |= bind_call_params(c, id, comp_cmethod_in_chain(c, ci, name, NULL));
      }
      /* An anonymous Struct held in a LOCAL (`st = Struct.new(:x, :y)`) is the
         same class as the constant form -- class_var_static_ci resolves the
         local's reads to it -- so its construction types the members the same
         way. Without this every member of such a struct stayed poly and each
         field read and arithmetic on it went through the boxed path (#3984). */
      int lci = (rty && sp_streq(rty, "LocalVariableReadNode") && sp_streq(name, "new"))
                ? class_var_static_ci(c, recv) : -1;
      if (lci >= 0 && !c->classes[lci].is_struct) lci = -1;
      if ((rty && (sp_streq(rty, "ConstantReadNode") ||
                   sp_streq(rty, "ConstantPathNode"))) || lci >= 0) {
        int ci = lci >= 0 ? lci : comp_class_index(c, nt_str(nt, recv, "name"));
        if (ci >= 0) {
          if (sp_streq(name, "new") && c->classes[ci].is_struct) {
            changed |= struct_new_types_members(c, id, ci);
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
      /* `self.new(...)` in a struct's own class method constructs it too */
      if (sp_streq(name, "new") && rty && sp_streq(rty, "SelfNode")) {
        Scope *ss = comp_scope_of(c, id);
        if (ss && ss->is_cmethod && ss->class_id >= 0 && c->classes[ss->class_id].is_struct)
          changed |= struct_new_types_members(c, id, ss->class_id);
      }
      if (sp_streq(name, "new")) continue;
    }
    /* obj.method -> instance method params */
    TyKind rt = infer_type(c, recv);
    /* A receiver that settled on NO type is emitted BOXED, exactly as a poly
       one is, so the call is a runtime dispatch that can reach any user method
       of this name -- but neither branch below runs for it, so the call bound
       nothing and the callee's parameters were left to the OTHER call sites.
       The arm then took the boxed argument apart as whatever type those
       settled on: a String's payload arrived as an Integer (#4294).

       An argument with no type of its own says nothing to ty_unify, so the
       widening is stated here rather than through the binding. Post-
       convergence only -- during the fixpoint both the receiver and the
       argument may still settle, and this is not reversible -- and the flag
       carries it through the re-narrow reset, which clears poly parameters. */
    if (g_final_bind_pass && (rt == TY_UNKNOWN || rt == TY_POLY) && name) {
      int uargs = nt_ref(nt, id, "arguments");
      int uac = 0; const int *uav = uargs >= 0 ? nt_arr(nt, uargs, "arguments", &uac) : NULL;
      for (int a4 = 0; a4 < uac; a4++) {
        TyKind uat = infer_type(c, uav[a4]);
        if (uat != TY_UNKNOWN && uat != TY_POLY) continue;
        for (int k4 = 0; k4 < c->nclasses; k4++) {
          int umi = comp_method_in_chain(c, k4, name, NULL);
          if (umi < 0) continue;
          Scope *um = &c->scopes[umi];
          /* the parameter the call's layout hands the argument */
          ArgLayout L;
          call_layout(c, um, uav, uac, &L);
          int p4 = 0;
          while (p4 < um->nparams && layout_plain_arg(c, um, uav, &L, p4) != uav[a4]) p4++;
          arg_layout_free(&L);
          LocalVar *up = p4 < um->nparams && um->pnames[p4] ? scope_local(um, um->pnames[p4]) : NULL;
          if (!up || up->rbs_seeded) continue;
          /* Only a MACHINE SCALAR parameter. Unboxing into one is a hard type
             pun -- the payload word read as a number -- and nothing downstream
             can recover. A pointer-typed parameter is left alone on purpose:
             widening every one of them turned out to widen a parameter from
             an argument that is unsettled only because it is the very
             parameter under derivation (an mspec matcher's `o` in
             `@v.include?(o)`), and the rule then fed itself. */
          if (up->type != TY_INT && up->type != TY_FLOAT &&
              up->type != TY_BOOL && up->type != TY_SYMBOL) continue;
          up->type = TY_POLY; up->poly_dispatch_widened = 1; changed = 1;
        }
      }
    }
    if (ty_is_object(rt)) {
      int cid3 = ty_object_class(rt);
      int mi3 = comp_method_in_chain(c, cid3, name, NULL);
      /* Comparable: `a < b` etc. on an object with `<=>` but no direct `<`
         bind the argument to `<=>` param instead. */
      if (mi3 < 0 && (sp_streq(name, "<") || sp_streq(name, ">") ||
                      sp_streq(name, "<=") || sp_streq(name, ">=")))
        mi3 = comp_method_in_chain(c, cid3, "<=>", NULL);
      /* a method the program adds to Object, which the class's chain stops
         short of: the call reaches it (codegen's Object fallback), so its
         arguments type it (#5779) */
      if (mi3 < 0) {
        int oci3 = comp_class_index(c, "Object");
        if (oci3 >= 0 && oci3 != cid3) mi3 = comp_method_in_class(c, oci3, name);
      }
      changed |= bind_call_params(c, id, mi3);
      /* Also propagate to descendant overrides: codegen will emit a cls_id
         switch that calls each override, so each must have the right param
         types. */
      int nd3 = 0; const int *ds3 = comp_descendants(c, cid3, &nd3);
      for (int di = 0; di < nd3; di++) {
        int dmi3 = comp_method_in_class(c, ds3[di], name);
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
          /* Any BUILTIN operand, not just a scalar one: the runtime operator
             serves every tag it knows before a user-class arm is reached, so
             binding pins the user method's parameter to something that can
             never arrive there. `t >= Time.now` on a poly receiver bound
             Set#superset?'s `other` to Time, and the bundled set.rb then had
             a `Time#all?` in it and stopped compiling (#3799). A user object
             on the right IS a real candidate and still binds. */
          if (at2 == TY_UNKNOWN || at2 == TY_POLY || ty_is_object(at2)) op_scalar = 0;
        }
      }
      if (!op_scalar) {
        int npc = 0;
        const PolyCand *pcs = comp_poly_candidates(c, name, &npc);
        for (int pi = 0; pi < npc; pi++)
          changed |= bind_call_params(c, id, pcs[pi].mi);
        pcs = comp_cmethod_candidates(c, name, &npc);
        for (int pi = 0; pi < npc; pi++)
          changed |= bind_call_params(c, id, pcs[pi].mi);
      }
    }
    /* a builtin receiver (a Hash, an Array, ...) whose class the program does
       not reopen with the name: the call is Object's method (codegen's
       universal fallback), which then takes this call's arguments */
    else if (rt != TY_UNKNOWN && rt != TY_VOID && name) {
      int ocb = comp_class_index(c, "Object");
      int omb = ocb >= 0 ? comp_method_in_chain(c, ocb, name, NULL) : -1;
      if (omb >= 0 && !c->scopes[omb].is_cmethod) changed |= bind_call_params(c, id, omb);
    }
  }
  return changed;
}

/* The type a `for` loop binds to its index variable: position `pos` of a
   `for a, b in coll` destructure, or -1 for a single index. TY_UNKNOWN while
   the collection is not typed yet. */
static TyKind for_bound_type(Compiler *c, int coll, int pos) {
  TyKind ct = infer_type(c, coll);
  if (pos >= 0) {
    /* the element type of the inner array, or poly when the collection's
       element is not a concrete typed array */
    if (ty_is_array(ct)) {
      TyKind et = ty_array_elem(ct);
      if (ty_is_array(et)) return ty_array_elem(et);
    }
    return TY_POLY;
  }
  if (ct == TY_RANGE) return TY_INT;
  if (ct == TY_STR_RANGE) return TY_STRING;
  if (ty_is_array(ct)) return ty_array_elem(ct);
  if (ct == TY_POLY || ty_is_hash(ct)) return TY_POLY;
  return TY_UNKNOWN;
}

static int is_for_index_target(const NodeTable *nt, int node) {
  NT_FOREACH_KIND(nt, NK_ForNode, f) {
    int idx = nt_ref(nt, f, "index");
    if (idx == node) return 1;
    const char *ity = idx >= 0 ? nt_type(nt, idx) : NULL;
    if (!ity || !sp_streq(ity, "MultiTargetNode")) continue;
    int ln = 0;
    const int *lefts = nt_arr(nt, idx, "lefts", &ln);
    for (int i = 0; i < ln; i++) if (lefts[i] == node) return 1;
  }
  return 0;
}

/* Folds into `et` the type of every other write to the `for`-bound local
   `lv`. infer_write_types leaves iteration-bound locals alone, so without
   this a `t = "x"` after `for t in [7]` assigned a String into the loop's
   sp_int slot. */
static TyKind for_local_other_writes(Compiler *c, LocalVar *lv, const char *vn, TyKind et) {
  const NodeTable *nt = c->nt;
  for (int w = 0; w < nt->count; w++) {
    NodeKind k = nt_kind(nt, w);
    int is_op = k == NK_LocalVariableOperatorWriteNode || k == NK_LocalVariableOrWriteNode ||
                k == NK_LocalVariableAndWriteNode;
    int is_tgt = k == NK_LocalVariableTargetNode;
    if (k != NK_LocalVariableWriteNode && !is_op && !is_tgt) continue;
    const char *nm = nt_str(nt, w, "name");
    if (!nm || !sp_streq(nm, vn) || scope_local(comp_scope_of(c, w), nm) != lv) continue;
    TyKind wt;
    if (is_tgt) {
      if (is_for_index_target(nt, w)) continue;
      wt = TY_POLY;
    }
    else {
      int val = nt_ref(nt, w, "value");
      wt = comp_nil_chain_bottom(nt, val) >= 0 ? TY_NIL : infer_type(c, val);
      if (is_op && wt != TY_UNKNOWN && wt != et) wt = TY_POLY;
    }
    if (wt == TY_UNKNOWN || wt == TY_VOID) continue;
    TyKind u = ty_unify(et, wt);
    /* a nullable scalar needs the nil-sentinel marking a plain local gets */
    if (wt == TY_NIL && (u == TY_INT || u == TY_FLOAT)) u = TY_POLY;
    et = u;
  }
  return et;
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
    int ln = 1;
    const int *lefts = &idx;
    if (nt_kind(nt, idx) == NK_MultiTargetNode) lefts = nt_arr(nt, idx, "lefts", &ln);
    Scope *isc = comp_scope_of(c, idx);
    for (int i = 0; i < ln; i++) {
      const char *vn = nt_str(nt, lefts[i], "name");
      if (!vn) continue;
      /* Every `for` binding this NAME in this scope writes the same C slot,
         alone or as part of a destructure, so the slot has to hold all of
         their element types. Typed from one loop alone -- whichever the pass
         reached last -- the other one assigned a String element into an
         sp_int slot (#4168). */
      TyKind et = TY_UNKNOWN;
      int seen = 0;
      NT_FOREACH_KIND(nt, NK_ForNode, jd) {
        int jx = nt_ref(nt, jd, "index"), jc = nt_ref(nt, jd, "collection");
        if (jx < 0 || jc < 0 || comp_scope_of(c, jx) != isc) continue;
        int jn = 1;
        const int *jl = &jx;
        int jmulti = nt_kind(nt, jx) == NK_MultiTargetNode;
        if (jmulti) jl = nt_arr(nt, jx, "lefts", &jn);
        for (int j = 0; j < jn; j++) {
          const char *nm = nt_str(nt, jl[j], "name");
          if (!nm || !sp_streq(nm, vn)) continue;
          TyKind je = for_bound_type(c, jc, jmulti ? j : -1);
          if (je == TY_UNKNOWN) continue;
          et = seen ? ty_unify(et, je) : je;
          seen = 1;
        }
      }
      if (!seen || et == TY_UNKNOWN) continue;
      LocalVar *lv = scope_local_intern(isc, vn);
      lv->is_block_param = 1;  /* iteration-bound: survives the write-types reset */
      et = for_local_other_writes(c, lv, vn, et);
      if (lv->type != et) { lv->type = et; changed = 1; }
    }
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
    const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an > 1) continue;
    int blk = nt_ref(nt, id, "block");
    const char *bp0 = blk >= 0 ? block_param_name(c, blk, 0) : NULL;
    if (!bp0) continue;
    LocalVar *lv = scope_local_intern(comp_scope_of(c, id), bp0);
    lv->is_block_param = 1;   /* survives the write-types reset */
    /* an explicit tag is the block's parameter too */
    TyKind want = an == 1 ? infer_type(c, av[0]) : TY_STRING;
    if (want == TY_UNKNOWN) continue;
    if (an == 1 && lv->type != TY_UNKNOWN && lv->type != want) want = TY_POLY;
    if (lv->type != want) { lv->type = want; changed = 1; }
  }
  return changed;
}

/* The name of a numbered block parameter (`_1` .. `_9`, and `it`, which the
   parser lowers to `_1`). Normally the literal name -- but a scope holding more
   than one numbered-param block shares its local table, so those blocks would
   intern the SAME slot and their types would merge; scope_numbered_block_params
   gives each of them its own name and records it here (#4116). Every site that
   needs the name goes through this, so the rule lives in one place. */
const char *numbered_param_name(Compiler *c, int params_node, int idx) {
  static const char *names[] = {"_1","_2","_3","_4","_5","_6","_7","_8","_9"};
  if (idx < 0 || idx >= 9) return NULL;
  char key[8]; snprintf(key, sizeof key, "n%d", idx + 1);
  const char *gen = nt_str(c->nt, params_node, key);
  return (gen && *gen) ? gen : names[idx];
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
    return numbered_param_name(c, bp, idx);
  }
  int pn = nt_ref(c->nt, bp, "parameters");          /* ParametersNode */
  if (pn < 0) return NULL;
  int n = 0;
  const int *reqs = nt_arr(c->nt, pn, "requireds", &n);
  if (idx < n) return nt_str(c->nt, reqs[idx], "name");
  return NULL;
}

static const char *block_rest_kind_name(Compiler *c, int block, const char *field, const char *kind) {
  int bp = nt_ref(c->nt, block, "parameters");      /* BlockParametersNode */
  if (bp < 0) return NULL;
  const char *bpty = nt_type(c->nt, bp);
  if (bpty && sp_streq(bpty, "NumberedParametersNode")) return NULL;
  int pn = nt_ref(c->nt, bp, "parameters");          /* ParametersNode */
  if (pn < 0) return NULL;
  int rest = nt_ref(c->nt, pn, field);
  if (rest < 0) return NULL;
  const char *rty = nt_type(c->nt, rest);
  if (!rty || !sp_streq(rty, kind)) return NULL;
  return nt_str(c->nt, rest, "name");
}

/* The name of a block's trailing rest parameter (`|*a|`), or NULL if the block
   has none or it is anonymous (`|*|`). The rest collects the arguments past the
   required ones into an array. */
const char *block_rest_name(Compiler *c, int block) {
  return block_rest_kind_name(c, block, "rest", "RestParameterNode");  /* must be `*name` */
}

static const char *block_list_name(Compiler *c, int block, const char *list, int idx) {
  int bp = nt_ref(c->nt, block, "parameters");
  if (bp < 0) return NULL;
  const char *bpty = nt_type(c->nt, bp);
  if (bpty && sp_streq(bpty, "NumberedParametersNode")) return NULL;
  int pn = nt_ref(c->nt, bp, "parameters");
  if (pn < 0) return NULL;
  int n = 0;
  const int *ps = nt_arr(c->nt, pn, list, &n);
  if (idx < n) return nt_str(c->nt, ps[idx], "name");
  return NULL;
}

/* Name of a block's idx-th optional parameter (`|a, b=10|`), or NULL. */
const char *block_opt_name(Compiler *c, int block, int idx) {
  return block_list_name(c, block, "optionals", idx);
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

/* The parameter position idx of a yield of n plain arguments binds (n < 0:
   a count not known here): a required one; an optional after them when no
   post follows, whatever the count (`|a = 1, b|` hands its post b the last
   argument first); or a post the count lands there (block_fill hands the
   posts the last values); else NULL. A parameter the block appends to
   binds as an alias of the variable yielded to it at such a position
   (block_param_wants_alias), an optional or a post as a required one
   (#6179). */
const char *block_param_at(Compiler *c, int block, int idx, int n) {
  const char *bp = block_param_name(c, block, idx);
  if (bp) return bp;
  int bpn = nt_ref(c->nt, block, "parameters");
  int pn = bpn >= 0 && nt_kind(c->nt, bpn) == NK_BlockParametersNode ? nt_ref(c->nt, bpn, "parameters") : -1;
  if (pn < 0) return NULL;
  int rn = 0, on = 0, sn = 0;
  nt_arr(c->nt, pn, "requireds", &rn);
  nt_arr(c->nt, pn, "optionals", &on);
  nt_arr(c->nt, pn, "posts", &sn);
  if (sn == 0) return idx >= rn ? block_opt_name(c, block, idx - rn) : NULL;
  if (n < 0 || idx >= n) return NULL;
  int ot, ps;
  block_fill(rn, on, sn, block_rest_marker(c, block), n, &ot, &ps);
  return idx >= ps && idx < ps + sn ? block_post_name(c, block, idx - ps) : NULL;
}
/* The count of a call's or a yield's plain positional arguments, or -1 when
   one is a splat, a keyword hash or a block argument (the count is then
   the run time's). */
int call_plain_argc(Compiler *c, int call) {
  int a = nt_ref(c->nt, call, "arguments"), ac = 0;
  const int *av = a >= 0 ? nt_arr(c->nt, a, "arguments", &ac) : NULL;
  for (int i = 0; i < ac; i++) {
    NodeKind k = nt_kind(c->nt, av[i]);
    if (k == NK_SplatNode || k == NK_KeywordHashNode || k == NK_BlockArgumentNode) return -1;
  }
  return ac;
}

/* Name of a block's idx-th post-required parameter (`|a, *b, c|` -> c), or NULL. */
const char *block_post_name(Compiler *c, int block, int idx) {
  return block_list_name(c, block, "posts", idx);
}

/* 1 when a block's only parameter is a named rest (`|*r|`, `|*r, &b|`): no
   leading, optional or post-required parameter, and no keyword. */
int block_lone_rest(Compiler *c, int block) {
  const char *rest = block_rest_name(c, block);
  if (!rest || !*rest || block_param_name(c, block, 0) || block_opt_name(c, block, 0) ||
      block_post_name(c, block, 0)) return 0;
  int pn = nt_ref(c->nt, nt_ref(c->nt, block, "parameters"), "parameters");
  int nk = 0;
  nt_arr(c->nt, pn, "keywords", &nk);
  return nk == 0 && nt_ref(c->nt, pn, "keyword_rest") < 0;
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

/* A block taking only leading requireds -- two or more, or one and a
   trailing comma. CRuby auto-splats a lone Array into such a block even
   when the yield passed an (empty) `**h`; any other shape keeps it whole. */
int block_lead_only(Compiler *c, int block) {
  if (block_opt_name(c, block, 0) || block_post_name(c, block, 0)) return 0;
  int P = 0; while (block_param_name(c, block, P)) P++;
  if (!block_rest_marker(c, block)) return P > 1;
  int pn = nt_ref(c->nt, nt_ref(c->nt, block, "parameters"), "parameters");
  const char *rt = nt_type(c->nt, nt_ref(c->nt, pn, "rest"));
  return P >= 1 && rt && sp_streq(rt, "ImplicitRestNode");
}

/* CRuby's auto-splat, for a block, a proc and instance_exec alike: one
   Array passed alone is spread across the parameters when the block takes a
   required (leading or post) or two optionals, and more than one positional
   or a rest (a trailing comma's implicit one counts). `|a|`, `|a, k:|`,
   `|a = 1, *r|` and `|*r|` keep it whole. P, O, Q count the leading
   requireds, optionals and posts; R is a rest marker. */
int block_auto_splats(int P, int O, int Q, int R) {
  return (P + Q > 0 || O > 1) && (P + O + Q > 1 || R);
}

/* The static part of the proc distribution, for n positional values into
   P leading requireds, O optionals, Q posts and a rest marker R: *ot is how
   many optionals take a value, *ps the index of the first post's value
   (from there on, past n, the posts bind nil). Requireds (leading and post)
   take theirs first, the optionals what remains left to right, a rest the
   middle; values past them all are dropped. */
void block_fill(int P, int O, int Q, int R, int n, int *ot, int *ps) {
  int o = n - P - Q;
  if (o < 0) o = 0;
  if (o > O) o = O;
  int rl = R ? n - P - o - Q : 0;
  if (rl < 0) rl = 0;
  *ot = o;
  *ps = P + o + rl;
}

/* Does the block declare `**nil`, refusing keywords? */
int block_no_keywords(Compiler *c, int block) {
  int bp = nt_ref(c->nt, block, "parameters");
  int pn = bp >= 0 ? nt_ref(c->nt, bp, "parameters") : -1;
  int kr = pn >= 0 ? nt_ref(c->nt, pn, "keyword_rest") : -1;
  return kr >= 0 && nt_type(c->nt, kr) && sp_streq(nt_type(c->nt, kr), "NoKeywordsParameterNode");
}

/* Name of a block's `**kw` keyword-rest parameter, or NULL (also NULL for
   the anonymous `**`). */
const char *block_kwrest_name(Compiler *c, int block) {
  return block_rest_kind_name(c, block, "keyword_rest", "KeywordRestParameterNode");
}

/* Name of a block's idx-th keyword parameter (`|a:, b: 5|`), or NULL. */
const char *block_keyword_name(Compiler *c, int block, int idx) {
  return block_list_name(c, block, "keywords", idx);
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

/* Per scope, the sites that invoke its block: each `yield`, each
   `<&blk>.call(...)`, and each receiverless `instance_exec(args, &<blk>)`,
   which invokes the block with `args` (self aside) exactly as a yield does.
   block_sites_index walks the node table once and answers every scope; a
   walk per block call per fixpoint round was the block-parameter pass's
   time on a large program. It is rebuilt at the start of each
   infer_block_params round (and before the promote widening reads it),
   not kept while the table's counts hold: the desugars between rounds
   rewrite nodes in place -- a dead arm blanked to NilNodes
   (bi_subtree_blank), a call renamed or retyped -- and a list kept across
   them would read sites that are gone or miss ones that appeared. */
static int *bsi_start, *bsi_len, *bsi_node;
static int bsi_ns;
/* ...the calls that hand its named &block on with `&blk`, and whether a
   `super` does, which bind it wherever the callee does (block_reach); and
   whether it keeps the block (bsi_kept_mark) */
static int *bsi_fstart, *bsi_flen, *bsi_fnode;
static char *bsi_kept, *bsi_super;
static int block_site_scope(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int si = c->nscope[id];
  if (si < 0 || si >= c->nscopes) return -1;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_YieldNode) return si;
  if (k != NK_CallNode) return -1;
  const char *bp = c->scopes[si].blk_param;
  const char *nm = nt_str(nt, id, "name");
  if (!bp || !bp[0] || !nm) return -1;
  int recv = nt_ref(nt, id, "receiver"), named = -1;
  if (sp_streq(nm, "call")) named = recv;
  else if (sp_streq(nm, "instance_exec") && recv < 0) {
    int blk = nt_ref(nt, id, "block");
    named = blk >= 0 && nt_kind(nt, blk) == NK_BlockArgumentNode ? nt_ref(nt, blk, "expression") : -1;
  }
  if (named < 0 || nt_kind(nt, named) != NK_LocalVariableReadNode) return -1;
  const char *rn = nt_str(nt, named, "name");
  return rn && sp_streq(rn, bp) ? si : -1;
}
static int block_fwd_scope(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_CallNode || block_site_scope(c, id) >= 0) return -1;
  int si = c->nscope[id];
  if (si < 0 || si >= c->nscopes) return -1;
  const char *bp = c->scopes[si].blk_param;
  int blk = nt_ref(nt, id, "block");
  if (!bp || !bp[0] || blk < 0 || nt_kind(nt, blk) != NK_BlockArgumentNode) return -1;
  int e = nt_ref(nt, blk, "expression");
  if (e < 0 || nt_kind(nt, e) != NK_LocalVariableReadNode) return -1;
  const char *en = nt_str(nt, e, "name");
  return en && sp_streq(en, bp) ? si : -1;
}
static void bsi_build(Compiler *c, int (*of)(Compiler *, int), int **start, int **len, int **node) {
  const NodeTable *nt = c->nt;
  free(*start); free(*len); free(*node);
  int ns = c->nscopes, total = 0;
  *start = calloc((size_t)ns + 1, sizeof(int));
  *len = calloc((size_t)ns + 1, sizeof(int));
  for (int id = 0; id < nt->count; id++) {
    int s = of(c, id);
    if (s >= 0) { (*len)[s]++; total++; }
  }
  for (int s = 1; s < ns; s++) (*start)[s] = (*start)[s - 1] + (*len)[s - 1];
  *node = malloc(sizeof(int) * (size_t)(total + 1));
  memset(*len, 0, sizeof(int) * ((size_t)ns + 1));
  for (int id = 0; id < nt->count; id++) {
    int s = of(c, id);
    if (s >= 0) (*node)[(*start)[s] + (*len)[s]++] = id;
  }
}
/* A read of a method's named &block that lets the block go where no site
   of it shows what it is called with: kept in a variable, a container or a
   constant, returned, handed on as a plain argument, turned into something
   else (`b.to_proc`, `b.curry`), or read inside a nested lambda or proc,
   which can run it anywhere. Calling it, asking about it (`b.nil?`, `if b`,
   `b.arity`) and a `&b` forward (block_reach) keep it where it is. */
static int bsi_read_keeps(Compiler *c, const int *parent, int r) {
  const NodeTable *nt = c->nt;
  static const char *const ask[] = {
    "call", "[]", "yield", "===", "nil?", "!", "arity", "parameters",
    "lambda?", "==", "!=", "hash", "inspect", "to_s", "source_location", NULL };
  int p = parent[r];
  if (p < 0) return 1;
  NodeKind pk = nt_kind(nt, p);
  if (pk == NK_BlockArgumentNode) return 0;
  if (pk == NK_CallNode && nt_ref(nt, p, "receiver") == r) {
    const char *nm = nt_str(nt, p, "name");
    /* a call through what an operator answers (`(b || fb).call(v)`) is no
       site of the block (block_site_scope reads `b.call` alone), so there
       only the asking names, past the four calling ones, keep it in place */
    int via = nt_kind(nt, r) != NK_LocalVariableReadNode;
    for (int k = via ? 4 : 0; nm && ask[k]; k++) if (sp_streq(nm, ask[k])) return 0;
    return 1;
  }
  if ((pk == NK_IfNode || pk == NK_UnlessNode || pk == NK_WhileNode || pk == NK_UntilNode) &&
      nt_ref(nt, p, "predicate") == r) return 0;
  /* `b && x` answers b only where b is nil or false, so the block itself
     never leaves through it. `b || x` answers b where b is set, and `x || b`
     and `x && b` answer b whenever they reach it, so the block goes wherever
     that answer goes: a predicate drops it, `@cb = b || fb` keeps it. The
     question moves up through each such operator and each pair of
     parentheses, so `(b || true) && true` drops it too: it is the left of
     the `&&` that the block would have to leave through. */
  if (pk == NK_AndNode && nt_ref(nt, p, "left") == r) return 0;
  if (pk == NK_OrNode || pk == NK_AndNode || pk == NK_ParenthesesNode)
    return bsi_read_keeps(c, parent, p);
  if (pk == NK_StatementsNode && parent[p] >= 0 && nt_kind(nt, parent[p]) == NK_ParenthesesNode) {
    /* `(a; b)` answers its last statement; an earlier one's value is dropped */
    int n = 0; const int *body = nt_arr(nt, p, "body", &n);
    return n > 0 && body[n - 1] == r ? bsi_read_keeps(c, parent, p) : 0;
  }
  return 1;
}
static void bsi_kept_mark(Compiler *c) {
  const NodeTable *nt = c->nt;
  int n = nt->count, ns = c->nscopes;
  free(bsi_kept);
  bsi_kept = calloc((size_t)ns + 1, 1);
  int *parent = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
  /* the scopes of each definition that names a &block: an included
     module's method is copied into each includer, one scope per copy */
  int *def_first = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
  int *def_next = malloc(sizeof(int) * (size_t)(ns + 1));
  if (!bsi_kept || !parent || !def_first || !def_next) { free(parent); free(def_first); free(def_next); return; }
  for (int id = 0; id < n; id++) { parent[id] = -1; def_first[id] = -1; }
  /* the walk down from the root settles every node the program reaches, so
     an orphan a desugar left behind -- the ParenthesesNode a repointed
     receiver hangs off still holds the same child (#5272) -- cannot claim a
     child the tree already gave a parent. A scan of the rest then reaches
     what hangs off no root: a module's method is copied into each includer,
     and the copy's own `&block` reads are read here too, so leaving them
     without a parent would call every one of them kept. */
  int *stack = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
  if (!stack) { free(parent); free(def_first); free(def_next); return; }
  int sp = 0;
  if (nt->root_id >= 0 && nt->root_id < n) stack[sp++] = nt->root_id;
  while (sp > 0) {
    int id = stack[--sp];
    int nr = nt_num_refs(nt, id), na = nt_num_arrs(nt, id);
    for (int j = 0; j < nr; j++) {
      int ch = nt_ref_at(nt, id, j);
      if (ch >= 0 && ch < n && parent[ch] < 0 && ch != nt->root_id) { parent[ch] = id; stack[sp++] = ch; }
    }
    for (int j = 0; j < na; j++) {
      int an = 0; const int *av = nt_arr_at(nt, id, j, &an);
      for (int k = 0; k < an; k++)
        if (av[k] >= 0 && av[k] < n && parent[av[k]] < 0 && av[k] != nt->root_id) { parent[av[k]] = id; stack[sp++] = av[k]; }
    }
  }
  free(stack);
  for (int id = 0; id < n; id++) {
    int nr = nt_num_refs(nt, id), na = nt_num_arrs(nt, id);
    for (int j = 0; j < nr; j++) {
      int ch = nt_ref_at(nt, id, j);
      if (ch >= 0 && ch < n && parent[ch] < 0 && ch != nt->root_id) parent[ch] = id;
    }
    for (int j = 0; j < na; j++) {
      int an = 0; const int *av = nt_arr_at(nt, id, j, &an);
      for (int k = 0; k < an; k++)
        if (av[k] >= 0 && av[k] < n && parent[av[k]] < 0 && av[k] != nt->root_id) parent[av[k]] = id;
    }
  }
  for (int si = 1; si < ns; si++) {
    int d = c->scopes[si].def_node;
    def_next[si] = -1;
    if (d < 0 || d >= n || !c->scopes[si].blk_param || !c->scopes[si].blk_param[0]) continue;
    def_next[si] = def_first[d]; def_first[d] = si;
  }
  /* a `super` passes the method's block on: a bare one, one with
     arguments but no block, or `super(&blk)` */
  free(bsi_super);
  bsi_super = calloc((size_t)ns + 1, 1);
  for (int q = 0; bsi_super && q < n; q++) {
    NodeKind k = nt_kind(nt, q);
    int sq = c->nscope[q];
    if ((k != NK_SuperNode && k != NK_ForwardingSuperNode) || sq < 1 || sq >= ns) continue;
    const char *bp = c->scopes[sq].blk_param;
    int b = nt_ref(nt, q, "block"), e = b >= 0 && nt_kind(nt, b) == NK_BlockArgumentNode ? nt_ref(nt, b, "expression") : -1;
    if (bp && bp[0] && (b < 0 || (e >= 0 && nt_kind(nt, e) == NK_LocalVariableReadNode &&
                                  sp_streq(nt_str(nt, e, "name"), bp)))) bsi_super[sq] = 1;
  }
  for (int r = 0; r < n; r++) {
    if (nt_kind(nt, r) != NK_LocalVariableReadNode) continue;
    const char *rn = nt_str(nt, r, "name");
    int s0 = c->nscope[r];
    if (!rn || s0 < 1 || s0 >= ns) continue;
    const char *bp = c->scopes[s0].blk_param;
    if (bp && bp[0] && sp_streq(rn, bp)) {
      if (!bsi_kept[s0] && bsi_read_keeps(c, parent, r)) bsi_kept[s0] = 1;
      continue;
    }
    /* read inside a lambda or proc nested in the method: the definition
       it sits in, through any number of them */
    int d = -1, hops = 0;
    for (int q = parent[r]; q >= 0 && hops < 4096; q = parent[q], hops++)
      if (nt_kind(nt, q) == NK_DefNode) { d = q; break; }
    for (int si = d >= 0 ? def_first[d] : -1; si >= 0; si = def_next[si])
      if (sp_streq(rn, c->scopes[si].blk_param)) bsi_kept[si] = 1;
  }
  free(parent); free(def_first); free(def_next);
}
void block_sites_index(Compiler *c) {
  bsi_build(c, block_site_scope, &bsi_start, &bsi_len, &bsi_node);
  bsi_build(c, block_fwd_scope, &bsi_fstart, &bsi_flen, &bsi_fnode);
  bsi_kept_mark(c);
  bsi_ns = c->nscopes;
}
int block_sites(Compiler *c, int si, const int **sites) {
  if (!bsi_start) block_sites_index(c);
  *sites = NULL;
  if (si < 0 || si >= bsi_ns) return 0;
  *sites = bsi_node + bsi_start[si];
  return bsi_len[si];
}

/* The arguments site `site` of scope `si` binds the block of `call` (the
   call that passes it) with. A site that is the method's whole body and
   hands on exactly the method's own parameters -- `def run(*a, &b) =
   instance_exec(*a, &b)`, `b.call(x, **kw)` -- binds what the call passed,
   so the call's own arguments are those values, as for the instance_exec
   trampoline (ie_tramp_effective_arg): the splat of the rest param, an
   array of what nobody here has typed, says nothing about them. */
int block_site_args(Compiler *c, int si, int site, int call) {
  const NodeTable *nt = c->nt;
  int a = nt_ref(nt, site, "arguments");
  Scope *m = si >= 0 && si < c->nscopes ? &c->scopes[si] : NULL;
  int bn = 0; const int *bb = m && m->body >= 0 ? nt_arr(nt, m->body, "body", &bn) : NULL;
  if (call < 0 || bn != 1 || bb[0] != site || m->nparams <= 0 ||
      (m->rest_idx < 0 && m->kwrest_idx < 0)) return a;
  int ac = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
  if (ac != m->nparams) return a;
  for (int j = 0; j < ac; j++) {
    int v = av[j];
    if (j == m->rest_idx) v = nt_kind(nt, v) == NK_SplatNode ? nt_ref(nt, v, "expression") : -1;
    else if (j == m->kwrest_idx) {
      int en = 0; const int *el = nt_kind(nt, v) == NK_KeywordHashNode ? nt_arr(nt, v, "elements", &en) : NULL;
      v = en == 1 && nt_kind(nt, el[0]) == NK_AssocSplatNode ? nt_ref(nt, el[0], "value") : -1;
    }
    else if (m->pdefault && m->pdefault[j] >= 0) return a;
    const char *vn = v >= 0 && nt_kind(nt, v) == NK_LocalVariableReadNode ? nt_str(nt, v, "name") : NULL;
    if (!vn || !m->pnames[j] || !sp_streq(vn, m->pnames[j])) return a;
  }
  /* keywords the call passes reach the block as keywords only through the
     method's own `**kw`; without one the rest collects them as a Hash, a
     positional (`def run(*a, &b) = b.call(*a); run(1, k: "x") { |x, k: 0| }`
     binds k = 0), and the site is read as it stands */
  int ca = nt_ref(nt, call, "arguments"), cc = 0;
  const int *cv = ca >= 0 ? nt_arr(nt, ca, "arguments", &cc) : NULL;
  if (m->kwrest_idx < 0 && cc > 0 && nt_kind(nt, cv[cc - 1]) == NK_KeywordHashNode) return a;
  return ca;
}

/* A block's (or a proc literal's) parameters as its binders count them.
   `params` is the block's parameters node (BlockParametersNode, or
   NumberedParametersNode for `_1`.. and `it`) or a lambda's
   ParametersNode. */
void block_sig(Compiler *c, int params, int lambda, BlockSig *s) {
  const NodeTable *nt = c->nt;
  memset(s, 0, sizeof *s);
  s->pn = s->num = -1;
  s->lambda = lambda;
  if (params >= 0 && nt_kind(nt, params) == NK_NumberedParametersNode) {
    s->num = params;
    s->P = (int)nt_int(nt, params, "maximum", 0);
    return;
  }
  if (params >= 0 && nt_kind(nt, params) == NK_BlockParametersNode) params = nt_ref(nt, params, "parameters");
  if (params < 0) return;
  s->pn = params;
  nt_arr(nt, params, "requireds", &s->P);
  nt_arr(nt, params, "optionals", &s->O);
  nt_arr(nt, params, "posts", &s->Q);
  nt_arr(nt, params, "keywords", &s->nk);
  s->R = nt_ref(nt, params, "rest") >= 0;
  s->kw = s->nk > 0 || nt_ref(nt, params, "keyword_rest") >= 0;
}

/* The name of positional parameter i (the requireds, then the optionals,
   then the posts), or NULL for a destructuring or anonymous one. */
const char *block_sig_name(Compiler *c, const BlockSig *s, int i) {
  if (s->num >= 0) return numbered_param_name(c, s->num, i);
  int j = i;
  const char *group = "requireds";
  if (j >= s->P) {
    j -= s->P; group = "optionals";
    if (j >= s->O) { j -= s->O; group = "posts"; }
  }
  int n = 0; const int *v = nt_arr(c->nt, s->pn, group, &n);
  if (j >= n || nt_kind(c->nt, v[j]) == NK_MultiTargetNode) return NULL;
  return nt_str(c->nt, v[j], "name");
}

/* The keyword parameter k's node. */
static int block_sig_kw(Compiler *c, const BlockSig *s, int k) {
  int n = 0; const int *v = nt_arr(c->nt, s->pn, "keywords", &n);
  return k < n ? v[k] : -1;
}
const char *block_sig_kw_name(Compiler *c, const BlockSig *s, int k) {
  int kp = block_sig_kw(c, s, k);
  return kp >= 0 ? nt_str(c->nt, kp, "name") : NULL;
}

/* One more value into a parameter's type. A nil keeps a slot that has a nil
   of its own (an object, a String, a poly array: ty_unify's join), but an
   Integer or a Float the binders would read it into as a bare number takes
   the box, but for a positional's literal nil (bs_join_val). */
static TyKind bs_join(TyKind a, TyKind b) {
  TyKind u = ty_unify(a, b);
  if ((a == TY_NIL || b == TY_NIL) && (u == TY_INT || u == TY_FLOAT)) return TY_POLY;
  return u;
}

/* What value `v` binds as. An empty `[]` / `{}` literal has no type of its
   own but is still built as a container, so it is not the Integer an
   unpinned parameter defaults to (#4295): poly holds either. */
static TyKind bs_value(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  TyKind t = v >= 0 ? infer_type(c, v) : TY_NIL;
  if (t == TY_UNKNOWN && (nt_kind(nt, v) == NK_ArrayNode || nt_kind(nt, v) == NK_HashNode)) {
    int en = 0; nt_arr(nt, v, "elements", &en);
    if (en == 0) t = TY_POLY;
  }
  return t;
}

/* One more value `v` into a positional's type `a`. A literal nil is the
   exception to bs_join's box: a positional's binders write an Integer
   slot's own nil (SP_INT_NIL) for it, as they do for a missing value, so it
   only records BS_NIL in *flags, for the parameter's settling to judge
   whether the slot can hold it (block_settle_types, cs_type_params). Only a
   literal: a value typed nil this round, an ivar nothing but the
   constructor's nil has written yet, may take another type later, and an
   Integer parameter settled beside it stayed one after the box it took.
   An Integer or a Float value that may be nil (nullable_int_value: `i == 0
   ? nil : i`, a missed element read) records BS_NIL beside its type: it is
   emitted as the slot's sentinel when it is nil, which the binders hand
   through as they do a literal's. */
static TyKind bs_join_val(Compiler *c, TyKind a, int v, char *flags) {
  if (v >= 0 && nt_kind(c->nt, v) == NK_NilNode) { *flags |= BS_NIL; return a; }
  TyKind t = bs_value(c, v);
  if ((t == TY_INT || t == TY_FLOAT) && nullable_int_value(c, v)) *flags |= BS_NIL;
  return bs_join(a, t);
}

/* The values a splat of `x` spreads, into the gathered element type `e`
   and the count bounds. The binders lay a splat out at run time, so any
   positional may read an element of it. An array literal's plain elements
   (the splat-then-values rewrite leaves `yield(*e, 7)` as `*[*e, 7]`) are
   values sure to be there, though, and so are those of a local that holds
   one whose length nothing can change (splat_local_sure_lit), which spreads
   as that literal does; any other operand's length is the run time's. An
   operand not typed yet says nothing this round. A literal nil among them
   is recorded in *flags (bs_join_val). */
static void bs_splat(Compiler *c, int x, int np, TyKind *e, char *flags, int *nmin, int *nmax) {
  const NodeTable *nt = c->nt;
  *nmax = np + 1;
  int lit = splat_local_sure_lit(c, x);
  if (lit >= 0) x = lit;
  if (x >= 0 && nt_kind(nt, x) == NK_ArrayNode) {
    int en = 0; const int *el = nt_arr(nt, x, "elements", &en);
    for (int j = 0; j < en; j++) {
      if (nt_kind(nt, el[j]) == NK_SplatNode) { bs_splat(c, nt_ref(nt, el[j], "expression"), np, e, flags, nmin, nmax); continue; }
      *e = bs_join_val(c, *e, el[j], flags);
      (*nmin)++;
    }
    return;
  }
  TyKind xt = x >= 0 ? infer_type(c, x) : TY_POLY;
  TyKind et = ty_is_array(xt) ? ty_array_elem(xt) : xt == TY_UNKNOWN ? TY_UNKNOWN : TY_POLY;
  *e = bs_join(*e, et == TY_UNKNOWN && xt != TY_UNKNOWN ? TY_POLY : et);
}

/* Fold one site's values `av` (ac of them: the whole argument list, a
   trailing keyword hash included) into what each of a block's parameters
   receives, by the plan its binders follow (emit_block_binds, the proc
   prologue): pos[i] for the i-th positional (the P requireds, the O
   optionals, the Q posts; block_sig_name), absent[i] its flags -- BS_ABSENT
   where it may receive no value at all, BS_NIL where it may receive a
   literal nil (bs_join_val) -- and kws[k] for the k-th keyword.

   A trailing keyword hash is the keywords' when the block takes any, and
   then never a positional. A count only the run time knows -- a splat, or a
   trailing hash made only of `**` spreads, a positional only when
   non-empty -- gathers the values (emit_spread_args): a positional that may
   receive one takes the element type of them all, and only the values sure
   to be there make a parameter certain of one. Otherwise the count is
   static and block_fill places each value, the requireds and posts first,
   the optionals what remains. A lone Array (or a lone gathered one) into a
   block that auto-splats (block_auto_splats; never a lambda) spreads its
   elements, however many there are. An optional that may be left without a
   value takes its default. A keyword takes the value its key names, the
   values of every `**` operand and of a computed key (either may name it),
   and its default when it may be absent -- never a positional's. */
void block_site_types(Compiler *c, const BlockSig *s, const int *av, int ac,
                      TyKind *pos, char *absent, TyKind *kws) {
  const NodeTable *nt = c->nt;
  int P = s->P, O = s->O, Q = s->Q, np = P + O + Q;
  int kwh = -1;
  if (s->kw && ac > 0 && av[ac - 1] >= 0 && nt_kind(nt, av[ac - 1]) == NK_KeywordHashNode) kwh = av[--ac];
  for (int k = 0; k < s->nk; k++) {
    int kp = block_sig_kw(c, s, k);
    int vn = kwh >= 0 ? ie_kwhash_value(c, kwh, nt_str(nt, kp, "name")) : -1;
    TyKind t = vn >= 0 ? bs_value(c, vn) : TY_UNKNOWN;
    int en = 0; const int *els = kwh >= 0 ? nt_arr(nt, kwh, "elements", &en) : NULL;
    for (int e = 0; e < en; e++) {
      if (nt_kind(nt, els[e]) != NK_AssocSplatNode) continue;
      int x = nt_ref(nt, els[e], "value");
      TyKind ht = x >= 0 ? infer_type(c, x) : TY_POLY;
      TyKind vt = ty_is_hash(ht) ? ty_hash_val(ht) : TY_POLY;
      t = bs_join(t, vt == TY_UNKNOWN ? TY_POLY : vt);
    }
    t = bs_join(t, ie_kwhash_computed_type(c, kwh));
    if (vn < 0 && nt_kind(nt, kp) == NK_OptionalKeywordParameterNode)
      t = bs_join(t, bs_value(c, nt_ref(nt, kp, "value")));
    kws[k] = bs_join(kws[k], t);
  }
  int gather = 0, nmin = 0, nmax = 0;
  TyKind e = TY_UNKNOWN;
  char enil = 0;   /* a literal nil among the values e gathers */
  for (int k = 0; k < ac; k++) {
    if (av[k] >= 0 && nt_kind(nt, av[k]) == NK_SplatNode) {
      gather = 1;
      bs_splat(c, nt_ref(nt, av[k], "expression"), np, &e, &enil, &nmin, &nmax);
    }
    else {
      e = bs_join_val(c, e, av[k], &enil);
      if (k == ac - 1 && kwh_only_spreads(nt, av[k])) gather = 1;
      else nmin++;
      if (nmax <= np) nmax++;
    }
  }
  /* Keywords the block's keywords take keep a lone Array whole, empty ones
     too (emit_block_binds): only a `**` alone may spread it, for
     instance_exec, which drops an empty one before it yields, so the
     parameters then hold the Array or an element. */
  int kw_spreads = kwh >= 0 && kwh_only_spreads(nt, kwh);
  if (!s->lambda && (kwh < 0 || kw_spreads) && block_auto_splats(P, O, Q, s->R) &&
      (gather ? nmin <= 1 : ac == 1) &&
      (ty_is_array(e) || e == TY_POLY || e == TY_POLY_ARRAY)) {
    /* spread across the parameters: an element, or (gathered, or boxed)
       possibly the value itself */
    TyKind et = ty_is_array(e) ? ty_array_elem(e) : TY_POLY;
    if (et == TY_UNKNOWN) et = TY_POLY;
    if (gather || kw_spreads || !ty_is_array(e)) et = bs_join(et, e);
    e = et; gather = 1; nmin = 0; nmax = np + 1;
  }
  int ot = 0, ps = 0;
  if (!gather) block_fill(P, O, Q, s->R, ac, &ot, &ps);
  for (int i = 0; i < np; i++) {
    TyKind t = TY_UNKNOWN;
    int may = 1, sure = 1;   /* may receive a value; sure to */
    char f = 0;
    if (i >= P && i < P + O) {
      int oi = i - P;
      if (gather) { may = nmax - P - Q > oi; sure = nmin - P - Q > oi; t = may ? e : TY_UNKNOWN; }
      else if ((may = sure = oi < ot)) t = bs_join_val(c, t, av[P + oi], &f);
      if (gather && may) f |= enil;
      /* a gathered count is read at run time, and the binding keeps the
         default in its other arm even where the values are sure to reach
         the optional (`yield(*[1, 2])` into `|a, b = "d"|`) */
      if (!sure || gather) {
        int opn = 0; const int *opts = nt_arr(nt, s->pn, "optionals", &opn);
        t = bs_join_val(c, t, nt_ref(nt, opts[oi], "value"), &f);
      }
      pos[i] = bs_join(pos[i], t);
      absent[i] |= f;
      continue;
    }
    /* a required or a post: the k-th value it may take counting from the
       left, the posts right after what the optionals and a rest took */
    int at = i < P ? i : P + (i - P - O);
    if (gather) { may = nmax > at; sure = nmin > at; t = may ? e : TY_UNKNOWN; if (may) f |= enil; }
    else {
      int vi = i < P ? i : ps + (i - P - O);
      if ((may = sure = vi < ac)) t = bs_join_val(c, t, av[vi], &f);
    }
    pos[i] = bs_join(pos[i], t);
    if (!sure) f |= BS_ABSENT;
    absent[i] |= f;
  }
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

/* Builtins that keep the block they are handed (`proc(&b)`,
   `define_method(:m, &b)`, `Thread.new(&b)`) or run it with a self and
   arguments no site here shows (`o.instance_exec(x, &b)`). */
static int block_kept_by_builtin(const char *name) {
  static const char *const keep[] = {
    "proc", "lambda", "define_method", "define_singleton_method", "new",
    "instance_exec", "instance_eval", "class_exec", "module_exec",
    "class_eval", "module_eval", "to_enum", "enum_for", "at_exit", "trap",
    "define_finalizer", NULL };
  return str_in(name, keep);
}

/* Where a block handed to scope `si` is bound besides si's own sites: the
   calls si hands its &block on to, with `&blk` or with `super`, bind it
   too, through every site of the callee (`def m(&b) = (inner(&b); yield
   1)`), which this folds into pos/absent/kws as block_site_types does. Returns 1 when the
   block goes where no site can be seen: kept as a value (bsi_kept_mark) or
   handed to a builtin that keeps it. Its parameters then take whatever the
   escaped calls pass, which only the box holds. `fold` is 0 for the scope
   whose sites the caller folded already; `seen` stops a recursive forward. */
static int block_reach(Compiler *c, int si, const BlockSig *s, TyKind *pos,
                       char *absent, TyKind *kws, char *seen, int fold, int depth) {
  const NodeTable *nt = c->nt;
  if (si < 1 || si >= c->nscopes || seen[si] || depth > 16) return 0;
  seen[si] = 1;
  if (bsi_kept && si < bsi_ns && bsi_kept[si]) return 1;
  int np = s->P + s->O + s->Q;
  if (fold) {
    const int *sites = NULL;
    int ns = block_sites(c, si, &sites);
    for (int k = 0; k < ns; k++) {
      int ya = block_site_args(c, si, sites[k], -1);
      int yc = 0; const int *yv = ya >= 0 ? nt_arr(nt, ya, "arguments", &yc) : NULL;
      block_site_types(c, s, yv, yc, pos, absent, pos + np);
    }
  }
  if (!bsi_fstart || si >= bsi_ns) return 0;
  if (bsi_super && bsi_super[si] && c->scopes[si].class_id >= 0) {
    int y = forwarding_yield_target(c, a_super_target(c, &c->scopes[si]), 0);
    if (y >= 0 && block_reach(c, y, s, pos, absent, kws, seen, 1, depth + 1)) return 1;
  }
  for (int k = 0; k < bsi_flen[si]; k++) {
    int f = bsi_fnode[bsi_fstart[si] + k];
    const char *fn = nt_str(nt, f, "name");
    int recv = nt_ref(nt, f, "receiver");
    int t = -1, byname = 0;
    if (recv < 0) t = comp_self_call_mi(c, f, fn);
    else if (nt_kind(nt, recv) == NK_ConstantReadNode || nt_kind(nt, recv) == NK_ConstantPathNode) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      if (ci >= 0) t = comp_cmethod_in_chain(c, ci, fn, NULL);
      if (ci >= 0 && t < 0 && sp_streq(fn, "new")) t = comp_method_in_chain(c, ci, "initialize", NULL);
    }
    else {
      TyKind rt = infer_type(c, recv);
      /* a receiver not typed yet says nothing this round: the box a guess
         took would stay for good */
      if (rt == TY_UNKNOWN && g_infer_optimistic) continue;
      if (ty_is_object(rt)) t = comp_method_in_chain(c, ty_object_class(rt), fn, NULL);
      /* a boxed one, or one still untyped, may be any class defining it */
      else byname = rt == TY_UNKNOWN || rt == TY_POLY;
    }
    if (t >= 0) {
      int y = forwarding_yield_target(c, t, 0);
      if (y >= 0 && block_reach(c, y, s, pos, absent, kws, seen, 1, depth + 1)) return 1;
      continue;
    }
    int found = 0;
    for (int u = 1; byname && u < c->nscopes; u++) {
      Scope *us = &c->scopes[u];
      if (us->is_cmethod || !us->name || !sp_streq(us->name, fn)) continue;
      int y = forwarding_yield_target(c, u, 0);
      if (y < 0) continue;
      found = 1;
      if (block_reach(c, y, s, pos, absent, kws, seen, 1, depth + 1)) return 1;
    }
    if (!found && block_kept_by_builtin(fn)) return 1;
  }
  return 0;
}

/* Widen the parameters of `block` for what block_reach found: all of them
   when the block is kept, else each one that does not already hold what a
   call the block is handed on to binds it with (posf/absentf/kwsf). Only
   ever to the box: the types the method's own sites settled stay what they
   are wherever the callee agrees with them. */
static int block_params_widen(Compiler *c, int block, const BlockSig *s, int kept,
                              const TyKind *posf, const char *absentf, const TyKind *kwsf) {
  Scope *bs = comp_scope_of(c, block);
  int np = s->P + s->O + s->Q, changed = 0;
  for (int i = 0; i < np + s->nk; i++) {
    int kw = i >= np;
    const char *bp = kw ? block_sig_kw_name(c, s, i - np) : block_sig_name(c, s, i);
    if (!bp) continue;
    TyKind tf = kw ? kwsf[i - np] : posf[i];
    if (!kw && absentf[i]) tf = ty_unify(tf, TY_POLY);
    LocalVar *lv = scope_local_intern(bs, bp); lv->is_block_param = 1;
    if (!kept && (tf == TY_UNKNOWN || ty_unify(lv->type, tf) == lv->type)) continue;
    if (lv->type != TY_POLY) { lv->type = TY_POLY; changed = 1; }
  }
  /* a destructuring parameter's leaves, which kept blocks bind too */
  for (int k = 0; kept; k++) {
    const char *bp = block_param_name(c, block, k);
    if (!bp) break;
    LocalVar *lv = scope_local_intern(bs, bp); lv->is_block_param = 1;
    if (lv->type != TY_POLY) { lv->type = TY_POLY; changed = 1; }
  }
  return changed;
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
      /* report the param as written, not the shadow rename's slot name */
      char disp[128]; snprintf(disp, sizeof disp, "%.*s", (int)block_param_written_len(rn), rn);
      long long fid = nt_int(nt, id, "node_file", -1);
      const char *file = fid >= 0 ? nt_file_path(nt, (int)fid) : NULL;
      if (!file) file = nt->source_file ? nt->source_file : "source.rb";
      fprintf(stderr, "spinel: %s:%d: a block splat parameter (*%s) is not supported by the `%s` lowering\n",
              file, (int)nt_int(nt, id, "node_line", 0), disp, nm);
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

/* Splice a block body into the enclosing scope `encl`: re-home every node
   that lived in the block's own scope (nested blocks keep theirs). The
   worklist grows: a fixed 250 left the children past it in the block's
   scope (a wide array literal in the body). */
static void rehome_block_body(Compiler *c, int body, int encl) {
  const NodeTable *nt = c->nt;
  Scope *bs = comp_scope_of(c, body);
  int bsi = bs ? (int)(bs - c->scopes) : -1;
  if (bsi < 0) return;
  int cap = 256, sp = 0;
  int *stack = (int *)malloc(sizeof(int) * (size_t)cap);
  if (!stack) return;
  stack[sp++] = body;
  while (sp > 0) {
    int nid = stack[--sp];
    if (nid < 0 || nid >= nt->count) continue;
    if (c->nscope[nid] == bsi) c->nscope[nid] = encl;
    const SpNode *nd = &nt->nodes[nid];
    int more = nd->nr;
    for (int i2 = 0; i2 < nd->na; i2++) more += nd->a[i2].n;
    if (sp + more > cap) {
      while (sp + more > cap) cap *= 2;
      int *g = (int *)realloc(stack, sizeof(int) * (size_t)cap);
      if (!g) break;
      stack = g;
    }
    for (int i2 = 0; i2 < nd->nr; i2++) stack[sp++] = nd->r[i2].ref;
    for (int i2 = 0; i2 < nd->na; i2++)
      for (int j2 = 0; j2 < nd->a[i2].n; j2++) stack[sp++] = nd->a[i2].ids[j2];
  }
  free(stack);
}

static void subtree_rename_local(NodeTable *nt, int root, const char *oldn, const char *newn, int depth);
/* Is local `name` written anywhere under `root`? */
static int exec_subtree_writes_local(const NodeTable *nt, int root, const char *name, int depth) {
  if (root < 0 || depth > 200) return 0;
  NodeKind k = nt_kind(nt, root);
  if ((k == NK_LocalVariableWriteNode || k == NK_LocalVariableTargetNode || k == NK_LocalVariableOperatorWriteNode ||
       k == NK_LocalVariableOrWriteNode || k == NK_LocalVariableAndWriteNode) &&
      nt_str(nt, root, "name") && sp_streq(nt_str(nt, root, "name"), name)) return 1;
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++)
    if (exec_subtree_writes_local(nt, nt_ref_at(nt, root, i), name, depth + 1)) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, root, i, &n);
    for (int j = 0; j < n; j++)
      if (exec_subtree_writes_local(nt, ids[j], name, depth + 1)) return 1;
  }
  return 0;
}
/* Does the code under `root` append to local `name` in place or hand it to
   a call? */
static int exec_subtree_lends_local(const NodeTable *nt, int root, const char *name, int depth) {
  if (root < 0 || depth > 200) return 0;
  if (nt_kind(nt, root) == NK_CallNode) {
    int r = nt_ref(nt, root, "receiver");
    if (r >= 0 && nt_kind(nt, r) == NK_LocalVariableReadNode && nt_str(nt, r, "name") &&
        sp_streq(nt_str(nt, r, "name"), name) && an_str_mutator_name(nt_str(nt, root, "name"))) return 1;
    int a = nt_ref(nt, root, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    for (int k = 0; k < an; k++)
      if (nt_kind(nt, av[k]) == NK_LocalVariableReadNode && nt_str(nt, av[k], "name") &&
          sp_streq(nt_str(nt, av[k], "name"), name)) return 1;
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++)
    if (exec_subtree_lends_local(nt, nt_ref_at(nt, root, i), name, depth + 1)) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, root, i, &n);
    for (int j = 0; j < n; j++)
      if (exec_subtree_lends_local(nt, ids[j], name, depth + 1)) return 1;
  }
  return 0;
}
/* An exec block's parameter `pn`, bound from `arg`, a String variable of the
   caller's, which the block appends to or hands on: `pn = arg` makes a
   second variable holding the String, whose appends reach the caller only
   through the alias rule, and that follows neither a hand-off nor a
   parameter of the caller's. When neither name is written in the body, the
   body reads `arg` itself instead (the caller renames `pn` to it), and its
   appends take the paths a direct one does (#6179). */
static int exec_param_reads_arg(Compiler *c, int body, const char *pn, int arg) {
  const NodeTable *nt = c->nt;
  if (!pn || arg < 0 || nt_kind(nt, arg) != NK_LocalVariableReadNode) return 0;
  const char *an = nt_str(nt, arg, "name");
  TyKind at = infer_type(c, arg);
  if (!an || (at != TY_STRING && at != TY_STRBUF)) return 0;
  if (exec_subtree_writes_local(nt, body, pn, 0) || exec_subtree_writes_local(nt, body, an, 0)) return 0;
  return exec_subtree_lends_local(nt, body, pn, 0);
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
      if (exec_param_reads_arg(c, body, pnames[k], pvals[k])) {
        subtree_rename_local(nt, body, pnames[k], nt_str(nt, pvals[k], "name"), 0);
        continue;
      }
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
    rehome_block_body(c, body, encl);
    changed = 1;
  }
  return changed;
}

/* Kernel functions spinel dispatches globally: a receiverless call to one of
   these inside an instance_eval splice must stay receiverless (CRuby finds
   them through the receiver's ancestry via Kernel; spinel's equivalents are
   free functions). */
int ie_kernel_global(const char *n) {
  static const char *const K[] = {
    "puts", "print", "p", "pp", "warn", "raise", "require", "require_relative",
    "printf", "sprintf", "format", "rand", "srand", "sleep", "exit", "abort",
    "loop", "lambda", "proc", "catch", "throw", "gets", "binding",
    "block_given?", "at_exit", "caller", "freeze", "frozen?", NULL };
  return str_in(n, K);
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

/* Give each receiverless call in an instance_eval/exec body (other than a
   Kernel global) a `self` receiver, so a body the splice below declines still
   dispatches it on the rebound self rather than on main. Skips nested
   class/module/def bodies and nested self-rebinding blocks. */
static int ie_subtree_self_calls(Compiler *c, int root, const char *cls, int depth) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (root < 0 || root >= nt->count || depth > 200) return 0;
  NodeKind k = nt_kind(nt, root);
  if (k == NK_ClassNode || k == NK_ModuleNode || k == NK_DefNode || k == NK_SingletonClassNode) return 0;
  /* this self is the receiver, not main: `self.m` keeps its receiver even
     where a top-level def `m` exists (desugar_main_self_call) */
  if (k == NK_SelfNode) {
    if (!nt_int(nt, root, "ie_self", 0)) nt_node_set_int(nt, root, "ie_self", 1);
    return 0;
  }
  int changed = 0, skip = -1;
  if (k == NK_CallNode) {
    const char *nm = nt_str(nt, root, "name");
    /* the receiver's own method binds ahead of a top-level def (a private
       Object method) or the Kernel one of that name */
    int answers = nm && ((cls && builtin_method_known(cls, nm)) ||
                         (builtin_object_method_known(nm) && comp_method_index(c, nm) < 0));
    if (nm && nt_ref(nt, root, "receiver") < 0 &&
        (answers || (!ie_kernel_global(nm) && comp_method_index(c, nm) < 0))) {
      int sn = nt_new_node(nt, "SelfNode");
      if (sn < 0) return 0;
      comp_grow_node_arrays(c);
      c->nscope[sn] = c->nscope[root];
      nt_node_set_ref(nt, root, "receiver", sn);
      changed = 1;
    }
    if (nm && (sp_streq(nm, "instance_eval") || sp_streq(nm, "instance_exec"))) skip = nt_ref(nt, root, "block");
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) {
    int ch = nt_ref_at(nt, root, i);
    if (ch != skip) changed |= ie_subtree_self_calls(c, ch, cls, depth + 1);
  }
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, root, i, &n);
    for (int j = 0; j < n; j++) changed |= ie_subtree_self_calls(c, ids[j], cls, depth + 1);
  }
  return changed;
}

/* Does the subtree under `node` read or write an ivar? */
static int subtree_has_ivar(const NodeTable *nt, int node, int depth) {
  if (node < 0) return 0;
  /* past the depth this follows, an ivar may be there: not spliced */
  if (depth > 200) return 1;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_InstanceVariableReadNode || k == NK_InstanceVariableWriteNode ||
      k == NK_InstanceVariableOrWriteNode || k == NK_InstanceVariableAndWriteNode ||
      k == NK_InstanceVariableOperatorWriteNode || k == NK_InstanceVariableTargetNode) return 1;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) if (subtree_has_ivar(nt, nt_ref_at(nt, node, i), depth + 1)) return 1;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int m = 0; const int *ids = nt_arr_at(nt, node, i, &m);
    for (int j = 0; j < m; j++) if (subtree_has_ivar(nt, ids[j], depth + 1)) return 1;
  }
  return 0;
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
    /* a boxed receiver: a receiverless call names the receiver's method, so
       it takes a `self` receiver and the poly dispatch answers it for a
       builtin value too; the body is spliced per class at emit time */
    if (rt == TY_POLY) {
      int blk = nt_ref(nt, id, "block");
      int body = blk >= 0 && nt_kind(nt, blk) == NK_BlockNode ? nt_ref(nt, blk, "body") : -1;
      if (body >= 0 && !subtree_has_kind(nt, body, NK_DefNode, 0))
        changed |= ie_subtree_self_calls(c, body, NULL, 0);
      continue;
    }
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
    /* the block's ivars are the receiver's, which has none: spliced here
       they read and wrote the caller's. The codegen path reads them nil
       and refuses a write (emit_call's non-object instance_exec). */
    if (subtree_has_ivar(nt, nt_ref(nt, blk, "parameters"), 0) || subtree_has_ivar(nt, body, 0)) continue;
    {
      const char *rcls = ty_is_array(rt) ? "Array" : ty_is_hash(rt) ? "Hash"
                       : rt == TY_RANGE ? "Range" : rt == TY_TIME ? "Time"
                       : rt == TY_REGEX ? "Regexp" : rt == TY_COMPLEX ? "Complex"
                       : rt == TY_RATIONAL ? "Rational" : builtin_class_of_type(rt);
      changed |= ie_subtree_self_calls(c, body, rcls, 0);
    }

    int is_exec = sp_streq(nm, "instance_exec");
    /* pvals[k]: the node bound to parameter pnames[k] -- a call argument, a
       keyword argument's value, or the parameter's own default */
    const char *pnames[16]; int pvals[16], pdef[16] = {0}, pnew[16] = {0}; int np = 0;
    {
      int bparams = nt_ref(nt, blk, "parameters");
      int params = bparams >= 0 ? nt_ref(nt, bparams, "parameters") : -1;
      int rn = 0; const int *reqs = params >= 0 ? nt_arr(nt, params, "requireds", &rn) : NULL;
      int on = 0; const int *opts = params >= 0 ? nt_arr(nt, params, "optionals", &on) : NULL;
      int kn = 0; const int *kws = params >= 0 ? nt_arr(nt, params, "keywords", &kn) : NULL;
      int an2 = nt_ref(nt, id, "arguments");
      int ac2 = 0; const int *av2 = nt_arr(nt, an2 >= 0 ? an2 : -1, "arguments", &ac2);
      /* A trailing `k: v` hash binds keyword parameters. Only literal symbol
         keys that each name a keyword, and cover the required ones, are
         bound here; any other keyword shape, a `**rest` taking keywords, or
         parameters after a `*rest` take the codegen path, which checks and
         binds them at run time. */
      int kwh = is_exec && ac2 > 0 && nt_kind(nt, av2[ac2 - 1]) == NK_KeywordHashNode ? av2[ac2 - 1] : -1;
      int pac = kwh >= 0 ? ac2 - 1 : ac2;
      int restp = params >= 0 ? nt_ref(nt, params, "rest") : -1;
      if (restp >= 0 && nt_kind(nt, restp) != NK_RestParameterNode) restp = -1;
      int kwrp = params >= 0 ? nt_ref(nt, params, "keyword_rest") : -1;
      int postn = 0; if (params >= 0) nt_arr(nt, params, "posts", &postn);
      if ((on > 0 || kn > 0 || restp >= 0 || kwrp >= 0 || postn > 0) && !is_exec) continue;
      if (postn > 0 || (kwrp >= 0 && (kwh >= 0 || nt_kind(nt, kwrp) != NK_KeywordRestParameterNode)))
        continue;
      if (restp >= 0 && !nt_str(nt, restp, "name")) continue;
      if (is_exec) {
        if (rn + on + kn + 2 > 16 || pac < rn || (pac > rn + on && restp < 0)) continue;
      }
      else { if (ac2 != 0 || rn > 1) continue; }   /* instance_eval { |s| }: s = self */
      int ok = 1;
      for (int k = 0; k < rn && ok; k++) {
        if (!(pnames[np] = nt_str(nt, reqs[k], "name"))) ok = 0;
        pvals[np++] = is_exec ? av2[k] : -1;
      }
      for (int k = 0; k < on && ok; k++) {
        if (!(pnames[np] = nt_str(nt, opts[k], "name"))) ok = 0;
        pdef[np] = rn + k >= pac;
        pvals[np] = pdef[np] ? nt_ref(nt, opts[k], "value") : av2[rn + k];
        np++;
      }
      /* The binds run in parameter order, so the call's arguments must all
         come before any default: a keyword argument following a used default,
         or keyword arguments out of parameter order, take the codegen path. */
      int defaulted = pac < rn + on, kmatched = 0;
      int ken = 0; const int *kels = kwh >= 0 ? nt_arr(nt, kwh, "elements", &ken) : NULL;
      for (int k = 0; k < kn && ok; k++) {
        if (!(pnames[np] = nt_str(nt, kws[k], "name"))) { ok = 0; break; }
        int v = ie_kwhash_value(c, kwh, pnames[np]);
        if (v >= 0) {
          if (defaulted || kmatched >= ken || nt_ref(nt, kels[kmatched], "value") != v) ok = 0;
          kmatched++;
        }
        else {
          v = nt_kind(nt, kws[k]) == NK_OptionalKeywordParameterNode ? nt_ref(nt, kws[k], "value") : -1;
          defaulted = pdef[np] = 1;
        }
        if (v < 0) ok = 0;
        pvals[np++] = v;
      }
      if (ok && kmatched != ken) ok = 0;
      /* `*rest` takes the positional arguments past the others, `**rest`
         an empty Hash when the call passes no keywords */
      if (ok && restp >= 0) {
        int ra = nt_new_node(nt, "ArrayNode");
        if (ra < 0) continue;
        if (pac > rn + on) nt_node_set_arr(nt, ra, "elements", av2 + rn + on, pac - rn - on);
        pnames[np] = nt_str(nt, restp, "name"); pnew[np] = 1; pvals[np++] = ra;
      }
      if (ok && kwrp >= 0) {
        int hn = nt_new_node(nt, "HashNode");
        if (hn < 0) continue;
        if (!(pnames[np] = nt_str(nt, kwrp, "name"))) ok = 0;
        pnew[np] = 1; pvals[np++] = hn;
      }
      if (!ok) continue;
    }

    char tmp[64];
    snprintf(tmp, sizeof tmp, "__ie_%s", comp_node_tag(c, id));
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
    for (int k = 0; k < np; k++)
      if (pdef[k]) ie_subtree_retarget(c, pvals[k], tmp, 0);

    int base = nt->count;
    int items[16 + 66]; int ni = 0;
    /* bind the receiver first (evaluated once), then the params */
    int wrecv = nt_new_node(nt, "LocalVariableWriteNode");
    if (wrecv < 0) continue;
    nt_node_set_str(nt, wrecv, "name", tmp);
    nt_node_set_ref(nt, wrecv, "value", recv);
    items[ni++] = wrecv;
    if (is_exec) {
      for (int k = 0; k < np; k++) {
        /* an unused param never gets a C declaration; splice the arg bare so
           its side effects still run (#2734). A later default reading it is
           a use. */
        int used = ie_subtree_uses_local(nt, body, pnames[k], 0);
        for (int j = k + 1; j < np && !used; j++)
          used = ie_subtree_uses_local(nt, pvals[j], pnames[k], 0);
        if (!used) { items[ni++] = pvals[k]; continue; }
        /* a String the block appends to is the caller's variable itself
           (exec_param_reads_arg); a later default reading it keeps the write */
        int later = 0;
        for (int j = k + 1; j < np && !later; j++) later = ie_subtree_uses_local(nt, pvals[j], pnames[k], 0);
        if (!pdef[k] && !pnew[k] && !later && exec_param_reads_arg(c, body, pnames[k], pvals[k])) {
          subtree_rename_local(nt, body, pnames[k], nt_str(nt, pvals[k], "name"), 0);
          continue;
        }
        int w = nt_new_node(nt, "LocalVariableWriteNode");
        if (w < 0) { ni = -1; break; }
        nt_node_set_str(nt, w, "name", pnames[k]);
        nt_node_set_ref(nt, w, "value", pvals[k]);
        items[ni++] = w;
      }
    }
    else if (np == 1 && ie_subtree_uses_local(nt, body, pnames[0], 0)) {
      /* instance_eval yields self to the sole param; an unused param binds
         nothing (#2734). A String receiver the block appends to through the
         param is the caller's variable itself (exec_param_reads_arg). */
      if (exec_param_reads_arg(c, body, pnames[0], recv))
        subtree_rename_local(nt, body, pnames[0], nt_str(nt, recv, "name"), 0);
      else {
        int rd = nt_new_node(nt, "LocalVariableReadNode");
        int w = nt_new_node(nt, "LocalVariableWriteNode");
        if (rd < 0 || w < 0) continue;
        nt_node_set_str(nt, rd, "name", tmp);
        nt_node_set_str(nt, w, "name", pnames[0]);
        nt_node_set_ref(nt, w, "value", rd);
        items[ni++] = w;
      }
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
    rehome_block_body(c, body, encl);
    for (int k = 0; k < np; k++) {
      if (pdef[k]) rehome_block_body(c, pvals[k], encl);
      if (pnew[k]) c->nscope[pvals[k]] = encl;
    }
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
/* A block that may be materialized as a real proc rather than spliced: its
   call name is one an instantiated user class owns as a yielding or
   &blk-taking method, so a poly receiver there dispatches and the block is
   lifted. Decided from the name alone because this runs before the types
   settle; over-answering only costs a wrapper the emitter would otherwise not
   need. */
static int block_may_lift_by_name(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_CallNode) return 0;
  if (nt_ref(nt, id, "block") < 0) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !poly_enum_op_for(nm)) return 0;
  for (int k = 0; k < c->nclasses; k++) {
    int mi = comp_method_in_chain(c, k, nm, NULL);
    if (mi < 0) continue;
    Scope *m = &c->scopes[mi];
    if (m->yields || (m->blk_param && m->blk_param[0])) return 1;
  }
  return 0;
}

static int subtree_proc_captures_name(Compiler *c, int root, const char *nm, int in_proc, int depth) {
  const NodeTable *nt = c->nt;
  if (root < 0 || root >= nt->count || depth > 200) return 0;
  /* Only the BLOCK of a liftable call becomes a proc; its receiver and
     arguments are evaluated in the enclosing frame, so a name read there is
     not a capture. Descending into the whole call as if it were a proc made
     `x.flatten.each { }` count as capturing `x` and wrapped a loop that never
     needed it. */
  if (!in_proc && block_may_lift_by_name(c, root)) {
    int lblk = nt_ref(nt, root, "block");
    if (lblk >= 0 && subtree_proc_captures_name(c, lblk, nm, 1, depth + 1)) return 1;
  }
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
    /* A generator's block runs once on its own fiber and its parameter is the
       yielder: wrapped in a lambda, `y << v` in a block the body passes to a
       user `each` stopped being a Fiber.yield and became Integer#<< on a
       laundered pointer, and the Enumerator answered []. (A Fiber or Thread
       body keeps the wrap: its parameter is an ordinary value, and a proc
       literal inside may capture it.) */
    if (a_is_fiber_or_gen_create(c, id)) {
      int gr = nt_ref(nt, id, "receiver");
      const char *grn = gr >= 0 ? nt_str(nt, gr, "name") : NULL;
      if (grn && sp_streq(grn, "Enumerator")) continue;
    }
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
    char wn[4][96];
    int wreqs[4], wreads[4]; int ok = 1;
    for (int k = 0; k < rn && ok; k++) {
      snprintf(wn[k], sizeof wn[k], "__cap_%s_%s", comp_node_tag(c, id), pn[k]);
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
    nt_node_set_int(nt, wlam, "cap_iife", 1);   /* called where made: its cells end with the call */
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
  return str_in(nm, E);
}
/* Whether an argument the streaming File.foreach evaluates after opening the
   file reads the same as one evaluated before it, as CRuby does: a literal or
   a local, which nothing in between can run code to change. Any other one is
   bound to a temp ahead of the open (desugar_dir_surface). */
static int foreach_arg_inert(const NodeTable *nt, int a) {
  switch (nt_kind(nt, a)) {
    case NK_StringNode: case NK_IntegerNode: case NK_NilNode:
    case NK_TrueNode: case NK_FalseNode: case NK_LocalVariableReadNode:
      return 1;
    default:
      return 0;
  }
}

/* Whether File.foreach call `id` can stream (desugar_dir_surface): a literal
   block, a path, and after it only arguments an IO's each_line takes as they
   are written -- a separator, a limit, `chomp:`. A splat, a block argument or
   another keyword (mode:, encoding:) keeps the readlines form. */
static int file_foreach_streams(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int blk = nt_ref(nt, id, "block");
  if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) return 0;
  int args = nt_ref(nt, id, "arguments");
  int n = 0; const int *a = args >= 0 ? nt_arr(nt, args, "arguments", &n) : NULL;
  if (!a || n < 1 || n > 4) return 0;
  for (int k = 0; k < n; k++) {
    NodeKind ak = nt_kind(nt, a[k]);
    if (ak == NK_SplatNode || ak == NK_BlockArgumentNode) return 0;
    if (ak == NK_KeywordHashNode) {
      if (k != n - 1 || k == 0) return 0;
      int en = 0; const int *ev = nt_arr(nt, a[k], "elements", &en);
      for (int j = 0; j < en; j++) {
        if (nt_kind(nt, ev[j]) != NK_AssocNode) return 0;
        int key = nt_ref(nt, ev[j], "key");
        if (key < 0 || nt_kind(nt, key) != NK_SymbolNode) return 0;
        const char *kn = nt_str(nt, key, "value");
        if (!kn || !sp_streq(kn, "chomp")) return 0;
        if (nt_ref(nt, ev[j], "value") < 0) return 0;
      }
    }
  }
  return 1;
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
        /* ...and no enclosing chain owns it either: comp_method_index sees
           only a TOP-LEVEL def, so a module's own `open` lost to File.open
           when a sibling called it bare (#4592) */
        !an_bare_call_class_owned(c, id) &&
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
    /* File.foreach(path, *rest){|l|} -> (File.open(path){|f| f.each_line(*rest){|l|}}; nil):
       the file is read a line at a time and closed however the block leaves.
       Only for arguments the IO's each_line takes as written -- positional
       ones and chomp: -- the rest keep the readlines form below. */
    if (sp_streq(rn, "File") && sp_streq(nm, "foreach") && file_foreach_streams(c, id)) {
      int fblk = nt_ref(nt, id, "block");
      int fargs = nt_ref(nt, id, "arguments");
      int fac = 0; const int *fav = nt_arr(nt, fargs, "arguments", &fac);
      int base = nt->count;
      char fnm[64]; snprintf(fnm, sizeof fnm, "__foreach_f_%s", comp_node_tag(c, id));
      int oargs = nt_new_node(nt, "ArgumentsNode");
      int rargs = fac > 1 ? nt_new_node(nt, "ArgumentsNode") : -1;
      int fread = nt_new_node(nt, "LocalVariableReadNode");
      int each = nt_new_node(nt, "CallNode");
      int ebody = nt_new_node(nt, "StatementsNode");
      int freq = nt_new_node(nt, "RequiredParameterNode");
      int fparams = nt_new_node(nt, "ParametersNode");
      int fbparams = nt_new_node(nt, "BlockParametersNode");
      int oblk = nt_new_node(nt, "BlockNode");
      int open = nt_new_node(nt, "CallNode");
      int nil2 = nt_new_node(nt, "NilNode");
      int fstmts = nt_new_node(nt, "StatementsNode");
      int fparen = nt_new_node(nt, "ParenthesesNode");
      if (oargs < 0 || (fac > 1 && rargs < 0) || fread < 0 || each < 0 || ebody < 0 || freq < 0 ||
          fparams < 0 || fbparams < 0 || oblk < 0 || open < 0 || nil2 < 0 || fstmts < 0 || fparen < 0) continue;
      /* The arguments after the path are read by each_line, after the file
         is opened; CRuby evaluates them first, and not at all past a missing
         file's raise. When one runs code, the path and each argument that
         is not a literal are bound to temps ahead of the open, in order. */
      int hargs[4] = { fav[0], fac > 1 ? fav[1] : -1, fac > 2 ? fav[2] : -1, fac > 3 ? fav[3] : -1 };
      int hitems[8 + 2]; int nhi = 0, hoist = 0;
      for (int k = 1; k < fac; k++) {
        if (nt_kind(nt, fav[k]) != NK_KeywordHashNode) { if (!foreach_arg_inert(nt, fav[k])) hoist = 1; continue; }
        int en = 0; const int *ev = nt_arr(nt, fav[k], "elements", &en);
        for (int j = 0; j < en; j++)
          if (!foreach_arg_inert(nt, nt_ref(nt, ev[j], "value"))) hoist = 1;
      }
      if (hoist) {
        int hk = 0, bad = 0;
        for (int k = 0; k < fac && !bad; k++) {
          int en = 1; const int *ev = NULL;
          int is_kw = nt_kind(nt, fav[k]) == NK_KeywordHashNode;
          if (is_kw) ev = nt_arr(nt, fav[k], "elements", &en);
          for (int j = 0; j < en && !bad; j++) {
            int v = is_kw ? nt_ref(nt, ev[j], "value") : fav[k];
            /* a local is bound as well: a later argument may assign it */
            if (foreach_arg_inert(nt, v) && nt_kind(nt, v) != NK_LocalVariableReadNode) continue;
            if (hk >= 8) { bad = 1; break; }
            char tnm[48]; snprintf(tnm, sizeof tnm, "__foreach_a%d_%d", hk++, id);
            int w = nt_new_node(nt, "LocalVariableWriteNode");
            int rd = nt_new_node(nt, "LocalVariableReadNode");
            Scope *sc = comp_scope_of(c, id);
            if (w < 0 || rd < 0 || !sc || !scope_local_intern(sc, tnm)) { bad = 1; break; }
            nt_node_set_str(nt, w, "name", tnm);
            nt_node_set_ref(nt, w, "value", v);
            nt_node_set_str(nt, rd, "name", tnm);
            hitems[nhi++] = w;
            if (is_kw) nt_node_set_ref(nt, (int)ev[j], "value", rd);
            else hargs[k] = rd;
          }
        }
        if (bad) continue;
      }
      nt_node_set_arr(nt, oargs, "arguments", hargs, 1);
      if (rargs >= 0) nt_node_set_arr(nt, rargs, "arguments", hargs + 1, fac - 1);
      nt_node_set_str(nt, fread, "name", fnm);
      nt_node_set_str(nt, each, "name", "each_line");
      nt_node_set_ref(nt, each, "receiver", fread);
      nt_node_set_ref(nt, each, "arguments", rargs);
      nt_node_set_ref(nt, each, "block", fblk);
      nt_node_set_arr(nt, ebody, "body", &each, 1);
      nt_node_set_str(nt, freq, "name", fnm);
      nt_node_set_arr(nt, fparams, "requireds", &freq, 1);
      nt_node_set_ref(nt, fbparams, "parameters", fparams);
      nt_node_set_ref(nt, oblk, "parameters", fbparams);
      nt_node_set_ref(nt, oblk, "body", ebody);
      nt_node_set_str(nt, open, "name", "open");
      nt_node_set_ref(nt, open, "receiver", recv);
      nt_node_set_ref(nt, open, "arguments", oargs);
      nt_node_set_ref(nt, open, "block", oblk);
      hitems[nhi++] = open; hitems[nhi++] = nil2;
      nt_node_set_arr(nt, fstmts, "body", hitems, nhi);
      nt_node_set_ref(nt, fparen, "body", fstmts);
      nt_node_set_str(nt, id, "name", "itself");
      nt_node_set_ref(nt, id, "receiver", fparen);
      nt_node_set_ref(nt, id, "arguments", -1);
      nt_node_set_ref(nt, id, "block", -1);
      comp_grow_node_arrays(c);
      for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[id];
      { Scope *sc = comp_scope_of(c, id);
        LocalVar *lv = sc ? scope_local_intern(sc, fnm) : NULL;
        if (lv) lv->is_block_param = 1; }
      changed = 1;
      continue;
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

    /* chdir(d, &b): the block a method forwards by name. Only a literal
       block reached the save/restore splice below, so a forwarded one was
       dropped -- the chdir became permanent and the block never ran, with
       nothing said (FileUtils.cd's own block form, #4803). Rewrite it into
       the literal form `chdir(d) { |__cd_p| b.call(__cd_p) }` and let the
       splice below do the rest. The parameter carries what Dir.chdir
       yields, the new directory, so a proc that takes it still gets it. */
    if (sp_streq(nm, "chdir") && blk >= 0 && nt_kind(nt, blk) == NK_BlockArgumentNode && an >= 1) {
      int bx = nt_ref(nt, blk, "expression");
      if (bx < 0 || nt_kind(nt, bx) == NK_SymbolNode) continue;
      char pnm[48]; snprintf(pnm, sizeof pnm, "__cd_p_%s", comp_node_tag(c, id));
      int base2 = nt->count;
      int req = nt_new_node(nt, "RequiredParameterNode");
      int params2 = nt_new_node(nt, "ParametersNode");
      int bparams2 = nt_new_node(nt, "BlockParametersNode");
      int pread = nt_new_node(nt, "LocalVariableReadNode");
      int cargs = nt_new_node(nt, "ArgumentsNode");
      int ccall = nt_new_node(nt, "CallNode");
      int cbody = nt_new_node(nt, "StatementsNode");
      int nblk = nt_new_node(nt, "BlockNode");
      if (req < 0 || params2 < 0 || bparams2 < 0 || pread < 0 || cargs < 0 ||
          ccall < 0 || cbody < 0 || nblk < 0) continue;
      nt_node_set_str(nt, req, "name", pnm);
      nt_node_set_arr(nt, params2, "requireds", &req, 1);
      nt_node_set_ref(nt, bparams2, "parameters", params2);
      nt_node_set_str(nt, pread, "name", pnm);
      { int pa = pread; nt_node_set_arr(nt, cargs, "arguments", &pa, 1); }
      nt_node_set_ref(nt, ccall, "receiver", bx);
      nt_node_set_str(nt, ccall, "name", "call");
      nt_node_set_ref(nt, ccall, "arguments", cargs);
      nt_node_set_ref(nt, ccall, "block", -1);
      { int cc = ccall; nt_node_set_arr(nt, cbody, "body", &cc, 1); }
      nt_node_set_ref(nt, nblk, "parameters", bparams2);
      nt_node_set_ref(nt, nblk, "body", cbody);
      nt_node_set_ref(nt, id, "block", nblk);
      comp_grow_node_arrays(c);
      { int encl2 = c->nscope[id];
        for (int j = base2; j < nt->count; j++) c->nscope[j] = encl2; }
      changed = 1; continue;
    }

    /* chdir(d) { body }: save, switch, run, restore -- the paren splice; the
       restore sits in an ensure so a body that raises (a failing filesystem
       call is enough now that those raise) does not leave the process in d */
    if (sp_streq(nm, "chdir") && blk >= 0 && nt_kind(nt, blk) == NK_BlockNode && an >= 1) {
      int body = nt_ref(nt, blk, "body");
      if (body < 0) continue;
      if (subtree_has_kind(nt, body, NK_DefNode, 0)) continue;
      int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
      if (bn > 58) continue;
      char sav[64], valn[64], dirn[64];
      snprintf(sav, sizeof sav, "__cd_sav_%s", comp_node_tag(c, id));
      snprintf(valn, sizeof valn, "__cd_val_%s", comp_node_tag(c, id));
      snprintf(dirn, sizeof dirn, "__cd_dir_%s", comp_node_tag(c, id));
      Scope *es = comp_scope_of(c, id);
      /* Dir.chdir yields the directory it switched to. The splice dropped
         the block's parameters, so `chdir(d) { |p| ... }` read p as nil.
         The argument goes into a temp, which the switch and the parameter
         both read (evaluating the argument expression twice would run its
         side effects twice). */
      const char *cdp0 = block_param_name(c, blk, 0);
      /* every local is interned before any pointer is taken: an intern can
         grow (realloc) the scope's locals, and a LocalVar * from an earlier
         one then points into the freed array -- a write through it
         crashed the compiler on macOS (valgrind: invalid write) */
      if (es) {
        scope_local_intern(es, sav); scope_local_intern(es, valn); scope_local_intern(es, dirn);
        if (cdp0) scope_local_intern(es, cdp0);
      }
      LocalVar *slv = es ? scope_local(es, sav) : NULL;
      LocalVar *vlv = es ? scope_local(es, valn) : NULL;
      LocalVar *dlv = es ? scope_local(es, dirn) : NULL;
      if (!slv || !vlv || !dlv) continue;
      slv->type = TY_STRING; slv->rbs_seeded = 1;
      LocalVar *plv = (cdp0 && es) ? scope_local(es, cdp0) : NULL;
      int aan = 0; const int *aav = nt_arr(nt, args, "arguments", &aan);
      if (aan < 1) continue;
      int base = nt->count;
      /* __dir = <arg> */
      int wdir = nt_new_node(nt, "LocalVariableWriteNode");
      int rdir = nt_new_node(nt, "LocalVariableReadNode");
      int rdir2 = plv ? nt_new_node(nt, "LocalVariableReadNode") : -1;
      int wparam = plv ? nt_new_node(nt, "LocalVariableWriteNode") : -1;
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
      /* begin __val = (body) ensure Dir.chdir(__sav) end */
      int beg = nt_new_node(nt, "BeginNode");
      int bstmts = nt_new_node(nt, "StatementsNode");
      int ens = nt_new_node(nt, "EnsureNode");
      int estmts = nt_new_node(nt, "StatementsNode");
      int ostmts = nt_new_node(nt, "StatementsNode");
      int oparen = nt_new_node(nt, "ParenthesesNode");
      if (pwdc<0||pwdr<0||wsav<0||cd1<0||cd1r<0||cd1a<0||pstmts<0||paren<0||wval<0||
          cd2<0||cd2r<0||cd2a<0||rsav<0||rval<0||beg<0||bstmts<0||ens<0||estmts<0||
          ostmts<0||oparen<0||wdir<0||rdir<0||(plv && (rdir2<0||wparam<0))) continue;
      nt_node_set_str(nt, wdir, "name", dirn);
      nt_node_set_ref(nt, wdir, "value", aav[0]);
      nt_node_set_str(nt, rdir, "name", dirn);
      if (plv) {
        nt_node_set_str(nt, rdir2, "name", dirn);
        nt_node_set_str(nt, wparam, "name", cdp0);
        nt_node_set_ref(nt, wparam, "value", rdir2);
      }
      nt_node_set_str(nt, pwdr, "name", "Dir");
      nt_node_set_str(nt, pwdc, "name", "pwd");
      nt_node_set_ref(nt, pwdc, "receiver", pwdr);
      nt_node_set_ref(nt, pwdc, "arguments", -1);
      nt_node_set_ref(nt, pwdc, "block", -1);
      nt_node_set_str(nt, wsav, "name", sav);
      nt_node_set_ref(nt, wsav, "value", pwdc);
      nt_node_set_str(nt, cd1r, "name", "Dir");
      nt_node_set_str(nt, cd1, "name", "chdir");
      nt_node_set_str(nt, cd1, "chdir_label", "dir_chdir0");  /* CRuby's block-form label */
      nt_node_set_ref(nt, cd1, "receiver", cd1r);
      { int a0 = rdir; nt_node_set_arr(nt, cd1a, "arguments", &a0, 1); }
      nt_node_set_ref(nt, cd1, "arguments", cd1a);
      nt_node_set_ref(nt, cd1, "block", -1);
      { int items[64]; int ni = 0;
        if (plv) items[ni++] = wparam;
        for (int k = 0; k < bn; k++) items[ni++] = bb[k];
        nt_node_set_arr(nt, pstmts, "body", items, ni); }
      nt_node_set_ref(nt, paren, "body", pstmts);
      nt_node_set_str(nt, wval, "name", valn);
      nt_node_set_ref(nt, wval, "value", paren);
      nt_node_set_str(nt, cd2r, "name", "Dir");
      nt_node_set_str(nt, cd2, "name", "chdir");
      nt_node_set_str(nt, cd2, "chdir_label", "dir_chdir0");
      nt_node_set_ref(nt, cd2, "receiver", cd2r);
      nt_node_set_str(nt, rsav, "name", sav);
      { int rs = rsav; nt_node_set_arr(nt, cd2a, "arguments", &rs, 1); }
      nt_node_set_ref(nt, cd2, "arguments", cd2a);
      nt_node_set_ref(nt, cd2, "block", -1);
      nt_node_set_str(nt, rval, "name", valn);
      { int w = wval; nt_node_set_arr(nt, bstmts, "body", &w, 1); }
      { int r2 = cd2; nt_node_set_arr(nt, estmts, "body", &r2, 1); }
      nt_node_set_ref(nt, ens, "statements", estmts);
      nt_node_set_ref(nt, beg, "statements", bstmts);
      nt_node_set_ref(nt, beg, "rescue_clause", -1);
      nt_node_set_ref(nt, beg, "else_clause", -1);
      nt_node_set_ref(nt, beg, "ensure_clause", ens);
      { int all[5] = { wdir, wsav, cd1, beg, rval };
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
      rehome_block_body(c, body, encl);
      changed = 1; continue;
    }
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
  /* A conditional whose arms decide whether there is a callable at all --
     `pr = cond ? nil : proc { |x| ... }` -- carries the literal in one arm.
     Stopping at the IfNode left the literal's parameters bound by nothing.
     Either arm may hold it; the first that resolves wins, as the local and
     constant routes below take the first write that does. */
  else if (sp_streq(ty, "IfNode") || sp_streq(ty, "UnlessNode")) {
    static int fcd_idepth = 0;
    if (fcd_idepth < 64) {
      fcd_idepth++;
      int arms[2];
      arms[0] = nt_ref(nt, ref, "statements");
      arms[1] = nt_ref(nt, ref, sp_streq(ty, "UnlessNode") ? "else_clause" : "subsequent");
      int ok3 = 0;
      for (int ai = 0; ai < 2 && !ok3; ai++) {
        int arm = arms[ai];
        if (arm < 0) continue;
        const char *aty = nt_type(nt, arm);
        if (aty && sp_streq(aty, "ElseNode")) arm = nt_ref(nt, arm, "statements");
        if (arm < 0) continue;
        int an4 = 0; const int *ab4 = nt_arr(nt, arm, "body", &an4);
        if (ab4 && an4 > 0) ok3 = fwd_callable_def(c, ab4[an4 - 1], out_body, out_pn);
      }
      fcd_idepth--;
      if (ok3) return 1;
    }
  }
  else if (sp_streq(ty, "ConstantReadNode")) {
    /* A lambda held in a CONSTANT resolves the same way one held in a local
       does. Without this the chain could not see the base proc's arity, so a
       curry never knew when it was fully applied and `F.curry[1][2]` answered
       an unapplied Proc (#4017). */
    const char *cn2 = nt_str(nt, ref, "name");
    for (int w = 0; cn2 && w < nt->count; w++) {
      if (nt_kind(nt, w) != NK_ConstantWriteNode) continue;
      const char *wn2 = nt_str(nt, w, "name");
      if (!wn2 || !sp_streq(wn2, cn2)) continue;
      int val2 = nt_ref(nt, w, "value");
      if (val2 >= 0 && is_proc_create(c, val2)) { create = val2; break; }
      if (val2 >= 0) {
        static int fcd_cdepth = 0;
        if (fcd_cdepth < 64) {
          fcd_cdepth++;
          int okc = fwd_callable_def(c, val2, out_body, out_pn);
          fcd_cdepth--;
          if (okc) return 1;
        }
      }
    }
  }
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

/* Does the block of this inject / reduce hand its memo parameter to a
   callable (`f.call(memo, ...)`) rather than filling it inline? Then no push
   is visible to type it from. */
static int ewo_memo_arg_scan(const NodeTable *nt, int id, const char *memo, int depth) {
  if (id < 0 || depth > 64) return 0;
  if (nt_kind(nt, id) == NK_CallNode) {
    int a = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    for (int k = 0; k < an && av; k++) {
      if (nt_kind(nt, av[k]) != NK_LocalVariableReadNode) continue;
      const char *vn = nt_str(nt, av[k], "name");
      if (vn && sp_streq(vn, memo)) return 1;
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (ewo_memo_arg_scan(nt, nt_ref_at(nt, id, i), memo, depth + 1)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++)
      if (ewo_memo_arg_scan(nt, ids[k], memo, depth + 1)) return 1;
  }
  return 0;
}
int ewo_memo_passed_to_callable_at(Compiler *c, int callid, int pidx) {
  const NodeTable *nt = c->nt;
  int blk = nt_ref(nt, callid, "block");
  if (blk < 0) return 0;
  const char *memo = block_param_name(c, blk, pidx);
  int body = nt_ref(nt, blk, "body");
  if (!memo || body < 0) return 0;
  return ewo_memo_arg_scan(nt, body, memo, 0);
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

/* An empty `[]` passed to a yielding user method takes the element kind the
   method pushes into it, directly or through the block parameter it yields
   it as: `f(xs, []) { |x, m| m << x }` with `def f(xs, memo); xs.each { |x|
   yield x, memo }; memo; end` builds an sp_IntArray, not a boxed array. The
   literal carries no kind of its own, and nothing read the callee's pushes
   for it before: the each_with_object emitter had this recovery for its one
   memo (ewo_memo_elem_type), and its Ruby definition (builtins/enumerable.rb)
   is the first of these methods. A scalar element kind is stamped on the
   literal (arr_want); an object element is narrow_object_arrays' to decide,
   and a poly push keeps the boxed default. */
/* ewo_scan_pushes, reporting an argument whose type is still open: a stamp
   taken before every push has settled would fix the literal on the first
   kind seen (`m << x; m << x.to_s` with `x` still unknown read as String). */
static void yarg_scan_pushes(Compiler *c, int id, const char *memo, TyKind *acc, int *open) {
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
      for (int k = 0; k < an; k++) {
        TyKind at = infer_type(c, argv[k]);
        if (at == TY_UNKNOWN) *open = 1;
        *acc = ty_unify(*acc, at);
      }
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(nt, id, i); if (ch >= 0) yarg_scan_pushes(c, ch, memo, acc, open); }
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) if (ids[k] >= 0) yarg_scan_pushes(c, ids[k], memo, acc, open); }
}

int narrow_empty_array_args_by_yield(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (!c->arr_want) return 0;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0 && nt_kind(nt, blk) != NK_BlockNode) continue;
    int anode = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
    if (an == 0) continue;
    int any_empty = 0;
    for (int j = 0; j < an; j++) {
      int a = av[j];
      if (a < 0 || a >= c->node_cap || nt_kind(nt, a) != NK_ArrayNode) continue;
      int en = 0; nt_arr(nt, a, "elements", &en);
      if (en == 0 && c->arr_want[a] == TY_UNKNOWN) any_empty = 1;
    }
    if (!any_empty) continue;
    /* with a block: an inlinable yielding method; without: a receiverless
       call to a user method, whose own pushes are the evidence */
    int mi = blk >= 0 ? call_user_yield_mi(c, id)
           : (nt_ref(nt, id, "receiver") < 0 ? comp_self_call_mi(c, id, nt_str(nt, id, "name")) : -1);
    if (mi < 0) continue;
    Scope *m = &c->scopes[mi];
    int mbody = m->body;
    int bbody = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
    if (mbody < 0 || (blk >= 0 && bbody < 0)) continue;
    for (int j = 0; j < m->nparams; j++) {
      int a = call_param_arg(c, m, av, an, j);
      if (a < 0 || a >= c->node_cap || nt_kind(nt, a) != NK_ArrayNode) continue;
      int en = 0; nt_arr(nt, a, "elements", &en);
      if (en != 0 || c->arr_want[a] != TY_UNKNOWN) continue;
      const char *pn = m->pnames[j];
      if (!pn) continue;
      TyKind acc = TY_UNKNOWN; int open = 0;
      yarg_scan_pushes(c, mbody, pn, &acc, &open);   /* the callee's own pushes */
      /* the block parameter the callee yields it as */
      int hi = mbody;
      for (int y = mbody; bbody >= 0 && y < nt->count && y <= hi; y++) {
        if (c->nscope[y] != mi || nt_kind(nt, y) != NK_YieldNode) { if (y == hi && y + 1 < nt->count && c->nscope[y + 1] == mi) hi = y + 1; continue; }
        if (y + 1 < nt->count && c->nscope[y + 1] == mi) hi = y + 1;
        int ya = nt_ref(nt, y, "arguments");
        int yn = 0; const int *yv = ya >= 0 ? nt_arr(nt, ya, "arguments", &yn) : NULL;
        for (int q = 0; q < yn; q++) {
          if (nt_kind(nt, yv[q]) != NK_LocalVariableReadNode) continue;
          const char *vn = nt_str(nt, yv[q], "name");
          if (!vn || !sp_streq(vn, pn)) continue;
          const char *bp = block_param_name(c, blk, q);
          if (bp) yarg_scan_pushes(c, bbody, bp, &acc, &open);
        }
      }
      if (!open && (acc == TY_INT || acc == TY_FLOAT || acc == TY_STRING)) {
        c->arr_want[a] = ty_array_of(acc);
        changed = 1;
      }
    }
  }
  return changed;
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

/* The maximum count Proc#curry(n) may name for `recv`'s base: requireds +
   optionals + posts of a visible definition -- CRuby's max_arity, which only
   the AST can see (the runtime meta carries no maximum). -1: unlimited (a
   rest), unknown (untraceable, keywords), or already fixed (the runtime
   validates a non-negative target arity by itself). */
int curry_count_max(Compiler *c, int recv) {
  NodeTable *nt = (NodeTable *)c->nt;
  int body = -1, pn = -1;
  /* the walker's return demands a body; the max only needs the params, and
     an empty lambda (`->(x, y = 2) { }`) has params but no body */
  (void)fwd_callable_def(c, recv, &body, &pn);
  if (pn < 0) return -1;
  if (nt_ref(nt, pn, "rest") >= 0) return -1;
  /* CRuby's max counts the whole keyword hash -- required, optional or
     **rest -- as one more slot */
  int kn = 0; nt_arr(nt, pn, "keywords", &kn);
  int kw1 = (kn > 0 || nt_ref(nt, pn, "keyword_rest") >= 0) ? 1 : 0;
  int rn = 0, on = 0, po = 0;
  nt_arr(nt, pn, "requireds", &rn);
  nt_arr(nt, pn, "optionals", &on);
  nt_arr(nt, pn, "posts", &po);
  return rn + on + po + kw1;
}

/* The arity and body-return type of the proc a curry was built from. */
static int curry_proc_base(Compiler *c, int recv, int *arity, TyKind *ret) {
  NodeTable *nt = (NodeTable *)c->nt;
  int body = -1, pn = -1;
  if (!fwd_callable_def(c, recv, &body, &pn)) return 0;
  /* CRuby's curry completes at the MIN arity, which counts trailing posts
     (`->(a, *r, z)`: 2) and required keywords (`->(a, b:)`: 2) alongside the
     leading requireds -- an undercount realized the curry early, calling the
     target short. */
  int rn = 0, po = 0, kreq = 0;
  if (pn >= 0) {
    nt_arr(nt, pn, "requireds", &rn);
    nt_arr(nt, pn, "posts", &po);
    int kn = 0; const int *kws = nt_arr(nt, pn, "keywords", &kn);
    for (int k = 0; k < kn && !kreq; k++) {
      const char *kty = kws ? nt_type(nt, kws[k]) : NULL;
      /* however many required keywords, CRuby's min counts the one hash */
      if (kty && sp_streq(kty, "RequiredKeywordParameterNode")) kreq = 1;
    }
  }
  *arity = rn + po + kreq;
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
      /* `curry(n)` fixes the arity the chain completes at, whatever the base
         proc declares -- a `proc { |a, b, c| }` curried at 2 realizes after
         two applications, and a variadic lambda takes its arity from n only
         (#3680). `curry(nil)` is CRuby's spelling of no count at all. With
         no count the chain completes at the base's REQUIRED count -- CRuby's
         min arity -- so a variadic base realizes on its first call, however
         many arguments it carries (`->(*a) { }.curry.call` invokes). A count
         the chain cannot read here (a variable, a to_int object) makes
         saturation a run-time property: hand the chain to the poly path. */
      int ca = nt_ref(nt, node, "arguments");
      int cac = 0; const int *cav = ca >= 0 ? nt_arr(nt, ca, "arguments", &cac) : NULL;
      if (cac == 1 && cav && nt_kind(nt, cav[0]) == NK_IntegerNode)
        *arity = (int)nt_int(nt, cav[0], "value", *arity);
      else if (cac >= 1 && !(cac == 1 && cav && nt_kind(nt, cav[0]) == NK_NilNode))
        return 0;
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
  /* A zero-arity base realizes on its first application, which carries no
     argument at all (`->() { 5 }.curry.call`) -- #3654 */
  *out_complete = (arity > 0 ? applied >= arity : applied >= 0);
  *out_ret = ret;
  return 1;
}


/* Locate the innermost same-scope StatementsNode and the index of the top-level
   statement whose subtree contains `id`. Returns 1 with *out_st


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


/* An Enumerable method whose receiver is a Hash or a Range, where only the
   Array arm exists: `{a: 1}.each_slice(2)`, `(1..5).sort`. Every one of these
   answers exactly what the same call on `receiver.to_a` answers -- Enumerable
   over a Hash walks its pairs, over a Range its elements -- so route it
   through that rather than leaving a compile-time refusal.

   The list is deliberately the methods whose result is NOT of the receiver's
   own kind. Hash#select and friends answer Hashes and have their own arms;
   rewriting one of those would change what it returns. Runs after inference,
   so the receiver's kind is known; the caller re-runs the fixpoint. */


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
      char nm[64]; snprintf(nm, sizeof nm, "__destr_%s_%d", comp_node_tag(c, L), k);
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
      /* the assignment is ours, not the program's: a body that had no
         statements still answers nil (#3679) */
      nt_node_set_int(nt, mw, "destr_splice", 1);
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

/* The assignment of `rd` to a single `for` target that is not a local: a
   global, instance, class variable or constant write, or the writer call an
   attribute or index target stands for. -1 for a target it cannot write. */
static int dfi_single_write(NodeTable *nt, int tgt, int rd) {
  const char *ty = nt_type(nt, tgt);
  if (!ty) return -1;
  static const char *const vars[][2] = {
    {"GlobalVariableTargetNode", "GlobalVariableWriteNode"},
    {"InstanceVariableTargetNode", "InstanceVariableWriteNode"},
    {"ClassVariableTargetNode", "ClassVariableWriteNode"},
    {"ConstantTargetNode", "ConstantWriteNode"},
  };
  for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++) {
    if (!sp_streq(ty, vars[i][0])) continue;
    const char *nm = nt_str(nt, tgt, "name");
    if (!nm) return -1;
    char nmbuf[256]; snprintf(nmbuf, sizeof nmbuf, "%s", nm);
    int w = nt_new_node(nt, vars[i][1]);
    if (w < 0) return -1;
    nt_node_set_str(nt, w, "name", nmbuf);
    nt_node_set_ref(nt, w, "value", rd);
    return w;
  }
  int is_attr = sp_streq(ty, "CallTargetNode"), is_index = sp_streq(ty, "IndexTargetNode");
  if (!is_attr && !is_index) return -1;
  int recv = nt_ref(nt, tgt, "receiver");
  char nmbuf[256];
  if (is_attr) {
    const char *nm = nt_str(nt, tgt, "name");
    if (!nm) return -1;
    snprintf(nmbuf, sizeof nmbuf, "%s", nm);
  }
  else snprintf(nmbuf, sizeof nmbuf, "[]=");
  int oargs = is_index ? nt_ref(nt, tgt, "arguments") : -1;
  int on = 0; const int *oa = oargs >= 0 ? nt_arr(nt, oargs, "arguments", &on) : NULL;
  int *av = malloc(sizeof(int) * (on + 1));
  if (!av) return -1;
  for (int i = 0; i < on; i++) av[i] = oa[i];
  av[on] = rd;
  int an = nt_new_node(nt, "ArgumentsNode");
  int call = an >= 0 ? nt_new_node(nt, "CallNode") : -1;
  if (call < 0) { free(av); return -1; }
  nt_node_set_arr(nt, an, "arguments", av, on + 1);
  free(av);
  nt_node_set_str(nt, call, "name", nmbuf);
  nt_node_set_ref(nt, call, "receiver", recv);
  nt_node_set_ref(nt, call, "arguments", an);
  nt_node_set_ref(nt, call, "block", -1);
  nt_node_set_str(nt, call, "call_operator", ".");
  return call;
}

/* Whether a `for` index binds only plain locals, which the loop assigns
   itself: one local, or a flat list of them. */
static int dfi_local_index(NodeTable *nt, int idx) {
  const char *ty = nt_type(nt, idx);
  if (!ty) return 1;
  if (sp_streq(ty, "LocalVariableTargetNode")) return 1;
  if (!sp_streq(ty, "MultiTargetNode")) return 0;
  if (nt_ref(nt, idx, "rest") >= 0) return 0;
  int rn = 0; nt_arr(nt, idx, "rights", &rn);
  if (rn > 0) return 0;
  int ln = 0; const int *l = nt_arr(nt, idx, "lefts", &ln);
  for (int i = 0; i < ln; i++) {
    const char *lt = nt_type(nt, l[i]);
    if (!lt || !sp_streq(lt, "LocalVariableTargetNode")) return 0;
  }
  return 1;
}

/* `for $g in coll`, `for @a, o.b in coll`, `for (a, b), *c in coll`: the loop
   assigns only plain locals, so any other index binds a fresh local and the
   body opens by assigning it to the real target -- a plain write for one
   target, a multiple assignment for several. */
int desugar_for_nonlocal_index(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int F = 0; F < n0; F++) {
    const char *ty = nt_type(nt, F);
    if (!ty || !sp_streq(ty, "ForNode")) continue;
    int idx = nt_ref(nt, F, "index");
    if (idx < 0 || dfi_local_index(nt, idx)) continue;
    char nm[48]; snprintf(nm, sizeof nm, "__for_%s", comp_node_tag(c, F));
    int lt = nt_new_node(nt, "LocalVariableTargetNode");
    int rd = lt >= 0 ? nt_new_node(nt, "LocalVariableReadNode") : -1;
    if (rd < 0) continue;
    nt_node_set_str(nt, lt, "name", nm);
    nt_node_set_str(nt, rd, "name", nm);
    int w;
    const char *ity = nt_type(nt, idx);
    if (sp_streq(ity, "MultiTargetNode")) {
      w = nt_new_node(nt, "MultiWriteNode");
      if (w < 0 || !bdp_fill_targets(nt, idx, w)) continue;
      nt_node_set_ref(nt, w, "value", rd);
      nt_node_set_int(nt, F, "for_packed", 1);
    }
    else w = dfi_single_write(nt, idx, rd);
    if (w < 0) continue;
    int body = nt_ref(nt, F, "statements");
    int on = 0; const int *ob = body >= 0 ? nt_arr(nt, body, "body", &on) : NULL;
    int *bb = malloc(sizeof(int) * (on + 1));
    if (!bb) continue;
    bb[0] = w;
    for (int i = 0; i < on; i++) bb[i + 1] = ob[i];
    if (body < 0) {
      body = nt_new_node(nt, "StatementsNode");
      if (body < 0) { free(bb); continue; }
      nt_node_set_ref(nt, F, "statements", body);
    }
    nt_node_set_arr(nt, body, "body", bb, on + 1);
    free(bb);
    nt_node_set_ref(nt, F, "index", lt);
    changed = 1;
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* Propagate `proc.call(args)` argument types onto the proc literal `create`'s
   required params, by the binding plan the proc prologue follows
   (block_site_types: auto-splat, gathered splats, the keyword hash). The
   rest, post, optional and keyword params are boxed for good. A concrete
   arg overrides a param still at its bare-int default (the fallback guess,
   no real evidence), otherwise joins it (bs_join). A param the call may
   leave without a value reads the slot's own nil, so it takes a type that
   has one. A literal nil passed at one call and an Integer or a Float at
   another keep the Integer or the Float, since the prologue reads a nil
   off the boxed side channel's tag as the slot's sentinel (anything under
   promote takes the box), and nil_passed then keeps the nil from
   overriding an Integer as it does the bare-int guess. Either is marked
   nullable here, where the marking pass sees it and marks a local copied
   from it too, and not only by the prologue that binds it. Returns 1 if
   any param type changed.
   Shared by the local-proc and inline-lambda call sites. */
static int cs_type_params(Compiler *c, int create, const int *argv, int argc) {
  NodeTable *nt = (NodeTable *)c->nt;
  int pn = a_proc_params_node(c, create);
  if (pn < 0) return 0;
  Scope *bs = comp_scope_of(c, create);
  const char *cty = nt_type(nt, create);
  const char *cnm = nt_str(nt, create, "name");
  int is_lambda = (cty && sp_streq(cty, "LambdaNode")) || (cnm && sp_streq(cnm, "lambda"));
  BlockSig s;
  block_sig(c, pn, is_lambda, &s);
  int np = s.P + s.O + s.Q;
  TyKind *pos = calloc((size_t)(np + s.nk + 1), sizeof(TyKind));
  char *absent = calloc((size_t)np + 1, 1);
  block_site_types(c, &s, argv, argc, pos, absent, pos + np);
  int changed = 0;
  for (int k = 0; k < s.P; k++) {
    const char *p = block_sig_name(c, &s, k);
    LocalVar *lv = p ? scope_local(bs, p) : NULL;
    TyKind at = pos[k];
    if (!lv) continue;
    /* A literal nil here (bs_join_val) beside an Integer, at this call or
       across two: the nil makes the param nil first, as it does the
       bare-int guess, and marks it nil_passed, after which an Integer keeps
       it an Integer and a literal nil no longer overrides that. A value
       merely typed nil beside it still takes the box. */
    int lit = !g_promote_mode && (absent[k] & BS_NIL);
    if ((absent[k] & BS_NIL) && at == TY_UNKNOWN) at = TY_NIL;
    TyKind lvn = lv->type == TY_NIL ? TY_UNKNOWN : lv->type;
    /* A Float is the same but for the guess: a param typed Float is one a
       call passed a Float, so a literal nil beside it keeps it at once. */
    int nil_int = !g_promote_mode &&
                  (at == TY_INT ? (lvn == TY_UNKNOWN || lvn == TY_INT) &&
                                  (lit || lv->nil_passed)
                   : at == TY_FLOAT ? (lvn == TY_UNKNOWN || lvn == TY_INT || lvn == TY_FLOAT) &&
                                      (lit || lv->nil_passed)
                   : lit && at == TY_NIL &&
                     (lv->type == TY_FLOAT || (lv->type == TY_INT && lv->nil_passed)));
    TyKind merged = nil_int ? (at == TY_NIL ? lv->type : at)
                  : at == TY_NIL && lv->type == TY_INT && lv->nil_passed ? TY_POLY
                  : at == TY_UNKNOWN || at == lv->type ? lv->type
                  : lv->type == TY_INT ? at : bs_join(lv->type, at);
    if ((absent[k] & BS_NIL) && (merged == TY_INT || merged == TY_FLOAT) && !nil_int) merged = TY_POLY;
    /* this call may leave it without a value: the prologue reads the
       slot's own nil, which an Integer, a Float, a String or an object has
       and a true/false or a Symbol slot does not (ty_unify keeps the first
       and boxes the rest) */
    if ((absent[k] & BS_ABSENT) && merged != TY_UNKNOWN) merged = ty_unify(merged, TY_NIL);
    if ((merged == TY_INT || merged == TY_FLOAT) && (nil_int || (absent[k] & BS_ABSENT)) &&
        !lv->nullable_int) { lv->nullable_int = 1; changed = 1; }
    if (lit && !lv->nil_passed) { lv->nil_passed = 1; changed = 1; }
    if (merged != lv->type) { lv->type = merged; changed = 1; }
    /* Reverse binding, as a method's (bind_call_params): an empty-`{}`-only
       local handed to a parameter the body types as a Hash is that Hash,
       filled through the reference; it takes the parameter's variant rather
       than the String-keyed default the body's keys do not fit. */
    if (k < argc && ty_is_hash(lv->type) && nt_kind(nt, argv[k]) == NK_LocalVariableReadNode) {
      int plain = 1;
      for (int q = 0; q < k && plain; q++)
        plain = nt_kind(nt, argv[q]) != NK_SplatNode && nt_kind(nt, argv[q]) != NK_KeywordHashNode;
      const char *an = plain ? nt_str(nt, argv[k], "name") : NULL;
      Scope *asc = an ? comp_scope_of(c, argv[k]) : NULL;
      LocalVar *al = asc ? scope_local(asc, an) : NULL;
      if (al && !al->is_param && !al->is_block_param && al->type != lv->type &&
          (al->type == TY_UNKNOWN || al->type == TY_POLY || ty_is_hash(al->type)) &&
          local_all_writes_empty_hash(c, asc, an)) {
        al->type = lv->type; changed = 1;
      }
    }
  }
  free(pos); free(absent);
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
    /* `_1` as the parser wrote it, and `_1__bNN` where a colliding scope's
       blocks were given their own (scope_numbered_block_params) -- the body
       names the second one, and interning the first instead left the block
       reading a slot the bind never wrote. */
    if (!(nm && nm[0] == '_' && nm[1] >= '1' && nm[1] <= '9' &&
          (nm[2] == '\0' || !strncmp(nm + 2, "__b", 3)))) continue;
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
static int *ple_pw = NULL;      /* quads: kind (0 local / 1 const / 2 ivar),
                                   scope-or-class index, write node, literal */
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
  if (!xty) return;
  /* A CONSTANT or an ivar holding the literal escapes just as a local does:
     `A = ->(x){...}; run(A)` hands the lambda to a method that calls it, and
     the literal kept the no-evidence int default -- `x[0]` on an Array
     argument compiled as an integer bit read and answered 0 (#3968). */
  int want_kind;
  int want_idx = -1;
  if (sp_streq(xty, "LocalVariableReadNode")) {
    Scope *sc = comp_scope_of(c, x);
    if (!sc) return;
    want_kind = 0; want_idx = (int)(sc - c->scopes);
  }
  else if (sp_streq(xty, "ConstantReadNode") || sp_streq(xty, "ConstantPathNode")) want_kind = 1;
  else if (sp_streq(xty, "InstanceVariableReadNode")) {
    Scope *sc = comp_scope_of(c, x);
    want_kind = 2; want_idx = sc ? sc->class_id : -1;
  }
  else return;
  const char *vn = nt_str(nt, x, "name");
  if (!vn) return;
  for (int i = 0; i < ple_npw; i++) {
    if (ple_pw[i * 4] != want_kind) continue;
    if (want_kind != 1 && ple_pw[i * 4 + 1] != want_idx) continue;
    const char *wn = nt_str(nt, ple_pw[i * 4 + 2], "name");
    if (!wn || !sp_streq(wn, vn)) continue;
    ple_escaped[ple_pw[i * 4 + 3]] = 1;
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
    for (int w = 0; w < n; w++) {
      NodeKind wk = nt_kind(nt, w);
      int kind;
      if (wk == NK_LocalVariableWriteNode) kind = 0;
      else if (wk == NK_ConstantWriteNode || wk == NK_ConstantPathWriteNode) kind = 1;
      else if (wk == NK_InstanceVariableWriteNode) kind = 2;
      else continue;
      int val = nt_ref(nt, w, "value");
      if (val < 0 || val >= n || !nt_type(nt, val) ||
          !(sp_streq(nt_type(nt, val), "LambdaNode") || is_proc_create(c, val)))
        continue;
      Scope *sc = comp_scope_of(c, w);
      if (!sc && kind != 1) continue;
      if (ple_npw >= cap) {
        cap = cap ? cap * 2 : 64;
        int *np = realloc(ple_pw, sizeof(int) * 4 * (size_t)cap);
        if (!np) break;
        ple_pw = np;
      }
      ple_pw[ple_npw * 4] = kind;
      ple_pw[ple_npw * 4 + 1] = kind == 0 ? (int)(sc - c->scopes)
                              : kind == 2 ? (sc ? sc->class_id : -1) : -1;
      ple_pw[ple_npw * 4 + 2] = w;
      ple_pw[ple_npw * 4 + 3] = val;
      ple_npw++;
    }
  }
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    int ca = nt_ref(nt, id, "arguments");
    if (ca < 0) continue;
    int an = 0; const int *av = nt_arr(nt, ca, "arguments", &an);
    for (int k = 0; k < an; k++) ple_mark_escaped(c, av[k]);
  }
  /* `m(&callable)`: a block argument rides the block slot rather than the
     argument list, so this scan never saw it. Handed to a user method it is
     invoked through the type-erased ABI like any other escaping proc, and
     nothing on this side can say what it will be called with -- the int
     default then met a String element and raised NoMethodError at run time.
     A builtin iterator's `&callable` is desugared into a literal block whose
     call site types the params exactly, and that runs before the default, so
     the precise answer still wins where there is one. */
  NT_FOREACH_KIND(nt, NK_BlockArgumentNode, ba) {
    ple_mark_escaped(c, nt_ref(nt, ba, "expression"));
  }
  /* A proc literal that is a parameter's DEFAULT shares that parameter with
     whatever proc a caller passes, so it is invoked through the same
     type-erased ABI as a proc passed as an argument. It was never marked, took
     the arithmetic Integer default, and read a Float argument as 0 through an
     int slot: `def initialize(sleeper: ->(s) { ... })` then
     `@sleeper.call(0.075)` saw 0. */
  NT_FOREACH_KIND(nt, NK_OptionalParameterNode, op) {
    ple_mark_escaped(c, nt_ref(nt, op, "value"));
  }
  NT_FOREACH_KIND(nt, NK_OptionalKeywordParameterNode, op) {
    ple_mark_escaped(c, nt_ref(nt, op, "value"));
  }
  /* A proc literal stored as a hash value or array element also escapes: it is
     read back as a boxed value and invoked through the type-erased ABI, so its
     args ride the boxed side-channel just like a proc passed as a call
     argument (#3178). A keyword argument, `m(f: ->(s) { ... })`, is an
     element of a KeywordHashNode and escapes into the callee the same way. */
  static const NodeKind hash_kinds[] = { NK_HashNode, NK_KeywordHashNode };
  for (int hk = 0; hk < 2; hk++)
  NT_FOREACH_KIND(nt, hash_kinds[hk], id) {
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
  /* A proc literal that is the fallback of `||=` / `&&=` (`blk ||= proc { |k|
     ... }`, the default for an absent block) shares its slot with a value
     this scan cannot see, and the slot is called as a poly. It escapes. */
  {
    static const NodeKind orw[] = { NK_LocalVariableOrWriteNode, NK_LocalVariableAndWriteNode,
                                    NK_InstanceVariableOrWriteNode, NK_InstanceVariableAndWriteNode };
    for (unsigned q = 0; q < sizeof orw / sizeof orw[0]; q++) {
      NT_FOREACH_KIND(nt, orw[q], id) ple_mark_escaped(c, nt_ref(nt, id, "value"));
    }
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
    /* `lambda { }` / `proc { }` escapes exactly as `->() { }` does -- the
       caller has the same type-erased proc either way. Testing only for the
       arrow left the method-call spellings' params defaulting to int, so a
       lambda returned from a method read a String argument as an Integer
       (#4035). The block-tail rule below has always tested both. */
    if (bn > 0 && bb[bn - 1] >= 0 && bb[bn - 1] < n &&
        nt_type(nt, bb[bn - 1]) &&
        (sp_streq(nt_type(nt, bb[bn - 1]), "LambdaNode") || is_proc_create(c, bb[bn - 1])))
      ple_escaped[bb[bn - 1]] = 1;
  }
  /* And a proc literal that is a LAMBDA's tail value: `->(step) { ->(acc) {
     step.call(acc) } }` answers the inner lambda, and the caller has only the
     type-erased proc, exactly as for a method return. Only the def and the
     do-block tails were scanned, so the arrow-bodied outer left the inner
     one's params on the arithmetic Integer default: the argument handed to
     `o.call(f).call([0])` was read through an int slot and came back as 0
     (#4328). A `lambda do ... end` outer went through the block rule and was
     already right, which is why only the arrow spelling failed. */
  NT_FOREACH_KIND(nt, NK_LambdaNode, id) {
    int body = nt_ref(nt, id, "body");
    if (body < 0 || !nt_type(nt, body) || !sp_streq(nt_type(nt, body), "StatementsNode")) continue;
    int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
    int tail = bn > 0 ? bb[bn - 1] : -1;
    if (tail >= 0 && tail < n && nt_type(nt, tail) &&
        (sp_streq(nt_type(nt, tail), "LambdaNode") || is_proc_create(c, tail)))
      ple_escaped[tail] = 1;
  }
  /* A proc literal that is a BLOCK's tail value escapes too: a collecting
     iterator (`(0..2).map { ->(s){ s } }`) boxes it into the result array,
     from which it is invoked through the type-erased ABI (#3242). */
  NT_FOREACH_KIND(nt, NK_BlockNode, id) {
    int body = nt_ref(nt, id, "body");
    if (body < 0 || !nt_type(nt, body) || !sp_streq(nt_type(nt, body), "StatementsNode")) continue;
    int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
    int tail = bn > 0 ? bb[bn - 1] : -1;
    /* Look through the capture wrapper. desugar_block_capture_wrap rewrites a
       block whose parameter is captured into `->(__cap){ <body> }.call(param)`,
       so the block's tail becomes that call and the value it yields is the
       WRAPPER's tail. Reading the block's tail alone found a call where the
       literal used to be, the literal was never marked as escaping, and its
       parameters took the arithmetic Integer default -- so a Proc collected out
       of `map` was called with the wrong representation (#4064). */
    for (int hop = 0; hop < 8 && tail >= 0 && tail < n; hop++) {
      if (nt_kind(nt, tail) != NK_CallNode) break;
      const char *tn = nt_str(nt, tail, "name");
      int trecv = nt_ref(nt, tail, "receiver");
      if (!tn || !sp_streq(tn, "call") || trecv < 0 || trecv >= n ||
          nt_kind(nt, trecv) != NK_LambdaNode) break;
      int wbody = nt_ref(nt, trecv, "body");
      if (wbody < 0 || !nt_type(nt, wbody) || !sp_streq(nt_type(nt, wbody), "StatementsNode")) break;
      int wn2 = 0; const int *wb = nt_arr(nt, wbody, "body", &wn2);
      if (wn2 <= 0) break;
      tail = wb[wn2 - 1];
    }
    if (tail >= 0 && tail < n && nt_type(nt, tail) &&
        (sp_streq(nt_type(nt, tail), "LambdaNode") || is_proc_create(c, tail)))
      ple_escaped[tail] = 1;
  }
}
static int proc_literal_escapes_as_arg(Compiler *c, int lit) {
  const NodeTable *nt = c->nt;
  if (ple_nt != nt || ple_ntc != nt->count) ple_build(c);
  return ple_escaped && lit >= 0 && lit < nt->count && ple_escaped[lit];
}

/* Type a proc literal's required params from one `.call` site, unless the
   literal also escapes (passed as an argument, stored in a container,
   returned). An escaping proc is invoked from scopes this scan cannot
   enumerate, so the visible site's argument types are not the whole picture:
   pinning to them makes the invisible sites read one representation through
   another (a poly-array argument arriving in an int-array-typed param) and
   answer garbage with no exception. Widen the requireds to poly instead --
   the rule the rest, post, optional and keyword params already follow. */
static int cs_type_params_site(Compiler *c, int create, const int *argv, int argc) {
  if (proc_literal_escapes_as_arg(c, create)) return widen_proc_params_poly(c, create);
  return cs_type_params(c, create, argv, argc);
}

/* True if this proc/lambda literal is handed on with `&` somewhere: it will
   then be driven through the proc ABI, whose arguments arrive boxed, so its
   parameters cannot hold a concrete scalar representation. (The rest, post,
   optional and keyword params are already permanently poly for the same
   reason; the requireds were left to be pinned by a `.call` site, which is
   not the only way in.) */
static int a_proc_forwarded_with_amp(Compiler *c, int create) {
  const NodeTable *nt = c->nt;
  Scope *cs = comp_scope_of(c, create);
  const char *lname = NULL;
  NT_FOREACH_KIND(nt, NK_LocalVariableWriteNode, w) {
    if (nt_ref(nt, w, "value") != create) continue;
    lname = nt_str(nt, w, "name");
    break;
  }
  if (!lname) return 0;
  NT_FOREACH_KIND(nt, NK_BlockArgumentNode, ba) {
    int ex = nt_ref(nt, ba, "expression");
    if (ex < 0 || nt_kind(nt, ex) != NK_LocalVariableReadNode) continue;
    const char *en = nt_str(nt, ex, "name");
    if (!en || !sp_streq(en, lname)) continue;
    if (comp_scope_of(c, ex) == cs) return 1;
  }
  return 0;
}

/* A block parameter no write site in its scope assigns. Its type comes from
   the binding alone; the usage pass, which runs ahead of the binders in a
   round, may still have guessed one from a push inside the block (`r <<
   1.5` reads as "r is a float array") before the receiver or the yield had
   a type. */
static int pure_block_param(Compiler *c, Scope *s, const char *name) {
  const NodeTable *nt = c->nt;
  for (int w = 0; w < nt->count; w++) {
    if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
    const char *wn = nt_str(nt, w, "name");
    if (wn && sp_streq(wn, name) && comp_scope_of(c, w) == s) return 0;
  }
  return 1;
}

/* Settle what the sites bound (block_site_types) into the parameters of
   block `blk`, which emit_block_binds binds: there a parameter left without
   a value is nil, which only the box holds for every type. A leading
   required or an optional every site binds an Integer, a literal nil or
   nothing (`yield(*xs)` of a run-time length, `yield 1; yield nil`, a
   `= nil` default) is the exception: its binders write the sentinel for a
   missing value (default_value) and for a nil one, so it stays an sp_int
   marked nullable, the mark every read that boxes or tests it for nil
   goes by. A Float does the same with its own sentinel (sp_float_nil). A
   post and a keyword still take the box for a nil, and so does either
   under promote, where an Integer is boxed anyway. A value not typed yet
   says nothing this round: boxed then, the parameter stayed boxed for
   good. A rest is the array that binder builds, and a `**kw` rest its
   hash. A parameter the sites boxed is marked site_boxed, 1 when they did
   it alone and 2 when something had boxed it first: the sites may still
   narrow once the fixpoint is over (narrow_site_boxed_block_params). */
int block_settle_types(Compiler *c, int blk, const BlockSig *s,
                       const TyKind *pos, const char *absent, const TyKind *kws) {
  Scope *bs = comp_scope_of(c, blk);
  int changed = 0;
  for (int i = 0; i < s->P + s->O + s->Q + s->nk; i++) {
    int kw = i >= s->P + s->O + s->Q;
    const char *bp = kw ? block_sig_kw_name(c, s, i - s->P - s->O - s->Q) : block_sig_name(c, s, i);
    if (!bp) continue;
    TyKind at = kw ? kws[i - s->P - s->O - s->Q] : pos[i];
    int fl = kw ? 0 : absent[i];
    LocalVar *lv = scope_local_intern(bs, bp); lv->is_block_param = 1;
    if ((fl & BS_ABSENT) && at == TY_UNKNOWN && g_infer_optimistic) continue;
    if ((fl & BS_NIL) && at == TY_UNKNOWN) at = TY_NIL;   /* nils alone */
    int nil_int = i < s->P + s->O && fl && (at == TY_INT || at == TY_FLOAT) && !g_promote_mode;
    if ((fl & BS_ABSENT) && !nil_int) at = ty_unify(at, TY_POLY);
    if ((fl & BS_NIL) && (at == TY_INT || at == TY_FLOAT) && !nil_int) at = TY_POLY;
    TyKind was = lv->type, m = ty_unify(lv->type, at);
    /* what the sites bind says what the parameter IS; an array kind the
       usage pass guessed from a push inside the block (`s << "z"` on a
       yielded String read as "s is a string array") is not evidence about
       that, and unified with the real type it made the parameter poly,
       where `<<` on the boxed immediate string built a new string and the
       yielded one never saw the append */
    if (ty_is_array(lv->type) && at != TY_UNKNOWN && !ty_is_array(at) &&
        at != TY_POLY && pure_block_param(c, bs, bp))
      m = at;
    if (nil_int && m == at && !lv->nullable_int) { lv->nullable_int = 1; changed = 1; }
    if (m == TY_POLY && !lv->site_boxed) lv->site_boxed = at == TY_POLY && was != TY_POLY ? 1 : 2;
    if (m != lv->type) { lv->type = m; changed = 1; }
  }
  const char *rest[2] = { block_rest_name(c, blk), block_kwrest_name(c, blk) };
  for (int r = 0; r < 2; r++) {
    if (!rest[r]) continue;
    LocalVar *lv = scope_local_intern(bs, rest[r]); lv->is_block_param = 1;
    TyKind m = r ? ty_unify(lv->type, TY_POLY_POLY_HASH) : TY_POLY_ARRAY;
    if (m != lv->type) { lv->type = m; changed = 1; }
  }
  return changed;
}

static int bp_widen(Scope *s, const char *name, TyKind t) {
  LocalVar *lv = scope_local_intern(s, name); lv->is_block_param = 1;
  return lv_widen(lv, t);
}

static int block_leaves_unify(Compiler *c, int block, Scope *s, TyKind t) {
  int changed = 0, lc = block_param_multi_count(c, block, 0);
  for (int li = 0; li < lc; li++) {
    const char *ln = block_param_multi_leaf(c, block, 0, li);
    if (!ln) continue;
    if (bp_widen(s, ln, t)) changed = 1;
  }
  return changed;
}

/* What binds `block`, handed by call `id` to method `mi` whose sites are
   `ym`'s (its yields when `yields`, else its &block's calls): pos/absent
   from every site (block_site_types), posf/absentf from the calls the
   method hands the block on to, and 1 when it is kept, so anything may call
   it (block_reach). The caller frees the four arrays. */
static int block_bind_plan(Compiler *c, int id, int block, int mi, int ym, int yields, BlockSig *s,
                           TyKind **pos, char **absent, TyKind **posf, char **absentf) {
  const NodeTable *nt = c->nt;
  const int *sites = NULL;
  int ns = block_sites(c, ym, &sites);
  block_sig(c, nt_ref(nt, block, "parameters"), 0, s);
  int np = s->P + s->O + s->Q;
  *pos = calloc((size_t)(np + s->nk + 1), sizeof(TyKind));
  *absent = calloc((size_t)np + 1, 1);
  if (!*pos || !*absent) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  /* a yielding method with no site in its own scope binds no values:
     every parameter nil or its default */
  for (int k = 0; k < ns || (yields && k == 0); k++) {
    int ya = k < ns ? block_site_args(c, ym, sites[k], mi == ym ? id : -1) : -1;
    int yc = 0;
    const int *yv = ya >= 0 ? nt_arr(nt, ya, "arguments", &yc) : NULL;
    block_site_types(c, s, yv, yc, *pos, *absent, *pos + np);
  }
  /* the method may also hand the block on, or keep it: those bind it
     too, and a kept one can be called with anything (block_reach) */
  char *seen = calloc((size_t)c->nscopes + 1, 1);
  *posf = calloc((size_t)(np + s->nk + 1), sizeof(TyKind));
  *absentf = calloc((size_t)np + 1, 1);
  if (!seen || !*posf || !*absentf) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int kept = block_reach(c, ym, s, *posf, *absentf, *posf + np, seen, 0, 0);
  free(seen);
  return kept;
}

/* The blocks the last infer_block_params settled from a yielding method's
   sites (block_settle_types): the call, the block, the method it resolved
   and the one whose yields bind it. narrow_site_boxed_block_params binds
   them again once the fixpoint is over. */
static int *bsn_v, bsn_n, bsn_cap;
static void bsn_note(int id, int block, int mi, int ym) {
  if (bsn_n + 4 > bsn_cap) {
    bsn_cap = bsn_cap ? bsn_cap * 2 : 64;
    int *nv = realloc(bsn_v, sizeof(int) * (size_t)bsn_cap);
    /* a lost note would leave a block unbound; a NULL table, a crash */
    if (!nv) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    bsn_v = nv;
  }
  bsn_v[bsn_n++] = id; bsn_v[bsn_n++] = block; bsn_v[bsn_n++] = mi; bsn_v[bsn_n++] = ym;
}

/* A table's rows spread into a block (`table.each { |row| yield(*row) }`)
   are poly all through the fixpoint: the nested Integer and Float array
   kinds show up only after it (narrow_object_arrays, and the locals and
   parameters read out of them). The parameters `yield(*row)` binds were
   settled from the boxed row and, a parameter only widening, stayed boxed.
   Once the rows are typed, bind the blocks block_settle_types settled from
   a yielding method's sites again (bsn_note) and settle a parameter the
   sites alone boxed (site_boxed 1) the way it settles an Integer or a Float
   with a nil or a missing value: an sp_int or an sp_float, marked nullable
   when a site may leave it without one. Only a leading required or an
   optional, and only when every block binding the local agrees (block
   parameters share their method's locals, so `|a, b|` in two blocks is one
   slot), nothing writes it, and no call the block is handed on to binds it
   otherwise. What was typed from the box stays the box, which the narrowed
   value boxes into. Promote boxes an Integer anyway. Returns the count. */
int narrow_site_boxed_block_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  if (g_promote_mode || bsn_n == 0) return 0;
  block_sites_index(c);
  int cap = 16, nc = 0;
  LocalVar **cl = malloc(sizeof(LocalVar *) * (size_t)cap);
  TyKind *ct = malloc(sizeof(TyKind) * (size_t)cap);
  char *cn = malloc((size_t)cap), *cbad = malloc((size_t)cap);
  int *cbind = malloc(sizeof(int) * (size_t)cap), *csc = malloc(sizeof(int) * (size_t)cap);
  if (!cl || !ct || !cn || !cbad || !cbind || !csc) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int r = 0; r < bsn_n; r += 4) {
    int id = bsn_v[r], block = bsn_v[r + 1], mi = bsn_v[r + 2], ym = bsn_v[r + 3];
    BlockSig s;
    block_sig(c, nt_ref(nt, block, "parameters"), 0, &s);
    Scope *bs = comp_scope_of(c, block);
    int any = 0;
    for (int i = 0; i < s.P + s.O && !any; i++) {
      const char *bp = block_sig_name(c, &s, i);
      LocalVar *lv = bp ? scope_local(bs, bp) : NULL;
      any = lv && lv->type == TY_POLY && lv->site_boxed == 1;
    }
    if (!any) continue;
    TyKind *pos, *posf; char *absent, *absentf;
    int kept = block_bind_plan(c, id, block, mi, ym, 1, &s, &pos, &absent, &posf, &absentf);
    for (int i = 0; i < s.P + s.O; i++) {
      const char *bp = block_sig_name(c, &s, i);
      LocalVar *lv = bp ? scope_local(bs, bp) : NULL;
      if (!lv || lv->type != TY_POLY || lv->site_boxed != 1) continue;
      TyKind at = pos[i];
      int bad = kept || !blkp_binds_param(c, block, bp) || absentf[i] ||
                (at != TY_INT && at != TY_FLOAT) ||
                (posf[i] != TY_UNKNOWN && ty_unify(at, posf[i]) != at);
      int k = 0;
      while (k < nc && cl[k] != lv) k++;
      if (k == nc) {
        if (nc == cap) {
          cap *= 2;
          cl = realloc(cl, sizeof(LocalVar *) * (size_t)cap); ct = realloc(ct, sizeof(TyKind) * (size_t)cap);
          cn = realloc(cn, (size_t)cap); cbad = realloc(cbad, (size_t)cap);
          cbind = realloc(cbind, sizeof(int) * (size_t)cap); csc = realloc(csc, sizeof(int) * (size_t)cap);
          if (!cl || !ct || !cn || !cbad || !cbind || !csc) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
        }
        cl[nc] = lv; ct[nc] = at; cn[nc] = 0; cbad[nc] = 0; cbind[nc] = 0;
        csc[nc] = (int)(bs - c->scopes); nc++;
      }
      if (bad || ct[k] != at) cbad[k] = 1;
      if (absent[i]) cn[k] = 1;
      cbind[k]++;
    }
    free(pos); free(absent); free(posf); free(absentf);
  }
  int n = 0;
  for (int k = 0; k < nc; k++) {
    LocalVar *lv = cl[k];
    if (cbad[k] || lv->is_param || lv->rbs_seeded) continue;
    int sc = csc[k], ok = 1, binders = 0;
    for (int w = comp_lvw_first_sc(c, sc, lv->name); w >= 0 && ok; w = comp_lvw_next_sc(c, w)) {
      const char *wn = nt_str(nt, w, "name");
      if (w < c->node_cap && c->nscope[w] == sc && wn && sp_streq(wn, lv->name)) ok = 0;
    }
    const NodeKind kinds[2] = { NK_BlockNode, NK_LambdaNode };
    for (int q = 0; q < 2 && ok; q++)
      for (int b = comp_kind_first(c, kinds[q]); b >= 0; b = comp_kind_next(c, b))
        if (b < c->node_cap && c->nscope[b] == sc && blkp_binds_param(c, b, lv->name)) binders++;
    if (!ok || binders != cbind[k]) continue;
    lv->type = ct[k];
    if (cn[k]) lv->nullable_int = 1;
    n++;
  }
  free(cl); free(ct); free(cn); free(cbad); free(cbind); free(csc);
  return n;
}

/* The initialize a `new` with a literal block hands the block to, when the
   call names no class constant: a class value's `k.new { }`, and a bare or
   `self.new { }` in a class method, which builds that class or a subclass.
   Only an initialize that runs the block (yields it, keeps it as `&b`, or
   forwards it) counts. Answers it when the call can reach one alone; when
   it can reach two or more, answers -1 and sets *several, and the block's
   parameters are boxed, since no one initialize's bindings type them. */
static int new_block_initialize(Compiler *c, int id, int *several) {
  const NodeTable *nt = c->nt;
  *several = 0;
  int recv = nt_ref(nt, id, "receiver");
  Scope *es = comp_scope_of(c, id);
  int cls = -1;
  if (recv < 0 || nt_kind(nt, recv) == NK_SelfNode) {
    if (!es || !es->is_cmethod || es->class_id < 0) return -1;
    cls = es->class_id;
  }
  else {
    NodeKind rk = nt_kind(nt, recv);
    if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) return -1;
    if (ty_is_object(infer_type(c, recv))) return -1;   /* an object's own `new` */
  }
  int found = -1;
  int nd = 0; const int *ds = cls >= 0 ? comp_descendants(c, cls, &nd) : NULL;
  int n = cls >= 0 ? nd + 1 : c->nclasses;
  for (int e = 0; e < n; e++) {
    int k = cls >= 0 ? (e == 0 ? cls : ds[e - 1]) : e;
    if (cls < 0 && !dynamic_new_may_reach(c, id, k)) continue;
    if (comp_cmethod_in_chain(c, k, "new", NULL) >= 0) continue;
    int mi = comp_method_in_chain(c, k, "initialize", NULL);
    if (mi < 0 || c->scopes[mi].is_cmethod || mi == found) continue;
    Scope *m = &c->scopes[mi];
    if (!m->yields && !(m->blk_param && m->blk_param[0]) && forwarding_yield_target(c, mi, 0) < 0)
      continue;
    if (found >= 0) { *several = 1; return -1; }
    found = mi;
  }
  return found;
}

/* The class method `K.run` names, for a constant receiver K. -1 if none. */
static int const_recv_cmethod_mi(Compiler *c, int recv, const char *name) {
  const NodeTable *nt = c->nt;
  if (recv < 0 || !name || nt_kind(nt, recv) != NK_ConstantReadNode || !nt_str(nt, recv, "name")) return -1;
  int cid = comp_class_index(c, nt_str(nt, recv, "name"));
  return cid >= 0 ? comp_cmethod_in_chain(c, cid, name, NULL) : -1;
}

/* `run(s, &method(:m))`: bind m's parameters from every place the block
   run receives is called -- a yield, a `blk.call` (block_sites), through a
   pure `...` forwarder (forwarding_yield_target), and through a call run
   hands its own `&blk` on to -- as a `method(:m).call(args)` binds them. */
static int bind_method_obj_block_sites(Compiler *c, int ymi, int tmi, int call, int depth) {
  const NodeTable *nt = c->nt;
  int y = depth > 4 ? -1 : forwarding_yield_target(c, ymi, 0);
  if (y < 0) return 0;
  int changed = 0;
  const int *sites = NULL;
  int ns = block_sites(c, y, &sites);
  for (int k = 0; k < ns; k++) {
    int ya = block_site_args(c, y, sites[k], y == ymi ? call : -1), yc = 0;
    const int *yv = ya >= 0 ? nt_arr(nt, ya, "arguments", &yc) : NULL;
    changed |= bind_args_params(c, sites[k], tmi, yv, yc);
  }
  Scope *m = &c->scopes[y];
  if (!m->blk_param || !m->blk_param[0]) return changed;
  NT_FOREACH_KIND(nt, NK_CallNode, u) {
    if (comp_scope_of(c, u) != m) continue;
    int ba = nt_ref(nt, u, "block");
    int bx = ba >= 0 && nt_kind(nt, ba) == NK_BlockArgumentNode ? nt_ref(nt, ba, "expression") : -1;
    if (bx < 0 || nt_kind(nt, bx) != NK_LocalVariableReadNode || !nt_str(nt, bx, "name") ||
        !sp_streq(nt_str(nt, bx, "name"), m->blk_param)) continue;
    const char *un = nt_str(nt, u, "name");
    int r = nt_ref(nt, u, "receiver");
    int t = -1;
    if (!un) continue;
    if (r < 0 || nt_kind(nt, r) == NK_SelfNode) t = comp_self_call_mi(c, u, un);
    else if (ty_is_object(infer_type(c, r))) t = comp_method_in_chain(c, ty_object_class(infer_type(c, r)), un, NULL);
    else t = const_recv_cmethod_mi(c, r, un);
    if (t >= 0 && t != y) changed |= bind_method_obj_block_sites(c, t, tmi, u, depth + 1);
  }
  return changed;
}

static int proc_params_poly(const NodeTable *nt, Scope *bs, int pn, const char *field) {
  int n = 0, changed = 0; const int *ids = nt_arr(nt, pn, field, &n);
  for (int j = 0; j < n; j++) {
    const char *pname = nt_str(nt, ids[j], "name");
    if (!pname) continue;
    LocalVar *lv = scope_local_intern(bs, pname);
    lv->is_block_param = 1;
    if (lv->type != TY_POLY) { lv->type = TY_POLY; changed = 1; }
  }
  return changed;
}

int infer_block_params(Compiler *c) {
  nn_inference_round(c);
  const NodeTable *nt = c->nt;
  int changed = 0;
  block_sites_index(c);
  bsn_n = 0;

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
        changed |= proc_params_poly(nt, bs, pn, "posts");
        changed |= proc_params_poly(nt, bs, pn, "optionals");
        /* keyword params bind out of the boxed kwargs hash, as the proc and
           lambda-call forms' do below. Left untyped here they turned poly
           only after the fixpoint, so what flowed from them (an ivar and the
           calls on it) was still unknown when the enumerable rewrite ran,
           and `@v.scan(re).filter_map { }` raised NoMethodError. */
        changed |= proc_params_poly(nt, bs, pn, "keywords");
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
    changed |= proc_params_poly(nt, bs, pn, "posts");
    changed |= proc_params_poly(nt, bs, pn, "optionals");
    /* Keyword params (`proc { |a:, b: 5| }`): the call-site kwargs arrive as a
       boxed hash on the proc ABI, so the param binds a boxed value. */
    if (a_proc_forwarded_with_amp(c, id)) changed |= proc_params_poly(nt, bs, pn, "requireds");
    changed |= proc_params_poly(nt, bs, pn, "keywords");
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
    /* by block_param_name, which names `_1, _2` too: read off the
       requireds, a numbered block's hash went untyped while the default
       proc declared it the hash */
    Scope *bs = comp_scope_of(c, blk);
    for (int k = 0; ; k++) {
      const char *p = block_param_name(c, blk, k);
      if (!p) break;
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
    /* An untyped receiver leaves the param untyped too -- body usage is what
       types it there (`[].tap { |a| a << 1 }` gets its array kind from the
       push). The SLOT still has to exist: codegen binds the param whether or
       not anything reads it, and an unread param nobody interned named an
       undeclared identifier (#3979). Intern without claiming a type. */
    int intern_only = 0;
    if (yields_self) {
      /* tap/then/yield_self yield self to the block param for ANY receiver
         type (a string, an int, an object), so type the param as rt. */
      if (rt == TY_UNKNOWN) intern_only = 1;
      /* A nil receiver yields nil, and TY_NIL is not a slot type -- it has no
         storage and the body still reads the parameter. Poly holds it. The
         `then` / `yield_self` arm further down already says this; without it
         here the two arms typed the same parameter differently on every round
         and the fixpoint ran to its cap (#4116). */
      if (rt == TY_NIL) rt = TY_POLY;
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
        const char *nm = numbered_param_name(c, pn, k - 1);
        if (!nm) continue;
        LocalVar *lv = scope_local_intern(bs, nm); lv->is_block_param = 1;
        if (!intern_only && lv->type != rt) { lv->type = rt; changed = 1; }
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
      if (!intern_only && lv->type != rt) { lv->type = rt; changed = 1; }
    }
  }

  /* recv.instance_exec(args) { |params| } : the block binds the call's
     arguments as a yield of them does (emit_block_binds), so the binding
     plan types its parameters (block_site_types). */
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
    /* mixed-args trampoline (`instance_exec(x, @base, 7, &b)`): the block
       binds the trampoline body's arguments, a read of one of the
       trampoline's own params standing for the caller's argument */
    int tramp_argc = !sp_streq(cname, "instance_exec") ? ie_tramp_effective_argc(c, id) : -1;
    /* ...unless the body hands on the trampoline's own parameters whole
       (`def run(**kw, &b) = instance_exec(**kw, &b)`): then the block binds
       the caller's arguments themselves (block_site_args) */
    if (tramp_argc >= 0) {
      int tmi = comp_method_in_chain(c, ty_object_class(infer_type(c, xrecv)), cname, NULL);
      int tb = tmi >= 0 ? c->scopes[tmi].body : -1;
      int bn = 0; const int *bb = tb >= 0 ? nt_arr(nt, tb, "body", &bn) : NULL;
      if (bn == 1 && block_site_args(c, tmi, bb[0], id) == iargs) tramp_argc = -1;
    }
    int *tav = tramp_argc >= 0 ? malloc(sizeof(int) * (size_t)(tramp_argc + 1)) : NULL;
    for (int k = 0; k < tramp_argc; k++) tav[k] = ie_tramp_effective_arg(c, id, k);
    BlockSig s;
    block_sig(c, pn, 0, &s);
    int np = s.P + s.O + s.Q;
    TyKind *pos = calloc((size_t)(np + s.nk + 1), sizeof(TyKind));
    char *absent = calloc((size_t)np + 1, 1);
    block_site_types(c, &s, tav ? tav : iav, tav ? tramp_argc : iac, pos, absent, pos + np);
    changed |= block_settle_types(c, blk, &s, pos, absent, pos + np);
    free(pos); free(absent); free(tav);
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
      if (cs_type_params_site(c, recv, argv, argc)) changed = 1;
      continue;
    }
    /* A proc reached through a name: type the literal that name was assigned.
       A LOCAL was the only name looked at, so the identical lambda written to a
       constant or an instance variable kept the no-evidence int default and
       answered Integer for whatever it was really called with (#3942). A
       constant is program-wide, so its write is matched by name alone; a local
       and an ivar are matched within their scope and class. */
    const char *varname = nt_str(nt, recv, "name");
    if (!varname) continue;
    int want_kind;
    if (sp_streq(rty, "LocalVariableReadNode")) want_kind = 0;
    else if (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode")) want_kind = 1;
    else if (sp_streq(rty, "InstanceVariableReadNode")) want_kind = 2;
    else continue;
    Scope *call_scope = comp_scope_of(c, id);
    int call_cls = call_scope ? call_scope->class_id : -1;
    /* the writes of that name, through the kind index (every match is
       taken, so the kinds' order does not matter) */
    static const NodeKind wk_local[] = { NK_LocalVariableWriteNode };
    static const NodeKind wk_const[] = { NK_ConstantWriteNode, NK_ConstantPathWriteNode };
    static const NodeKind wk_ivar[] = { NK_InstanceVariableWriteNode };
    const NodeKind *wks = want_kind == 0 ? wk_local : want_kind == 1 ? wk_const : wk_ivar;
    int nwk = want_kind == 1 ? 2 : 1;
    for (int wki = 0; wki < nwk; wki++)
    NT_FOREACH_KIND(nt, wks[wki], w) {
      if (want_kind == 0) {
        if (comp_scope_of(c, w) != call_scope) continue;
      }
      else if (want_kind == 2) {
        Scope *ws = comp_scope_of(c, w);
        if (!ws || ws->class_id != call_cls) continue;
      }
      const char *wname = nt_str(nt, w, "name");
      if (!wname || !sp_streq(wname, varname)) continue;
      int val = nt_ref(nt, w, "value");
      if (val < 0 || !is_proc_create(c, val)) continue;
      if (cs_type_params_site(c, val, argv, argc)) changed = 1;
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

    /* `run(s, &method(:m))` into a user method that yields: each yield
       calls m with its arguments, which type m's parameters as a
       `method(:m).call(args)` does. m's parameters took nothing from them,
       and a String yielded to a parameter only its body typed was read as
       an Integer (TypeError at run time). */
    if (nt_kind(nt, block) == NK_BlockArgumentNode) {
      int bx = nt_ref(nt, block, "expression");
      /* `&method(:m)` handed to a method that keeps its block arrives as
         `method(:m).to_proc` */
      if (bx >= 0 && nt_kind(nt, bx) == NK_CallNode && nt_str(nt, bx, "name") &&
          sp_streq(nt_str(nt, bx, "name"), "to_proc") && nt_ref(nt, bx, "arguments") < 0)
        bx = nt_ref(nt, bx, "receiver");
      int tmi = bx >= 0 && nt_kind(nt, bx) == NK_CallNode ? method_obj_target_mi(c, bx) : -1;
      int ymi = -1;
      if (tmi >= 0 && !method_call_param_shift(c, bx, tmi)) {
        if (recv < 0) {
          Scope *self = comp_scope_of(c, id);
          if (self && self->class_id >= 0) ymi = comp_method_in_chain(c, self->class_id, name, NULL);
          if (ymi < 0) ymi = comp_method_index(c, name);
        }
        else if (sp_streq(name, "new") && (nt_kind(nt, recv) == NK_ConstantReadNode ||
                                           nt_kind(nt, recv) == NK_ConstantPathNode)) {
          int cid = nt_str(nt, recv, "name") ? comp_class_index(c, nt_str(nt, recv, "name")) : -1;
          if (cid >= 0) ymi = comp_method_in_chain(c, cid, "initialize", NULL);
        }
        else if (ty_is_object(infer_type(c, recv))) ymi = comp_method_in_chain(c, ty_object_class(infer_type(c, recv)), name, NULL);
        else ymi = const_recv_cmethod_mi(c, recv, name);
      }
      if (ymi >= 0 && forwarding_yield_target(c, ymi, 0) >= 0) {
        changed |= bind_method_obj_block_sites(c, ymi, tmi, id, 0);
        continue;
      }
    }

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

    /* File.open(args) { |f| ... } / IO.open: f is the handle, TY_IO.
       This said TY_POLY, which predates TY_IO -- and infer_return_types
       derives TY_IO for the same slot from the same evidence, so the two
       traded it and neither yielded: the fixpoint ran to its 128-round cap on
       anything that reached `Pathname#open`, which is `require "pathname"`
       (#4116). Naming the handle is the fix that also stops the fight: a slot
       typed TY_IO reaches #gets directly instead of through the runtime's
       sp_poly_as_io. */
    if (recv >= 0 && sp_streq(name, "open") && nt_type(nt, recv) &&
        sp_streq(nt_type(nt, recv), "ConstantReadNode") && nt_str(nt, recv, "name") &&
        (sp_streq(nt_str(nt, recv, "name"), "File") ||
         sp_streq(nt_str(nt, recv, "name"), "IO"))) {
      const char *p0 = block_param_name(c, block, 0);
      if (p0) { LocalVar *l = scope_local_intern(comp_scope_of(c, block), p0); l->is_block_param = 1;
                if (l->type != TY_IO) { l->type = TY_IO; changed = 1; } }
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

    /* each_with_index / reduce / inject rewritten onto builtins/enumerable.rb.
       The copy yields the row, but that yield is visited before the copy's
       own each has typed it, and a block parameter only widens, so the
       caller's parameter stuck at poly and the row was boxed on the way in.
       The table is argument 0. A numeric row takes that element type here,
       the same binding the call had before the rewrite. A flat array falls
       through and is typed from the yield, which is that definition's job. */
    if (recv < 0 && name && strncmp(name, "__enum_", 7) == 0) {
      int ewi = strncmp(name, "__enum_each_with_index__", 24) == 0;
      int red = strncmp(name, "__enum_reduce__", 15) == 0 || strncmp(name, "__enum_inject__", 15) == 0;
      int eargs = nt_ref(nt, id, "arguments");
      int ean = 0; const int *eav = eargs >= 0 ? nt_arr(nt, eargs, "arguments", &ean) : NULL;
      TyKind ert = (ean >= 1 && eav) ? infer_type(c, eav[0]) : TY_UNKNOWN;
      TyKind et = ty_is_array(ert) ? ty_array_elem(ert) : TY_UNKNOWN;
      int enp = 0; while (block_param_name(c, block, enp)) enp++;
      int row_shape = ewi ? enp > 0 && enp <= 2 : red && ean == 1 && enp == 2;
      /* A poly array may still narrow to a table of rows. Typing the
         parameter from the yield now would pin it at poly, and it only
         widens. Wait while that is still possible. Once the fixpoint gives
         up on a narrower type, fall through and let the yield type it. */
      if (row_shape && g_infer_optimistic &&
          (ert == TY_POLY_ARRAY || et == TY_UNKNOWN)) {
        /* Mark the parameters now, before a type is known. The next round's
           write pass resets every local that is not a block parameter and
           re-derives it from `ci[j] = s`, which reads as a poly slot. A
           block parameter only widens, so that guess would stick. */
        Scope *ws = comp_scope_of(c, block);
        for (int k = 0; k < enp; k++) {
          const char *ep = block_param_name(c, block, k);
          if (!ep) break;
          LocalVar *lp = scope_local_intern(ws, ep);
          lp->is_block_param = 1;
        }
        continue;
      }
      if (row_shape && (et == TY_FLOAT_ARRAY || et == TY_INT_ARRAY)) {
        Scope *es = comp_scope_of(c, block);
        const char *ep0 = block_param_name(c, block, 0);
        if (ep0) {
          if (bp_widen(es, ep0, et)) changed = 1;
        }
        const char *ep1 = block_param_name(c, block, 1);
        if (ep1) {
          TyKind want = ewi ? TY_INT : et;          if (bp_widen(es, ep1, want)) changed = 1;
        }
        continue;
      }
    }

    /* call to a user yielding method: block params take the yield arg types */
    {
      int mi = -1;
      if (recv < 0) {
        mi = comp_cbody_call_mi(c, id, name);
        if (mi < 0) mi = comp_method_index(c, name);
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
        /* Class.new { |...| }: the yielding method is Class#initialize.
           A ConstantPATH receiver counts: `N::Conn` names a class as much as
           `Conn` does, and reading only the bare form left `mi` unresolved,
           which drops through to the poly widening below (#4416). */
        int recv_is_const = nt_type(nt, recv) &&
                            (sp_streq(nt_type(nt, recv), "ConstantReadNode") ||
                             sp_streq(nt_type(nt, recv), "ConstantPathNode"));
        if (mi < 0 && sp_streq(name, "new") && recv_is_const) {
          const char *cname = nt_str(nt, recv, "name");
          int cid = cname ? comp_class_index(c, cname) : -1;
          if (cid >= 0) mi = comp_method_in_chain(c, cid, "initialize", NULL);
        }
        /* Class.method { ... }: look up the class method. This is where the
           block's parameters get their types from what the method YIELDS, so
           missing the path spelling did not fail loudly -- it typed the
           parameter poly, and the call inside the block then went through a
           class switch instead of a direct call. On a name the caller's own
           class also defines, that switch opens an arm for the CALLER, and
           inlining a yielding method into itself exhausts the inline depth:
           the "calls itself recursively" diagnostic on #4416 is this, three
           steps downstream. */
        if (mi < 0 && recv_is_const) {
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
          /* UNKNOWN here is not the same claim as POLY. POLY says the
             receiver really can be several things; UNKNOWN only says this
             round has not typed it yet, and both answers below (adopting a
             user method's yield types, widening the params to poly) are
             irreversible once taken. A parameter whose call site types it
             one round later would be judged on the guess instead of on the
             answer -- `flat.each { |k, v| sub[k] = v }` widened `sub` to
             poly even though `flat` settles as a String->String hash, and
             the widened hash then no longer fits an RBS-declared
             Hash[String, untyped] slot (#4100). Wait: the second stage runs
             with g_infer_optimistic cleared, and a receiver still UNKNOWN
             there is genuinely untypable. */
          if (rt0 == TY_UNKNOWN && g_infer_optimistic) continue;
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
          /* ...but a name the builtin Enumerable surface also owns can reach
             a container at run time, through the dispatch's builtin arm. The
             adopted method's yield types describe only the user arm, so a
             block typed from them binds the wrong thing on the other one
             (a String element into an Integer slot). Widen instead (#3409).
             This does not depend on a class having been ADOPTED: with two or
             more candidates nothing is adopted and the widening was skipped,
             so the param kept whatever an earlier round had guessed. A second
             Struct in the file was enough to change the answer, because every
             Struct defines `each` (#4086). More candidates is a stronger case
             for poly, not a weaker one. */
          /* Several candidates and none adopted: the call is a dispatch over
             them, and the builtin rules below must not type the block from
             the NAME -- a poly `each_line` read as an IO's walk bound the
             Integer a user each_line yielded into a String slot. */
          if (ndef > 0 && (poly_enum_op_for(name) || (mi < 0 && rt0 == TY_POLY))) {
            Scope *bs2 = comp_scope_of(c, block);
            for (int k = 0; ; k++) {
              const char *bp2 = block_param_name(c, block, k);
              if (!bp2) break;
              LocalVar *lv2 = scope_local_intern(bs2, bp2);
              lv2->is_block_param = 1;
              if (lv2->type != TY_POLY) { lv2->type = TY_POLY; changed = 1; }
            }
            continue;
          }
        }
      }
      /* `k.new { }` on a class value and a bare `new { }` in a class
         method: the block is the initialize's, as a constant's `new` gives
         it above. Unresolved, its parameters were typed from the body
         alone -- `t << x` made one an Array -- and the initialize handed
         it an Integer or a String: the program crashed. */
      if (mi < 0 && sp_streq(name, "new")) {
        int several = 0;
        mi = new_block_initialize(c, id, &several);
        if (several) {
          /* every parameter, the optionals, posts and keywords too, as a
             kept block's are (block_params_widen) */
          BlockSig s;
          block_sig(c, nt_ref(nt, block, "parameters"), 0, &s);
          size_t nw = (size_t)(s.P + s.O + s.Q + s.nk) + 1;
          TyKind *tf = (TyKind *)calloc(nw, sizeof *tf);
          char *ab = (char *)calloc(nw, 1);
          if (!tf || !ab) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
          changed |= block_params_widen(c, block, &s, 1, tf, ab, tf + s.P + s.O + s.Q);
          free(tf); free(ab);
          continue;
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
      /* A block the method yields to, calls through its &block, or hands to
         a receiverless instance_exec: every such site binds it, and the
         binding plan types its parameters from all of them at once
         (block_site_types). A method that keeps its &block without
         yielding runs the block as a proc, whose prologue binds only the
         requireds typed and reads a missing one as the slot's own nil (a
         true/false or a Symbol one, which has none, takes the box). An
         Integer or a Float one a site may leave without a value, or pass a
         nil, is marked nullable here, as cs_type_params marks a proc
         literal's. */
      int yields = yld_mi >= 0 && c->scopes[yld_mi].yields;
      if (yields || (mi >= 0 && c->scopes[mi].blk_param && c->scopes[mi].blk_param[0])) {
        BlockSig s;
        TyKind *pos, *posf; char *absent, *absentf;
        int kept = block_bind_plan(c, id, block, mi, yields ? yld_mi : mi, yields, &s, &pos, &absent, &posf, &absentf);
        int np = s.P + s.O + s.Q;
        if (yields) {
          changed |= block_settle_types(c, block, &s, pos, absent, pos + np);
          bsn_note(id, block, mi, yld_mi);
        }
        else {
          Scope *bs = comp_scope_of(c, block);
          for (int k = 0; k < s.P; k++) {
            const char *bp = block_sig_name(c, &s, k);
            TyKind at = pos[k] == TY_UNKNOWN && (absent[k] & BS_NIL) ? TY_NIL : pos[k];
            if (!bp || at == TY_UNKNOWN) continue;
            LocalVar *lv = scope_local_intern(bs, bp); lv->is_block_param = 1;
            TyKind merged = ty_unify(lv->type, absent[k] ? ty_unify(at, TY_NIL) : at);
            if ((absent[k] & BS_NIL) && (merged == TY_INT || merged == TY_FLOAT) && g_promote_mode) merged = TY_POLY;
            if ((merged == TY_INT || merged == TY_FLOAT) && absent[k] && !lv->nullable_int) { lv->nullable_int = 1; changed = 1; }
            if (merged != lv->type) { lv->type = merged; changed = 1; }
          }
        }
        changed |= block_params_widen(c, block, &s, kept, posf, absentf, posf + np);
        free(pos); free(absent); free(posf); free(absentf);
        continue;
      }
      /* A block handed to a user method that neither yields nor names a
         &block (nor forwards to one that does) never runs, and codegen drops
         it. Its params then had no evidence at all and stayed UNKNOWN, which
         is "not yet", not "nothing": a `return response[...]` inside such a
         block typed UNKNOWN, unified with the raising body's void, and the
         method came out void -- so a caller reading its value was refused
         (#4431). The block's value flows nowhere, so poly costs nothing. */
      if (mi >= 0 && !c->scopes[mi].yields &&
          !(c->scopes[mi].blk_param && c->scopes[mi].blk_param[0]) &&
          forwarding_yield_target(c, mi, 0) < 0) {
        Scope *bs = comp_scope_of(c, block);
        for (int k = 0; ; k++) {
          const char *bp = block_param_name(c, block, k);
          if (!bp) break;
          LocalVar *lv = scope_local_intern(bs, bp); lv->is_block_param = 1;
          if (lv->type != TY_POLY) { lv->type = TY_POLY; changed = 1; }
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
    /* A narrowed object array's walk binds its element boxed into a boxed
       parameter (the emitters' box_to_poly), as the array's elements were
       before it narrowed: the parameter keeps the element type a poly array
       gives it. Left untyped, the block's own uses typed it -- `r[k] = v`
       made it a hash, and an sp_X * element was bound into it (#4879). */
    if (ty_is_obj_array(rt)) rt = TY_POLY_ARRAY;
    const char *p0 = block_param_name(c, block, 0);
    if (!p0 && !block_param_is_multi(c, block, 0)) continue;

    /* then / yield_self: block param receives the receiver value */
    if ((sp_streq(name, "then") || sp_streq(name, "yield_self")) && p0) {
      Scope *bs = comp_scope_of(c, block);
      if (bp_widen(bs, p0, rt == TY_NIL ? TY_POLY : rt)) changed = 1;
      continue;
    }

    TyKind pt = TY_UNKNOWN;
    /* a BOXED receiver's step is an Integer's or a Float's only at run time
       (the two-owner face row, #4763): the param takes the box, so the body
       is typed once and both arms bind it boxed */
    if (sp_streq(name, "step") && rt == TY_POLY && p0) pt = TY_POLY;
    else if (sp_streq(name, "step") && (rt == TY_INT || rt == TY_FLOAT)) {
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
      int rnn = an_unparen(nt, recv);
      if (rnn >= 0 && nt_type(nt, rnn) && sp_streq(nt_type(nt, rnn), "RangeNode")) {
        int lo = nt_ref(nt, rnn, "left"), hi = nt_ref(nt, rnn, "right");
        if ((lo >= 0 && infer_type(c, lo) == TY_FLOAT) ||
            (hi >= 0 && infer_type(c, hi) == TY_FLOAT)) isf = 1;
      }
      pt = isf ? TY_FLOAT : TY_INT;
    }
    else if (sp_streq(name, "step") && (rt == TY_RATIONAL || rt == TY_BIGINT))
      pt = TY_POLY;  /* yields boxed Rational/Integer values (#2566); a Bignum receiver walks boxed too (#4779) */
    else if ((sp_streq(name, "times") || sp_streq(name, "upto") ||
         sp_streq(name, "downto")) && rt == TY_INT)
      pt = TY_INT;
    /* on a boxed receiver the block runs through the dispatch with its
       argument boxed; typed during inference, not only when emitted, so what
       the block computes from it is typed too (left unknown, `v = i * 10`
       contributed nothing and `v` came out a String beside `v = "s"`) */
    else if ((sp_streq(name, "times") || sp_streq(name, "upto") ||
              sp_streq(name, "downto") || sp_streq(name, "step")) && rt == TY_POLY)
      pt = TY_POLY;
    else if (rt == TY_POLY && sp_streq(name, "each_line"))
      pt = TY_STRING;  /* File/IO object yielding lines */
    /* a typed File/IO (or ARGF) yields lines and chars as Strings, bytes and
       codepoints as Integers -- typed here, with the other block params, so
       the body is inferred against it: set only where infer_type reaches the
       call, `h << line` had already made `h = []` a PolyArray */
    else if ((rt == TY_IO || rt == TY_ARGF) &&
             (sp_streq(name, "each_line") || sp_streq(name, "each") ||
              (rt == TY_IO && sp_streq(name, "each_char")) || (rt == TY_ARGF && sp_streq(name, "each_string"))))
      pt = TY_STRING;
    else if (rt == TY_IO && (sp_streq(name, "each_byte") || sp_streq(name, "each_codepoint")))
      pt = TY_INT;
    else if (rt == TY_POLY && sp_streq(name, "each_byte"))
      pt = TY_INT;
    else if (rt == TY_STRING && (sp_streq(name, "each_char") || sp_streq(name, "each_line") || sp_streq(name, "upto") ||
                                 sp_streq(name, "chars") || sp_streq(name, "lines") || sp_streq(name, "split")))
      pt = TY_STRING;  /* split { |piece| } yields each substring */
    else if ((rt == TY_STRING || rt == TY_POLY) &&
             (sp_streq(name, "gsub") || sp_streq(name, "sub") ||
              sp_streq(name, "gsub!") || sp_streq(name, "sub!")))   /* the bang forms are rewritten to these */
      /* block receives the matched substring -- a String whatever the
         receiver's static type is, so a BOXED receiver yields one too. Left at
         TY_STRING for the typed receiver only, the poly form declared the
         param sp_RbVal while the emitter assigned it the raw substring. */
      pt = TY_STRING;
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
          if (bp_widen(scs, pn2, TY_STRING)) changed = 1;
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
      int frn = an_unparen(nt, recv);
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
              sp_streq(name, "min_by") || sp_streq(name, "max_by") ||
              /* the index-finding family binds the element exactly as its
                 siblings do; it was left off, so a block param that no other
                 site typed stayed unknown and got no declaration (#3409) */
              sp_streq(name, "find_index") || sp_streq(name, "index") ||
              sp_streq(name, "rindex") ||
              /* Same binding, same omission: every remaining sibling that
                 yields one element (or, for the pairwise ones, two). A param
                 no other site typed stayed unknown and got no declaration, so
                 the emitted body referenced an undeclared local and the C
                 compile failed outright (#3448). */
              sp_streq(name, "filter") || sp_streq(name, "find_all") ||
              sp_streq(name, "flat_map") || sp_streq(name, "collect_concat") ||
              sp_streq(name, "filter_map") || sp_streq(name, "partition") ||
              sp_streq(name, "group_by") || sp_streq(name, "none?") ||
              sp_streq(name, "one?") || sp_streq(name, "count") ||
              sp_streq(name, "sum") || sp_streq(name, "take_while") ||
              sp_streq(name, "drop_while") || sp_streq(name, "each_entry") ||
              /* reverse_each walks the same elements as each, from the other
                 end: its block binds them the same way (#3987) */
              sp_streq(name, "reverse_each") ||
              sp_streq(name, "bsearch") ||
              sp_streq(name, "chunk_while") || sp_streq(name, "slice_when") ||
              /* a Hash's own walks: a boxed receiver runs them on the
                 PolyPolyHash it is taken as (or dispatches them at run time)
                 and binds every key and value boxed. Left off, a param typed
                 in a round that still read the receiver as its `{}` arm's
                 StrPolyHash stayed a String, and the boxed key was assigned
                 to a const char * */
              sp_streq(name, "transform_keys") || sp_streq(name, "transform_values") ||
              sp_streq(name, "each_key") || sp_streq(name, "each_value") ||
              sp_streq(name, "delete_if") || sp_streq(name, "keep_if") ||
              sp_streq(name, "select!") || sp_streq(name, "filter!") ||
              sp_streq(name, "reject!")))
      pt = TY_POLY;

    /* array.each_cons(n) / each_slice(n) { |a, b, ...| } -- a single param
       binds the n-element sub-array; multiple params destructure elements.
       Also handles |(a, b)| destructuring: leaves bind to element type. */
    if ((sp_streq(name, "each_cons") || sp_streq(name, "each_slice")) && ty_is_array(rt)) {
      Scope *es = comp_scope_of(c, block);
      int np = 0; while (block_param_name(c, block, np)) np++;
      if (np == 0 && block_param_is_multi(c, block, 0)) {
        TyKind elem = ty_array_elem(rt);
        changed |= block_leaves_unify(c, block, es, elem);
      }
      else {
        for (int pj = 0; pj < np; pj++) {
          const char *pn = block_param_name(c, block, pj);
          TyKind want = (np == 1) ? rt : ty_array_elem(rt);          if (bp_widen(es, pn, want)) changed = 1;
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
            if (elem2 != TY_UNKNOWN) changed |= block_leaves_unify(c, block, es2, elem2);
          }
          else {
            for (int pj2 = 0; pj2 < np2; pj2++) {
              const char *pn2 = block_param_name(c, block, pj2);
              if (!pn2) break;
              if (bp_widen(es2, pn2, bp_t2)) changed = 1;
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
            if (bp_widen(wi_es, idx_p, TY_INT)) changed = 1;
          }
          if (block_param_is_multi(c, block, 0)) {
            /* |(a, b), i|: destructure first multi-target param */
            changed |= block_leaves_unify(c, block, wi_es, elem_t);
          }
          else {
            /* |pair, i|: pair gets the sub-array type */
            const char *pair_p = block_param_name(c, block, 0);
            if (pair_p) {
              if (bp_widen(wi_es, pair_p, ec_arr_t)) changed = 1;
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
      int chain_arr = an_indexed_each_source(nt, recv);
      TyKind chain_at = chain_arr >= 0 ? infer_type(c, chain_arr) : TY_UNKNOWN;
      if (ty_is_array(chain_at) && block >= 0) {
        TyKind elem = ty_array_elem(chain_at);
        Scope *bs = comp_scope_of(c, block);
        int rargs = nt_ref(nt, id, "arguments"); int rargc = 0;
        const int *rargv = rargs >= 0 ? nt_arr(nt, rargs, "arguments", &rargc) : NULL;
        TyKind acc_t = (rargc > 0 && rargv) ? infer_type(c, rargv[0]) : elem;
        if (acc_t == TY_UNKNOWN) acc_t = elem;
        if (p0) {
          if (bp_widen(bs, p0, acc_t)) changed = 1;
        }
        if (block_param_is_multi(c, block, 1)) {
          int lc = block_param_multi_count(c, block, 1);
          for (int li = 0; li < lc; li++) {
            const char *ln = block_param_multi_leaf(c, block, 1, li);
            if (!ln) continue;
            TyKind want = (li == 0) ? elem : TY_INT;            if (bp_widen(bs, ln, want)) changed = 1;
          }
        }
        else {
          const char *pp = block_param_name(c, block, 1);
          if (pp) {
            TyKind pairt = (elem == TY_INT) ? TY_INT_ARRAY : TY_POLY_ARRAY;            if (bp_widen(bs, pp, pairt)) changed = 1;
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
      int chain_arr = an_indexed_each_source(nt, recv);
      TyKind chain_at = chain_arr >= 0 ? infer_type(c, chain_arr) : TY_UNKNOWN;
      /* Only the |v, i| two-param form (v = element, i = index); single-param
         and destructure forms have method-dependent semantics and are left to
         other rules (the codegen path bails on them too). */
      const char *vp = block_param_name(c, block, 0);
      const char *ip = block_param_name(c, block, 1);
      if (ty_is_array(chain_at) && !block_param_is_multi(c, block, 0) && vp && ip) {
        TyKind elem = ty_array_elem(chain_at);
        Scope *bs = comp_scope_of(c, block);
        if (bp_widen(bs, vp, elem)) changed = 1;
        if (bp_widen(bs, ip, TY_INT)) changed = 1;
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
            if (bp_widen(wis, p0, ty_array_elem(arr_t))) changed = 1;
          }
          const char *idx_p = block_param_name(c, block, 1);
          if (idx_p) {
            if (bp_widen(wis, idx_p, TY_INT)) changed = 1;
          }
          continue;
        }
      }
    }

    /* array.combination(k)/permutation(k) { |c| } binds the k-element sub-array */
    if ((sp_streq(name, "combination") || sp_streq(name, "permutation")) && ty_is_array(rt)) {
      if (bp_widen(comp_scope_of(c, block), p0, rt)) changed = 1;
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
        if (bp_widen(cs, pn, ty_array_elem(rt))) changed = 1;
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
      /* An empty `[]` / `{}` seed the block only hands to a callable has no
         fill to type it from; the element type of the RECEIVER is not what it
         holds, so answer the general boxed container (#3657). */
      if (rargc > 0 && rargv && acc_t == TY_UNKNOWN) {
        const char *s0 = nt_type(nt, rargv[0]);
        int sn0 = 0;
        if (s0 && sp_streq(s0, "ArrayNode") &&
            (nt_arr(nt, rargv[0], "elements", &sn0), sn0 == 0) &&
            ewo_memo_passed_to_callable_at(c, id, 0))
          acc_t = TY_POLY_ARRAY;
        else if (s0 && sp_streq(s0, "HashNode") &&
                 (nt_arr(nt, rargv[0], "elements", &sn0), sn0 == 0))
          acc_t = TY_POLY_POLY_HASH;
      }
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
      if (bp_widen(rs, p0, acc_t)) changed = 1;
      const char *rp1 = block_param_name(c, block, 1);
      if (rp1) {
        if (bp_widen(rs, rp1, et2)) changed = 1;
      }
      continue;
    }

    /* array.each_with_index { |x, i| } binds element + int index */
    if (sp_streq(name, "each_with_index") && ty_is_array(rt)) {
      Scope *es = comp_scope_of(c, block);
      if (!p0) continue;
      if (bp_widen(es, p0, ty_array_elem(rt))) changed = 1;
      const char *p1 = block_param_name(c, block, 1);
      if (p1) {
        if (bp_widen(es, p1, TY_INT)) changed = 1;
      }
      continue;
    }

    /* array.zip(other) { |a, b| } binds element of recv + element of other */
    /* str.match(/re/) { |m| }: the block receives the MatchData */
    if (sp_streq(name, "match") && p0) {
      int margs = nt_ref(nt, id, "arguments");
      int mac = 0; const int *mav = margs >= 0 ? nt_arr(nt, margs, "arguments", &mac) : NULL;
      const char *mrt = nt_type(nt, recv), *mat = mac > 0 ? nt_type(nt, mav[0]) : NULL;
      /* the pattern may also be a Regexp-typed local or an interpolated
         literal rather than a bare /re/ node (#3642) */
      if ((mrt && sp_streq(mrt, "RegularExpressionNode")) ||
          (mat && sp_streq(mat, "RegularExpressionNode")) ||
          rt == TY_REGEX || (mac > 0 && infer_type(c, mav[0]) == TY_REGEX)) {
        Scope *ms = comp_scope_of(c, block);
        if (bp_widen(ms, p0, TY_MATCHDATA)) changed = 1;
        continue;
      }
    }
    if (sp_streq(name, "zip") && ty_is_array(rt)) {
      Scope *zs = comp_scope_of(c, block);
      const char *zp1s = block_param_name(c, block, 1);
      LocalVar *ep0 = scope_local_intern(zs, p0); ep0->is_block_param = 1;
      /* a SOLO param receives the boxed TUPLE ([e1, e2]); two params
         auto-splat it */
      if (lv_widen(ep0, zp1s ? ty_array_elem(rt) : TY_POLY)) changed = 1;
      const char *zp1 = zp1s;
      if (zp1) {
        int zargs = nt_ref(nt, id, "arguments");
        int zargc = 0; const int *zargv = zargs >= 0 ? nt_arr(nt, zargs, "arguments", &zargc) : NULL;
        TyKind et2 = (zargc > 0 && zargv && ty_is_array(infer_type(c, zargv[0])))
                     ? ty_array_elem(infer_type(c, zargv[0])) : ty_array_elem(rt);
        if (bp_widen(zs, zp1, et2)) changed = 1;
      }
      continue;
    }

    /* hash.merge/merge!/update(other) { |k, v1, v2| } binds key + both values */
    if ((sp_streq(name, "merge") || sp_streq(name, "merge!") || sp_streq(name, "update")) &&
        ty_is_hash(rt)) {
      Scope *ms = comp_scope_of(c, block);
      if (bp_widen(ms, p0, ty_hash_key(rt))) changed = 1;
      const char *mp1 = block_param_name(c, block, 1);
      const char *mp2 = block_param_name(c, block, 2);
      const char *mps[2]; mps[0] = mp1; mps[1] = mp2;
      for (int mi2 = 0; mi2 < 2; mi2++) {
        if (!mps[mi2]) continue;
        if (bp_widen(ms, mps[mi2], ty_hash_val(rt))) changed = 1;
      }
      continue;
    }

    /* array.product(other) { |pair| } binds the boxed pair array */
    if (sp_streq(name, "product") && ty_is_array(rt) && p0) {
      Scope *aps = comp_scope_of(c, block);
      if (bp_widen(aps, p0, TY_POLY)) changed = 1;
      continue;
    }
    /* array.fetch(i) { |i| } binds the (int) index */
    if (sp_streq(name, "fetch") && ty_is_array(rt) && p0) {
      Scope *afs = comp_scope_of(c, block);
      if (bp_widen(afs, p0, TY_INT)) changed = 1;
      continue;
    }
    /* hash.fetch(key) { |k| } binds the looked-up key */
    if (sp_streq(name, "fetch") && ty_is_hash(rt)) {
      Scope *fs = comp_scope_of(c, block);
      if (bp_widen(fs, p0, ty_hash_key(rt))) changed = 1;
      continue;
    }

    /* hash.transform_keys { |k| } binds key; transform_values { |v| } value */
    if ((sp_streq(name, "transform_keys") || sp_streq(name, "transform_values")) && ty_is_hash(rt)) {
      Scope *hs = comp_scope_of(c, block);
      TyKind want = sp_streq(name, "transform_keys") ? ty_hash_key(rt) : ty_hash_val(rt);      if (bp_widen(hs, p0, want)) changed = 1;
      continue;
    }

    /* hash.each_value { |v| } binds value; each_key { |k| } binds key */
    if ((sp_streq(name, "each_value") || sp_streq(name, "each_key")) && ty_is_hash(rt)) {
      Scope *hs = comp_scope_of(c, block);
      LocalVar *vp = scope_local_intern(hs, p0); vp->is_block_param = 1;
      TyKind want = sp_streq(name, "each_value") ? ty_hash_val(rt) : ty_hash_key(rt);
      /* a boxed-value hash whose values are all one class binds that class,
         decided for the slot as a whole, so it replaces the boxed value type
         an earlier round joined in (#4846) */
      if (sp_streq(name, "each_value")) {
        int hcls = hv_value_class(c, recv);
        if (hcls >= 0) {
          if (vp->type != ty_object(hcls)) { vp->type = ty_object(hcls); changed = 1; }
          continue;
        }
      }
      if (lv_widen(vp, want)) changed = 1;
      continue;
    }

    /* hash.each / each_pair { |k, v| } or { |(k,v)| } binds two params.
       Also handles mutating iteration (delete_if / select! / reject! / keep_if). */
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
         sp_streq(name, "each_with_index")) && ty_is_hash(rt)) {
      Scope *hs = comp_scope_of(c, block);
      /* |(k,v)| or |(k,v), memo| destructuring (MultiTargetNode first param) */
      if (block_param_is_multi(c, block, 0)) {
        int lc = block_param_multi_count(c, block, 0);
        if (lc >= 1) {
          const char *kn = block_param_multi_leaf(c, block, 0, 0);
          if (kn) {
            if (bp_widen(hs, kn, ty_hash_key(rt))) changed = 1;
          }
        }
        if (lc >= 2) {
          const char *vn = block_param_multi_leaf(c, block, 0, 1);
          if (vn) {
            if (bp_widen(hs, vn, ty_hash_val(rt))) changed = 1;
          }
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
          if (bp_widen(hs, p0, pair_solo ? TY_POLY : ty_hash_key(rt))) changed = 1;
        }
        if (p1) {
          if (bp_widen(hs, p1, ty_hash_val(rt))) changed = 1;
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
            if (bp_widen(ds, pname2, inner_elem)) changed = 1;
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
          if (lv_widen(lp2, TY_POLY)) changed = 1;
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
    if (ty_is_array(lv->type) && !ty_is_array(pt)) {
      /* ...unless the variable is a PURE block parameter, one no write site
         in the scope assigns: its array type can only have come from the
         usage pass reading a push inside the block (`@rows.each { |r| r <<
         1.5 }`) ahead of this binding, in the round before the receiver had
         a type. The push says what the element holds, not what the element
         is; the receiver does, and it says poly. Keeping the array type
         assigned the loop's sp_RbVal element to an sp_FloatArray * and the
         program did not compile. */
      if (pt == TY_UNKNOWN || !pure_block_param(c, s, p0)) continue;
      lv->type = pt; changed = 1;
      continue;
    }
    if (lv_widen(lv, pt)) changed = 1;
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

/* The node a `return` returns: its one argument, else the ReturnNode itself
   (a bare `return`, or `return a, b`). */
static int return_value_node(Compiler *c, int id) {
  int args = nt_ref(c->nt, id, "arguments");
  if (args < 0) return id;
  int n = 0;
  const int *a = nt_arr(c->nt, args, "arguments", &n);
  return (a && n == 1) ? a[0] : id;
}

/* Defined in codegen_fold.c (linked in). */
int is_descendant(Compiler *c, int k, int anc);

/* True when the method body's tail statement unconditionally raises, so the C
   function never reaches a return (used to widen its void type to the override
   return type -- the unreachable "return value" can safely take that type). */
/* Does this method's body end in a call that resolves to nothing -- no user
   method, no reader, no builtin the emitter knows? Codegen answers such a call
   with a NoMethodError raise, so the method never returns a value. */
static int scope_tail_unresolved_call(Compiler *c, int s) {
  const NodeTable *nt = c->nt;
  int body = c->scopes[s].body;
  if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) return 0;
  int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
  if (bn <= 0) return 0;
  int last = bb[bn - 1];
  const char *ty = nt_type(nt, last);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  int recv = nt_ref(nt, last, "receiver");
  if (recv < 0) return 0;
  TyKind lt = infer_type(c, last);
  if (lt != TY_VOID && lt != TY_UNKNOWN) return 0;
  /* A builtin-typed receiver: the name is either one of that type's methods
     (and would have a type) or nothing at all. An object/poly/unknown receiver
     is not decided here -- the fixpoint may still settle it. */
  TyKind rt = infer_type(c, recv);
  /* Only an instance of a builtin whose method set is closed: a class
     reference (`Hash.new`) or a container the fixpoint has not settled can
     still grow a type, and pinning the return here would freeze the caller's
     shape before that happened. */
  if (rt != TY_STRING && rt != TY_STRBUF && rt != TY_INT && rt != TY_BIGINT &&
      rt != TY_FLOAT && rt != TY_SYMBOL && rt != TY_BOOL) return 0;
  const char *nm = nt_str(nt, last, "name");
  if (!nm) return 0;
  /* Only a REOPEN of the receiver's own builtin can answer it: an unrelated
     user class defining the same name is not a candidate here (the receiver is
     statically a String / Integer / ...), and treating it as one put the
     method back to void as soon as any library happened to share the name. */
  { const char *bn = rt == TY_STRING || rt == TY_STRBUF ? "String"
                   : rt == TY_INT || rt == TY_BIGINT ? "Integer"
                   : rt == TY_FLOAT ? "Float"
                   : rt == TY_SYMBOL ? "Symbol" : NULL;
    if (bn) {
      int bc = comp_class_index(c, bn);
      if (bc >= 0 && (comp_method_in_chain(c, bc, nm, NULL) >= 0 ||
                      comp_reader_in_chain(c, bc, nm, NULL))) return 0;
    }
    int oc = comp_class_index(c, "Object");
    if (oc >= 0 && (comp_method_in_chain(c, oc, nm, NULL) >= 0 ||
                    comp_reader_in_chain(c, oc, nm, NULL))) return 0;
  }
  return 1;
}

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
    unsigned b = sp_strhash(c->scopes[s].name) % (unsigned)rn_buckets;
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
    if (mi < 0) mi = comp_included_method_index(c, name, call_id);
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
  /* per scope, the kinds (an_empty_container_kind bits) of the empty `[]` /
     `{}` its explicit returns answer: untyped, they vanish from ret_acc */
  char *ret_empty = (char *)calloc((size_t)(ns > 0 ? ns : 1), 1);
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
    /* `return x unless block_given?` (or `if !block_given?`) in a yielding
       method: the return a BLOCKLESS call answers. Its type is kept apart
       (Scope.ret_noblock) so the call with a block, which the inliner
       specializes with the guard folded away, reads the body proper: the
       union of an Enumerator with the memo `each_with_object` answers was
       neither. The guarded returns are found from their guard, since a
       return node does not know its parent. */
    unsigned char *noblk = (unsigned char *)calloc((size_t)(nt->count ? nt->count : 1), 1);
    for (int id = 0; noblk && id < nt->count; id++) {
      NodeKind gk = nt_kind(nt, id);
      if (gk != NK_UnlessNode && gk != NK_IfNode) continue;
      int pred = nt_ref(nt, id, "predicate");
      if (pred < 0) continue;
      int bg = -1;
      if (gk == NK_UnlessNode) bg = pred;
      else if (nt_kind(nt, pred) == NK_CallNode && nt_str(nt, pred, "name") &&
               sp_streq(nt_str(nt, pred, "name"), "!")) bg = nt_ref(nt, pred, "receiver");
      if (bg < 0 || nt_kind(nt, bg) != NK_CallNode || nt_ref(nt, bg, "receiver") >= 0) continue;
      const char *bn = nt_str(nt, bg, "name");
      if (!bn || !sp_streq(bn, "block_given?")) continue;
      if (nt_ref(nt, id, "subsequent") >= 0) continue;   /* an else arm: not a plain guard */
      /* only a guard at the method's top level stops every blockless call:
         one under `if x.nil?` lets the call with an x run on to the body,
         and typed as the guard's nil it printed nil for a found index
         (#5097). A nested guard's return is an ordinary return. */
      Scope *gs = comp_scope_of(c, id);
      int gn = 0; const int *gb = gs && gs->body >= 0 ? nt_arr(nt, gs->body, "body", &gn) : NULL;
      int top = 0;
      for (int k = 0; k < gn && !top; k++) top = gb[k] == id;
      if (!top) continue;
      int stm = nt_ref(nt, id, "statements");
      int sn = 0; const int *sb = stm >= 0 ? nt_arr(nt, stm, "body", &sn) : NULL;
      for (int k = 0; k < sn; k++) if (nt_kind(nt, sb[k]) == NK_ReturnNode) noblk[sb[k]] = 1;
    }
    for (int s = 1; s < ns; s++) c->scopes[s].ret_noblock = TY_UNKNOWN;
    for (int id = 0; id < nt->count; id++) {
      if (nt_kind(nt, id) != NK_ReturnNode) continue;
      Scope *rs = comp_scope_of(c, id);
      if (!rs) continue;
      int si = (int)(rs - c->scopes);
      if (si < 0 || si >= ns) continue;
      TyKind rt = (ret_narrow && ret_narrow[id]) ? ret_narrow[id] : return_node_type(c, id);
      if (noblk && noblk[id] && rs->yields) {
        rs->ret_noblock = rs->ret_noblock == TY_UNKNOWN ? rt : ty_unify(rs->ret_noblock, rt);
        continue;
      }
      ret_acc[si] = has_ret[si] ? ty_unify(ret_acc[si], rt) : rt;
      has_ret[si] = 1;
      if (rt == TY_UNKNOWN && ret_empty)
        ret_empty[si] |= (char)an_empty_container_kind(c, return_value_node(c, id));
      if (ret_head && ret_next) { ret_next[id] = ret_head[si]; ret_head[si] = id; }
    }
    free(noblk);
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
      /* Same shape, and the same reason. RBS `Integer` covers both machine
         ints and bignums, so a body that grew a bignum is a valid inhabitant
         of the declared type -- but the seed had already pinned the signature
         to sp_int, and returning an sp_Bigint* through it truncates the
         pointer and answers garbage. Declaring the type correctly made the
         program worse than not declaring it at all (#3518). */
      else if (sc->ret == TY_INT) {
        TyKind br = sc->body >= 0 ? infer_type(c, sc->body) : TY_UNKNOWN;
        if (has_ret && has_ret[s]) br = ty_unify(br, ret_acc[s]);
        if (br == TY_BIGINT) { sc->ret = TY_BIGINT; changed = 1; }
      }
      continue;
    }
    /* A return narrowed to a pointer array is pinned the same way. The body
       still reads the poly array, and those two array KINDS unify to the plain
       poly SCALAR -- so re-deriving would make the slot strictly worse, and
       reporting that as a change every round runs the fixpoint to its cap. */
    if (sc->ret_oa_pin != TY_UNKNOWN) continue;
    /* synthesized compiler_state methods carry a fixed return type (no AST). */
    if (sc->cs_synth) continue;
    /* A lowered self-recursive yield method returns its block's value through
       a raw sp_int carrier pinned when the lowering rewrites the scope
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
    /* a bare `Array.new` tail returns the poly array a marked `[]` tail
       does (mark_empty_literal_tails) */
    if (r == TY_UNKNOWN && !empty_body && !(has_ret && has_ret[s]) &&
        an_empty_container_kind(c, sc->body) == 1)
      r = TY_POLY_ARRAY;
    /* A yielding method whose body ends in `if block_given? ... else ... end`
       has two values, one per call form, and the inliner keeps only the arm a
       call site takes: the block arm types the call with a block, the else
       arm the call without one (Scope.ret_noblock, read by method_call_ret).
       Unified, the Enumerator of the one arm and the memo of the other made
       the method poly for both (builtins/enumerable.rb). */
    if (!empty_body && !tail_unreachable && sc->yields && nt_kind(nt, sc->body) == NK_StatementsNode) {
      int bn3 = 0; const int *bb3 = nt_arr(nt, sc->body, "body", &bn3);
      int last = bn3 > 0 ? bb3[bn3 - 1] : -1;
      int pred = last >= 0 && nt_kind(nt, last) == NK_IfNode ? nt_ref(nt, last, "predicate") : -1;
      int sub = last >= 0 ? nt_ref(nt, last, "subsequent") : -1;
      if (pred >= 0 && nt_kind(nt, pred) == NK_CallNode && nt_ref(nt, pred, "receiver") < 0 &&
          nt_str(nt, pred, "name") && sp_streq(nt_str(nt, pred, "name"), "block_given?") &&
          sub >= 0 && nt_kind(nt, sub) == NK_ElseNode) {
        int ts = nt_ref(nt, last, "statements"), es = nt_ref(nt, sub, "statements");
        int tn3 = 0; const int *tb3 = ts >= 0 ? nt_arr(nt, ts, "body", &tn3) : NULL;
        int en3 = 0; const int *eb3 = es >= 0 ? nt_arr(nt, es, "body", &en3) : NULL;
        TyKind et = en3 > 0 ? infer_type(c, eb3[en3 - 1]) : TY_NIL;
        if (et != TY_UNKNOWN && sc->ret_noblock != et) sc->ret_noblock = et;
        /* a block arm ending in `yield` is typed per call site by
           method_call_ret, as a yield-tailed body is; any other block arm
           types the method alone, even while it is still unknown: letting
           the unified body type stand in would hand a call with a block the
           else arm's Enumerator on the round before the memo settles, and a
           local only widens from there */
        if (!(tn3 > 0 && nt_kind(nt, tb3[tn3 - 1]) == NK_YieldNode)) {
          TyKind bt = tn3 > 0 ? infer_type(c, tb3[tn3 - 1]) : TY_NIL;
          r = bt != TY_UNKNOWN && has_ret && has_ret[s] ? ty_unify(bt, ret_acc[s]) : bt;
          goto ret_decided;
        }
      }
    }
    /* explicit returns within this scope (collected above) */
    if (!tail_unreachable && has_ret && has_ret[s]) {
      /* An empty `[]` / `{}` value is still untyped, and the unify took the
         other values' type for it: `return 1 if b; {}` returned the hash
         pointer through an sp_int. Beside a value of another kind it is a
         container all the same, so the method boxes (the if/else rule). */
      int tk = empty_body ? 0 : an_empty_container_kind(c, sc->body);
      if (r == TY_UNKNOWN && an_empty_container_disagrees(tk, ret_acc[s])) r = TY_POLY;
      r = ty_unify(r, ret_acc[s]);
    }
    if (has_ret && has_ret[s] && ret_empty && ret_empty[s] &&
        (an_empty_container_disagrees(ret_empty[s] & 1, r) ||
         an_empty_container_disagrees(ret_empty[s] & 2, r)))
      r = TY_POLY;
    ret_decided:
    /* A value a caller's parameter stores elements of another kind into is
       the general Array (widen_array_sources); its values re-derive their
       typed kinds, which unify to the poly scalar when they differ. One that
       stops being an array at all loses the pin. */
    if (sc->ret_poly_array_pin) {
      if (r == TY_UNKNOWN || r == TY_POLY || (ty_is_array(r) && !ty_is_ptr_array(r))) r = TY_POLY_ARRAY;
      else sc->ret_poly_array_pin = 0;
    }
    /* Post-backstop re-runs fill returns whose body only settled after the
       main fixpoint (a `r = expr; r` chain, #1670). Adopting a NEW poly there
       is a net loss: the late-settling chains that matter are scalar, while a
       previously-UNKNOWN return deriving poly is typically a store-style
       method whose value no caller reads -- boxing it puts an sp_RbVal
       return in optcarrot's hottest poke path for ~4% fps. Keep those at
       their pre-pass type; the main fixpoint still widens to poly freely. */
    if (g_ret_no_new_poly == 1 && r == TY_POLY && sc->ret != TY_POLY) continue;
    /* At 2 (the late ivar-widening re-run) ONE transition is taken: a return
       that had a concrete type follows its body to poly, because the ivar the
       body answers widened after the return was derived and a String reading
       over a poly value does not build (#4451). Everything else is left where
       the earlier, gated re-runs settled it. */
    if (g_ret_no_new_poly == 2 &&
        !(r == TY_POLY && sc->ret != TY_POLY && sc->ret != TY_UNKNOWN && sc->ret != TY_VOID && sc->ret != TY_NIL)) continue;
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
    /* A tail that nothing resolves derives UNKNOWN every round. The arm at the
       end of this pass owns that case -- it gives such a method POLY when a
       caller reads its value -- and its own guard then skips the method. Undo
       that here and the two take turns to the fixpoint's cap (#4116). Narrow
       on purpose: only an established POLY, and only for the shape that arm
       claims, so a return that re-derives UNKNOWN for any other reason still
       corrects downward. */
    if (r == TY_UNKNOWN && sc->ret == TY_POLY && scope_tail_unresolved_call(c, s)) continue;
    if (r != sc->ret) {
      if (ty_degraded(r) && !ty_degraded(sc->ret)) {
        /* the value that degraded the return: the body's tail if its type
           did, else the first explicit return whose value did, else (two
           concrete kinds that disagree) the tail, with the first return as
           the other side */
        int node = -1, other = -1; TyKind then = TY_UNKNOWN;
        int tail = (!empty_body && !tail_unreachable && sc->body >= 0) ? sc->body : -1;
        if (tail >= 0 && tail < c->node_cap && ty_degraded(c->ntype[tail])) { node = tail; then = c->ntype[tail]; }
        else if (ret_head && ret_next) {
          for (int rid = ret_head[s]; rid >= 0; rid = ret_next[rid]) {
            TyKind rt = return_node_type(c, rid);
            if (ty_degraded(rt)) { node = return_value_node(c, rid); then = rt; break; }
          }
        }
        if (node < 0) {
          /* two concrete kinds met. One side is the tail when it has a type
             at this point, else the first typed `return`; the other is the
             first `return` of a different kind (the list's head can be one
             of the same kind, which said "String, where a `return` gives
             String"; String and its mutable refinement are one kind). */
          TyKind tt = (tail >= 0 && tail < c->node_cap) ? c->ntype[tail] : TY_UNKNOWN;
          if (tt != TY_UNKNOWN) { node = tail; then = tt; }
          if (ret_head)
            for (int rid = ret_head[s]; rid >= 0; rid = ret_next[rid]) {
              TyKind rt = return_node_type(c, rid);
              if (rt == TY_UNKNOWN) continue;
              if (node < 0) { node = return_value_node(c, rid); then = rt; continue; }
              int same = rt == then || ((rt == TY_STRING || rt == TY_STRBUF) && (then == TY_STRING || then == TY_STRBUF));
              if (!same) { other = return_value_node(c, rid); break; }
            }
          if (node < 0) { node = tail; then = r; }
        }
        sc->ret_why.node = node; sc->ret_why.other = other; sc->ret_why.prev = sc->ret;
        sc->ret_why.then = then; sc->ret_why.round = g_infer_round;
      }
      else if (!ty_degraded(r)) why_reset(&sc->ret_why);
      sc->ret = r; changed = 1;
    }
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
    if (sc->ret_oa_pin != TY_UNKNOWN) continue;
    if (!scope_tail_raises(c, s)) continue;
    if (rn_nscopes != c->nscopes) rn_build(c);
    TyKind unified = TY_VOID;
    int use_idx = rn_buckets > 0;
    int t = use_idx ? rn_head[sp_strhash(sc->name) % (unsigned)rn_buckets] : 1;
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

  /* A method whose body ends in a call nothing resolves infers a void return:
     codegen turns that tail into a NoMethodError raise, so the value is never
     produced. But a caller that READS the value (`@text = j.text`) still needs
     a typed result, and void gave it none -- the emitted C assigned a void
     expression. Type those returns poly; the value is unreachable either way. */
  {
    char *stmt_pos = NULL;
    for (int s = 1; s < c->nscopes; s++) {
      Scope *sc = &c->scopes[s];
      if (!sc->name || (sc->ret != TY_VOID && sc->ret != TY_UNKNOWN)) continue;
      if (sc->ret_specialized || sc->ret_rbs_seeded || sc->cs_synth) continue;
      if (sc->ret_oa_pin != TY_UNKNOWN) continue;
      if (!scope_tail_unresolved_call(c, s)) continue;
      if (!stmt_pos) {
        stmt_pos = (char *)calloc((size_t)c->nt->count, 1);
        if (!stmt_pos) break;
        for (int n = 0; n < c->nt->count; n++) {
          if (nt_kind(c->nt, n) != NK_StatementsNode) continue;
          int bn2 = 0; const int *bb2 = nt_arr(c->nt, n, "body", &bn2);
          for (int k = 0; k < bn2; k++) if (bb2[k] >= 0) stmt_pos[bb2[k]] = 1;
        }
      }
      int value_used = 0;
      for (int n = 0; n < c->nt->count && !value_used; n++) {
        if (stmt_pos[n]) continue;
        const char *nty = nt_type(c->nt, n);
        if (!nty || !sp_streq(nty, "CallNode")) continue;
        const char *nnm = nt_str(c->nt, n, "name");
        if (nnm && sp_streq(nnm, sc->name)) value_used = 1;
      }
      /* Only when it moves. This assigned and reported unconditionally, so a
         return already settled on POLY answered "changed" every round and the
         fixpoint ran to its cap (#4116). */
      if (value_used && sc->ret != TY_POLY) { sc->ret = TY_POLY; changed = 1; }
    }
    free(stmt_pos);
  }

  /* A return narrowed to a pointer array keeps that decision across this pass,
     for the reason the local pin is re-asserted in infer_write_types: the body
     still reads the poly array, and re-deriving from it hands back the plain
     poly SCALAR. Reported as no change, so the fixpoint can still settle. */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (sc->ret_oa_pin == TY_UNKNOWN) continue;
    if (sc->ret == TY_POLY_ARRAY || sc->ret == sc->ret_oa_pin) sc->ret = sc->ret_oa_pin;
    else sc->ret_oa_pin = TY_UNKNOWN;
  }

  free(ret_acc); free(has_ret); free(ret_empty); free(ret_head); free(ret_next); free(ret_narrow);
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
  /* a multiple assignment's attribute or index target calls its writer */
  else if (sp_streq(ty, "CallTargetNode")) nm = nt_str(nt, id, "name");
  else if (sp_streq(ty, "IndexTargetNode")) nm = "[]=";
  else {
    size_t tl = strlen(ty);
    if (tl > 17 && (sp_streq(ty + tl - 17, "OperatorWriteNode")))
      nm = nt_str(nt, id, "binary_operator");
  }
  /* `r[k] op= v`, `r[k] ||= v` and `r[k] &&= v` read and write the element:
     a receiver with its own [] / []= is called for both */
  const char *names[3] = { nm, NULL, NULL };
  int k0 = nt_kind(nt, id);
  char resolved[32];
  if (k0 == NK_CallNode && nm) {
    int r = nt_ref(nt, id, "receiver");
    NodeKind rk = r >= 0 ? nt_kind(nt, r) : NK_NONE;
    const char *cn = (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) ? nt_str(nt, r, "name") : NULL;
    int k = cn ? comp_class_index(c, cn) : -1;
    int mi = k >= 0 ? comp_cmethod_in_chain(c, k, nm, NULL) : -1;
    if (mi >= 0) { snprintf(resolved, sizeof resolved, "\x01%d", mi); names[0] = resolved; }
  }
  char arm_name[300];
  if (k0 == NK_CallNode && nm && names[0] == nm && nt_int(nt, id, "dyn_arm", 0) > 0) {
    snprintf(arm_name, sizeof arm_name, "\x02%s", nm);
    names[0] = arm_name;
  }
  /* `K.new(...).m`: Class#new answers an instance of K (initialize cannot
     change that), so the call reaches an instance method and never a class
     method of the same name. Marking it by bare name kept every `def self.m`
     alive -- raylib's `Color.new.set(...)` resurrected the never-called
     `def self.set` whose `self[:r] = ...` is a provable NoMethodError. Only
     when K is one of the program's classes with no class-side `new` of its
     own (a user `self.new` may answer anything). */
  char inst_name[300];
  if (k0 == NK_CallNode && nm && names[0] == nm) {
    int r = nt_ref(nt, id, "receiver");
    if (r >= 0 && nt_kind(nt, r) == NK_CallNode && nt_str(nt, r, "name") &&
        sp_streq(nt_str(nt, r, "name"), "new")) {
      int rr = nt_ref(nt, r, "receiver");
      NodeKind rrk = rr >= 0 ? nt_kind(nt, rr) : NK_NONE;
      const char *kn = (rrk == NK_ConstantReadNode || rrk == NK_ConstantPathNode) ? nt_str(nt, rr, "name") : NULL;
      int k = kn ? comp_class_index(c, kn) : -1;
      /* Class.new / Module.new / Struct.new / Data answer a new CLASS, whose
         class methods include the ones it inherits */
      if (k >= 0 && !comp_class_is_module(c, &c->classes[k]) &&
          !sp_streq(kn, "Class") && !sp_streq(kn, "Module") &&
          !sp_streq(kn, "Struct") && !sp_streq(kn, "Data") &&
          comp_cmethod_in_chain(c, k, "new", NULL) < 0) {
        snprintf(inst_name, sizeof inst_name, "\x03%s", nm);
        names[0] = inst_name;
      }
    }
  }
  if (k0 == NK_IndexOperatorWriteNode || k0 == NK_IndexOrWriteNode || k0 == NK_IndexAndWriteNode) {
    names[1] = "[]"; names[2] = "[]=";
  }
  for (int j = 0; j < 3; j++) {
    if (!names[j]) continue;
    int found = 0;
    for (int i = 0; i < *n; i++) if (sp_streq((*out)[i], names[j])) { found = 1; break; }
    if (!found) {
      if (*n >= *cap) { *cap = *cap ? *cap * 2 : 8; *out = realloc(*out, sizeof(char *) * (size_t)*cap); }
      (*out)[(*n)++] = strdup(names[j]);
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
    if (masgn_tuple_rhs(nt, v))
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

/* Does `var`'s value flow into `target` through the assignment pairs?
   Breadth-first with a visited set, capped at the same 10 hops the old
   depth-first walk allowed. The answers are identical -- a path that
   repeats a variable can always be shortened, so "some path of at most 10
   hops" is "shortest path of at most 10 hops" -- but the old walk re-ran
   the whole branch fan-out at every hop with no memory of where it had
   been, and a machine-generated loop body dense with `a = b` rows made
   that exponential: one 53k-line program spent 45% of its whole analyze
   in this function. */
static int bi_reaches(const BiPair *pairs, int np, const char *var, const char *target, int depth) {
  (void)depth;
  if (sp_streq(var, target)) return 1;
  if (np <= 0) return 0;
  const char *frontier[BI_MAX_PAIRS + 1];
  unsigned char seen[BI_MAX_PAIRS];
  int nf = 0, nseen = 0;
  const char *seen_names[BI_MAX_PAIRS + 1];
  frontier[nf++] = var;
  seen_names[nseen++] = var;
  memset(seen, 0, sizeof seen[0] * (size_t)np);
  /* 11 hops: the old walk tested the name BEFORE its depth cutoff, so a
     target 11 edges out was still found */
  for (int hop = 0; hop < 11 && nf > 0; hop++) {
    const char *next[BI_MAX_PAIRS + 1];
    int nn = 0;
    for (int f = 0; f < nf; f++) {
      for (int i = 0; i < np; i++) {
        if (seen[i] || !sp_streq(pairs[i].src, frontier[f])) continue;
        seen[i] = 1;   /* each pair contributes its dst once */
        int dup = 0;
        for (int k = 0; k < nseen && !dup; k++)
          if (sp_streq(seen_names[k], pairs[i].dst)) dup = 1;
        if (dup) continue;
        if (sp_streq(pairs[i].dst, target)) return 1;
        if (nseen <= BI_MAX_PAIRS) seen_names[nseen++] = pairs[i].dst;
        if (nn <= BI_MAX_PAIRS) next[nn++] = pairs[i].dst;
      }
    }
    memcpy(frontier, next, sizeof next[0] * (size_t)nn);
    nf = nn;
  }
  return 0;
}

static void bi_promote(Compiler *c, int write_id, const char *lname) {
  Scope *s = comp_scope_of(c, write_id);
  LocalVar *lv = s ? scope_local(s, lname) : NULL;
  if (!lv || lv->rbs_seeded) return;
  /* A parameter is typed from its call sites; one nothing has bound carries
     no evidence that the loop's `x = x + x` is integer growth at all. Such
     a parameter belongs to a method reached only through `method(:name)`
     and Method#call, whose arguments arrive boxed and may be Floats: widened
     to Bignum here, the thunk refused the Float (#4695). A parameter
     bound int from a call site still widens; a body local derives its type
     from the body the scan is reading and widens from unknown as before. */
  if (lv->is_param && lv->type == TY_UNKNOWN) return;
  if (lv->type == TY_UNKNOWN || lv->type == TY_INT) lv->type = TY_BIGINT;
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
