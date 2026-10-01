#include "analyze_internal.h"
#include <stdio.h>
#include <stdlib.h>

/* Debug: trace a single ivar's type transitions. Gated by SP_IVWATCH=<name>
   (bare name, no @). Zero-cost when the env var is unset. */
void sp_ivwatch(const char *name, const char *where, TyKind old, TyKind nw) {
  if (old == nw || !name) return;
  static const char *want = NULL;
  static int inited = 0;
  if (!inited) { want = getenv("SP_IVWATCH"); inited = 1; }
  if (!want) return;
  if (name[0] == '@') name++;        /* match with or without leading @ */
  if (!sp_streq(want, name)) return;
  fprintf(stderr, "[ivwatch %s] %-28s %d(%s) -> %d(%s)\n",
          name, where, (int)old, ty_name(old < 1000 ? old : TY_POLY),
          (int)nw, ty_name(nw < 1000 ? nw : TY_POLY));
}

/* `...` forwards the caller's args verbatim, so rather than a rest array we
   synthesize concrete positional params whose count is the widest positional
   arg count across this method's call sites (the compiler already knows the
   args it receives). Returns that count. Matches call sites by name -- the
   common free-function / single-definition forwarding case (#1288). */
static int forwarding_call_arity(Compiler *c, const char *mname) {
  const NodeTable *nt = c->nt;
  int maxarg = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *cn = nt_str(nt, id, "name");
    if (!cn || !sp_streq(cn, mname)) continue;
    int a = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (an == 0) continue;
    /* a `foo(...)` forwarding call is not a concrete arg count */
    if (an == 1 && nt_type(nt, av[0]) && sp_streq(nt_type(nt, av[0]), "ForwardingArgumentsNode")) continue;
    int pos = an;
    if (an > 0 && nt_type(nt, av[an - 1]) && sp_streq(nt_type(nt, av[an - 1]), "KeywordHashNode")) pos = an - 1;
    if (pos > maxarg) maxarg = pos;
  }
  return maxarg;
}

static void add_kwhash_key_params(const NodeTable *nt, Scope *s, int kwh) {
  int en = 0; const int *els = nt_arr(nt, kwh, "elements", &en);
  for (int e = 0; e < en; e++) {
    int key = nt_ref(nt, els[e], "key");
    const char *kty = key >= 0 ? nt_type(nt, key) : NULL;
    const char *kn = (kty && sp_streq(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
    if (!kn) continue;
    int dup = 0;
    for (int p = 0; p < s->nparams; p++) if (s->pnames[p] && sp_streq(s->pnames[p], kn)) { dup = 1; break; }
    if (!dup) scope_add_param(s, kn, -1);
  }
}

void collect_def_params(Compiler *c, int def_id, Scope *s) {
  int pn = nt_ref(c->nt, def_id, "parameters");
  if (pn < 0) return;
  int rn = 0;
  const int *reqs = nt_arr(c->nt, pn, "requireds", &rn);
  for (int i = 0; i < rn; i++) {
    const char *pname = nt_str(c->nt, reqs[i], "name");
    if (pname) scope_add_param(s, pname, -1);
  }
  int on = 0;
  const int *opts = nt_arr(c->nt, pn, "optionals", &on);
  for (int i = 0; i < on; i++) {
    const char *pname = nt_str(c->nt, opts[i], "name");
    int dv = nt_ref(c->nt, opts[i], "value");
    if (pname) scope_add_param(s, pname, dv);
  }
  int rp = nt_ref(c->nt, pn, "rest");
  if (rp >= 0) {
    const char *rpty = nt_type(c->nt, rp);
    if (rpty && sp_streq(rpty, "RestParameterNode")) {
      const char *rname = nt_str(c->nt, rp, "name");
      /* An anonymous `*` (Ruby 3.0 `def m(a, *) = f(a, *)`) has no name; give it
         a synthetic one so it is a real rest local, and the anonymous `*` at the
         forwarding call site resolves to it (the same name-independent model the
         anonymous `&` block forward uses). */
      if (!rname) rname = "__anon_rest";
      if (rname) {
        if (s->nparams % 8 == 0) {
          s->pnames  = realloc(s->pnames,  sizeof(char *) * (size_t)(s->nparams + 8));
          s->pdefault = realloc(s->pdefault, sizeof(int)    * (size_t)(s->nparams + 8));
        }
        s->pdefault[s->nparams] = -1;
        s->pnames[s->nparams++] = strdup(rname);
        LocalVar *lv = scope_local_intern(s, rname);
        lv->is_param = 1;
        slot_rule(c, lv, TY_POLY_ARRAY, -1, "by construction: a splat parameter holds the extra arguments of every call, untyped");
        s->rest_idx = s->nparams - 1;
      }
    }
  }
  /* post-splat required parameters (Prism "posts" array) */
  int postn = 0;
  const int *posts = nt_arr(c->nt, pn, "posts", &postn);
  for (int i = 0; i < postn; i++) {
    const char *pname = nt_str(c->nt, posts[i], "name");
    if (pname) scope_add_param(s, pname, -1);
  }
  if (postn > 0) s->npost_rest = postn;
  int kn = 0;
  const int *kws = nt_arr(c->nt, pn, "keywords", &kn);
  for (int i = 0; i < kn; i++) {
    const char *pty = nt_type(c->nt, kws[i]);
    if (!pty) continue;
    const char *pname = nt_str(c->nt, kws[i], "name");
    int dv = sp_streq(pty, "OptionalKeywordParameterNode") ? nt_ref(c->nt, kws[i], "value") : -1;
    if (pname) scope_add_param(s, pname, dv);
  }
  int kwrp = nt_ref(c->nt, pn, "keyword_rest");
  if (kwrp >= 0) {
    const char *kwrpty = nt_type(c->nt, kwrp);
    if (kwrpty && sp_streq(kwrpty, "KeywordRestParameterNode")) {
      const char *kwrname = nt_str(c->nt, kwrp, "name");
      /* An anonymous `**` (`def m(**) = f(**)`) has no name; give it a synthetic
         one so it is a real kwrest local that the anonymous `**` at the forwarding
         call site resolves to (mirrors __anon_rest for positional `*`). */
      if (!kwrname) kwrname = "__anon_kwrest";
      if (kwrname) {
        LocalVar *lv = scope_local_intern(s, kwrname);
        lv->is_param = 1;
        lv->type = TY_SYM_POLY_HASH;
        if (s->nparams % 8 == 0) {
          s->pnames   = realloc(s->pnames,   sizeof(char *) * (size_t)(s->nparams + 8));
          s->pdefault = realloc(s->pdefault, sizeof(int)    * (size_t)(s->nparams + 8));
        }
        s->pdefault[s->nparams] = -1;
        s->pnames[s->nparams++] = strdup(kwrname);
        s->kwrest_idx = s->nparams - 1;
      }
    }
  }
  int bp = nt_ref(c->nt, pn, "block");
  if (bp >= 0 && nt_type(c->nt, bp) && sp_streq(nt_type(c->nt, bp), "BlockParameterNode")) {
    const char *bn = nt_str(c->nt, bp, "name");
    s->blk_param = strdup(bn ? bn : "");
    /* Register the &block param as a local so mark_proc_captures can find it
       and mark it is_cell when a nested proc body captures it. */
    if (bn && bn[0]) {
      LocalVar *blv = scope_local_intern(s, bn);
      blv->is_param = 1;
      blv->type = TY_PROC;
    }
  }
  /* `def foo(...)`: Prism attaches a ForwardingParameterNode as keyword_rest.
     Synthesize concrete positional params __fwd_0.. (arity from the call
     sites); their types fall out of the normal call-site param seeding and a
     `bar(...)` body forwards them directly -- no rest/splat machinery (#1288). */
  {
    int kwr = nt_ref(c->nt, pn, "keyword_rest");
    if (kwr >= 0 && nt_type(c->nt, kwr) &&
        sp_streq(nt_type(c->nt, kwr), "ForwardingParameterNode") && s->name) {
      /* Concrete leading params (`def f(a, ...)`) consume the first call args;
         only the remainder is forwarded, so synthesize one __fwd_ slot per
         forwarded arg, not per total arg. The zero-leading case (`def f(...)`)
         is unchanged (fwd_base == 0). */
      int fwd_base = s->nparams;
      int arity = forwarding_call_arity(c, s->name);
      int nfwd = arity - fwd_base; if (nfwd < 0) nfwd = 0;
      for (int i = 0; i < nfwd; i++) {
        char nm[24]; snprintf(nm, sizeof nm, "__fwd_%d", i);
        scope_add_param(s, nm, -1);
      }
      /* Keyword args ride the same model: spinel compiles keyword params as
         positional C params mapped at the call site by name, so synthesizing a
         param named after each forwarded key lets the positional forward carry
         it (#1288). Collect the union of keys across the call sites. */
      const NodeTable *nt = c->nt;
      for (int id = 0; id < nt->count; id++) {
        const char *ty = nt_type(nt, id);
        if (!ty || !sp_streq(ty, "CallNode") || !nt_str(nt, id, "name") ||
            !sp_streq(nt_str(nt, id, "name"), s->name)) continue;
        int a = nt_ref(nt, id, "arguments");
        int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
        if (an == 0 || !nt_type(nt, av[an - 1]) ||
            !sp_streq(nt_type(nt, av[an - 1]), "KeywordHashNode")) continue;
        add_kwhash_key_params(nt, s, av[an - 1]);
      }
    }
  }
}

/* True if `s` is a `def m(...)` forwarding method (keyword_rest is a
   ForwardingParameterNode). */
static int scope_is_forwarding(Compiler *c, Scope *s) {
  if (!s || s->def_node < 0) return 0;
  int pn = nt_ref(c->nt, s->def_node, "parameters");
  if (pn < 0) return 0;
  int kwr = nt_ref(c->nt, pn, "keyword_rest");
  return kwr >= 0 && nt_type(c->nt, kwr) &&
         sp_streq(nt_type(c->nt, kwr), "ForwardingParameterNode");
}

/* The method `s`'s body forwards `...` to (a `callee(...)` call, `super(...)`
   or a bare `super`). Returns the callee scope index of the first such call
   that resolves, or -1 if none does. `*sole`, when asked, says whether every
   forwarding call resolves, and to that one scope. */
static int forwarding_target_scan(Compiler *c, Scope *s, int *sole) {
  const NodeTable *nt = c->nt;
  int first = -1, one = 1;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || comp_scope_of(c, id) != s) continue;
    int is_super = sp_streq(ty, "ForwardingSuperNode");
    if (!is_super) {
      if (!sp_streq(ty, "CallNode") && !sp_streq(ty, "SuperNode")) continue;
      int a = nt_ref(nt, id, "arguments");
      int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
      if (an != 1 || !nt_type(nt, av[0]) ||
          !sp_streq(nt_type(nt, av[0]), "ForwardingArgumentsNode")) continue;
      is_super = sp_streq(ty, "SuperNode");
    }
    int mi = -1;
    if (is_super) {
      if (s->class_id >= 0 && !s->is_cmethod && s->name) {
        int par = comp_super_parent(c, s->class_id, 0);
        mi = par >= 0 ? comp_method_in_chain(c, par, s->name, NULL) : -1;
      }
    }
    else {
      const char *cn = nt_str(nt, id, "name");
      /* a receiver-qualified target is not resolved here */
      if (cn && nt_ref(nt, id, "receiver") < 0) {
        mi = comp_method_index(c, cn);
        if (mi < 0 && s->class_id >= 0) mi = comp_method_in_chain(c, s->class_id, cn, NULL);
        if (mi < 0 && s->class_id >= 0) mi = comp_cmethod_in_chain(c, s->class_id, cn, NULL);
      }
    }
    if (mi < 0 || (first >= 0 && mi != first)) one = 0;
    if (first < 0 && mi >= 0) first = mi;
  }
  if (sole) *sole = one && first >= 0;
  return first;
}
static int forwarding_target_idx(Compiler *c, Scope *s) {
  return forwarding_target_scan(c, s, NULL);
}

/* A `Klass.new(...)` call whose receiver names a class that constructs
   through `init` (its `initialize` resolves to that scope). */
static int new_site_reaches(Compiler *c, int id, int init) {
  const NodeTable *nt = c->nt;
  const char *cn = nt_str(nt, id, "name");
  if (!cn || !sp_streq(cn, "new")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  const char *rty = recv >= 0 ? nt_type(nt, recv) : NULL;
  if (!rty || (!sp_streq(rty, "ConstantReadNode") && !sp_streq(rty, "ConstantPathNode")))
    return 0;
  int cid = comp_class_index(c, nt_str(nt, recv, "name"));
  return cid >= 0 && comp_method_in_chain(c, cid, "initialize", NULL) == init;
}

/* `def initialize(...)` is reached through `Klass.new(...)`, never a call named
   `initialize`, so its forwarded params come from the `new` sites. */
static void initialize_forwarding_params(Compiler *c, int init) {
  Scope *s = &c->scopes[init];
  const NodeTable *nt = c->nt;
  int lead = 0;
  while (lead < s->nparams && (!s->pnames[lead] || strncmp(s->pnames[lead], "__fwd_", 6) != 0)) lead++;
  int nfwd = 0;
  for (int p = 0; p < s->nparams; p++)
    if (s->pnames[p] && strncmp(s->pnames[p], "__fwd_", 6) == 0) nfwd++;
  /* The synthesized parameters are as many as the widest `new` site passes,
     and a narrower site fills the rest with nil, which the forward then
     hands to the parent in place of a default it should have left alone
     (`B.new(1)` beside `B.new(1, 5)` into `initialize(a, b = 2)` gave
     [1, nil]); a splat has no count to size them by, and a parent *rest
     takes what remains. None of these has a right answer in this model, so
     they are refused rather than compiled wrong. */
  int pos_min = -1, pos_max = -1;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode") || !new_site_reaches(c, id, init)) continue;
    int a = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (an == 1 && nt_type(nt, av[0]) && sp_streq(nt_type(nt, av[0]), "ForwardingArgumentsNode")) continue;
    int kwh = an > 0 && nt_type(nt, av[an - 1]) &&
              sp_streq(nt_type(nt, av[an - 1]), "KeywordHashNode") ? av[an - 1] : -1;
    int pos = kwh >= 0 ? an - 1 : an;
    for (int k = 0; k < pos; k++)
      if (nt_kind(nt, av[k]) == NK_SplatNode)
        unsupported_feature(c, id, "a splat argument to `new` of a class whose initialize forwards `...`: "
                                   "the forward's parameters are sized from the call's argument count");
    if (pos_min < 0 || pos < pos_min) pos_min = pos;
    if (pos > pos_max) pos_max = pos;
    if (pos_min != pos_max)
      unsupported_feature(c, id, "`new` sites passing different numbers of arguments to a class whose "
                                 "initialize forwards `...`: a shorter call would pass nil where the parent "
                                 "expects its default");
    while (nfwd < pos - lead) {
      char nm[24]; snprintf(nm, sizeof nm, "__fwd_%d", nfwd++);
      scope_add_param(s, nm, -1);
    }
    if (kwh >= 0) add_kwhash_key_params(nt, s, kwh);
  }
  {
    int tgt = forwarding_target_idx(c, s);
    if (tgt >= 0 && tgt != init && c->scopes[tgt].rest_idx >= 0)
      unsupported_feature(c, s->def_node, "an initialize forwarding `...` to a parent initialize with a *rest "
                                          "parameter: the forward's fixed parameters cannot fill a rest");
  }
}

/* Chained `...`: a forwarding method called only via another `f(...)` forward
   has no concrete call site, so its call-site arity is 0. Top its synthesized
   positional params up to its forwarding target's arity, to a fixpoint, so
   `def h(...); f(...); end; def f(...); g(a,b); end` propagates g's arity back
   through f and h (#1288). */
void topup_forwarding_arity(Compiler *c) {
  for (int s = 1; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (sc->name && sp_streq(sc->name, "initialize") && !sc->is_cmethod &&
        sc->class_id >= 0 && scope_is_forwarding(c, sc))
      initialize_forwarding_params(c, s);
  }
  int changed = 1;
  for (int iter = 0; iter < 32 && changed; iter++) {
    changed = 0;
    for (int s = 1; s < c->nscopes; s++) {
      Scope *sc = &c->scopes[s];
      if (!scope_is_forwarding(c, sc)) continue;
      int sole = 0;
      int tgt = forwarding_target_scan(c, sc, &sole);
      if (tgt < 0 || tgt == s) continue;
      /* a forwarder whose every `...` reaches one method keeps its keyword
         policy (scope_refuses_keywords); one that picks among several
         cannot say which will take the keywords, so none is recorded:
         `def w(c, ...) = c ? m(...) : n(...)` must not refuse `n`'s
         keywords because `m` says `**nil` */
      sc->fwd_target1 = sole ? tgt + 1 : 0;
      int want = c->scopes[tgt].nparams;
      while (sc->nparams < want) {
        char nm[24]; snprintf(nm, sizeof nm, "__fwd_%d", sc->nparams);
        scope_add_param(sc, nm, -1);
        changed = 1;
      }
    }
  }
}

/* Is node v's value used? A statement list, a branch, a begin and a rescue
   pass it on to whatever holds them. A statement before a list's last, a
   loop body, an ensure clause, a method body and the block of an iterator
   that ignores its block's value throw it away. */
static int super_value_used(const NodeTable *nt, const int *par, int v) {
  for (int p; (p = par[v]) >= 0; v = p) {
    switch (nt_kind(nt, p)) {
    case NK_StatementsNode: {
      int n = 0;
      const int *st = nt_arr(nt, p, "body", &n);
      if (n == 0 || st[n - 1] != v) return 0;
      break;
    }
    case NK_IfNode: case NK_UnlessNode: case NK_CaseNode: case NK_CaseMatchNode:
      if (nt_ref(nt, p, "predicate") == v) return 1;
      break;
    case NK_WhileNode: case NK_UntilNode:
      return nt_ref(nt, p, "predicate") == v;
    case NK_InNode: case NK_RescueNode:
      if (nt_ref(nt, p, "statements") != v && nt_ref(nt, p, "subsequent") != v) return 1;
      break;
    case NK_ElseNode: case NK_ParenthesesNode: case NK_BeginNode:
      break;
    case NK_DefNode:
      return 0;
    case NK_ReturnNode:
      /* `return super` leaves initialize, whose value `new` drops; only a
         lambda's return hands the value on. */
      for (int q = par[p]; q >= 0; q = par[q]) {
        if (nt_kind(nt, q) == NK_LambdaNode) return 1;
        if (nt_kind(nt, q) == NK_DefNode) return 0;
      }
      return 0;
    case NK_BlockNode: {
      int call = par[p];
      const char *cn = call >= 0 && nt_kind(nt, call) == NK_CallNode ? nt_str(nt, call, "name") : NULL;
      static const char *const it[] = { "each", "each_with_index", "each_index", "times", "upto",
                                        "downto", "step", "loop", "each_slice", "each_cons",
                                        "reverse_each", "tap", NULL };
      for (int k = 0; cn && it[k]; k++)
        if (sp_streq(cn, it[k])) return 0;
      return 1;
    }
    default: {
      const char *ty = nt_type(nt, p);
      if (ty && sp_streq(ty, "EnsureNode")) return 0;
      if (ty && sp_streq(ty, "WhenNode") && nt_ref(nt, p, "statements") == v) break;
      if (ty && sp_streq(ty, "ArgumentsNode") && par[p] >= 0 && nt_kind(nt, par[p]) == NK_ReturnNode) {
        int na = 0;
        nt_arr(nt, p, "arguments", &na);
        if (na == 1) break;   /* `return super`; `return super, x` builds an Array of it */
      }
      return 1;
    }
    }
  }
  return 0;
}

/* initialize is emitted as a void C function (method_is_void), so `super`
   in a subclass's initialize has no value to give when the parent's
   initialize is one the program wrote: `c = super` assigned the void call.
   CRuby answers the parent's last value. A super whose value is used --
   `c = super`, but also the last statement of a branch or a begin whose
   value is used -- is refused rather than compiled into C that does not
   build. */
void refuse_super_init_value(Compiler *c) {
  const NodeTable *nt = c->nt;
  int *par = NULL;
  static const NodeKind kinds[] = { NK_SuperNode, NK_ForwardingSuperNode };
  for (int k = 0; k < 2; k++) {
    int cnt = 0;
    const int *ids = nt_nodes_of_kind(nt, kinds[k], &cnt);
    for (int i = 0; i < cnt; i++) {
      int v = ids[i];
      Scope *s = comp_scope_of(c, v);
      if (!s || s->is_cmethod || s->class_id < 0 || !s->name || !sp_streq(s->name, "initialize")) continue;
      int pc = comp_super_parent(c, s->class_id, 0);
      if (pc < 0 || comp_method_in_chain(c, pc, "initialize", NULL) < 0) continue;
      if (!par) {   /* each node's parent, built once a candidate turns up */
        par = malloc(sizeof(int) * (size_t)(nt->count > 0 ? nt->count : 1));
        for (int n = 0; n < nt->count; n++) par[n] = -1;
        for (int p = 0; p < nt->count; p++) {
          int nr = nt_num_refs(nt, p), na = nt_num_arrs(nt, p);
          for (int r = 0; r < nr + na; r++) {
            int n = 1, one = r < nr ? nt_ref_at(nt, p, r) : -1;
            const int *kids = r < nr ? &one : nt_arr_at(nt, p, r - nr, &n);
            for (int j = 0; kids && j < n; j++)
              if (kids[j] >= 0 && kids[j] < nt->count) par[kids[j]] = p;
          }
        }
      }
      if (!super_value_used(nt, par, v)) continue;
      unsupported_feature(c, v, "unsupported value of `super` in initialize: initialize is compiled to "
                                "return nothing, so the parent's last value is not there to answer "
                                "(see docs/limitations.md)");
    }
  }
  free(par);
}

/* `super(...)` in a Struct or Data initialize reaches the built-in one that
   sets the members, which has no method scope to forward into. Spell the
   forward out from the synthesized params, `super(__fwd_0, .., k: k)`, so it
   sets the members as the same explicit super would. */
void expand_struct_forwarding_super(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_SuperNode) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an == 0 || !nt_type(nt, av[an - 1]) ||
        !sp_streq(nt_type(nt, av[an - 1]), "ForwardingArgumentsNode")) continue;
    int si = id < c->node_cap ? c->nscope[id] : -1;
    if (si <= 0 || si >= c->nscopes) continue;
    Scope *s = &c->scopes[si];
    if (s->is_cmethod || s->class_id < 0 || !s->name || !sp_streq(s->name, "initialize") ||
        !scope_is_forwarding(c, s)) continue;
    ClassInfo *cls = &c->classes[s->class_id];
    if (!cls->is_struct && !cls->is_data) continue;
    if (cls->parent >= 0 && comp_method_in_chain(c, cls->parent, "initialize", NULL) >= 0) continue;
    int pn = nt_ref(nt, s->def_node, "parameters");
    int nreq = 0, nopt = 0;
    nt_arr(nt, pn, "requireds", &nreq);
    nt_arr(nt, pn, "optionals", &nopt);
    int first = nt->count;
    int *na = malloc(sizeof(int) * (size_t)(an + s->nparams + 1));
    int *kels = malloc(sizeof(int) * (size_t)(s->nparams + 1));
    if (!na || !kels) { free(na); free(kels); continue; }
    int nn = 0, nk = 0;
    for (int a = 0; a < an - 1; a++) na[nn++] = av[a];
    for (int p = nreq + nopt; p < s->nparams; p++) {
      const char *nm = s->pnames[p];
      if (!nm || p == s->rest_idx || p == s->kwrest_idx) continue;
      int lr = nt_new_node(nt, "LocalVariableReadNode");
      nt_node_set_str(nt, lr, "name", nm);
      nt_node_set_int(nt, lr, "depth", 0);
      if (strncmp(nm, "__fwd_", 6) == 0) { na[nn++] = lr; continue; }
      int sy = nt_new_node(nt, "SymbolNode");
      nt_node_set_str(nt, sy, "value", nm);
      int as = nt_new_node(nt, "AssocNode");
      nt_node_set_ref(nt, as, "key", sy);
      nt_node_set_ref(nt, as, "value", lr);
      kels[nk++] = as;
    }
    if (nk > 0) {
      int kwh = nt_new_node(nt, "KeywordHashNode");
      nt_node_set_arr(nt, kwh, "elements", kels, nk);
      na[nn++] = kwh;
    }
    nt_node_set_arr(nt, args, "arguments", na, nn);
    free(na); free(kels);
    comp_grow_node_arrays(c);
    for (int k = first; k < nt->count; k++) c->nscope[k] = si;
  }
}

void walk_scope(Compiler *c, int id, int scope_idx, int class_id);

/* String form of an int/string/symbol literal node, for compile-time
   `define_method` name interpolation. Returns malloc'd, or NULL. */
char *dm_lit_str(Compiler *c, int lit) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, lit);
  if (!ty) return NULL;
  if (sp_streq(ty, "IntegerNode")) {
    char buf[32]; snprintf(buf, sizeof buf, "%lld", (long long)nt_int(nt, lit, "value", 0));
    return strdup(buf);
  }
  if (sp_streq(ty, "StringNode")) {
    const char *s = nt_str(nt, lit, "content");
    if (!s) s = nt_str(nt, lit, "unescaped");
    return s ? strdup(s) : NULL;
  }
  if (sp_streq(ty, "SymbolNode")) { const char *s = nt_str(nt, lit, "value"); return s ? strdup(s) : NULL; }
  return NULL;
}

/* Evaluate a `define_method(<name-expr>)` name with the each-loop variable
   `bv` bound to literal `lit`. Handles string/symbol literals, a bare loop
   variable, and (interpolated) string/symbol nodes. Returns malloc'd name
   or NULL when not statically resolvable. */
char *dm_eval_name(Compiler *c, int node, const char *bv, int lit) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty) return NULL;
  if (sp_streq(ty, "StringNode")) {
    const char *s = nt_str(nt, node, "content");
    if (!s) s = nt_str(nt, node, "unescaped");
    return s ? strdup(s) : NULL;
  }
  if (sp_streq(ty, "SymbolNode")) { const char *s = nt_str(nt, node, "value"); return s ? strdup(s) : NULL; }
  if (sp_streq(ty, "LocalVariableReadNode")) {
    const char *nm = nt_str(nt, node, "name");
    if (nm && bv && sp_streq(nm, bv)) return dm_lit_str(c, lit);
    return NULL;
  }
  if (sp_streq(ty, "EmbeddedStatementsNode")) {
    int body = nt_ref(nt, node, "statements");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    if (bn != 1) return NULL;
    return dm_eval_name(c, bb[0], bv, lit);
  }
  if (sp_streq(ty, "InterpolatedStringNode") || sp_streq(ty, "InterpolatedSymbolNode")) {
    int pn = 0; const int *parts = nt_arr(nt, node, "parts", &pn);
    char *out = strdup("");
    for (int k = 0; k < pn; k++) {
      char *p = dm_eval_name(c, parts[k], bv, lit);
      if (!p) { free(out); return NULL; }
      size_t no = strlen(out) + strlen(p) + 1;
      char *merged = malloc(no); snprintf(merged, no, "%s%s", out, p);
      free(out); free(p); out = merged;
    }
    return out;
  }
  return NULL;
}

/* TyKind of an int/string/symbol literal node (for the unrolled method's
   subst-var type and return type). */
TyKind dm_lit_type(Compiler *c, int lit) {
  const char *ty = nt_type(c->nt, lit);
  if (!ty) return TY_UNKNOWN;
  if (sp_streq(ty, "IntegerNode")) return TY_INT;
  if (sp_streq(ty, "StringNode"))  return TY_STRING;
  if (sp_streq(ty, "SymbolNode"))  return TY_SYMBOL;
  return TY_UNKNOWN;
}

/* Detect `[lit, ...].each { |v| define_method("m_#{v}") { body } }` in a
   class body and synthesize one method scope per literal element, each with
   a compile-time substitution of `v`. Returns 1 if handled. */
int collect_dm_each_unroll(Compiler *c, int id, int class_id) {
  const NodeTable *nt = c->nt;
  if (class_id < 0) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "each")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "ArrayNode")) return 0;
  int blk = nt_ref(nt, id, "block");
  if (blk < 0) return 0;
  /* block parameter name */
  int pn = nt_ref(nt, blk, "parameters");
  int inner = pn >= 0 ? nt_ref(nt, pn, "parameters") : -1;
  int pnode = inner >= 0 ? inner : pn;
  int rnp = 0; const int *reqs = pnode >= 0 ? nt_arr(nt, pnode, "requireds", &rnp) : NULL;
  if (rnp < 1) return 0;
  const char *bv = nt_str(nt, reqs[0], "name");
  if (!bv) return 0;
  /* block body must be a single define_method call */
  int body = nt_ref(nt, blk, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn != 1) return 0;
  int dc = bb[0];
  if (!nt_type(nt, dc) || !sp_streq(nt_type(nt, dc), "CallNode")) return 0;
  const char *dcn = nt_str(nt, dc, "name");
  if (!dcn || !sp_streq(dcn, "define_method") || nt_ref(nt, dc, "receiver") >= 0) return 0;
  int dargs = nt_ref(nt, dc, "arguments");
  int dan = 0; const int *dav = dargs >= 0 ? nt_arr(nt, dargs, "arguments", &dan) : NULL;
  if (dan < 1) return 0;
  int dblk = nt_ref(nt, dc, "block");
  if (dblk < 0) return 0;
  int dbody = nt_ref(nt, dblk, "body");
  /* Keyword parameters are bound only by a DefNode's parameters (see
     desugar_define_method_keywords), which an unrolled scope does not
     have: a call's `k: 5` would bind the whole Hash to k. Leave those. */
  { int dpn0 = nt_ref(nt, dblk, "parameters");
    int dps0 = dpn0 >= 0 ? nt_ref(nt, dpn0, "parameters") : -1;
    int dkn0 = 0;
    if (dps0 >= 0) (void)nt_arr(nt, dps0, "keywords", &dkn0);
    if (dkn0 > 0) return 0; }
  /* iterate the array literal's elements */
  int en = 0; const int *elems = nt_arr(nt, recv, "elements", &en);
  if (en == 0) return 0;
  for (int k = 0; k < en; k++) {
    TyKind lt = dm_lit_type(c, elems[k]);
    if (lt == TY_UNKNOWN) return 0;  /* non-literal element: bail (unhandled) */
    char *mname = dm_eval_name(c, dav[0], bv, elems[k]);
    if (!mname) return 0;
    Scope *ms = comp_scope_new(c, mname, dc);
    free(mname);
    ms->body = dbody;
    ms->class_id = class_id;
    ms->dm_subst_name = strdup(bv);
    ms->dm_subst_node = elems[k];
    /* the define_method block's parameters are the method's, as for a
       literal name (`|*args|`, `|w, fill = "-"|`): left out, the body read
       parameters the method did not have */
    int dpn = nt_ref(nt, dblk, "parameters");
    if (dpn >= 0 && nt_kind(nt, dpn) == NK_BlockParametersNode) {
      collect_def_params(c, dpn, ms);
      int dps = nt_ref(nt, dpn, "parameters"), drn = 0;
      if (dps >= 0) (void)nt_arr(nt, dps, "requireds", &drn);
      ms->nrequired = drn;
    }
    /* the loop var reads inside the body resolve to the literal type */
    LocalVar *lv = scope_local_intern(ms, bv);
    lv->type = lt;
    lv->is_param = 1;  /* not a real C param, but keeps it out of decls */
    /* Walk the (shared) define_method body in this synthetic scope so its
       nodes get nscope attribution. The last element wins for the shared
       body nodes; that is fine since all elements share the value type. */
    int ms_idx = c->nscopes - 1;
    if (dbody >= 0) walk_scope(c, dbody, ms_idx, class_id);
  }
  return 1;
}

/* The class name a TyKind denotes (for `<x>.class` alias resolution). */
const char *builtin_class_of_type(TyKind t) {
  if (t == TY_INT || t == TY_BIGINT) return "Integer";
  if (t == TY_FLOAT) return "Float";
  if (t == TY_STRING) return "String";
  if (t == TY_SYMBOL) return "Symbol";
  if (t == TY_RANGE) return "Range";
  if (t == TY_TIME) return "Time";
  if (t == TY_IO) return "File";
  if (t == TY_CLASS) return "Class";
  if (t == TY_NIL) return "NilClass";
  if (ty_is_array(t)) return "Array";
  if (ty_is_hash(t)) return "Hash";
  return NULL;
}

/* If `cname` is a constant assigned a class value (`CONST = SomeClass` or
   `CONST = <expr>.class`), return the underlying class name so `class CONST`
   reopens that class. Returns NULL if `cname` is a plain new class name. */
/* Index of every ConstantWriteNode id, cached per node table. resolve_class_alias
   is called once per class/module definition during walk_scope; scanning all
   nodes each time made it O(class_defs * nodes) on a flattened runtime. Which
   nodes are ConstantWriteNodes is stable across the pass (only their names may
   have been rewritten earlier, and we re-read those fresh), so the id list can
   be built once and reused. Rebuilt if the node table (pointer or count)
   changes, e.g. a second compile in the same process. */
static const NodeTable *rca_nt = NULL;
static int *rca_ids = NULL;
static int rca_n = 0, rca_ntcount = -1;
const char *resolve_class_alias(Compiler *c, const char *cname) {
  const NodeTable *nt = c->nt;
  if (rca_nt != nt || rca_ntcount != nt->count) {
    free(rca_ids);
    rca_ids = malloc((size_t)nt->count * sizeof(int));
    rca_n = 0;
    if (rca_ids) {
      for (int id = 0; id < nt->count; id++) {
        const char *ty = nt_type(nt, id);
        if (ty && sp_streq(ty, "ConstantWriteNode")) rca_ids[rca_n++] = id;
      }
    }
    rca_nt = nt;
    rca_ntcount = nt->count;
  }
  for (int ii = 0; ii < rca_n; ii++) {
    int id = rca_ids[ii];
    const char *n = nt_str(nt, id, "name");
    if (!n || !sp_streq(n, cname)) continue;
    int v = nt_ref(nt, id, "value");
    if (v < 0) return NULL;
    const char *vty = nt_type(nt, v);
    if (vty && (sp_streq(vty, "ConstantReadNode") || sp_streq(vty, "ConstantPathNode"))) {
      const char *vn = nt_str(nt, v, "name");
      if (vn && (comp_class_index(c, vn) >= 0 || is_builtin_class_name(vn))) return vn;
    }
    if (vty && sp_streq(vty, "CallNode") && nt_str(nt, v, "name") &&
        sp_streq(nt_str(nt, v, "name"), "class")) {
      int r = nt_ref(nt, v, "receiver");
      if (r >= 0) return builtin_class_of_type(infer_type(c, r));
    }
    return NULL;
  }
  return NULL;
}

/* compiler_state_* class macros: declare a bag of typed instance variables
   and auto-synthesize init/dump/set methods.
   The CRuby shim that would define these via define_method is dead code under
   RUBY_ENGINE != "ruby"; spinel recognizes the macros natively here. */
const char *cs_macro_kind(const char *nm) {
  if (!nm) return NULL;
  if (sp_streq(nm, "compiler_state_int")) return "int";
  if (sp_streq(nm, "compiler_state_str")) return "str";
  if (sp_streq(nm, "compiler_state_sa"))  return "sa";
  if (sp_streq(nm, "compiler_state_ia"))  return "ia";
  return NULL;
}
static TyKind cs_field_type(const char *kind) {
  if (sp_streq(kind, "str")) return TY_STRING;
  if (sp_streq(kind, "sa"))  return TY_STR_ARRAY;
  if (sp_streq(kind, "ia"))  return TY_INT_ARRAY;
  return TY_INT;
}
/* CS_SYNTH_* markers; mirrored in codegen. */
enum { CS_INIT = 1, CS_DUMP, CS_SET_INT, CS_SET_STR, CS_SET_SA, CS_SET_IA };
static void cs_synth_method(Compiler *c, int class_id, int def_node, const char *name,
                            int cs_synth, TyKind ret, const char **pnames,
                            const TyKind *ptypes, int nparams) {
  for (int s = 0; s < c->nscopes; s++)
    if (c->scopes[s].class_id == class_id && c->scopes[s].name &&
        sp_streq(c->scopes[s].name, name)) return;  /* already present */
  Scope *s = comp_scope_new(c, name, def_node);
  s->class_id = class_id;
  s->cs_synth = cs_synth;
  s->ret = ret;
  s->body = -1;
  for (int i = 0; i < nparams; i++) {
    scope_add_param(s, pnames[i], -1);
    LocalVar *lv = scope_local(s, pnames[i]);
    if (lv) lv->type = ptypes[i];
  }
}
void collect_compiler_state(Compiler *c, int id, int class_id) {
  const NodeTable *nt = c->nt;
  const char *kind = cs_macro_kind(nt_str(nt, id, "name"));
  if (!kind || class_id < 0) return;
  ClassInfo *ci = &c->classes[class_id];
  /* synthesize the 6 methods once (on the first compiler_state_* decl) */
  { const char *p0[] = {"buf"}; TyKind t0[] = {TY_STRING};
    const char *p2i[] = {"name", "val"}; TyKind t2i[] = {TY_STRING, TY_INT};
    const char *p2s[] = {"name", "val"}; TyKind t2s[] = {TY_STRING, TY_STRING};
    const char *p2sa[] = {"name", "val"}; TyKind t2sa[] = {TY_STRING, TY_STR_ARRAY};
    const char *p2ia[] = {"name", "val"}; TyKind t2ia[] = {TY_STRING, TY_INT_ARRAY};
    cs_synth_method(c, class_id, id, "init_compiler_state", CS_INIT, TY_INT, NULL, NULL, 0);
    cs_synth_method(c, class_id, id, "dump_compiler_state_ir", CS_DUMP, TY_STRING, p0, t0, 1);
    cs_synth_method(c, class_id, id, "compiler_state_set_int", CS_SET_INT, TY_INT, p2i, t2i, 2);
    cs_synth_method(c, class_id, id, "compiler_state_set_str", CS_SET_STR, TY_INT, p2s, t2s, 2);
    cs_synth_method(c, class_id, id, "compiler_state_set_sa", CS_SET_SA, TY_INT, p2sa, t2sa, 2);
    cs_synth_method(c, class_id, id, "compiler_state_set_ia", CS_SET_IA, TY_INT, p2ia, t2ia, 2);
  }
  int args = nt_ref(nt, id, "arguments");
  int an = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  for (int a = 0; a < an; a++) {
    const char *aty = nt_type(nt, argv[a]);
    if (!aty || !sp_streq(aty, "SymbolNode")) continue;
    const char *fname = nt_str(nt, argv[a], "value");
    if (!fname) continue;
    char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", fname);
    int iv = comp_ivar_intern(ci, ivn);
    ci->ivar_types[iv] = cs_field_type(kind);
    if (ci->ncs >= ci->ccs) {
      ci->ccs = ci->ccs ? ci->ccs * 2 : 16;
      ci->cs_names = realloc(ci->cs_names, sizeof(char *) * (size_t)ci->ccs);
      ci->cs_kinds = realloc(ci->cs_kinds, sizeof(char *) * (size_t)ci->ccs);
    }
    ci->cs_names[ci->ncs] = strdup(fname);
    ci->cs_kinds[ci->ncs] = strdup(kind);
    ci->ncs++;
  }
}

/* The literal name a receiverless `define_method(:m, ...)` call defines, else NULL. */
static const char *dm_defined_name(const NodeTable *nt, int call) {
  const char *nm = nt_str(nt, call, "name");
  if (!nm || !sp_streq(nm, "define_method") || nt_ref(nt, call, "receiver") >= 0) return NULL;
  int args = nt_ref(nt, call, "arguments");
  int an = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an < 1) return NULL;
  const char *aty = nt_type(nt, argv[0]);
  if (aty && sp_streq(aty, "SymbolNode")) return nt_str(nt, argv[0], "value");
  if (aty && sp_streq(aty, "StringNode")) return nt_str(nt, argv[0], "content");
  return NULL;
}

/* If `id` is a receiverless `define_method(:lit) { }` that walk_scope will
   register as a method scope, return that literal method name; else NULL.
   walk_scope only registers when the name is a literal symbol/string AND a block
   is present, so this mirrors that exact gate. Keeping class_eval_reopen_class's
   purity test in lockstep with it prevents a `define_method` that the registrar
   silently skips (blockless, or a dynamic name) from making the block look like a
   pure reopen -- which would no-op the whole call and drop it without a diagnostic. */
static const char *dm_registerable_name(const NodeTable *nt, int id) {
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode") || nt_ref(nt, id, "block") < 0) return NULL;
  return dm_defined_name(nt, id);
}

/* `Klass.class_eval { ... }` / `Klass.module_eval { ... }` (and the bare/`self.`
   forms inside a class body) where the target is a known class and the block body
   is purely method definitions (`def` or a registerable `define_method`). Returns
   the target's class index, else -1.

   The receiver may be a constant (`Klass` / `M::Klass`), resolved by short name;
   or `self`/absent, which reopens `enclosing_class` -- the class whose body we are
   directly in (analyze passes g_cbody_direct, codegen passes g_class_body_id, both
   -1 inside method bodies). Restricting to definition-only blocks keeps a
   class_eval that runs other code falling through to the normal (unsupported) path
   instead of being silently dropped. Used by both analyze (to register the methods
   on the target) and codegen (to emit the call as a no-op). */
int class_eval_reopen_class(Compiler *c, int id, int enclosing_class) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode")) return -1;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || (!sp_streq(nm, "class_eval") && !sp_streq(nm, "module_eval") &&
              !sp_streq(nm, "class_exec") && !sp_streq(nm, "module_exec"))) return -1;
  int blk = nt_ref(nt, id, "block");
  if (blk < 0) return -1;
  int recv = nt_ref(nt, id, "receiver");
  const char *recv_ty = recv >= 0 ? nt_type(nt, recv) : NULL;
  int ci;
  if (recv < 0 || (recv_ty && sp_streq(recv_ty, "SelfNode"))) {
    /* bare / `self.` receiver reopens the enclosing class -- but only at
       class-body level, where `self` is the class object. */
    if (enclosing_class < 0) return -1;
    ci = enclosing_class;
  }
  else if (recv_ty && (sp_streq(recv_ty, "ConstantReadNode") ||
                         sp_streq(recv_ty, "ConstantPathNode"))) {
    const char *recv_name = nt_str(nt, recv, "name");
    if (!recv_name) return -1;
    ci = comp_class_index(c, recv_name);
    if (ci < 0) return -1;
  }
  else {
    return -1;
  }
  int body = nt_ref(nt, blk, "body");
  int n = 0; const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
  for (int k = 0; k < n; k++) {
    const char *sty = nt_type(nt, stmts[k]);
    if (sty && sp_streq(sty, "DefNode")) continue;
    if (dm_registerable_name(nt, stmts[k])) continue;
    return -1;  /* a non-definition statement: not a pure reopen */
  }
  return ci;
}

/* A method added to Class is a class method of every class: the top-level
   `class Class` / `Class.class_eval { }` body becomes this module, and
   register_extends extends it into every root class, whose subclasses inherit. */
static const char *const class_reopen_mod = "Class__reopen";

static int is_class_const(const NodeTable *nt, int id) {
  return id >= 0 && nt_kind(nt, id) == NK_ConstantReadNode &&
         nt_str(nt, id, "name") && sp_streq(nt_str(nt, id, "name"), "Class");
}

int class_reopen_cmethod(Compiler *c, int recv, const char *name) {
  int cm = comp_class_index(c, class_reopen_mod);
  if (cm < 0 || !name) return -1;
  if (recv >= 0) {
    NodeKind rk = nt_kind(c->nt, recv);
    const char *cn = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode ? nt_str(c->nt, recv, "name") : NULL;
    int ci = cn ? comp_class_index(c, cn) : -1;
    if (!cn || !builtin_class_id(cn) || is_builtin_module_name(cn) ||
        (ci >= 0 && comp_cmethod_in_chain(c, ci, name, NULL) >= 0)) return -1;
  }
  return comp_cmethod_in_class(c, cm, name);
}

static int is_class_eval_name(const char *nm) {
  return nm && (sp_streq(nm, "class_eval") || sp_streq(nm, "module_eval") ||
                sp_streq(nm, "class_exec") || sp_streq(nm, "module_exec"));
}

void desugar_class_reopen(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  /* The reopening is carried under a name of its own, which the extension
     pass later finds by that name: a declaration the program itself writes
     under it would be taken for the reopening (from the #5078 review). */
  for (int id = 0; id < nt->count; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k != NK_ClassNode && k != NK_ModuleNode && k != NK_ConstantWriteNode) continue;
    int cp = k == NK_ConstantWriteNode ? id : nt_ref(nt, id, "constant_path");
    const char *cn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (cn && sp_streq(cn, class_reopen_mod))
      unsupported_feature(c, id, "the constant name Class__reopen is reserved by the compiler");
  }
  int body = nt_ref(nt, nt->root_id, "statements");
  int n = 0;
  const int *st = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
  for (int i = 0; i < n; i++) {
    int s = st[i];
    if (nt_kind(nt, s) == NK_ClassNode) {
      int cp = nt_ref(nt, s, "constant_path");
      if (!is_class_const(nt, cp)) continue;
      if (nt_ref(nt, s, "superclass") >= 0)
        unsupported_feature(c, s, "`class Class < ...` is not supported: reopen Class without a superclass");
      nt_node_set_type(nt, s, "ModuleNode");
      nt_node_set_str(nt, cp, "name", class_reopen_mod);
      continue;
    }
    if (nt_kind(nt, s) != NK_CallNode || !is_class_eval_name(nt_str(nt, s, "name"))) continue;
    int recv = nt_ref(nt, s, "receiver"), blk = nt_ref(nt, s, "block");
    if (!is_class_const(nt, recv)) continue;
    /* top-level already: what is not supported is the shape, so say that
       rather than the placement message below (from the #5078 review) */
    if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode ||
        nt_ref(nt, blk, "parameters") >= 0 || nt_ref(nt, s, "arguments") >= 0) {
      char msg[256];
      snprintf(msg, sizeof msg, "Class.%s adds methods to Class only with a literal block and "
                                "no block parameters or arguments", nt_str(nt, s, "name"));
      unsupported_feature(c, s, msg);
      continue;
    }
    int bbody = nt_ref(nt, blk, "body");
    long long line = nt_int(nt, s, "node_line", 0), file = nt_int(nt, s, "node_file", 0),
              col = nt_int(nt, s, "node_col", 0);
    nt_node_set_ref(nt, blk, "body", -1);
    nt_node_reset(nt, s, "ModuleNode");
    nt_node_set_int(nt, s, "node_line", line);
    nt_node_set_int(nt, s, "node_file", file);
    nt_node_set_int(nt, s, "node_col", col);
    nt_node_set_ref(nt, s, "constant_path", recv);
    nt_node_set_ref(nt, s, "body", bbody);
    nt_node_set_str(nt, recv, "name", class_reopen_mod);
  }
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!is_class_eval_name(nm) || !is_class_const(nt, nt_ref(nt, id, "receiver"))) continue;
    char msg[256];
    snprintf(msg, sizeof msg, "Class.%s is not supported here: methods are added to Class only "
                              "by a top-level `class Class` or `Class.class_eval { ... }`", nm);
    unsupported_feature(c, id, msg);
  }
}

/* A statement of a `class << self` body: a def is a class method of
   `target_class`, also one inside an if/unless/else there (`class << self;
   if cond; def m; end; else; def m; end; end`, as Loofah defines its entry
   points, #5358). A define_method there is a class method too. Anything
   else is walked as usual. */
static int g_dm_sclass;
static void sclass_walk_stmt(Compiler *c, int s, int scope_idx, int target_class, int depth) {
  const NodeTable *nt = c->nt;
  if (s < 0 || s >= nt->count) return;
  NodeKind k = nt_kind(nt, s);
  if (k == NK_CallNode && dm_registerable_name(nt, s)) {
    g_dm_sclass = 1;
    walk_scope(c, s, scope_idx, target_class);
    return;
  }
  /* `private def m` / `protected def m` / `public def m`: the def is the
     visibility call's argument, and still a class method */
  if (k == NK_CallNode && nt_ref(nt, s, "receiver") < 0 && nt_ref(nt, s, "block") < 0) {
    const char *vn = nt_str(nt, s, "name");
    int va = nt_ref(nt, s, "arguments");
    int vc = 0; const int *vv = va >= 0 ? nt_arr(nt, va, "arguments", &vc) : NULL;
    if (vn && (sp_streq(vn, "private") || sp_streq(vn, "protected") || sp_streq(vn, "public")) &&
        vc == 1 && nt_kind(nt, vv[0]) == NK_DefNode && nt_ref(nt, vv[0], "receiver") < 0) {
      c->nscope[s] = scope_idx;
      c->node_cbody[s] = g_cbody_class_id;
      c->nscope[va] = scope_idx;
      c->node_cbody[va] = g_cbody_class_id;
      sclass_walk_stmt(c, vv[0], scope_idx, target_class, depth + 1);
      return;
    }
  }
  if (k == NK_DefNode && nt_ref(nt, s, "receiver") < 0) {
    const char *name = nt_str(nt, s, "name");
    if (!name) return;
    Scope *sc = comp_scope_new(c, name, s);
    int new_idx = c->nscopes - 1;
    sc->body = nt_ref(nt, s, "body");
    sc->class_id = target_class;
    sc->is_cmethod = 1;
    collect_def_params(c, s, sc);
    /* Assign scope to the def node and its body */
    c->nscope[s] = new_idx;
    c->node_cbody[s] = g_cbody_class_id;
    if (sc->body >= 0) walk_scope(c, sc->body, new_idx, target_class);
    return;
  }
  const char *ty = nt_type(nt, s);
  int cond = k == NK_IfNode || k == NK_UnlessNode || k == NK_StatementsNode ||
             (ty && sp_streq(ty, "ElseNode"));
  if (!cond || depth > 64) { walk_scope(c, s, scope_idx, target_class); return; }
  c->nscope[s] = scope_idx;
  c->node_cbody[s] = g_cbody_class_id;
  if (k == NK_StatementsNode) {
    int n = 0; const int *b = nt_arr(nt, s, "body", &n);
    for (int i = 0; i < n; i++) sclass_walk_stmt(c, b[i], scope_idx, target_class, depth + 1);
    return;
  }
  walk_scope(c, nt_ref(nt, s, "predicate"), scope_idx, target_class);
  sclass_walk_stmt(c, nt_ref(nt, s, "statements"), scope_idx, target_class, depth + 1);
  sclass_walk_stmt(c, nt_ref(nt, s, "subsequent"), scope_idx, target_class, depth + 1);
  sclass_walk_stmt(c, nt_ref(nt, s, "else_clause"), scope_idx, target_class, depth + 1);
}

void walk_scope(Compiler *c, int id, int scope_idx, int class_id) {
  if (id < 0 || id >= c->nt->count) return;
  c->nscope[id] = scope_idx;
  c->node_cbody[id] = g_cbody_class_id;
  const char *ty = nt_type(c->nt, id);
  int child = scope_idx;
  int child_class = class_id;
  int dm_sclass = g_dm_sclass;
  g_dm_sclass = 0;

  /* `class << self; def X; ...; end; end` -- treat body defs as class methods. */
  if (ty && sp_streq(ty, "SingletonClassNode")) {
    /* `class << self` inside a class body defines class methods on the
       enclosing class; `class << Const` (a constant naming a class/module)
       defines them on that named class instead. A singleton-class block on an
       arbitrary object (`class << obj`) has no per-object dispatch here, so
       only the resolvable receivers are special-cased; anything else falls
       through to the generic walk and is rejected loudly during codegen. */
    int target_class = class_id;
    int supported = 0;
    int sexpr = nt_ref(c->nt, id, "expression");
    const char *exty = sexpr >= 0 ? nt_type(c->nt, sexpr) : NULL;
    if (exty && sp_streq(exty, "SelfNode")) {
      supported = 1;
    }
    else if (exty && sp_streq(exty, "ConstantReadNode")) {
      const char *cn = nt_str(c->nt, sexpr, "name");
      int ci = cn ? comp_class_index(c, cn) : -1;
      if (ci >= 0) {
        target_class = ci;
        supported = 1;
      }
    }
    if (supported) {
      int sbody = nt_ref(c->nt, id, "body");
      if (sbody >= 0) {
        int n = 0;
        const int *stmts = nt_arr(c->nt, sbody, "body", &n);
        for (int k = 0; k < n; k++) {
          int s = stmts[k];
          const char *sty = nt_type(c->nt, s);
          if (!sty) continue;
          sclass_walk_stmt(c, s, scope_idx, target_class, 0);
        }
        c->nscope[id] = scope_idx;
        c->nscope[sbody] = scope_idx;
      }
      return;
    }
    /* Unsupported receiver: fall through to the generic walk below. */
  }

  if (ty && (sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode"))) {
    int cp = nt_ref(c->nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(c->nt, cp, "name") : NULL;
    /* `module String` reopening a builtin CLASS is CRuby's TypeError; reject
       with that message instead of colliding with the runtime's sp_<Name> C
       type (a raw C error). A lexically nested or path-qualified
       `module A::Encoding` names a fresh constant in CRuby, but the generated
       C type is the bare tail name and still collides -- refuse that loudly
       too, as unsupported rather than TypeError. */
    if (sp_streq(ty, "ModuleNode") && cname &&
        is_builtin_class_name(cname) && !is_builtin_module_name(cname)) {
      int ln = (int)nt_int(c->nt, id, "node_line", 0);
      const char *file = c->nt->source_file ? c->nt->source_file : "source.rb";
      int toplevel = class_id < 0 && cp >= 0 && nt_type(c->nt, cp) &&
                     sp_streq(nt_type(c->nt, cp), "ConstantReadNode");
      if (toplevel)
        fprintf(stderr, "spinel: %s:%d: %s is not a module (TypeError)\n", file, ln, cname);
      else
        fprintf(stderr, "spinel: %s:%d: unsupported module name '%s': "
                        "collides with the builtin class of that name\n", file, ln, cname);
      exit(1);
    }
    /* `class CONST` where CONST aliases an existing class reopens that class.
       Rewrite the AST name so every later pass (registration, includes) agrees. */
    if (cname && cp >= 0 && comp_class_index(c, cname) < 0) {
      const char *real = resolve_class_alias(c, cname);
      if (real) {
        char buf[256]; snprintf(buf, sizeof buf, "%s", real);  /* copy: set frees cname */
        nt_set_str((NodeTable *)c->nt, cp, "name", buf);
        cname = nt_str(c->nt, cp, "name");
      }
    }
    if (cname && comp_class_index(c, cname) < 0) {
      comp_class_new(c, cname, id);
      child_class = c->nclasses - 1;
      c->classes[child_class].enclosing_class = class_id;
    }
    else if (cname) {
      child_class = comp_class_index(c, cname);  /* reopened class/module */
      /* A class can be opened bare first (no superclass -- e.g. just to hold a
         nested class) and reopened later with `< Super`. The parent link is read
         from def_node's "superclass" ref, so prefer the opening that declares one;
         otherwise the superclass is lost and a subclass's overrides aren't
         dispatched against the right ancestor chain (matz/spinel#1477). */
      if (child_class >= 0 && nt_ref(c->nt, id, "superclass") >= 0 &&
          c->classes[child_class].def_node >= 0 &&
          nt_ref(c->nt, c->classes[child_class].def_node, "superclass") < 0) {
        c->classes[child_class].def_node = id;
      }
    }
  }
  else if (ty && sp_streq(ty, "DefNode")) {
    const char *name = nt_str(c->nt, id, "name");
    Scope *s = comp_scope_new(c, name, id);
    int new_idx = c->nscopes - 1;
    s->body = nt_ref(c->nt, id, "body");
    s->class_id = class_id;   /* instance method of the enclosing class */
    /* `def self.foo` / `def Klass.foo`: a class (singleton) method. */
    int defrecv = nt_ref(c->nt, id, "receiver");
    if (defrecv >= 0) {
      s->is_cmethod = 1;
      /* `def Klass.foo` with an explicit constant receiver (typically defined
         outside the class body, where the enclosing class_id is -1) attaches
         to that class's singleton chain rather than becoming a top-level free
         function. `def self.foo` keeps the enclosing class. */
      const char *rty = nt_type(c->nt, defrecv);
      if (rty && sp_streq(rty, "ConstantReadNode")) {
        int rci = comp_class_index(c, nt_str(c->nt, defrecv, "name"));
        if (rci >= 0) s->class_id = rci;
      }
    }
    collect_def_params(c, id, s);
    child = new_idx;
  }
  else if (ty && sp_streq(ty, "CallNode")) {
    /* `Klass.class_eval/module_eval { defs }` (and the bare/`self.` forms in a
       class body) reopens the class: its block body's `def` and `define_method`
       become instance methods on it, exactly like a `class Klass ... end` reopen.
       Set child_class to the target so the generic recursion below registers them
       there (and register_locals interns any ivars first assigned inside those
       methods). g_cbody_direct gives the enclosing class for bare/self receivers. */
    {
      int ce_ci = class_eval_reopen_class(c, id, g_cbody_direct);
      if (ce_ci >= 0) child_class = ce_ci;
    }
    /* [lits].each { |v| define_method("m_#{v}") { body } } -- unroll into one
       method per element. Handled wholesale; skip the generic recursion so
       the inner define_method isn't also processed as a normal call. */
    if (class_id >= 0 && collect_dm_each_unroll(c, id, class_id)) return;
    /* compiler_state_int/str/sa/ia :fields -- declare ivars + synthesize the
       init/dump/set methods (the metaprogramming is native, not define_method). */
    if (class_id >= 0 && cs_macro_kind(nt_str(c->nt, id, "name"))) {
      collect_compiler_state(c, id, class_id);
      return;
    }
    /* define_method(:literal_name) { ... }: register as a method scope.
       At class scope it becomes an instance method; at top level a free
       function (class_id stays -1), matching `def`. */
    const char *dm_cn = nt_str(c->nt, id, "name");
    int dm_recv = nt_ref(c->nt, id, "receiver");
    int dm_is_dm  = dm_cn && sp_streq(dm_cn, "define_method") && dm_recv < 0;
    int dm_is_dsm = dm_cn && sp_streq(dm_cn, "define_singleton_method");
    /* define_singleton_method registers a class method on the resolved target:
       a class constant receiver, `self` in a class body, or no receiver (the
       enclosing class). An arbitrary-instance singleton has no compile-time
       class, so it is not registered (the later call rejects). */
    int dm_cmethod = dm_is_dm && dm_sclass, dm_cls = class_id, dm_ok = dm_is_dm, dm_defer = 0;
    if (dm_is_dsm) {
      dm_cmethod = 1;
      const char *dsm_rty = dm_recv >= 0 ? nt_type(c->nt, dm_recv) : NULL;
      if (dm_recv < 0) dm_cls = class_id;
      else if (dsm_rty && (sp_streq(dsm_rty, "ConstantReadNode") || sp_streq(dsm_rty, "ConstantPathNode")))
        dm_cls = comp_class_index(c, nt_str(c->nt, dm_recv, "name"));
      else if (dsm_rty && sp_streq(dsm_rty, "SelfNode")) dm_cls = class_id;
      else dm_cls = -1;
      dm_ok = dm_cls >= 0;
      /* A const/local receiver that is NOT a class is an object singleton
         method: still create the scope (as an instance method, class_id
         deferred to -1), so register_singleton_defs can reattach it to the
         synthesized subclass. */
      if (!dm_ok && dm_recv >= 0 && dsm_rty &&
          (sp_streq(dsm_rty, "ConstantReadNode") || sp_streq(dsm_rty, "LocalVariableReadNode"))) {
        dm_ok = 1; dm_defer = 1; dm_cmethod = 0; dm_cls = -1;
      }
    }
    if (dm_ok) {
      int dm_args = nt_ref(c->nt, id, "arguments");
      int dm_na = 0;
      const int *dm_argv = dm_args >= 0 ? nt_arr(c->nt, dm_args, "arguments", &dm_na) : NULL;
      if (dm_na >= 1) {
        const char *dm_aty = nt_type(c->nt, dm_argv[0]);
        const char *dm_mname = NULL;
        if (dm_aty && sp_streq(dm_aty, "SymbolNode"))
          dm_mname = nt_str(c->nt, dm_argv[0], "value");
        else if (dm_aty && sp_streq(dm_aty, "StringNode"))
          dm_mname = nt_str(c->nt, dm_argv[0], "content");
        int dm_blk = nt_ref(c->nt, id, "block");
        if (dm_mname && dm_blk >= 0) {
          Scope *dm_s = comp_scope_new(c, dm_mname, id);
          int dm_new_idx = c->nscopes - 1;
          dm_s->body = nt_ref(c->nt, dm_blk, "body");
          dm_s->class_id = dm_cls;
          dm_s->is_cmethod = dm_cmethod;
          /* the block's params are the defined method's params (e.g. the
             `&:to_s`-rewritten `{ |_spx| _spx.to_s }`'s _spx). */
          int dm_pn = nt_ref(c->nt, dm_blk, "parameters");
          int dm_inner = dm_pn >= 0 ? nt_ref(c->nt, dm_pn, "parameters") : -1;
          int dm_pnode = dm_inner >= 0 ? dm_inner : dm_pn;
          int dm_rn = 0; const int *dm_reqs = dm_pnode >= 0 ? nt_arr(c->nt, dm_pnode, "requireds", &dm_rn) : NULL;
          for (int p = 0; p < dm_rn; p++) {
            const char *pnm = nt_str(c->nt, dm_reqs[p], "name");
            if (pnm) scope_add_param(dm_s, pnm, -1);
          }
          /* a defined method takes its parameters with METHOD semantics, so a
             defaulted one is a real parameter with that default, and the
             required count is what a call must supply (#3752) */
          dm_s->nrequired = dm_rn;
          { int dm_on = 0;
            const int *dm_opts = dm_pnode >= 0 ? nt_arr(c->nt, dm_pnode, "optionals", &dm_on) : NULL;
            for (int p = 0; p < dm_on; p++) {
              const char *pnm = nt_str(c->nt, dm_opts[p], "name");
              int dv = nt_ref(c->nt, dm_opts[p], "value");
              if (pnm) scope_add_param(dm_s, pnm, dv);
            } }
          child = dm_new_idx;
        }
      }
    }
  }

  int saved_cbody = g_cbody_class_id;
  int saved_direct = g_cbody_direct;
  if (child_class >= 0) g_cbody_class_id = child_class;
  /* g_cbody_direct tracks the class whose body we are *directly* in (where `self`
     is the class). A method/block scope is entered exactly when `child` was
     reassigned (DefNode/define_method/block); there `self` is no longer the class,
     so clear it. ClassNode/ModuleNode leave child == scope_idx. */
  if (child != scope_idx) g_cbody_direct = -1;
  else if (child_class >= 0) g_cbody_direct = child_class;

  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) {
    int r = nt_ref_at(c->nt, id, i);
    if (r >= 0) walk_scope(c, r, child, child_class);
  }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(c->nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (ids[j] >= 0) walk_scope(c, ids[j], child, child_class);
  }
  g_cbody_class_id = saved_cbody;
  g_cbody_direct = saved_direct;
}

/* A `module_function` call of module `ci`'s body: bare, it turns on the mode
   for the defs that follow; `module_function :m1, :m2` (or a def) marks the
   named methods. 1 when `s` is one. */
static int mf_call_apply(Compiler *c, int ci, int s, int *in_mf) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, s) != NK_CallNode || nt_ref(nt, s, "receiver") >= 0) return 0;
  const char *nm = nt_str(nt, s, "name");
  if (!nm || !sp_streq(nm, "module_function")) return 0;
  int an = 0;
  int anode = nt_ref(nt, s, "arguments");
  const int *aargs = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
  if (an == 0) { *in_mf = 1; return 1; }
  for (int ai = 0; ai < an; ai++) {
    const char *aty = nt_type(nt, aargs[ai]);
    const char *aval = NULL;
    if (aty && sp_streq(aty, "SymbolNode")) aval = nt_str(nt, aargs[ai], "value");
    else if (aty && sp_streq(aty, "DefNode") && nt_ref(nt, aargs[ai], "receiver") < 0)
      aval = nt_str(nt, aargs[ai], "name");
    if (!aval) continue;
    for (int mi = 0; mi < c->nscopes; mi++) {
      if (c->scopes[mi].class_id == ci && !c->scopes[mi].is_cmethod &&
          c->scopes[mi].name && sp_streq(c->scopes[mi].name, aval)) {
        c->scopes[mi].is_cmethod = 1;
        c->scopes[mi].is_module_function = 1;
      }
    }
  }
  return 1;
}

/* A `module_function` call inside a body statement's value takes effect as
   the bare statement does. Nested bodies with their own self are not
   walked. */
static void mf_apply_nested(Compiler *c, int ci, int node, int *in_mf) {
  const NodeTable *nt = c->nt;
  if (node < 0) return;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode ||
      k == NK_BlockNode || k == NK_LambdaNode)
    return;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) {
    int ch = nt_ref_at(nt, node, i);
    if (ch < 0) continue;
    mf_call_apply(c, ci, ch, in_mf);
    mf_apply_nested(c, ci, ch, in_mf);
  }
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int m = 0; const int *ids = nt_arr_at(nt, node, i, &m);
    for (int j = 0; j < m; j++) {
      mf_call_apply(c, ci, ids[j], in_mf);
      mf_apply_nested(c, ci, ids[j], in_mf);
    }
  }
}

/* Mark methods following `module_function` in a module body as class-level
   (is_cmethod=1, no self param). This lets them be called as bare functions
   when their module is included at the top level. */
void register_module_functions(Compiler *c) {
  const NodeTable *nt = c->nt;
  /* Every module BODY, not just the one recorded as the module's def_node: a
     module reopened in a second file (a gem's version.rb naming the module,
     then its main file) keeps the first body as def_node, so the reopen's
     `module_function` was never seen and its methods stayed instance-level --
     `M.helper(x)` then had no callee and refused to compile (#3969). */
  for (int dn = 0; dn < nt->count; dn++) {
    if (nt_kind(nt, dn) != NK_ModuleNode) continue;
    int ci = -1;
    for (int k = 0; k < c->nclasses && ci < 0; k++) if (c->classes[k].def_node == dn) ci = k;
    if (ci < 0) {
      /* a ModuleNode names its module through constant_path, not a "name" */
      int cp = nt_ref(nt, dn, "constant_path");
      const char *mnm = cp >= 0 ? nt_str(nt, cp, "name") : nt_str(nt, dn, "name");
      ci = mnm ? comp_class_index(c, mnm) : -1;
    }
    if (ci < 0) continue;
    int body = nt_ref(nt, dn, "body");
    if (body < 0) continue;
    int bn = 0;
    const int *stmts = nt_arr(nt, body, "body", &bn);
    int in_module_function = 0;
    /* `extend self` is the other spelling of module_function, and unlike it
       the placement carries no meaning: it makes EVERY method of the module
       callable on the module itself, wherever in the body it sits. So scan
       for it first rather than letting the positional flag below decide.
       (module_function also makes the instance copies private; a module
       written this way is called as `M.f` -- the same assumption the
       positional form already makes.) */
    for (int k = 0; k < bn; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty || !sp_streq(sty, "CallNode") || nt_ref(nt, s, "receiver") >= 0) continue;
      const char *nm = nt_str(nt, s, "name");
      if (!nm || !sp_streq(nm, "extend")) continue;
      int an = 0;
      int anode = nt_ref(nt, s, "arguments");
      const int *aargs = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
      if (an == 1 && nt_kind(nt, aargs[0]) == NK_SelfNode) { in_module_function = 1; break; }
    }
    for (int k = 0; k < bn; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty) continue;
      if (mf_call_apply(c, ci, s, &in_module_function)) continue;
      /* `x = module_function def m`, `p(module_function :m)` */
      mf_apply_nested(c, ci, s, &in_module_function);
      if (sp_streq(sty, "DefNode") && in_module_function) {
        const char *mname = nt_str(nt, s, "name");
        if (!mname) continue;
        for (int mi = 0; mi < c->nscopes; mi++) {
          if (c->scopes[mi].def_node == s) {
            c->scopes[mi].is_cmethod = 1;
            c->scopes[mi].is_module_function = 1;
            break;
          }
        }
      }
    }
  }
}

/* Method name carried by a `private`/`public`/`protected` symbol/string arg. */
static const char *vis_arg_name(const NodeTable *nt, int arg) {
  const char *aty = nt_type(nt, arg);
  if (!aty) return NULL;
  if (sp_streq(aty, "SymbolNode")) return nt_str(nt, arg, "value");
  if (sp_streq(aty, "StringNode")) {
    const char *s = nt_str(nt, arg, "content");
    return s ? s : nt_str(nt, arg, "unescaped");
  }
  return NULL;
}

/* Record `kind` for the methods an attr_reader/writer/accessor call declares
   (writers as "x="), e.g. for `private attr_reader :x` or a bare attr under a
   private/protected section. */
static void vis_record(ClassInfo *cls, const char *name, int kind, int sg) {
  if (sg) comp_cmethod_vis_set(cls, name, kind);
  else comp_method_vis_set(cls, name, kind);
}

static void vis_apply_attr(Compiler *c, ClassInfo *cls, int call, int kind, int sg) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, call, "name");
  if (!nm) return;
  int reader = sp_streq(nm, "attr_reader") || sp_streq(nm, "attr_accessor") ||
               sp_streq(nm, "attr");
  int writer = sp_streq(nm, "attr_writer") || sp_streq(nm, "attr_accessor");
  if (!reader && !writer) return;
  int args = nt_ref(nt, call, "arguments");
  int an = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  for (int i = 0; i < an; i++) {
    const char *base = vis_arg_name(nt, argv[i]);
    if (!base) continue;
    if (reader) vis_record(cls, base, kind, sg);
    if (writer) {
      char buf[256];
      snprintf(buf, sizeof buf, "%s=", base);
      vis_record(cls, buf, kind, sg);
    }
  }
}

/* An alias copies its target's visibility as it stands at the alias; a later
   `private :old` leaves the alias as it was. A target this body has not
   declared (an inherited method) keeps resolving through the alias. */
static void vis_alias(ClassInfo *cls, const char *nw, const char *od) {
  if (!nw || !od) return;
  for (int i = 0; i < cls->nvis; i++)
    if (sp_streq(cls->vis_names[i], od)) { comp_method_vis_set(cls, nw, cls->vis_kinds[i]); return; }
}

/* One `private` / `protected` / `public` call of a class body: bare, it
   switches the section mode in *cur; otherwise it records the named methods'
   visibility. */
static void vis_apply_call(Compiler *c, ClassInfo *cls, int s, int kind, int sg, int *cur) {
  const NodeTable *nt = c->nt;
  int args = nt_ref(nt, s, "arguments");
  int an = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an == 0) { *cur = kind; return; }  /* bare: switch the section mode */
  for (int i = 0; i < an; i++) {
    const char *aty = nt_type(nt, argv[i]);
    const char *mn = vis_arg_name(nt, argv[i]);
    if (mn) { vis_record(cls, mn, kind, sg); continue; }
    if (nt_kind(nt, argv[i]) == NK_ArrayNode) {  /* private [:a, :b] */
      int en = 0; const int *ev = nt_arr(nt, argv[i], "elements", &en);
      for (int j = 0; j < en; j++) {
        const char *emn = vis_arg_name(nt, ev[j]);
        if (emn) vis_record(cls, emn, kind, sg);
      }
      continue;
    }
    if (aty && sp_streq(aty, "DefNode")) {
      const char *dn = nt_str(nt, argv[i], "name");
      if (dn && nt_ref(nt, argv[i], "receiver") < 0)
        vis_record(cls, dn, kind, sg);
    }
    else if (aty && sp_streq(aty, "CallNode")) {
      const char *dn = dm_defined_name(nt, argv[i]);
      const char *acn = nt_str(nt, argv[i], "name");
      if (dn) vis_record(cls, dn, kind, sg);  /* private define_method(:m) { } */
      else if (acn && sp_streq(acn, "alias_method")) {  /* private alias_method :a, :b */
        int aa = nt_ref(nt, argv[i], "arguments");
        int aan = 0;
        const int *aav = aa >= 0 ? nt_arr(nt, aa, "arguments", &aan) : NULL;
        const char *anm = aan == 2 ? vis_arg_name(nt, aav[0]) : NULL;
        if (anm) vis_record(cls, anm, kind, sg);
      }
      else vis_apply_attr(c, cls, argv[i], kind, sg);  /* private attr_reader :x */
    }
  }
}

static int vis_call_kind(const NodeTable *nt, int s) {
  if (s < 0 || nt_kind(nt, s) != NK_CallNode || nt_ref(nt, s, "receiver") >= 0) return -1;
  const char *nm = nt_str(nt, s, "name");
  return !nm ? -1 : sp_streq(nm, "private")   ? SP_VIS_PRIVATE   :
                    sp_streq(nm, "protected") ? SP_VIS_PROTECTED :
                    sp_streq(nm, "public")    ? SP_VIS_PUBLIC : -1;
}

static int vis_cmethod_call_kind(const NodeTable *nt, int s) {
  if (s < 0 || nt_kind(nt, s) != NK_CallNode || nt_ref(nt, s, "receiver") >= 0) return -1;
  const char *nm = nt_str(nt, s, "name");
  return !nm ? -1 : sp_streq(nm, "private_class_method") ? SP_VIS_PRIVATE :
                    sp_streq(nm, "public_class_method")  ? SP_VIS_PUBLIC : -1;
}

/* private_class_method / public_class_method: the named class methods'
   visibility */
static void vis_apply_cmethod_call(ClassInfo *cls, const NodeTable *nt, int s, int ckind) {
  int args = nt_ref(nt, s, "arguments");
  int an = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  for (int i = 0; i < an; i++) {
    int en = 1;
    const int *ev = &argv[i];
    if (nt_kind(nt, argv[i]) == NK_ArrayNode) ev = nt_arr(nt, argv[i], "elements", &en);
    for (int j = 0; ev && j < en; j++) {
      const char *mn = vis_arg_name(nt, ev[j]);
      /* private_class_method def self.m ... end */
      if (!mn && nt_kind(nt, ev[j]) == NK_DefNode && nt_ref(nt, ev[j], "receiver") >= 0)
        mn = nt_str(nt, ev[j], "name");
      if (mn) comp_cmethod_vis_set(cls, mn, ckind);
    }
  }
}

/* A visibility call inside a class-body statement's value (`x = private def
   m`, `p(private :a)`) takes effect as the bare statement does. Nested bodies
   with their own self are not walked. */
static void vis_apply_nested(Compiler *c, ClassInfo *cls, int node, int sg, int *cur) {
  const NodeTable *nt = c->nt;
  if (node < 0) return;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode ||
      k == NK_BlockNode || k == NK_LambdaNode)
    return;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) {
    int ch = nt_ref_at(nt, node, i);
    int kind = vis_call_kind(nt, ch), ckind = sg ? -1 : vis_cmethod_call_kind(nt, ch);
    if (kind >= 0) vis_apply_call(c, cls, ch, kind, sg, cur);
    if (ckind >= 0) vis_apply_cmethod_call(cls, nt, ch, ckind);
    vis_apply_nested(c, cls, ch, sg, cur);
  }
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int m = 0; const int *ids = nt_arr_at(nt, node, i, &m);
    for (int j = 0; j < m; j++) {
      int kind = vis_call_kind(nt, ids[j]), ckind = sg ? -1 : vis_cmethod_call_kind(nt, ids[j]);
      if (kind >= 0) vis_apply_call(c, cls, ids[j], kind, sg, cur);
      if (ckind >= 0) vis_apply_cmethod_call(cls, nt, ids[j], ckind);
      vis_apply_nested(c, cls, ids[j], sg, cur);
    }
  }
}

/* Walk one class/module body in lexical order, recording each method's
   visibility (default public). Handles a bare `private`/`protected`/`public`
   (switches the mode for following defs/attrs), the `private :a, :b` /
   `private def m;end` / `private attr_reader :x` argument forms, and plain
   `def`/`attr_*` declarations under the active mode. Class methods are a
   separate axis: `sg` walks a `class << self` body into the class-method
   table, which `private_class_method` / `public_class_method` also write. */
static void register_method_visibility_body(Compiler *c, ClassInfo *cls, int body, int sg) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
  int cur = SP_VIS_PUBLIC;
  for (int k = 0; k < n; k++) {
    int s = stmts[k];
    const char *sty = nt_type(nt, s);
    if (!sty) continue;
    if (sp_streq(sty, "DefNode")) {
      const char *mname = nt_str(nt, s, "name");
      /* `def initialize` is private whatever section it sits in; only a
         later `public :initialize` makes it public */
      if (mname && nt_ref(nt, s, "receiver") < 0)
        vis_record(cls, mname, !sg && sp_streq(mname, "initialize") ? SP_VIS_PRIVATE : cur, sg);
      continue;
    }
    if (!sg && sp_streq(sty, "SingletonClassNode")) {
      int ex = nt_ref(nt, s, "expression");
      if (ex >= 0 && nt_kind(nt, ex) == NK_SelfNode)
        register_method_visibility_body(c, cls, nt_ref(nt, s, "body"), 1);
      continue;
    }
    if (sp_streq(sty, "AliasMethodNode")) {
      int nn = nt_ref(nt, s, "new_name"), on = nt_ref(nt, s, "old_name");
      if (!sg) vis_alias(cls, nn >= 0 ? vis_arg_name(nt, nn) : NULL, on >= 0 ? vis_arg_name(nt, on) : NULL);
      continue;
    }
    vis_apply_nested(c, cls, s, sg, &cur);
    if (!sp_streq(sty, "CallNode") || nt_ref(nt, s, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, s, "name");
    if (!nm) continue;
    if (sp_streq(nm, "alias_method")) {
      int args = nt_ref(nt, s, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an == 2 && !sg) vis_alias(cls, vis_arg_name(nt, argv[0]), vis_arg_name(nt, argv[1]));
      continue;
    }
    const char *dmn = dm_defined_name(nt, s);
    if (dmn) { vis_record(cls, dmn, cur, sg); continue; }
    int ckind = sg ? -1 : vis_cmethod_call_kind(nt, s);
    if (ckind >= 0) {
      vis_apply_cmethod_call(cls, nt, s, ckind);
      continue;
    }
    int kind = vis_call_kind(nt, s);
    if (kind >= 0) {
      vis_apply_call(c, cls, s, kind, sg, &cur);
      continue;
    }
    /* Record attr visibility unconditionally (like a plain `def`), so a public
       attr in a subclass overrides an inherited private/protected method rather
       than resolving up the chain to the ancestor's visibility. */
    if (sp_streq(nm, "attr_reader") || sp_streq(nm, "attr_writer") ||
        sp_streq(nm, "attr_accessor") || sp_streq(nm, "attr"))
      vis_apply_attr(c, cls, s, cur, sg);
  }
}

/* Record per-method visibility for every class/module body, including reopened
   bodies (each `class Foo ... end` opening starts public, like CRuby). */
void register_method_visibility(Compiler *c) {
  int *bci, *bnode;
  int nb = class_body_list(c, &bci, &bnode);
  for (int b = 0; b < nb; b++) register_method_visibility_body(c, &c->classes[bci[b]], bnode[b], 0);
  free(bci);
  free(bnode);
}

void register_locals(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (sp_streq(ty, "LocalVariableWriteNode") ||
        sp_streq(ty, "LocalVariableTargetNode") ||
        sp_streq(ty, "LocalVariableReadNode") ||
        sp_streq(ty, "LocalVariableOperatorWriteNode") ||
        sp_streq(ty, "LocalVariableOrWriteNode") ||
        sp_streq(ty, "LocalVariableAndWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm) {
        LocalVar *lv = scope_local_intern(comp_scope_of(c, id), nm);
        /* or_write_only is a two-bit scratch here: 1 = an or-write was seen,
           2 = a definite write was seen. Normalised to the flag below. */
        if (sp_streq(ty, "LocalVariableOrWriteNode")) lv->or_write_only |= 1;
        else if (!sp_streq(ty, "LocalVariableReadNode")) lv->or_write_only |= 2;
      }
    }
    if (sp_streq(ty, "InstanceVariableWriteNode") ||
        sp_streq(ty, "InstanceVariableReadNode") ||
        sp_streq(ty, "InstanceVariableOperatorWriteNode") ||
        /* the multi-assign target form (`@a, @b = ...`) also defines the ivar,
           so it must be interned for the struct field (#3273). */
        sp_streq(ty, "InstanceVariableTargetNode")) {
      const char *nm = nt_str(nt, id, "name");
      Scope *s = comp_scope_of(c, id);
      if (nm && s->class_id >= 0) comp_ivar_intern(&c->classes[s->class_id], nm);
    }
  }
  /* Normalise the scratch: only "an or-write and nothing else" leaves the
     local without a definite assignment. Parameters are bound on entry, so
     they are assigned however the scratch reads. */
  for (int si = 0; si < c->nscopes; si++) {
    Scope *s = &c->scopes[si];
    for (int li = 0; li < s->nlocals; li++) {
      LocalVar *lv = &s->locals[li];
      lv->or_write_only = (lv->or_write_only == 1 && !lv->is_param && !lv->is_block_param);

    }
  }
}

/* `Const = Struct.new(:a, :b)` / `Const = Data.define(:a, :b)` defines a
   class named Const whose positional members are attr_accessors. Register
   it as a class with one ivar + reader + writer per member. */
int is_c_ident(const char *s);

/* Is CallNode `val` a `Struct.new(...)` / `Data.define(...)`? */
int is_struct_call(Compiler *c, int val) {
  const NodeTable *nt = c->nt;
  if (val < 0 || !nt_type(nt, val) || !sp_streq(nt_type(nt, val), "CallNode")) return 0;
  const char *mn = nt_str(nt, val, "name");
  int vr = nt_ref(nt, val, "receiver");
  const char *rn = vr >= 0 && nt_type(nt, vr) && sp_streq(nt_type(nt, vr), "ConstantReadNode")
                   ? nt_str(nt, vr, "name") : NULL;
  return rn && ((sp_streq(rn, "Struct") && mn && sp_streq(mn, "new")) ||
                (sp_streq(rn, "Data") && mn && sp_streq(mn, "define")));
}

/* The class body a definition node carries: a ClassNode's or ModuleNode's
   own, or the block of `Name = Struct.new(...) do ... end` (and
   Data.define), whose write is the class's def_node. Reading "body" off that
   write found nothing, so an `include` / `extend` / `prepend` in the block
   was dropped. -1 when there is none. */
int class_def_body(Compiler *c, int def_node) {
  const NodeTable *nt = c->nt;
  if (def_node < 0) return -1;
  NodeKind k = nt_kind(nt, def_node);
  if (k != NK_ConstantWriteNode && k != NK_LocalVariableWriteNode) return nt_ref(nt, def_node, "body");
  int val = nt_ref(nt, def_node, "value");
  if (!is_struct_call(c, val)) return -1;
  int blk = nt_ref(nt, val, "block");
  if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) return -1;
  int bb = nt_ref(nt, blk, "body");
  return bb >= 0 && nt_kind(nt, bb) == NK_StatementsNode ? bb : -1;
}

/* The name of the class a node opens a body of: a ClassNode's or
   ModuleNode's, or the constant of `Name = Struct.new(...) do ... end`. A
   Struct that a `class Name` elsewhere reopens keeps that ClassNode as its
   def_node, so the block is found as one more body. NULL otherwise. */
static const char *class_body_name(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_ClassNode || k == NK_ModuleNode) {
    int cp = nt_ref(nt, id, "constant_path");
    return cp >= 0 ? nt_str(nt, cp, "name") : NULL;
  }
  if (k == NK_ConstantWriteNode && class_def_body(c, id) >= 0) return nt_str(nt, id, "name");
  /* `k = Struct.new(...) do ... end` defines the anonymous class it homes */
  if (k == NK_LocalVariableWriteNode && class_def_body(c, id) >= 0)
    for (int ci = 0; ci < c->nclasses; ci++)
      if (c->classes[ci].def_node == id) return c->classes[ci].name;
  return NULL;
}

/* For each class, the `Name = Struct.new/Data.define(...) do ... end` write
   whose block comes before the class's def_node -- a later `class Name`
   reopening became the def_node -- or -1. That block is the class's first
   body in source order, so its mixins go in before the reopening's. */
static int *struct_block_before_def(Compiler *c) {
  int *sw = malloc(sizeof(int) * (size_t)(c->nclasses > 0 ? c->nclasses : 1));
  for (int ci = 0; ci < c->nclasses; ci++) sw[ci] = -1;
  for (int id = 0; id < c->nt->count; id++) {
    if (nt_kind(c->nt, id) != NK_ConstantWriteNode) continue;
    const char *cname = class_body_name(c, id);
    int ci = cname ? comp_class_index(c, cname) : -1;
    if (ci >= 0 && sw[ci] < 0 && id < c->classes[ci].def_node) sw[ci] = id;
  }
  return sw;
}

/* Every body that defines or reopens a class, in the order the class-body
   passes walk them: per class, a Struct.new/Data.define block that a later
   `class Name` reopens, then its def_node's body; then every other body in
   node order. *out_ci[i] is the class, *out_body[i] the body (-1 for none).
   The caller frees both arrays. */
int class_body_list(Compiler *c, int **out_ci, int **out_body) {
  int nb = 0, cap = 64;
  int *bci = malloc((size_t)cap * sizeof(int));
  int *bnode = malloc((size_t)cap * sizeof(int));
  #define ADD_BODY(CI, NODE) do { \
    if (nb == cap) { cap *= 2; bci = realloc(bci, (size_t)cap * sizeof(int)); bnode = realloc(bnode, (size_t)cap * sizeof(int)); } \
    bci[nb] = (CI); bnode[nb] = (NODE); nb++; } while (0)
  int *sw = struct_block_before_def(c);
  for (int ci = 0; ci < c->nclasses; ci++) {
    if (sw[ci] >= 0) ADD_BODY(ci, class_def_body(c, sw[ci]));
    ADD_BODY(ci, class_def_body(c, c->classes[ci].def_node));
  }
  for (int id = 0; id < c->nt->count; id++) {
    const char *cname = class_body_name(c, id);
    if (!cname) continue;
    int ci = comp_class_index(c, cname);
    if (ci < 0) continue;
    if (id == c->classes[ci].def_node || id == sw[ci]) continue;
    ADD_BODY(ci, class_def_body(c, id));
  }
  #undef ADD_BODY
  free(sw);
  *out_ci = bci;
  *out_body = bnode;
  return nb;
}

/* Register the symbol members of a Struct.new(...) call onto `cls`. */
/* Resolve a Struct.new / Data.define member argument to its literal symbol
   name: a SymbolNode directly, or a local variable whose writes in the same
   scope are all the SAME symbol literal (compile-time const propagation, so a
   `name = :port; Struct.new(name)` still names the member) (#3112). A local
   with any non-symbol / conflicting write is unresolvable -> NULL. */
static const char *resolve_member_symbol(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0 || !nt_type(nt, node)) return NULL;
  if (sp_streq(nt_type(nt, node), "SymbolNode")) return nt_str(nt, node, "value");
  if (nt_kind(nt, node) != NK_LocalVariableReadNode) return NULL;
  const char *vn = nt_str(nt, node, "name");
  if (!vn) return NULL;
  Scope *scp = comp_scope_of(c, node);
  const char *found = NULL;
  for (int w = 0; w < nt->count; w++) {
    if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
    const char *wn = nt_str(nt, w, "name");
    if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != scp) continue;
    int wv = nt_ref(nt, w, "value");
    if (wv < 0 || !nt_type(nt, wv) || !sp_streq(nt_type(nt, wv), "SymbolNode"))
      return NULL;   /* a non-symbol write: not resolvable */
    const char *sm = nt_str(nt, wv, "value");
    if (!sm) return NULL;
    if (found && !sp_streq(found, sm)) return NULL;  /* conflicting writes */
    found = sm;
  }
  return found;
}

/* Resolve a node to a compile-time list of symbol member names (`out`, up to
   `cap`), returning the count or -1 when not statically resolvable. Follows
   a symbol-array literal, a single-array-literal-write local, and the
   order-preserving/deterministic array transforms whose result is still a
   fixed symbol list -- so `%i[a b].reverse` and `keys = %i[a b]; keys.reverse`
   register their members in the right order (#3135). */
static int resolve_symbol_list(Compiler *c, int node, const char **out, int cap, int depth) {
  const NodeTable *nt = c->nt;
  if (node < 0 || depth > 8) return -1;
  const char *ty = nt_type(nt, node);
  if (!ty) return -1;
  if (sp_streq(ty, "ArrayNode")) {
    int en = 0; const int *els = nt_arr(nt, node, "elements", &en);
    if (en > cap) return -1;
    for (int e = 0; e < en; e++) {
      if (!nt_type(nt, els[e]) || !sp_streq(nt_type(nt, els[e]), "SymbolNode")) return -1;
      out[e] = nt_str(nt, els[e], "value");
      if (!out[e]) return -1;
    }
    return en;
  }
  if (nt_kind(nt, node) == NK_LocalVariableReadNode) {
    const char *vn = nt_str(nt, node, "name");
    Scope *scp = comp_scope_of(c, node);
    /* A single-write local resolves to its value. A reassigned local
       (`keys = keys.reverse`) would need node-order dataflow to pick the
       write that reaches this read, and its RHS reads the same name -- that
       is copy-propagation territory, out of scope for this static resolver,
       so a multiply-written local stays unresolved. The transform can still
       be applied inline at the Struct.new (`Struct.new(*keys.reverse)`). */
    int src = -1;
    for (int w = 0; vn && w < nt->count; w++) {
      if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != scp) continue;
      if (src >= 0) return -1;
      src = nt_ref(nt, w, "value");
    }
    if (src < 0) return -1;
    return resolve_symbol_list(c, src, out, cap, depth + 1);
  }
  /* a receiverless method on a resolvable symbol list that yields another
     fixed symbol list: reverse (order flip), sort/sort_by/uniq (deterministic
     reorder), rotate (fixed shift). The member NAMES and their order are all
     compile-time known, so the Struct is still statically typed. */
  if (sp_streq(ty, "CallNode")) {
    int recv = nt_ref(nt, node, "receiver");
    if (recv < 0) return -1;
    const char *mn = nt_str(nt, node, "name");
    if (!mn) return -1;
    int args = nt_ref(nt, node, "arguments");
    int an = 0; if (args >= 0) nt_arr(nt, args, "arguments", &an);
    int n = resolve_symbol_list(c, recv, out, cap, depth + 1);
    if (n < 0) return -1;
    if (sp_streq(mn, "reverse") && an == 0) {
      for (int i = 0; i < n / 2; i++) { const char *t = out[i]; out[i] = out[n-1-i]; out[n-1-i] = t; }
      return n;
    }
    if (sp_streq(mn, "uniq") && an == 0) {
      int w = 0;
      for (int i = 0; i < n; i++) {
        int dup = 0;
        for (int j = 0; j < w; j++) if (sp_streq(out[i], out[j])) { dup = 1; break; }
        if (!dup) out[w++] = out[i];
      }
      return w;
    }
    if (sp_streq(mn, "sort") && an == 0 && nt_ref(nt, node, "block") < 0) {
      for (int i = 1; i < n; i++) {
        const char *k = out[i]; int j = i - 1;
        while (j >= 0 && strcmp(out[j], k) > 0) { out[j+1] = out[j]; j--; }
        out[j+1] = k;
      }
      return n;
    }
    return -1;   /* a transform that does not preserve a fixed symbol list */
  }
  return -1;
}

void register_struct_members(Compiler *c, ClassInfo *cls, int val) {
  const NodeTable *nt = c->nt;
  cls->is_struct = 1;
  {
    int vr = nt_ref(nt, val, "receiver");
    const char *rn = vr >= 0 && nt_type(nt, vr) && sp_streq(nt_type(nt, vr), "ConstantReadNode")
                     ? nt_str(nt, vr, "name") : NULL;
    if (rn && sp_streq(rn, "Data")) cls->is_data = 1;
  }
  int args = nt_ref(nt, val, "arguments");
  int an = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  /* The string-named form `Struct.new("Foo", ...)` registers a `Struct::Foo`
     constant -- a legacy pattern spinel deliberately drops (see limitations.md).
     Reject it with a pointer to the modern `Foo = Struct.new(...)` form rather
     than leaving `Struct::Foo` untyped and failing with a raw C error (#3080). */
  if (an >= 1 && argv && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "StringNode")) {
    int ln = (int)nt_int(nt, val, "node_line", 0);
    const char *file = nt->source_file ? nt->source_file : "source.rb";
    fprintf(stderr, "spinel: %s:%d: Struct.new with a string name (the Struct::Name "
                    "form) is not supported; use `Name = Struct.new(...)`\n", file, ln);
    exit(1);
  }
  for (int a = 0; a < an; a++) {
    /* trailing `keyword_init: true` -> the members are keyword-initialized;
       Struct#keyword_init? reports it (a KeywordHashNode holds the pairs). */
    if (nt_type(nt, argv[a]) && sp_streq(nt_type(nt, argv[a]), "KeywordHashNode")) {
      int en = 0; const int *elems = nt_arr(nt, argv[a], "elements", &en);
      for (int e = 0; e < en; e++) {
        int key = nt_ref(nt, elems[e], "key");
        const char *kn = key >= 0 ? nt_str(nt, key, "unescaped") : NULL;
        if (!kn) kn = key >= 0 ? nt_str(nt, key, "value") : NULL;
        if (kn && sp_streq(kn, "keyword_init")) {
          int kv = nt_ref(nt, elems[e], "value");
          const char *kvt = kv >= 0 ? nt_type(nt, kv) : NULL;
          if (kvt && sp_streq(kvt, "TrueNode")) cls->kw_init = 1;
          else if (kvt && sp_streq(kvt, "FalseNode")) cls->kw_init = -1;  /* explicit false */
        }
      }
      continue;
    }
    /* `Data.define(*syms)` / `Struct.new(*syms)`: a splatted member list whose
       source is a literal symbol array (directly, or a local's sole array-
       literal write) resolves at compile time to the member names (#2973). */
    if (nt_type(nt, argv[a]) && sp_streq(nt_type(nt, argv[a]), "SplatNode")) {
      int se = nt_ref(nt, argv[a], "expression");
      const char *syms[128];
      int en = se >= 0 ? resolve_symbol_list(c, se, syms, 128, 0) : -1;
      for (int e = 0; e < en; e++) {
        const char *sm = syms[e];
        if (!sm) continue;
        char siv[256]; snprintf(siv, sizeof siv, "@%s", sm);
        comp_member_intern(cls, siv);
        comp_add_reader(cls, sm);
        if (!cls->is_data) comp_add_writer(cls, sm);
      }
      continue;
    }
    /* a SymbolNode directly, or a local that const-propagates to one (#3112) */
    const char *m = resolve_member_symbol(c, argv[a]);
    if (!m) continue;
    char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", m);
    comp_member_intern(cls, ivn);
    comp_add_reader(cls, m);
    /* a Data member is read-only: no `x=` to call, answer or list */
    if (!cls->is_data) comp_add_writer(cls, m);
  }
}

/* Singleton methods on a constant/local that statically holds one user
   object (def CONST.m / def x.m). CRuby gives the object a hidden singleton
   class; the AOT analogue is a synthesized anonymous subclass carrying those
   methods, with the binding's type retargeted to it. The subclass masquerades
   as its parent everywhere Ruby-visible (see singleton_visible_ci). Only a
   statically-traceable receiver qualifies: a constant or local with exactly
   one `= <UserClass>.new(...)` write. Others keep today's behavior (the def is
   left unattached and the later call rejects).

   Resolve `Recv.new`'s constant name to a user-class index (no builtins). */
static int sg_new_class_ci(Compiler *c, int val) {
  const NodeTable *nt = c->nt;
  if (val < 0 || nt_kind(nt, val) != NK_CallNode) return -1;
  const char *nm = nt_str(nt, val, "name");
  if (!nm || !sp_streq(nm, "new")) return -1;
  int recv = nt_ref(nt, val, "receiver");
  if (recv < 0 || nt_kind(nt, recv) != NK_ConstantReadNode) return -1;
  int ci = comp_class_index(c, nt_str(nt, recv, "name"));
  if (ci < 0) return -1;
  /* Only a plain user class can be subclassed here: Object/BasicObject use an
     opaque base struct with no cls_id field, and native/exception/struct
     classes have special layouts a synthesized subclass cannot carry. */
  const char *cn = c->classes[ci].name;
  if (cn && (sp_streq(cn, "Object") || sp_streq(cn, "BasicObject"))) return -1;
  if (c->classes[ci].is_native_class || c->classes[ci].is_struct ||
      c->classes[ci].is_data || class_is_exc_subclass(c, ci)) return -1;
  return ci;
}

/* The single defining write of a constant/local `name` (in scope `owner_scope`
   for locals; -1 = a constant). Returns the write node if there is exactly one
   and its value is `<UserClass>.new(...)`, else -1; *out_ci gets the class. */
static int sg_single_new_write(Compiler *c, const char *name, int is_const,
                               Scope *owner_scope, int *out_ci) {
  const NodeTable *nt = c->nt;
  int write = -1, ci = -1, count = 0;
  for (int w = 0; w < nt->count; w++) {
    NodeKind k = nt_kind(nt, w);
    if (is_const) { if (k != NK_ConstantWriteNode) continue; }
    else if (k != NK_LocalVariableWriteNode) continue;
    const char *wn = nt_str(nt, w, "name");
    if (!wn || !sp_streq(wn, name)) continue;
    if (!is_const && comp_scope_of(c, w) != owner_scope) continue;
    count++;
    write = w;
  }
  if (count != 1) return -1;
  ci = sg_new_class_ci(c, nt_ref(nt, write, "value"));
  if (ci < 0) return -1;
  *out_ci = ci;
  return write;
}

/* wnode -> synthesized subclass index map, so every singleton def/extend on
   one binding shares one subclass. comp_class_index cannot be trusted mid-pass
   (its index excludes classes added during this pass). */
typedef struct { int *wkey, *wci, n, cap, seq; } SgMap;
/* The subclass a binding currently ends at, or -1. */
static int sg_map_last(SgMap *m, int wnode) {
  for (int i = 0; i < m->n; i++) if (m->wkey[i] == wnode) return m->wci[i];
  return -1;
}

/* Add a LINK to the binding's singleton chain. Each extended module gets its
   own link so the modules stack the way CRuby's ancestry does -- B1's `tag`
   overrides A1's and its `super` reaches A1's, where one shared subclass kept
   whichever module got there first and dropped the rest. `is_singleton_of`
   keeps naming the ORIGINAL user class, which is what `class` and
   `instance_of?` answer; `parent` is the previous link. */
static int sg_chain_link(Compiler *c, SgMap *m, int wnode, int parent_ci) {
  int prev = sg_map_last(m, wnode);
  /* `parent_ci` is re-derived from the `.new` receiver, which an earlier link
     already retargeted -- chase back to the class the PROGRAM wrote, which is
     what `class` and `instance_of?` answer and what the names read from. */
  int orig = parent_ci;
  while (orig >= 0 && orig < c->nclasses && c->classes[orig].is_singleton_of)
    orig = c->classes[orig].is_singleton_of - 1;
  const NodeTable *nt = c->nt;
  char snm[128];
  snprintf(snm, sizeof snm, "%s__sg_%s_%d",
           c->classes[orig].name ? c->classes[orig].name : "Obj", comp_node_tag(c, wnode), m->seq++);
  ClassInfo *sc = comp_class_new(c, snm, wnode);
  int newci = (int)(sc - c->classes);
  sc->parent = prev >= 0 ? prev : orig;
  sc->is_singleton_of = orig + 1;
  /* the binding's type is the LAST link: retarget on every addition */
  { int wval = nt_ref(nt, wnode, "value");
    int wrecv = nt_ref(nt, wval, "receiver");
    nt_node_set_str((NodeTable *)nt, wrecv, "name", snm); }
  for (int i = 0; i < m->n; i++) if (m->wkey[i] == wnode) { m->wci[i] = newci; return newci; }
  if (m->n >= m->cap) {
    m->cap = m->cap ? m->cap * 2 : 8;
    m->wkey = realloc(m->wkey, sizeof(int) * (size_t)m->cap);
    m->wci = realloc(m->wci, sizeof(int) * (size_t)m->cap);
  }
  m->wkey[m->n] = wnode; m->wci[m->n] = newci; m->n++;
  return newci;
}

static int sg_get_or_make(Compiler *c, SgMap *m, int wnode, int parent_ci) {
  int cur = sg_map_last(m, wnode);
  if (cur >= 0) return cur;
  const NodeTable *nt = c->nt;
  char snm[128];
  snprintf(snm, sizeof snm, "%s__sg_%s", c->classes[parent_ci].name ? c->classes[parent_ci].name : "Obj", comp_node_tag(c, wnode));
  ClassInfo *sc = comp_class_new(c, snm, wnode);
  int newci = (int)(sc - c->classes);
  sc->parent = parent_ci;
  sc->is_singleton_of = parent_ci + 1;
  /* retarget the `= Parent.new(...)` receiver to the synthesized class so the
     binding's type becomes ty_object(newci) and .new builds it. */
  int wval = nt_ref(nt, wnode, "value");
  int wrecv = nt_ref(nt, wval, "receiver");
  nt_node_set_str((NodeTable *)nt, wrecv, "name", snm);
  if (m->n >= m->cap) { m->cap = m->cap ? m->cap * 2 : 8; m->wkey = realloc(m->wkey, sizeof(int) * (size_t)m->cap); m->wci = realloc(m->wci, sizeof(int) * (size_t)m->cap); }
  m->wkey[m->n] = wnode; m->wci[m->n] = newci; m->n++;
  return newci;
}

/* The binding a singleton node targets: fills *is_const / *rn / *owner (the
   enclosing scope, for a local) and returns the receiver node, or -1. */
static int sg_binding(Compiler *c, int id, int recv, int *is_const, const char **rn, Scope **owner) {
  const NodeTable *nt = c->nt;
  if (recv < 0) return -1;
  NodeKind rk = nt_kind(nt, recv);
  if (rk != NK_ConstantReadNode && rk != NK_LocalVariableReadNode) return -1;
  *is_const = (rk == NK_ConstantReadNode);
  *rn = nt_str(nt, recv, "name");
  if (!*rn) return -1;
  if (*is_const && comp_class_index(c, *rn) >= 0) return -1;  /* class method */
  /* the local's binding scope is the node's ENCLOSING scope (the receiver read
     is walked under the method/call scope, so its own nscope is wrong). */
  *owner = *is_const ? NULL : comp_scope_of(c, id);
  return recv;
}

static void class_note_included_mod(Compiler *c, int ci, int mod_ci) {
  ClassInfo *cif = &c->classes[ci];
  for (int m = 0; m < cif->nincluded_mods; m++)
    if (cif->included_mods[m] == mod_ci) return;
  if (cif->nincluded_mods >= cif->cincluded_mods) {
    cif->cincluded_mods = cif->cincluded_mods ? cif->cincluded_mods * 2 : 4;
    int *nm = realloc(cif->included_mods, sizeof(int) * (size_t)cif->cincluded_mods);
    if (!nm) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    cif->included_mods = nm;
  }
  cif->included_mods[cif->nincluded_mods++] = mod_ci;
}

/* Copy module `mod_ci`'s instance methods onto subclass `newci` (obj.extend). */
static void sg_transplant_module(Compiler *c, int mod_ci, int newci) {
  const NodeTable *nt = c->nt;
  /* Record the membership the way `include` does, so the extended object
     answers is_a?(Mod) -- it reported false, because only the methods were
     transplanted and nothing said the synthesized subclass was a member
     (#4080). */
  class_note_included_mod(c, newci, mod_ci);
  int snap = c->nscopes;
  for (int ms = 0; ms < snap; ms++) {
    Scope *src = &c->scopes[ms];
    if (src->class_id != mod_ci || src->is_cmethod || !src->name) continue;
    if (comp_method_in_class(c, newci, src->name) >= 0) continue;
    Scope *dst = comp_scope_new(c, src->name, src->def_node);
    int dst_idx = (int)(dst - c->scopes);
    src = &c->scopes[ms];   /* comp_scope_new may realloc */
    /* Clone + re-walk the module body under the subclass so implicit-self
       calls (and ivar reads) re-attribute to it, not the module -- exactly
       what `include` does (a shared body would resolve `name` against the
       module). */
    if (src->body >= 0) {
      int nb = nt_clone_subtree((NodeTable *)nt, src->body);
      if (nb >= 0) {
        comp_grow_node_arrays(c);
        src = &c->scopes[ms]; dst = &c->scopes[dst_idx];
        dst->body = nb;
        walk_scope(c, nb, dst_idx, newci);
      }
      else dst->body = src->body;
    }
    dst->class_id = newci;
    dst->is_cmethod = 0;    /* an instance method of the singleton subclass */
    dst->reachable = src->reachable;
    dst->yields = src->yields;
    dst->nrequired = src->nrequired;
    dst->rest_idx = src->rest_idx;
    dst->npost_rest = src->npost_rest;
    dst->kwrest_idx = src->kwrest_idx;
    src->is_transplanted_source = 1;   /* the module original is copied away */
    dst->origin_module_ci = mod_ci + 1;  /* #owner names the module (#3662) */
    if (src->blk_param) dst->blk_param = strdup(src->blk_param);
    dst->nparams = src->nparams;
    if (src->nparams > 0) {
      dst->pnames = malloc(sizeof(char *) * (size_t)src->nparams);
      dst->pdefault = malloc(sizeof(int) * (size_t)src->nparams);
      for (int p = 0; p < src->nparams; p++) {
        dst->pnames[p] = src->pnames[p] ? strdup(src->pnames[p]) : NULL;
        dst->pdefault[p] = src->pdefault ? src->pdefault[p] : -1;
      }
      /* Give the copy its own parameter LOCALS, carrying the original's types.
         Walking the cloned body only interns a parameter the body READS, and a
         method that just hands its parameter to `super` reads none of them
         (`def render(text) = "[#{super}]"`). The caller then found no slot to
         take the parameter's type from and passed the argument's own, which
         the emitted signature -- built from the same missing slot as poly --
         did not accept (#3951). */
      for (int p = 0; p < src->nparams; p++) {
        if (!dst->pnames[p]) continue;
        LocalVar *sp_lv = scope_local(src, dst->pnames[p]);
        LocalVar *dp = scope_local_intern(dst, dst->pnames[p]);
        if (!dp) continue;
        dp->is_param = 1;
        if (sp_lv && dp->type == TY_UNKNOWN) dp->type = sp_lv->type;
        src = &c->scopes[ms]; dst = &c->scopes[dst_idx];  /* intern may realloc */
      }
      scope_own_defaults(c, dst_idx);
    }
  }
}

/* Stamp the node that BRINGS a synthesized singleton subclass into being with
   that subclass's index. Codegen emits the runtime activation there (the object
   carries its parent's cls_id until then, #4084) and cannot re-derive which
   subclass from the receiver: a `def obj.m` receiver is not an ordinary
   expression and carries no inferred type, which is the same reason the dsm
   call already needed its own mark. */
static void sg_mark_activation(NodeTable *nt, int id, int newci) {
  char buf[16];
  snprintf(buf, sizeof buf, "%d", newci);
  nt_node_set_str(nt, id, "sg_activates", buf);
}

/* Does this `def <recv>.m` body need a `self` -- an ivar of its own, or the
   receiver itself? Without a synthesized subclass the def falls through to the
   ordinary emitter, which gives it no self: such a body then reads its @ivars
   as the ENCLOSING class's and names an undeclared `self`. A body that touches
   neither compiles as a plain function, dead or not, which is what several
   ruby/spec examples rely on -- so only the ones that cannot are refused. */
static int sg_def_needs_self(Compiler *c, int def_id) {
  const NodeTable *nt = c->nt;
  int sc = -1;
  for (int s = 0; s < c->nscopes; s++)
    if (c->scopes[s].def_node == def_id) { sc = s; break; }
  if (sc < 0) return 0;
  for (int n = 0; n < nt->count && n < c->node_cap; n++) {
    if (c->nscope[n] != sc) continue;
    NodeKind k = nt_kind(nt, n);
    if (k == NK_SelfNode ||
        k == NK_InstanceVariableReadNode || k == NK_InstanceVariableWriteNode ||
        k == NK_InstanceVariableOperatorWriteNode ||
        k == NK_InstanceVariableOrWriteNode || k == NK_InstanceVariableAndWriteNode ||
        k == NK_InstanceVariableTargetNode)
      return 1;
  }
  return 0;
}

void register_singleton_defs(Compiler *c) {
  /* not const: the pass marks a resolved dsm call on the node itself */
  NodeTable *nt = (NodeTable *)c->nt;
  SgMap m = {0};
  /* Find `def <recv>.m`, `<recv>.define_singleton_method(:m){}`, and
     `<recv>.extend(Mod)` whose receiver is a constant or local that statically
     holds one user object. Group by the binding so they share one synthesized
     subclass; a non-traceable receiver keeps today's behavior. */
  for (int id = 0; id < nt->count; id++) {
    NodeKind idk = nt_kind(nt, id);
    int recv, is_extend = 0, is_dsm = 0, is_scls = 0;
    if (idk == NK_DefNode) {
      recv = nt_ref(nt, id, "receiver");
    }
    else if (idk == NK_CallNode) {
      const char *cn = nt_str(nt, id, "name");
      if (cn && sp_streq(cn, "define_singleton_method")) {
        recv = nt_ref(nt, id, "receiver"); is_dsm = 1;
        /* only a real dsm with a block is a singleton def */
        if (nt_ref(nt, id, "block") < 0) continue;
      }
      else if (cn && sp_streq(cn, "extend")) { recv = nt_ref(nt, id, "receiver"); is_extend = 1; }
      else continue;
    }
    else if (idk == NK_SingletonClassNode) {
      /* `class << obj; def m; ...; end; end` -- the block form of def obj.m.
         `class << self` / `class << <Class>` are class-method sugar handled in
         walk_scope; only an instance const/local receiver routes here. */
      recv = nt_ref(nt, id, "expression"); is_scls = 1;
    }
    else continue;

    int is_const = 0; const char *rn = NULL; Scope *owner = NULL;
    if (sg_binding(c, id, recv, &is_const, &rn, &owner) < 0) continue;
    int parent_ci = -1;
    int wnode = sg_single_new_write(c, rn, is_const, owner, &parent_ci);
    if (wnode < 0) {
      /* Not traceable to one `new` of a user class, so there is no subclass to
         synthesize. A `def <recv>.m` then fell through to the ordinary def
         emitter, which has no singleton to attach it to: the body came out as
         a plain function of the enclosing scope, with `self` undeclared and
         its `@ivars` read as the enclosing class's. Say so here rather than
         letting the C compiler report it against generated code (#4169).
         extend / define_singleton_method keep their own fallbacks. */
      if (idk == NK_DefNode && !is_extend && !is_dsm && !is_scls &&
          sg_def_needs_self(c, id))
        unsupported_feature(c, id, "singleton method that needs a self, on a "
                                   "receiver that is not one user-class instance");
      continue;   /* not statically traceable: leave as today */
    }
    /* a user-defined method of the singleton name is that method, not the
       machinery (#2652). */
    if (is_dsm && comp_method_in_chain(c, parent_ci, "define_singleton_method", NULL) >= 0) continue;

    if (is_extend) {
      /* A user-defined `extend`/`define_singleton_method` of the same name is
         that method, not the singleton machinery (#2652) -- leave it alone. */
      if (comp_method_in_chain(c, parent_ci, "extend", NULL) >= 0) continue;
      /* Every argument must be a known module/class constant; otherwise this is
         not a compile-time-resolvable extend (a runtime value, an Integer). */
      int anode = nt_ref(nt, id, "arguments");
      int an = 0; const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
      if (an == 0) continue;
      int all_mods = 1;
      for (int j = 0; j < an; j++) {
        NodeKind ak = nt_kind(nt, args[j]);
        if ((ak != NK_ConstantReadNode && ak != NK_ConstantPathNode) ||
            comp_class_index(c, nt_str(nt, args[j], "name")) < 0) { all_mods = 0; break; }
      }
      if (!all_mods) continue;
      /* One link per module, in argument order, so `extend(A, B)` leaves B
         nearest the object -- CRuby's ancestry. The statement activates the
         last link; the earlier ones are reached through it. */
      /* `extend(A, B)` leaves A nearest the object (CRuby inserts the list so
         the FIRST argument ends up closest), so build the links back to front:
         the last one made is the one the binding points at. */
      int newci = -1;
      for (int j = an - 1; j >= 0; j--) {
        newci = sg_chain_link(c, &m, wnode, parent_ci);
        sg_transplant_module(c, comp_class_index(c, nt_str(nt, args[j], "name")), newci);
      }
      sg_mark_activation(nt, id, newci);
      continue;
    }
    int newci = sg_get_or_make(c, &m, wnode, parent_ci);
    sg_mark_activation(nt, id, newci);
    if (is_scls) {
      /* reattach every DefNode in the singleton-class body. The generic walk
         already created each as a (receiverless) scope; only class_id/is_cmethod
         need flipping, exactly like def obj.m. */
      int sbody = nt_ref(nt, id, "body");
      int bn = 0; const int *stmts = sbody >= 0 ? nt_arr(nt, sbody, "body", &bn) : NULL;
      for (int k = 0; k < bn; k++) {
        if (nt_kind(nt, stmts[k]) != NK_DefNode) continue;
        for (int ds = 1; ds < c->nscopes; ds++) {
          if (c->scopes[ds].def_node == stmts[k]) {
            c->scopes[ds].class_id = newci;
            c->scopes[ds].is_cmethod = 0;
            break;
          }
        }
      }
      continue;
    }
    /* def / dsm: reattach the scope whose def_node == id to the subclass. */
    for (int ds = 1; ds < c->nscopes; ds++) {
      if (c->scopes[ds].def_node == id) {
        c->scopes[ds].class_id = newci;
        c->scopes[ds].is_cmethod = 0;
        break;
      }
    }
    /* Mark the dsm CALL as compile-time resolved. The emitter cannot re-derive
       this from the receiver's type: a local that also carries a `def obj.m`
       widens to poly, and reading it back as "not a singleton subclass" sent
       the call to the unsupported-feature diagnostic, which stopped the build
       on a form the constant spelling compiles. */
    if (is_dsm) nt_node_set_str(nt, id, "sg_resolved", "1");
  }
  free(m.wkey); free(m.wci);
}

/* Stamp every ConstantWriteNode with the index of the class or module whose
   body lexically encloses it. `Const = Struct.new(...)` defines a class just
   as `class Const` does, but it arrives here as a plain constant write with no
   scope around it, so the class was created with enclosing_class == -1 and
   class_ruby_name -- which builds the Ruby-visible name by walking that link
   -- answered the bare leaf. `Probe::Block.name` was "Block" where a declared
   `class Block` in the same module answered "Probe::Block" (#4271).
   Registration keys on the leaf name either way; only the visible name moves.
   Runs after walk_scope, so the enclosing module already has its class. */
static void stamp_const_write_encl(Compiler *c, int id, int ci) {
  const NodeTable *nt = c->nt;
  if (id < 0) return;
  const char *ty = nt_type(nt, id);
  if (!ty) return;
  if (sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) {
    int cp = nt_ref(nt, id, "constant_path");
    const char *mn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    int mci = mn ? comp_class_index(c, mn) : -1;
    if (mci >= 0) ci = mci;
  }
  else if (sp_streq(ty, "ConstantWriteNode") && ci >= 0)
    nt_node_set_int((NodeTable *)nt, id, "sg_encl_class", ci);
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) stamp_const_write_encl(c, nt_ref_at(nt, id, i), ci);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int m = 0; const int *ids = nt_arr_at(nt, id, i, &m);
    for (int k = 0; k < m; k++) stamp_const_write_encl(c, ids[k], ci);
  }
}

void register_structs(Compiler *c) {
  const NodeTable *nt = c->nt;
  stamp_const_write_encl(c, nt->root_id, -1);
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    /* Const = Struct.new(:a, :b) */
    if (sp_streq(ty, "ConstantWriteNode")) {
      const char *cname = nt_str(nt, id, "name");
      int val = nt_ref(nt, id, "value");
      if (!cname || !is_c_ident(cname) || !is_struct_call(c, val)) continue;
      int ci = comp_class_index(c, cname);
      if (ci >= 0) {
        /* A `class D` reopening this constant pre-created a memberless class
           (walk_scope runs first and does not know D is a Struct/Data), so its
           methods could not resolve the generated readers. Register the members
           onto that existing class instead of skipping it; guard against
           re-registering an already-populated Struct/Data class. */
        ClassInfo *ex = &c->classes[ci];
        if (!ex->is_struct && !ex->is_data)
          register_struct_members(c, ex, val);
      }
      else {
        ClassInfo *ni = comp_class_new(c, cname, id);
        int encl = (int)nt_int(nt, id, "sg_encl_class", -1);
        if (encl >= 0 && encl < c->nclasses) ni->enclosing_class = encl;
        register_struct_members(c, ni, val);
      }
    }
    /* k = Struct.new(:a, :b): an anonymous struct class held in a local.
       Synthesize a uniquely named class keyed to the WRITE node (def_node);
       class_var_static_ci resolves the local's reads to it, so .new/.members
       and the member accessors dispatch like the constant form. #inspect
       omits the synthetic name (CRuby shows `#<struct a=1, b=2>`). */
    else if (sp_streq(ty, "LocalVariableWriteNode")) {
      int val = nt_ref(nt, id, "value");
      if (!is_struct_call(c, val)) continue;
      char an[48];
      snprintf(an, sizeof an, "StructAnon_%s", comp_node_tag(c, id));
      ClassInfo *cls = comp_class_new(c, an, id);
      cls->is_anon_struct = 1;
      c->anon_struct_ids_valid = 0;
      register_struct_members(c, cls, val);
    }
    /* Inline `Data.define(...).method(...)` / `Struct.new(...).method(...)`: the
       struct/data call is the receiver of another call, with no name to hold it.
       Synthesize an anon class keyed to the call node itself so the receiver
       resolution (.new / .members / .class) can find it. #2682 */
    else if (sp_streq(ty, "CallNode") && is_struct_call(c, id)) {
      /* ...or used as a VALUE anywhere else (`p Struct.new(:a)`, an element,
         a return): the class still has to exist to be printed or passed, and
         with none registered the call reported `Struct.new` as undefined
         (#4031). Only a call some name already holds is skipped -- the
         constant and local arms above register those, keyed by the write. */
      int is_recv = 0, is_write_value = 0;
      for (int p = 0; p < nt->count && !is_recv; p++)
        if (nt_kind(nt, p) == NK_CallNode && nt_ref(nt, p, "receiver") == id) is_recv = 1;
      for (int p = 0; p < nt->count && !is_write_value; p++) {
        NodeKind pk = nt_kind(nt, p);
        if ((pk == NK_ConstantWriteNode || pk == NK_LocalVariableWriteNode ||
             pk == NK_ClassNode) &&
            (nt_ref(nt, p, "value") == id || nt_ref(nt, p, "superclass") == id))
          is_write_value = 1;
      }
      if (is_recv || !is_write_value) {
        char an[48];
        snprintf(an, sizeof an, "StructAnon_%s", comp_node_tag(c, id));
        ClassInfo *cls = comp_class_new(c, an, id);
        cls->is_anon_struct = 1;
        c->anon_struct_ids_valid = 0;
        register_struct_members(c, cls, id);
      }
    }
    /* class X < Struct.new(:a, :b); ... end */
    else if (sp_streq(ty, "ClassNode")) {
      int sup = nt_ref(nt, id, "superclass");
      if (!is_struct_call(c, sup)) continue;
      int cp = nt_ref(nt, id, "constant_path");
      const char *cname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
      int ci = cname ? comp_class_index(c, cname) : -1;
      if (ci >= 0) register_struct_members(c, &c->classes[ci], sup);
    }
  }
}

/* Fix scope class_id for DefNodes inside Struct.new { } blocks.
   walk_scope runs before register_structs, so defs in struct blocks get
   class_id=-1. This pass corrects them after the class is registered. */
void fix_struct_block_scopes(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    NodeKind wk = nt_kind(nt, id);
    if (wk != NK_ConstantWriteNode && wk != NK_LocalVariableWriteNode) continue;
    int bbody = class_def_body(c, id);
    if (bbody < 0) continue;
    /* `k = Struct.new(...) do ... end` held in a local: register_structs
       named its class after the write */
    char an[48];
    const char *cname = nt_str(nt, id, "name");
    if (wk == NK_LocalVariableWriteNode) { snprintf(an, sizeof an, "StructAnon_%s", comp_node_tag(c, id)); cname = an; }
    int ci = cname ? comp_class_index(c, cname) : -1;
    if (ci < 0) continue;
    int bn = 0;
    const int *stmts = nt_arr(nt, bbody, "body", &bn);
    for (int k = 0; k < bn; k++) {
      const char *sty = nt_type(nt, stmts[k]);
      if (!sty) continue;
      /* `class << self` in the block: walk_scope already made its defs class
         methods, but of no class (the block had none yet), so they were
         missing from the struct and a bare `new` in one had no receiver
         (#4823). Home them the same way. */
      if (sp_streq(sty, "SingletonClassNode")) {
        int sx = nt_ref(nt, stmts[k], "expression");
        if (sx < 0 || nt_kind(nt, sx) != NK_SelfNode) continue;
        int sb = nt_ref(nt, stmts[k], "body");
        int sn = 0;
        const int *sst = sb >= 0 ? nt_arr(nt, sb, "body", &sn) : NULL;
        for (int j = 0; j < sn; j++) {
          if (nt_kind(nt, sst[j]) != NK_DefNode) continue;
          for (int s = 0; s < c->nscopes; s++)
            if (c->scopes[s].def_node == sst[j] && c->scopes[s].is_cmethod) {
              c->scopes[s].class_id = ci;
              break;
            }
        }
        continue;
      }
      int dn = stmts[k];
      /* `private def m` / `protected def m` / `public def m` */
      if (sp_streq(sty, "CallNode") && nt_ref(nt, dn, "receiver") < 0 && nt_ref(nt, dn, "block") < 0) {
        const char *vn = nt_str(nt, dn, "name");
        int va = nt_ref(nt, dn, "arguments");
        int vc = 0; const int *vv = va >= 0 ? nt_arr(nt, va, "arguments", &vc) : NULL;
        if (vn && (sp_streq(vn, "private") || sp_streq(vn, "protected") || sp_streq(vn, "public")) &&
            vc == 1 && nt_kind(nt, vv[0]) == NK_DefNode && nt_ref(nt, vv[0], "receiver") < 0)
          dn = vv[0];
      }
      if (nt_kind(nt, dn) != NK_DefNode) continue;
      /* Find the scope whose def_node == dn and fix its class_id */
      for (int s = 0; s < c->nscopes; s++) {
        if (c->scopes[s].def_node == dn) {
          c->scopes[s].class_id = ci;
          break;
        }
      }
    }
  }
}

/* Process attr_accessor/reader/writer call: register ivars + reader/writer names.
   If `singleton` is non-zero, registers singleton (class-level) accessors instead. */
void register_attr_call(Compiler *c, ClassInfo *cls, int s, int singleton) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, s, "name");
  if (!nm) return;
  int accessor = sp_streq(nm, "attr_accessor") ||
                 sp_streq(nm, "attribute") || sp_streq(nm, "attributes");
  int reader = sp_streq(nm, "attr_reader") || accessor || sp_streq(nm, "attr");
  int writer = sp_streq(nm, "attr_writer") || accessor;
  if (!reader && !writer) return;
  int args = nt_ref(nt, s, "arguments");
  int an = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  for (int a = 0; a < an; a++) {
    const char *aty = nt_type(nt, argv[a]);
    if (!aty || !sp_streq(aty, "SymbolNode")) continue;
    const char *base = nt_str(nt, argv[a], "value");
    if (!base) continue;
    if (singleton) {
      if (reader) comp_add_sg_reader(cls, base);
      if (writer) comp_add_sg_writer(cls, base);
    }
else {
      char ivname[256];
      snprintf(ivname, sizeof ivname, "@%s", base);
      comp_ivar_intern(cls, ivname);
      if (reader) comp_add_reader(cls, base);
      if (writer) comp_add_writer(cls, base);
    }
  }
}

static int cmethod_names_ivar(Compiler *c, int ci, const char *ivname) {
  const NodeTable *nt = c->nt;
  static const NodeKind kinds[] = { NK_InstanceVariableReadNode, NK_InstanceVariableWriteNode,
                                    NK_InstanceVariableOrWriteNode, NK_InstanceVariableAndWriteNode,
                                    NK_InstanceVariableOperatorWriteNode };
  for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++)
    NT_FOREACH_KIND(nt, kinds[k], id) {
      int si = c->nscope[id];
      const char *nm = nt_str(nt, id, "name");
      if (si >= 0 && si < c->nscopes && c->scopes[si].is_cmethod &&
          c->scopes[si].class_id == ci && nm && sp_streq(nm, ivname)) return 1;
    }
  return 0;
}

/* An attr call whose value the class body uses (`r = attr_reader :a`,
   `p(attr_accessor :b)`) declares its methods as the bare statement does.
   Nested bodies with their own self are not walked. */
static void register_attrs_in_value(Compiler *c, ClassInfo *cls, int node, int singleton) {
  const NodeTable *nt = c->nt;
  if (node < 0) return;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode ||
      k == NK_BlockNode || k == NK_LambdaNode)
    return;
  if (k == NK_CallNode && nt_ref(nt, node, "receiver") < 0) register_attr_call(c, cls, node, singleton);
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) register_attrs_in_value(c, cls, nt_ref_at(nt, node, i), singleton);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int m = 0; const int *ids = nt_arr_at(nt, node, i, &m);
    for (int j = 0; j < m; j++) register_attrs_in_value(c, cls, ids[j], singleton);
  }
}

/* Collect attr_reader/attr_writer/attr_accessor declarations in class
   bodies, registering backing ivars + reader/writer method names.
   Also scans class << self bodies for singleton-level attr_accessors. */
void register_attrs_body(Compiler *c, ClassInfo *cls, int body) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
  for (int k = 0; k < n; k++) {
    int s = stmts[k];
    const char *sty = nt_type(nt, s);
    if (!sty) continue;
    if (sp_streq(sty, "CallNode")) {
      register_attr_call(c, cls, s, 0);
      /* `private attr_writer :x` declares the writer too: the visibility
         call's argument is the attr call (#4922) */
      const char *vn = nt_str(nt, s, "name");
      if (vn && nt_ref(nt, s, "receiver") < 0 &&
          (sp_streq(vn, "private") || sp_streq(vn, "protected") || sp_streq(vn, "public"))) {
        int va = nt_ref(nt, s, "arguments");
        int vc = 0; const int *vv = va >= 0 ? nt_arr(nt, va, "arguments", &vc) : NULL;
        for (int q = 0; q < vc; q++)
          if (nt_kind(nt, vv[q]) == NK_CallNode) register_attr_call(c, cls, vv[q], 0);
      }
      /* `p(attr_reader :a)` */
      int ca = nt_ref(nt, s, "arguments");
      int cn = 0; const int *cv = ca >= 0 ? nt_arr(nt, ca, "arguments", &cn) : NULL;
      for (int q = 0; q < cn; q++) register_attrs_in_value(c, cls, cv[q], 0);
    }
    else if (sp_streq(sty, "LocalVariableWriteNode") || sp_streq(sty, "ConstantWriteNode") ||
             sp_streq(sty, "InstanceVariableWriteNode") || sp_streq(sty, "ClassVariableWriteNode") ||
             sp_streq(sty, "GlobalVariableWriteNode"))
      register_attrs_in_value(c, cls, nt_ref(nt, s, "value"), 0);
    else if (sp_streq(sty, "SingletonClassNode")) {
      /* class << self; attr_accessor :x; end */
      int sbody = nt_ref(nt, s, "body");
      if (sbody < 0) continue;
      int sn = 0;
      const int *sstmts = nt_arr(nt, sbody, "body", &sn);
      for (int j = 0; j < sn; j++) {
        int ss = sstmts[j];
        const char *ssty = nt_type(nt, ss);
        if (ssty && sp_streq(ssty, "CallNode"))
          register_attr_call(c, cls, ss, 1);
        else if (ssty && sp_streq(ssty, "LocalVariableWriteNode"))
          register_attrs_in_value(c, cls, nt_ref(nt, ss, "value"), 1);
      }
      /* An accessor whose name the CLASS BODY also assigns as `@x` names the
         class-level ivar, which is where `def self.m; @x; end` reads too: mark
         it so both spellings share civ_<Class>_<x> (#3776). */
      for (int j = 0; j < cls->nsg_readers + cls->nsg_writers; j++) {
        const char *base = j < cls->nsg_readers ? cls->sg_readers[j]
                                                : cls->sg_writers[j - cls->nsg_readers];
        char ivname[256];
        snprintf(ivname, sizeof ivname, "@%s", base);
        for (int k2 = 0; k2 < n; k2++) {
          const char *wty = nt_type(nt, stmts[k2]);
          if (!wty || !sp_streq(wty, "InstanceVariableWriteNode")) continue;
          const char *wnm = nt_str(nt, stmts[k2], "name");
          if (wnm && sp_streq(wnm, ivname)) { comp_add_sg_civ(cls, base); break; }
        }
        if (!comp_is_sg_civ(cls, base) && cmethod_names_ivar(c, (int)(cls - c->classes), ivname))
          comp_add_sg_civ(cls, base);
      }
    }
  }
}

void register_attrs(Compiler *c) {
  int *bci, *bnode;
  int nb = class_body_list(c, &bci, &bnode);
  for (int b = 0; b < nb; b++) register_attrs_body(c, &c->classes[bci[b]], bnode[b]);
  free(bci);
  free(bnode);
}

/* Classify a modifier/if predicate as a compile-time constant: 1 = always
   truthy, 0 = always falsy, -1 = a runtime value. Only literal true / non-nil
   literals and false / nil fold; anything else (a call, constant, variable) is
   runtime. */
static int alias_pred_const(const NodeTable *nt, int pred) {
  if (pred < 0) return -1;
  const char *t = nt_type(nt, pred);
  if (!t) return -1;
  /* unwrap parentheses: `if (cond)`. An empty `()` is nil (falsy); multiple
     statements are not folded (conservative). */
  while (sp_streq(t, "ParenthesesNode")) {
    int stmts = nt_ref(nt, pred, "body");  /* ParenthesesNode -> StatementsNode */
    int n = 0;
    const int *body = stmts >= 0 ? nt_arr(nt, stmts, "body", &n) : NULL;
    if (n == 0 || !body) return 0;
    if (n != 1) return -1;
    pred = body[0];
    if (pred < 0) return -1;
    t = nt_type(nt, pred);
    if (!t) return -1;
  }
  if (sp_streq(t, "FalseNode") || sp_streq(t, "NilNode")) return 0;
  if (sp_streq(t, "TrueNode") || sp_streq(t, "IntegerNode") || sp_streq(t, "FloatNode") ||
      sp_streq(t, "StringNode") || sp_streq(t, "SymbolNode") || sp_streq(t, "ArrayNode") ||
      sp_streq(t, "HashNode") || sp_streq(t, "RegularExpressionNode"))
    return 1;
  return -1;
}

/* Collect `alias new old` (AliasMethodNode) and `alias_method :new, :old`
   (CallNode) statements in class bodies into the class alias table. */
/* A name an alias bound while a `def` of that same name is still to come in
   the class: up to that def the name means the aliased body, after it the def
   does, so the alias table (which would outlive the def) cannot hold it. */
typedef struct { int cid; char *name; char *target; int until; } AliasPending;
static AliasPending *g_alias_pending;
static int g_nalias_pending, g_calias_pending;

/* The first `def name` of class cid after node `at`, or -1. Matched by the
   DefNode's own name, which outlives a capture's rename of the scope. */
static int alias_def_after(Compiler *c, int cid, const char *name, int at) {
  const NodeTable *nt = c->nt;
  int first = -1;
  for (int si = 1; si < c->nscopes; si++) {
    Scope *sc = &c->scopes[si];
    if (sc->class_id != cid || sc->is_cmethod || sc->def_node <= at) continue;
    if (nt_kind(nt, sc->def_node) != NK_DefNode) continue;
    const char *dn = nt_str(nt, sc->def_node, "name");
    if (dn && sp_streq(dn, name) && (first < 0 || sc->def_node < first)) first = sc->def_node;
  }
  return first;
}

static const char *alias_pending_target(int cid, const char *name, int at) {
  const char *t = NULL;
  for (int i = 0; i < g_nalias_pending; i++) {
    AliasPending *ap = &g_alias_pending[i];
    if (ap->cid == cid && at < ap->until && sp_streq(ap->name, name)) t = ap->target;
  }
  return t;
}

/* Bind alias `nw` to method name `target`: an alias-table entry, or, when the
   class defines `nw` again later, a pending binding that ends at that def. */
static void alias_bind(Compiler *c, ClassInfo *cls, int cid, const char *nw, const char *target, int alias_node) {
  int redef = alias_def_after(c, cid, nw, alias_node);
  if (redef < 0) { comp_add_alias_from(cls, nw, target, alias_node); return; }
  if (g_nalias_pending >= g_calias_pending) {
    g_calias_pending = g_calias_pending ? g_calias_pending * 2 : 8;
    g_alias_pending = realloc(g_alias_pending, sizeof *g_alias_pending * (size_t)g_calias_pending);
    if (!g_alias_pending) { fprintf(stderr, "out of memory\n"); exit(1); }
  }
  AliasPending *ap = &g_alias_pending[g_nalias_pending++];
  ap->cid = cid; ap->name = strdup(nw); ap->target = strdup(target); ap->until = redef;
}

/* An alias captures the definition in effect where it appears. When the target
   is redefined LATER in the same body, a name mapping would resolve to the new
   definition, so the earlier one is renamed to the alias instead (#3737), and
   later aliases of that definition map to that name. When the class defines
   the alias's own name again later, the definition is renamed to
   `<name>#<n>` instead, a name no Ruby `def` can take, so redefining that
   alias leaves the other aliases on the captured body; the alias itself is
   bound only until its redefinition. Returns 1 when it registered the alias
   itself. */
static int alias_capture_earlier_def(Compiler *c, ClassInfo *cls,
                                     const char *nw, const char *od, int alias_node) {
  if (!nw || !od || !cls->name) return 0;
  int cid = comp_class_index(c, cls->name);
  if (cid < 0) return 0;
  const char *pending = alias_pending_target(cid, od, alias_node);
  if (pending) {
    char *pt = strdup(pending);
    alias_bind(c, cls, cid, nw, pt, alias_node);
    free(pt);
    return 1;
  }
  const NodeTable *nt = c->nt;
  int before = -1, after = 0;
  for (int si = 1; si < c->nscopes; si++) {
    Scope *sc = &c->scopes[si];
    if (sc->class_id != cid || sc->is_cmethod || sc->is_proc_form || !sc->name) continue;
    const char *dn = sc->def_node >= 0 && nt_kind(nt, sc->def_node) == NK_DefNode
                     ? nt_str(nt, sc->def_node, "name") : NULL;
    if (!sp_streq(sc->name, od) && !(dn && sp_streq(dn, od))) continue;
    if (sc->def_node >= 0 && sc->def_node < alias_node) {
      if (before < 0 || sc->def_node > c->scopes[before].def_node) before = si;
    }
    else after = 1;
  }
  if (before < 0 || !after) {
    if (alias_def_after(c, cid, nw, alias_node) < 0) return 0;
    alias_bind(c, cls, cid, nw, before >= 0 ? c->scopes[before].name : od, alias_node);
    return 1;
  }
  if (sp_streq(c->scopes[before].name, od)) {
    int nw_redef = alias_def_after(c, cid, nw, alias_node) >= 0;
    char hidden[512];
    snprintf(hidden, sizeof hidden, "%s#%s", od, comp_node_tag(c, alias_node));
    free(c->scopes[before].name);
    c->scopes[before].name = strdup(nw_redef ? hidden : nw);
    if (!nw_redef) return 1;
  }
  char *target = strdup(c->scopes[before].name);
  alias_bind(c, cls, cid, nw, target, alias_node);
  free(target);
  return 1;
}

/* The primitives whose reopen a call on a typed receiver -- or on self in
   the reopen -- is dispatched for, and so where an alias may capture the
   builtin method itself. */
static int alias_prim_class(const char *cn) {
  return cn && (sp_streq(cn, "String") || sp_streq(cn, "Integer") || sp_streq(cn, "Float") ||
                sp_streq(cn, "Symbol") || sp_streq(cn, "Time"));
}
/* Did the program define `od` in class cid -- a def, or an alias of that
   name -- before node `at`? */
static int alias_target_defined_before(Compiler *c, ClassInfo *cls, int cid, const char *od, int at) {
  const NodeTable *nt = c->nt;
  for (int si = 1; si < c->nscopes; si++) {
    Scope *sc = &c->scopes[si];
    if (sc->class_id != cid || sc->is_cmethod || sc->def_node < 0 || sc->def_node >= at) continue;
    const char *dn = nt_kind(nt, sc->def_node) == NK_DefNode ? nt_str(nt, sc->def_node, "name") : NULL;
    if ((sc->name && sp_streq(sc->name, od)) || (dn && sp_streq(dn, od))) return 1;
  }
  for (int i = 0; i < cls->naliases; i++)
    if (cls->alias_node[i] >= 0 && cls->alias_node[i] < at && sp_streq(cls->alias_new[i], od)) return 1;
  return 0;
}

static void alias_register(Compiler *c, ClassInfo *cls, const char *nw, const char *od, int s) {
  if (alias_capture_earlier_def(c, cls, nw, od, s)) return;
  comp_add_alias_from(cls, nw, od, s);
  /* In a reopened primitive, an alias of a name the program has not defined
     there yet names the builtin method: it keeps naming it when the class
     later defines or re-aliases that name (`alias_method :plus_without, :+`
     ahead of `alias_method :+, :plus_with`). */
  int cid = cls->name ? comp_class_index(c, cls->name) : -1;
  if (s >= 0 && cid >= 0 && alias_prim_class(cls->name) &&
      !alias_target_defined_before(c, cls, cid, od, s)) {
    for (int i = cls->naliases - 1; i >= 0; i--)
      if (cls->alias_node[i] == s && sp_streq(cls->alias_new[i], nw)) { cls->alias_builtin[i] = 1; break; }
  }
}

void register_aliases_body(Compiler *c, ClassInfo *cls, int body) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
  for (int k = 0; k < n; k++) {
    int s = stmts[k];
    const char *sty = nt_type(nt, s);
    if (!sty) continue;
    if (sp_streq(sty, "AliasMethodNode")) {
      int nn = nt_ref(nt, s, "new_name");
      int on = nt_ref(nt, s, "old_name");
      const char *nw = nn >= 0 ? nt_str(nt, nn, "value") : NULL;
      const char *od = on >= 0 ? nt_str(nt, on, "value") : NULL;
      alias_register(c, cls, nw, od, s);
    }
    else if (sp_streq(sty, "CallNode")) {
      const char *nm = nt_str(nt, s, "name");
      /* `private alias_method :a, :b` defines the alias it wraps */
      if (nm && nt_ref(nt, s, "receiver") < 0 &&
          (sp_streq(nm, "private") || sp_streq(nm, "protected") || sp_streq(nm, "public"))) {
        int pa = nt_ref(nt, s, "arguments");
        int pn = 0;
        const int *pv = pa >= 0 ? nt_arr(nt, pa, "arguments", &pn) : NULL;
        if (pn == 1 && nt_kind(nt, pv[0]) == NK_CallNode && nt_ref(nt, pv[0], "receiver") < 0) {
          s = pv[0];
          nm = nt_str(nt, s, "name");
        }
      }
      if (!nm || !sp_streq(nm, "alias_method")) continue;
      int args = nt_ref(nt, s, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an >= 2 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "SymbolNode") &&
          nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "SymbolNode"))
      { const char *anw = nt_str(nt, argv[0], "value"), *aod = nt_str(nt, argv[1], "value");
        alias_register(c, cls, anw, aod, s); }
    }
    else if (sp_streq(sty, "SingletonClassNode")) {
      /* `class << self; alias_method :a, :b; end` names a CLASS method; the
         alias table is consulted by the class-method lookup too, so register
         it the same way rather than leaving the name undefined (#3776) */
      register_aliases_body(c, cls, nt_ref(nt, s, "body"));
    }
    else if (sp_streq(sty, "IfNode") || sp_streq(sty, "UnlessNode")) {
      /* A statement modifier (`alias a b if cond`) wraps the alias in an IfNode;
         a full if/elsif/else chains through `subsequent`. An alias resolves at
         compile time, so register only the branch the conditions statically
         select, following the chain. A non-constant guard selects nothing: the
         alias cannot be created conditionally with static method tables, so the
         name is left unresolved and rejects loudly if used. */
      int curr = s;
      while (curr >= 0) {
        const char *cty = nt_type(nt, curr);
        if (!cty) break;
        if (sp_streq(cty, "ElseNode")) {
          register_aliases_body(c, cls, nt_ref(nt, curr, "statements"));
          break;
        }
        if (!sp_streq(cty, "IfNode") && !sp_streq(cty, "UnlessNode")) break;
        int is_unless = sp_streq(cty, "UnlessNode");
        int pc = alias_pred_const(nt, nt_ref(nt, curr, "predicate"));
        int then_runs = is_unless ? (pc == 0) : (pc == 1);
        int else_runs = is_unless ? (pc == 1) : (pc == 0);
        if (then_runs) { register_aliases_body(c, cls, nt_ref(nt, curr, "statements")); break; }
        if (else_runs) curr = nt_ref(nt, curr, is_unless ? "else_clause" : "subsequent");
        else break;  /* non-constant: select nothing */
      }
    }
  }
}

void register_aliases(Compiler *c) {
  const NodeTable *nt = c->nt;
  int *bci, *bnode;
  int nb = class_body_list(c, &bci, &bnode);
  for (int b = 0; b < nb; b++) register_aliases_body(c, &c->classes[bci[b]], bnode[b]);
  free(bci);
  free(bnode);
  /* Pass 3: the top level, whose methods live on the Toplevel pseudo-class.
     It is not a ClassNode, so neither pass above saw it and a top-level
     `alias b a` left b undefined (#3730). */
  {
    /* only when there IS one: creating the pseudo-class for every program
       shifts every class index and is not free */
    int have_tl_alias = 0;
    for (int pid = 0; pid < nt->count && !have_tl_alias; pid++) {
      const char *pty = nt_type(nt, pid);
      if (!pty || !sp_streq(pty, "ProgramNode")) continue;
      int sb = nt_ref(nt, pid, "statements");
      int sn = 0; const int *ss = sb >= 0 ? nt_arr(nt, sb, "body", &sn) : NULL;
      for (int k = 0; k < sn; k++) {
        const char *sty2 = nt_type(nt, ss[k]);
        if (sty2 && sp_streq(sty2, "AliasMethodNode")) { have_tl_alias = 1; break; }
      }
    }
    if (have_tl_alias) {
      int tl = comp_class_index(c, "Toplevel");
      if (tl < 0) { comp_class_new(c, "Toplevel", -1); tl = c->nclasses - 1; }
      for (int pid = 0; pid < nt->count; pid++) {
        const char *pty = nt_type(nt, pid);
        if (!pty || !sp_streq(pty, "ProgramNode")) continue;
        register_aliases_body(c, &c->classes[tl], nt_ref(nt, pid, "statements"));
      }
    }
  }
}

void register_undefs_body(Compiler *c, ClassInfo *cls, int body) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
  for (int k = 0; k < n; k++) {
    int s = stmts[k];
    const char *sty = nt_type(nt, s);
    if (!sty || !sp_streq(sty, "UndefNode")) continue;
    int names_n = 0;
    const int *names = nt_arr(nt, s, "names", &names_n);
    for (int j = 0; j < names_n; j++) {
      const char *mname = nt_str(nt, names[j], "value");
      if (mname) comp_add_undef(cls, mname);
    }
  }
}

void register_undefs(Compiler *c) {
  int *bci, *bnode;
  int nb = class_body_list(c, &bci, &bnode);
  for (int b = 0; b < nb; b++) register_undefs_body(c, &c->classes[bci[b]], bnode[b]);
  free(bci);
  free(bnode);
}

int is_c_ident(const char *s) {
  if (!s || !*s) return 0;
  for (const char *p = s; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '_')) return 0;
  return 1;
}

/* Register global variables ($g) and top-level constants (FOO). */
void register_globals_consts(Compiler *c) {
  const NodeTable *nt = c->nt;
  /* Pass 1: collect alias $copy $orig mappings first so pass 2 can skip them. */
  NT_FOREACH_KIND(nt, NK_AliasGlobalVariableNode, id) {
    int nw_id  = nt_ref(nt, id, "new_name");
    int old_id = nt_ref(nt, id, "old_name");
    const char *nw  = nw_id  >= 0 ? nt_str(nt, nw_id,  "name") : NULL;
    const char *old = old_id >= 0 ? nt_str(nt, old_id, "name") : NULL;
    if (nw && nw[0] == '$' && is_c_ident(nw + 1) &&
        old && old[0] == '$' && is_c_ident(old + 1)) {
      comp_gvar_intern(c, old + 1);             /* intern the original */
      comp_add_gvar_alias(c, nw + 1, old + 1); /* $new -> $old */
    }
  }
  /* `$-v` and `$-w` are the interpreter's other names for `$VERBOSE`, and
     `$-d` for `$DEBUG`: they read and write the one flag. */
  comp_add_gvar_alias(c, "-v", "VERBOSE");
  comp_add_gvar_alias(c, "-w", "VERBOSE");
  comp_add_gvar_alias(c, "-d", "DEBUG");
  /* Pass 2: intern all other globals (skipping alias names). */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (sp_streq(ty, "GlobalVariableWriteNode") || sp_streq(ty, "GlobalVariableReadNode") ||
        sp_streq(ty, "GlobalVariableOperatorWriteNode") || sp_streq(ty, "GlobalVariableTargetNode") ||
        sp_streq(ty, "GlobalVariableOrWriteNode") || sp_streq(ty, "GlobalVariableAndWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      /* skip alias names - they resolve to the original and need no separate slot */
      if (nm && nm[0] == '$' && is_c_ident(nm + 1) &&
          sp_streq(nm + 1, comp_resolve_gvar(c, nm + 1)))
        comp_gvar_intern(c, nm + 1);
      else if (nm && nm[0] == '$' && nm[1] == '-' && !sp_streq(nm + 1, comp_resolve_gvar(c, nm + 1)))
        comp_gvar_intern(c, comp_resolve_gvar(c, nm + 1));
    }
    else if (sp_streq(ty, "AliasGlobalVariableNode")) {
      /* already handled in pass 1 */
    }
    else if (sp_streq(ty, "ConstantTargetNode") || sp_streq(ty, "ConstantPathTargetNode")) {
      /* target in a multi-write: A, B = expr (a definite write); a path
         target (`Mod::A, ::B = ...`) interns its leaf name flat, as the
         path write does */
      const char *nm = nt_str(nt, id, "name");
      if (nm && is_c_ident(nm) && comp_class_index(c, nm) < 0)
        comp_const_intern(c, nm)->const_def_write = 1;
    }
    else if (sp_streq(ty, "ConstantPathWriteNode") || sp_streq(ty, "ConstantPathOrWriteNode") ||
             sp_streq(ty, "ConstantPathAndWriteNode") || sp_streq(ty, "ConstantPathOperatorWriteNode")) {
      /* `Mod::X = v` / `Mod::X ||= v`: register the leaf constant by name so it
         gets a runtime slot. The module path is not modeled as a namespace; the
         leaf name is interned flat like a top-level constant. */
      int tgt = nt_ref(nt, id, "target");
      const char *nm = tgt >= 0 ? nt_str(nt, tgt, "name") : NULL;
      if (nm && is_c_ident(nm) && comp_class_index(c, nm) < 0) {
        LocalVar *cv = comp_const_intern(c, nm);
        if (sp_streq(ty, "ConstantPathWriteNode")) cv->const_def_write = 1;
      }
    }
    else if (sp_streq(ty, "ConstantOrWriteNode") || sp_streq(ty, "ConstantAndWriteNode") ||
             sp_streq(ty, "ConstantOperatorWriteNode")) {
      /* `CONST ||= v` (and friends) may be the constant's only definition */
      const char *nm = nt_str(nt, id, "name");
      if (nm && is_c_ident(nm) && comp_class_index(c, nm) < 0)
        comp_const_intern(c, nm);
    }
    else if (sp_streq(ty, "ConstantWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      /* a constant bound to a regex literal is resolved at compile time to a
         precompiled pattern, not stored as a runtime value */
      int rv = nt_ref(nt, id, "value");
      if (rv >= 0 && nt_type(nt, rv) && sp_streq(nt_type(nt, rv), "CallNode") &&
          nt_str(nt, rv, "name") && sp_streq(nt_str(nt, rv, "name"), "freeze"))
        rv = nt_ref(nt, rv, "receiver");
      int is_regex_const = rv >= 0 && nt_type(nt, rv) && sp_streq(nt_type(nt, rv), "RegularExpressionNode");
      /* regex constants: store with type TY_REGEX so call-type inference works */
      if (nm && is_regex_const) {
        LocalVar *cv = comp_const_intern(c, nm);
        cv->type = TY_REGEX;
      }
      /* a Struct/Data const names a class, not a value constant.
         Do NOT skip when the name collides with a module: M::V = "str" is a
         value constant even though top-level `module V` exists. */
      if (nm && is_c_ident(nm) && !is_regex_const) {
        LocalVar *cv = comp_const_intern(c, nm);
        cv->const_def_write = 1;
        /* `CONST = SomeClass.new(...)`: reads of CONST during the new()
           (i.e. inside initialize or anything it calls) must raise
           NameError, since CONST is not yet bound. */
        int v = nt_ref(nt, id, "value");
        const char *vty = v >= 0 ? nt_type(nt, v) : NULL;
        if (vty && sp_streq(vty, "CallNode") && nt_str(nt, v, "name") &&
            sp_streq(nt_str(nt, v, "name"), "new")) {
          int vr = nt_ref(nt, v, "receiver");
          if (vr >= 0 && nt_type(nt, vr) && sp_streq(nt_type(nt, vr), "ConstantReadNode") &&
              nt_str(nt, vr, "name") && comp_class_index(c, nt_str(nt, vr, "name")) >= 0)
            cv->init_guarded = 1;
        }
      }
    }
  }
}

/* Extract a symbol or string literal text from a node, or NULL. */
const char *ffi_arg_str(const NodeTable *nt, int nid) {
  if (nid < 0) return NULL;
  const char *ty = nt_type(nt, nid);
  if (!ty) return NULL;
  if (sp_streq(ty, "SymbolNode")) return nt_str(nt, nid, "value");
  if (sp_streq(ty, "StringNode")) return nt_str(nt, nid, "content");
  return NULL;
}

/* Resolve an ffi type-list argument to the ArrayNode it names, folding the
   compile-time forms a real adapter writes: `[:float].freeze`, a constant
   holding the list, and `[:float] * 24` (which the 24- and 25-parameter
   geometry entry points are written as). Answers the array node and, for the
   repeat form, how many times to repeat it. -1 when it is not constant. */
static int ffi_type_array_node(Compiler *c, int nid, int *out_repeat, int depth) {
  const NodeTable *nt = c->nt;
  *out_repeat = 1;
  if (nid < 0 || depth > 8) return -1;
  const char *ty = nt_type(nt, nid);
  if (!ty) return -1;
  if (sp_streq(ty, "ArrayNode")) return nid;
  if (sp_streq(ty, "CallNode")) {
    const char *nm = nt_str(nt, nid, "name");
    int recv = nt_ref(nt, nid, "receiver");
    if (!nm || recv < 0) return -1;
    if (sp_streq(nm, "freeze") || sp_streq(nm, "dup") || sp_streq(nm, "to_a"))
      return ffi_type_array_node(c, recv, out_repeat, depth + 1);
    if (sp_streq(nm, "*")) {
      int a = nt_ref(nt, nid, "arguments");
      int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
      if (an != 1 || !av) return -1;
      int n = ffi_arg_int(nt, av[0]);
      if (n < 0 || n > 4096) return -1;
      int inner_rep = 1;
      int arr = ffi_type_array_node(c, recv, &inner_rep, depth + 1);
      if (arr < 0) return -1;
      *out_repeat = n * inner_rep;
      return arr;
    }
    return -1;
  }
  if (sp_streq(ty, "ConstantReadNode")) {
    const char *cn = nt_str(nt, nid, "name");
    if (!cn) return -1;
    NT_FOREACH_KIND(nt, NK_ConstantWriteNode, w) {
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, cn)) continue;
      return ffi_type_array_node(c, nt_ref(nt, w, "value"), out_repeat, depth + 1);
    }
  }
  return -1;
}

/* Extract an integer literal value, or -1. */
int ffi_arg_int(const NodeTable *nt, int nid) {
  if (nid < 0) return -1;
  const char *ty = nt_type(nt, nid);
  if (!ty) return -1;
  if (sp_streq(ty, "IntegerNode")) return (int)nt_int(nt, nid, "value", 0);
  return -1;
}

/* Map an FFI spec string to the Spinel TyKind used for return types. */
TyKind ffi_spec_to_ty(const char *spec) {
  const FfiSpecInfo *info = ffi_spec_lookup(spec);
  return info ? info->ty : TY_UNKNOWN;
}

/* Loud reject of an FFI declaration called with too few arguments. Arity is
   purely syntactic, so unlike a non-literal argument (which the DSL may fold
   from a compile-time string/int form -- see test/i1011.rb) a missing argument
   is always an author error: report it against its source line and stop,
   instead of silently dropping the decl and surfacing an opaque `unsupported`
   at the eventual call site. This is an analyze-phase error -- `unsupported`
   is a codegen primitive whose recovery context is not armed here. */
__attribute__((noreturn)) static void ffi_decl_error(Compiler *c, int node, const char *msg) {
  const NodeTable *nt = c->nt;
  int ln  = (int)nt_int(nt, node, "node_line", 0);
  int fid = (int)nt_int(nt, node, "node_file", 0);
  const char *file = nt_file_path(nt, fid);
  if (!file || !*file) file = nt->source_file;
  if (!file || !*file) file = "source.rb";
  fprintf(stderr, "spinel: %s:%d: %s\n", file, ln, msg);
  exit(1);
}

/* Lexically collapse "." and ".." segments of an absolute-or-relative path,
   in place (the folded-string buffer is ours). Mirrors File.expand_path's
   lexical behavior for the compile-time folds below. */
static void ffi_path_collapse(char *p, size_t size) {
  char out[1024]; size_t o = 0;
  int abs = p[0] == '/';
  const char *s = p;
  while (*s) {
    while (*s == '/') s++;
    const char *seg = s;
    while (*s && *s != '/') s++;
    size_t sl = (size_t)(s - seg);
    if (sl == 0) continue;
    if (sl == 1 && seg[0] == '.') continue;
    if (sl == 2 && seg[0] == '.' && seg[1] == '.') {
      /* A relative path keeps a leading ".." (nothing precedes it to pop),
         and stacks further ".." onto an already-leading ".." run -- lexical
         cleanpath, like Pathname#cleanpath. An absolute path's ".." above
         root is dropped. */
      int prev_dotdot = o >= 3 && out[o - 1] == '.' && out[o - 2] == '.' && out[o - 3] == '/';
      if (!abs && (o == 0 || prev_dotdot)) {
        if (o + 4 >= sizeof out) return;
        out[o++] = '/'; out[o++] = '.'; out[o++] = '.';
      }
      else {
        while (o > 0 && out[o - 1] != '/') o--;  /* pop the previous segment */
        if (o > 0) o--;
      }
      continue;
    }
    if (o + 1 + sl + 1 >= sizeof out) return;  /* too long: leave as-is */
    out[o++] = '/';
    memcpy(out + o, seg, sl); o += sl;
  }
  out[o] = 0;
  if (abs) snprintf(p, size, "%s", o ? out : "/");
  else snprintf(p, size, "%s", o ? out + 1 : ".");
}

/* Fold a compile-time string expression for FFI decl arguments: a plain
   literal, adjacent literals ("a" "b"), String#+ of foldable halves,
   __dir__, and File.expand_path(<foldable>[, <foldable>]). Returns a
   malloc'd string, or NULL when the expression is not compile-time
   foldable. (test/i1011.rb pins the contract.) */
static char *ffi_fold_str(Compiler *c, int nid) {
  const NodeTable *nt = c->nt;
  if (nid < 0) return NULL;
  const char *ty = nt_type(nt, nid);
  if (!ty) return NULL;
  if (sp_streq(ty, "StringNode")) {
    const char *s = nt_str(nt, nid, "content");
    if (!s) s = nt_str(nt, nid, "unescaped");
    return s ? strdup(s) : NULL;
  }
  if (sp_streq(ty, "InterpolatedStringNode")) {
    /* adjacent literals fold; any embedded expression does not */
    int pn = 0; const int *parts = nt_arr(nt, nid, "parts", &pn);
    size_t total = 1;
    for (int i = 0; i < pn; i++) {
      if (!nt_type(nt, parts[i]) || !sp_streq(nt_type(nt, parts[i]), "StringNode")) return NULL;
      const char *p = nt_str(nt, parts[i], "content");
      if (!p) p = nt_str(nt, parts[i], "unescaped");
      if (!p) return NULL;
      total += strlen(p);
    }
    char *r = malloc(total);
    if (!r) { perror("malloc"); exit(1); }
    r[0] = 0;
    for (int i = 0; i < pn; i++) {
      const char *p = nt_str(nt, parts[i], "content");
      if (!p) p = nt_str(nt, parts[i], "unescaped");
      strcat(r, p);
    }
    return r;
  }
  if (sp_streq(ty, "CallNode")) {
    const char *nm = nt_str(nt, nid, "name");
    int rcv = nt_ref(nt, nid, "receiver");
    int args = nt_ref(nt, nid, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (nm && sp_streq(nm, "+") && rcv >= 0 && an == 1) {
      char *l = ffi_fold_str(c, rcv);
      char *r = l ? ffi_fold_str(c, av[0]) : NULL;
      if (l && r) {
        size_t n = strlen(l) + strlen(r) + 1;
        char *j = malloc(n);
        if (!j) { perror("malloc"); exit(1); }
        snprintf(j, n, "%s%s", l, r);
        free(l); free(r);
        return j;
      }
      free(l); free(r);
      return NULL;
    }
    if (nm && sp_streq(nm, "__dir__") && rcv < 0 && an == 0) {
      /* the source file's directory (same convention as the codegen fold);
         in a required file, that file's own (#4839) */
      char dir[1024];
      an_node_dir(nt, nid, dir, sizeof dir);
      return strdup(dir);
    }
    if (nm && sp_streq(nm, "expand_path") && rcv >= 0 && (an == 1 || an == 2) &&
        nt_type(nt, rcv) && sp_streq(nt_type(nt, rcv), "ConstantReadNode") &&
        nt_str(nt, rcv, "name") && sp_streq(nt_str(nt, rcv, "name"), "File")) {
      char *rel = ffi_fold_str(c, av[0]);
      if (!rel) return NULL;
      char *base = NULL;
      if (an == 2) { base = ffi_fold_str(c, av[1]); if (!base) { free(rel); return NULL; } }
      char joined[1024];
      if (rel[0] == '/' || !base) snprintf(joined, sizeof joined, "%s", rel);
      else snprintf(joined, sizeof joined, "%s/%s", base, rel);
      free(rel); free(base);
      ffi_path_collapse(joined, sizeof joined);
      return strdup(joined);
    }
  }
  return NULL;
}

/* Append `add` to a semicolon-joined per-module list, allocating the string or
   growing it in place. The ffi_lib and ffi_cflags merges share this. */
static void ffi_semi_append(char **slot, const char *add) {
  if (!*slot) { *slot = strdup(add); if (!*slot) { perror("strdup"); exit(1); } return; }
  size_t n = strlen(*slot) + 1 + strlen(add) + 1;
  char *merged = malloc(n);
  if (!merged) { perror("malloc"); exit(1); }
  snprintf(merged, n, "%s;%s", *slot, add);
  free(*slot);
  *slot = merged;
}

static void *ffi_grow(void *p, int n, int *cap, int init, size_t sz) {
  if (n < *cap) return p;
  *cap = *cap ? *cap * 2 : init;
  p = realloc(p, sz * (size_t)*cap);
  if (!p) { perror("realloc"); exit(1); }
  return p;
}

/* Register a ffi_func / ffi_const / ffi_buffer / ffi_read_* declared in
   module bodies. Called during analyze_program before fixpoint. */
void register_ffi_decls(Compiler *c) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_ModuleNode, id) {
    int cp = nt_ref(nt, id, "constant_path");
    const char *mname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!mname) continue;
    /* A module that extends FFI::Library is the ffi gem's: its ffi_lib,
       attach_function, callback and typedef are runtime calls into the
       bundled package (packages/ffi), not this compile-time DSL. */
    if (comp_ffi_library_module(nt, mname)) continue;
    int body = nt_ref(nt, id, "body");
    int sn = 0;
    const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &sn) : NULL;
    /* Pre-scan for `native_lib "feat"`: its require-gate feature name is
       stamped onto every native_func of this module regardless of order. */
    const char *mod_feat = NULL;
    for (int k = 0; k < sn; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty || !sp_streq(sty, "CallNode")) continue;
      if (nt_ref(nt, s, "receiver") >= 0) continue;
      const char *dn = nt_str(nt, s, "name");
      if (dn && sp_streq(dn, "native_lib")) {
        int a = nt_ref(nt, s, "arguments");
        int na = 0;
        const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &na) : NULL;
        if (na >= 1) mod_feat = ffi_arg_str(nt, av[0]);
        break;
      }
    }
    /* Pre-scan for `native_struct "Name", "sp_CStruct"[, "free_sym"]`: registers
       Name as a native (C-backed) class so native_new/native_method below can
       bind to its class index, regardless of declaration order.

       Every declaration in the module is registered, not just the first: with
       two of them the second class did not exist at all, and its `native_new`
       and `native_method` bound to the first one's index -- the emitted extern
       gave `sp_Second_new` the return type of `sp_First`. `native_cid` seeds
       from the FIRST, which is what a `native_new` written ahead of its own
       `native_struct` binds to; the walk below re-points it as each
       declaration is passed, so several classes in one module bind in
       declaration order. */
    int native_cid = -1;
    for (int k = 0; k < sn; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty || !sp_streq(sty, "CallNode")) continue;
      if (nt_ref(nt, s, "receiver") >= 0) continue;
      const char *dn = nt_str(nt, s, "name");
      if (!dn || !sp_streq(dn, "native_struct")) continue;
      int a = nt_ref(nt, s, "arguments");
      int na = 0;
      const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &na) : NULL;
      if (na < 2) continue;
      const char *clsname = ffi_arg_str(nt, av[0]);
      const char *cstruct = ffi_arg_str(nt, av[1]);
      const char *freesym = na >= 3 ? ffi_arg_str(nt, av[2]) : NULL;
      if (!clsname || !cstruct) continue;
      /* A qualified declaration ("IO::Buffer") registers by its LEAF -- the
         constant-path lookups are leaf-keyed throughout -- and keeps the
         qualified spelling as the Ruby-visible name (class_ruby_name), so
         `b.class` and sp_class_to_s render it as CRuby does. */
      const char *leaf = strrchr(clsname, ':');
      leaf = leaf ? leaf + 1 : clsname;
      int ex = comp_class_index(c, leaf);
      /* Any class body spelled under the BARE leaf (`class Buffer`) shares
         the leaf key with a qualified declaration ("IO::Buffer"), and the
         leaf-keyed class table would silently merge the user's class into
         the native one (native flag, c_struct, display name all
         overwritten -- whichever registers first). Refuse that loudly. A
         reopen spelled with the same qualified path (the binding's own
         `class IO::Buffer ... end`) is the class and passes. */
      if (leaf != clsname) {
        NT_FOREACH_KIND(nt, NK_ClassNode, ucn) {
          int ucp = nt_ref(nt, ucn, "constant_path");
          const char *ucpty = ucp >= 0 ? nt_type(nt, ucp) : NULL;
          const char *ucpn = ucp >= 0 ? nt_str(nt, ucp, "name") : NULL;
          if (!ucpn || !sp_streq(ucpn, leaf)) continue;
          /* a class the collision qualifier already registered under its
             own path (`Cache::Store` beside "X509::Store") is a distinct
             class, not a merge: its registered name is no longer the leaf */
          { const char *reg = nt_str(nt, ucn, "name");
            if (reg && !sp_streq(reg, leaf)) continue; }
          /* rebuild the definition's own qualified spelling; a bare
             `class Buffer`, a differently-qualified `class Other::Buffer`,
             and a root-anchored `class ::Buffer` all share the leaf key
             without BEING this class */
          char qn[256];
          qn[0] = 0;
          if (ucpty && sp_streq(ucpty, "ConstantPathNode")) {
            const char *segs[8];
            int nseg = 0, ok = 1;
            for (int par = nt_ref(nt, ucp, "parent"); par >= 0 && ok; ) {
              const char *pty = nt_type(nt, par);
              const char *pn = nt_str(nt, par, "name");
              if (!pty || !pn || nseg >= 8) { ok = 0; break; }
              segs[nseg++] = pn;
              if (sp_streq(pty, "ConstantReadNode")) break;
              if (!sp_streq(pty, "ConstantPathNode")) { ok = 0; break; }
              par = nt_ref(nt, par, "parent");   /* -1 = root anchor: done */
            }
            if (ok) {
              for (int si = nseg - 1; si >= 0; si--) {
                if (qn[0]) strncat(qn, "::", sizeof qn - strlen(qn) - 1);
                strncat(qn, segs[si], sizeof qn - strlen(qn) - 1);
              }
              if (qn[0]) strncat(qn, "::", sizeof qn - strlen(qn) - 1);
            }
          }
          strncat(qn, leaf, sizeof qn - strlen(qn) - 1);
          if (!sp_streq(qn, clsname))
            ffi_decl_error(c, ucn,
                           "this class shares its name with a native class declared under a "
                           "qualified path (constant lookups are leaf-keyed, so the two would "
                           "merge); rename the class");
        }
      }
      int cid;
      if (ex >= 0) cid = ex;
      else { comp_class_new(c, leaf, -1); cid = c->nclasses - 1; }
      if (native_cid < 0) native_cid = cid;
      ClassInfo *nc = &c->classes[cid];
      nc->is_native_class = 1;
      if (leaf != clsname && !nc->ruby_name_cache) nc->ruby_name_cache = strdup(clsname);
      free(nc->c_struct); nc->c_struct = strdup(cstruct);
      if (freesym) { free(nc->native_free); nc->native_free = strdup(freesym); }
    }
    for (int k = 0; k < sn; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty || !sp_streq(sty, "CallNode")) continue;
      if (nt_ref(nt, s, "receiver") >= 0) continue;
      const char *dname = nt_str(nt, s, "name");
      if (!dname) continue;
      int anode = nt_ref(nt, s, "arguments");
      int an = 0;
      const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;

      /* native_func :name, [arg_specs], ret_spec, "c_symbol" (Path B).
         Specs are the spinel type language (any/string/int/float/bool). */
      if (sp_streq(dname, "native_func")) {
        if (an < 4) continue;
        const char *fname = ffi_arg_str(nt, args[0]);
        const char *arr_ty = nt_type(nt, args[1]);
        const char *ret_spec = ffi_arg_str(nt, args[2]);
        const char *csym = ffi_arg_str(nt, args[3]);
        if (!fname || !ret_spec || !csym || !arr_ty || !sp_streq(arr_ty, "ArrayNode")) continue;
        int en = 0;
        const int *elems = nt_arr(nt, args[1], "elements", &en);
        char **arg_specs = malloc(sizeof(char *) * (size_t)(en + 1));
        for (int ei = 0; ei < en; ei++) {
          const char *spec = ffi_arg_str(nt, elems[ei]);
          arg_specs[ei] = strdup(spec ? spec : "");
        }
        if (c->n_native_funcs >= c->c_native_funcs) {
          c->c_native_funcs = c->c_native_funcs ? c->c_native_funcs * 2 : 16;
          c->native_funcs = realloc(c->native_funcs, sizeof(NativeFunc) * (size_t)c->c_native_funcs);
        }
        int ni = c->n_native_funcs++;
        c->native_funcs[ni].mod  = strdup(mname);
        c->native_funcs[ni].name = strdup(fname);
        c->native_funcs[ni].ret  = strdup(ret_spec);
        c->native_funcs[ni].csym = strdup(csym);
        c->native_funcs[ni].feat = strdup(mod_feat ? mod_feat : "");
        c->native_funcs[ni].args = arg_specs;
        c->native_funcs[ni].nargs = en;
        continue;
      }

      /* native_new [arg_specs], "csym"  and
         native_method :name, [arg_specs], ret_spec, "csym"
         bind a native class's constructor / instance methods to C symbols. */
      if ((sp_streq(dname, "native_new") || sp_streq(dname, "native_method")) && native_cid >= 0) {
        int is_ctor = sp_streq(dname, "native_new");
        int need = is_ctor ? 2 : 4;
        if (an < need) continue;
        const char *mname_m = is_ctor ? "new" : ffi_arg_str(nt, args[0]);
        int arr_i = is_ctor ? 0 : 1;
        const char *arr_ty = nt_type(nt, args[arr_i]);
        const char *ret_spec = is_ctor ? "" : ffi_arg_str(nt, args[2]);
        const char *csym = ffi_arg_str(nt, args[is_ctor ? 1 : 3]);
        if (!mname_m || !csym || !ret_spec || !arr_ty || !sp_streq(arr_ty, "ArrayNode")) continue;
        int en = 0;
        const int *elems = nt_arr(nt, args[arr_i], "elements", &en);
        char **arg_specs = malloc(sizeof(char *) * (size_t)(en + 1));
        for (int ei = 0; ei < en; ei++) {
          const char *spec = ffi_arg_str(nt, elems[ei]);
          arg_specs[ei] = strdup(spec ? spec : "");
        }
        if (c->n_native_methods >= c->c_native_methods) {
          c->c_native_methods = c->c_native_methods ? c->c_native_methods * 2 : 16;
          c->native_methods = realloc(c->native_methods, sizeof(NativeMethod) * (size_t)c->c_native_methods);
        }
        int mi = c->n_native_methods++;
        c->native_methods[mi].class_id = native_cid;
        c->native_methods[mi].kind = is_ctor ? 1 : 0;
        c->native_methods[mi].name = strdup(mname_m);
        c->native_methods[mi].ret  = strdup(ret_spec);
        c->native_methods[mi].csym = strdup(csym);
        c->native_methods[mi].args = arg_specs;
        c->native_methods[mi].rest = en > 0 && sp_streq(arg_specs[en - 1], "rest");
        c->native_methods[mi].nargs = c->native_methods[mi].rest ? en - 1 : en;
        continue;
      }
      /* The classes themselves are registered in the pre-scan above; what this
         pass takes from a `native_struct` is which class the declarations that
         follow it belong to. */
      if (sp_streq(dname, "native_struct")) {
        const char *sname = an >= 1 ? ffi_arg_str(nt, args[0]) : NULL;
        const char *sleaf = sname ? strrchr(sname, ':') : NULL;
        sleaf = sleaf ? sleaf + 1 : sname;
        int scid = sleaf ? comp_class_index(c, sleaf) : -1;
        if (scid >= 0) native_cid = scid;
        continue;
      }

      /* native_obj_reflect: the package consumes the generic object->hash
         reflection (sp_obj_to_hash); codegen installs it when Structs exist. */
      if (sp_streq(dname, "native_obj_reflect")) { c->native_obj_reflect = 1; continue; }

      /* native_obj "packages/<pkg>/<file>.o": a carried C object linked only
         when this module's require-gate feature is present (Path B). */
      if (sp_streq(dname, "native_obj")) {
        if (an < 1) continue;
        const char *objp = ffi_arg_str(nt, args[0]);
        for (int k = 0; objp && k < c->n_native_objs; k++) if (sp_streq(c->native_objs[k].path, objp)) objp = NULL;
        if (!objp) continue;
        if (c->n_native_objs >= c->c_native_objs) {
          c->c_native_objs = c->c_native_objs ? c->c_native_objs * 2 : 8;
          c->native_objs = realloc(c->native_objs, sizeof(NativeObj) * (size_t)c->c_native_objs);
        }
        int oi = c->n_native_objs++;
        c->native_objs[oi].mod  = strdup(mname);
        c->native_objs[oi].path = strdup(objp);
        c->native_objs[oi].feat = strdup(mod_feat ? mod_feat : "");
        continue;
      }

      if (sp_streq(dname, "ffi_lib")) {
        if (an < 1) ffi_decl_error(c, s, "`ffi_lib` needs a library name");
        const char *libname = ffi_arg_str(nt, args[0]);
        if (!libname) continue;  /* non-literal (e.g. a compile-time fold): tolerate */
        /* find or create the per-module lib entry, then semicolon-merge */
        int mi = -1;
        for (int li = 0; li < c->n_ffi_libs; li++)
          if (sp_streq(c->ffi_libs[li].mod, mname)) { mi = li; break; }
        if (mi < 0) {
          c->ffi_libs = ffi_grow(c->ffi_libs, c->n_ffi_libs, &c->c_ffi_libs, 8, sizeof(FfiLib));
          c->ffi_libs[c->n_ffi_libs].mod   = strdup(mname);
          c->ffi_libs[c->n_ffi_libs].names = strdup(libname);
          c->n_ffi_libs++;
        }
        else ffi_semi_append(&c->ffi_libs[mi].names, libname);
        continue;
      }

      if (sp_streq(dname, "ffi_cflags")) {
        if (an < 1) ffi_decl_error(c, s, "`ffi_cflags` needs a flag string");
        char *cflag = ffi_fold_str(c, args[0]);
        /* A flag string that silently vanishes fails the LINK with an opaque
           error much later; a non-foldable argument is a loud analyze error
           instead (adjacent literals, String#+, __dir__ and
           File.expand_path all fold). */
        if (!cflag)
          ffi_decl_error(c, s, "`ffi_cflags` expects a compile-time string "
                               "(a literal, adjacent literals, String#+, __dir__, "
                               "or File.expand_path of those)");
        /* find or create the per-module cflag entry, then semicolon-merge */
        int mi = -1;
        for (int ci = 0; ci < c->n_ffi_cflags; ci++)
          if (sp_streq(c->ffi_cflags[ci].mod, mname)) { mi = ci; break; }
        if (mi < 0) {
          c->ffi_cflags = ffi_grow(c->ffi_cflags, c->n_ffi_cflags, &c->c_ffi_cflags, 8, sizeof(FfiCflag));
          c->ffi_cflags[c->n_ffi_cflags].mod = strdup(mname);
          c->ffi_cflags[c->n_ffi_cflags].val = strdup(cflag);
          c->n_ffi_cflags++;
        }
        else ffi_semi_append(&c->ffi_cflags[mi].val, cflag);
        free(cflag);
        continue;
      }

      if (sp_streq(dname, "ffi_source")) {
        if (an < 1) ffi_decl_error(c, s, "`ffi_source` needs a C source string");
        char *source = ffi_fold_str(c, args[0]);
        if (!source)
          ffi_decl_error(c, s, "`ffi_source` expects a compile-time string "
                               "(a literal, heredoc, adjacent literals, String#+, "
                               "__dir__, or File.expand_path of those)");
        c->ffi_sources = ffi_grow(c->ffi_sources, c->n_ffi_sources, &c->c_ffi_sources, 4, sizeof(FfiSource));
        c->ffi_sources[c->n_ffi_sources].mod = strdup(mname);
        c->ffi_sources[c->n_ffi_sources].val = source;
        c->n_ffi_sources++;
        continue;
      }

      if (sp_streq(dname, "ffi_func") || sp_streq(dname, "attach_function")) {
        /* ffi-gem compat: `attach_function :name, [types], :ret` is
           `ffi_func`; the gem's 4-arg rename form
           `attach_function :ruby_name, :c_name, [types], :ret` supplies
           the C symbol separately. */
        if (an < 3) {
          char emsg[128];
          snprintf(emsg, sizeof emsg, "`%s` needs a name, an argument-type array, and a return type", dname);
          ffi_decl_error(c, s, emsg);
        }
        /* a trailing `blocking: true` (the ffi gem's own keyword): the call
           leaves the world while it runs, see FfiFunc.blocking */
        int blocking = 0;
        if (an >= 1 && nt_type(nt, args[an - 1]) && sp_streq(nt_type(nt, args[an - 1]), "KeywordHashNode")) {
          int kn = 0; const int *kel = nt_arr(nt, args[an - 1], "elements", &kn);
          for (int ki = 0; ki < kn; ki++) {
            int kk = nt_ref(nt, kel[ki], "key"), kv = nt_ref(nt, kel[ki], "value");
            const char *ks = kk >= 0 ? ffi_arg_str(nt, kk) : NULL;
            if (ks && sp_streq(ks, "blocking") && kv >= 0 && nt_type(nt, kv) && sp_streq(nt_type(nt, kv), "TrueNode"))
              blocking = 1;
          }
          an--;
        }
        int a_arr = args[1], a_ret = args[2], a_csym = -1;
        if (sp_streq(dname, "attach_function") && an >= 4) {
          a_csym = args[1]; a_arr = args[2]; a_ret = args[3];
        }
        const char *fname = ffi_arg_str(nt, args[0]);
        if (!fname) continue;  /* non-literal name: tolerate */
        /* arg type array: an array literal, or one of the constant-valued
           forms an adapter writes for a long list. Anything else used to be
           dropped in silence, and the failure surfaced at the first CALL of
           the undeclared function, naming a line nowhere near it (#3804). */
        int rep = 1;
        int arr_node = ffi_type_array_node(c, a_arr, &rep, 0);
        if (arr_node < 0) {
          char emsg[192];
          snprintf(emsg, sizeof emsg,
                   "`%s`'s argument-type list must be an array of type names "
                   "(an array literal, a constant holding one, or `[...] * n`)", dname);
          ffi_decl_error(c, s, emsg);
        }
        int base_n = 0;
        const int *elems = nt_arr(nt, arr_node, "elements", &base_n);
        int en = base_n * rep;
        char **arg_specs = malloc(sizeof(char*) * (size_t)(en + 1));
        if (!arg_specs) { perror("malloc"); exit(1); }
        for (int ei = 0; ei < en; ei++) {
          const char *spec = ffi_arg_str(nt, elems[ei % (base_n ? base_n : 1)]);
          arg_specs[ei] = strdup(spec ? spec : "");
        }
        const char *ret_spec = ffi_arg_str(nt, a_ret);
        if (!ret_spec) {
          for (int ei = 0; ei < en; ei++) free(arg_specs[ei]);
          free(arg_specs);
          continue;
        }
        const char *csym = a_csym >= 0 ? ffi_arg_str(nt, a_csym) : NULL;
        /* grow array */
        c->ffi_funcs = ffi_grow(c->ffi_funcs, c->n_ffi_funcs, &c->c_ffi_funcs, 16, sizeof(FfiFunc));
        int fi = c->n_ffi_funcs++;
        c->ffi_funcs[fi].mod  = strdup(mname);
        c->ffi_funcs[fi].name = strdup(fname);
        c->ffi_funcs[fi].csym = csym ? strdup(csym) : NULL;
        c->ffi_funcs[fi].ret   = strdup(ret_spec);
        c->ffi_funcs[fi].args  = arg_specs;
        c->ffi_funcs[fi].nargs = en;
        c->ffi_funcs[fi].blocking = blocking;
        continue;
      }

      if (sp_streq(dname, "ffi_const")) {
        if (an < 2) ffi_decl_error(c, s, "`ffi_const` needs a name and an integer value");
        const char *kname = ffi_arg_str(nt, args[0]);
        if (!kname) continue;  /* non-literal name: tolerate */
        int val = ffi_arg_int(nt, args[1]);
        c->ffi_consts = ffi_grow(c->ffi_consts, c->n_ffi_consts, &c->c_ffi_consts, 16, sizeof(FfiConst));
        int ci2 = c->n_ffi_consts++;
        c->ffi_consts[ci2].mod  = strdup(mname);
        c->ffi_consts[ci2].name = strdup(kname);
        c->ffi_consts[ci2].val  = val;
        continue;
      }

      if (sp_streq(dname, "ffi_buffer")) {
        if (an < 2) ffi_decl_error(c, s, "`ffi_buffer` needs a name and a byte size");
        const char *bname = ffi_arg_str(nt, args[0]);
        if (!bname) continue;  /* non-literal name: tolerate */
        int bsize = ffi_arg_int(nt, args[1]);
        if (bsize <= 0) continue;  /* non-literal or non-positive size: tolerate */
        c->ffi_bufs = ffi_grow(c->ffi_bufs, c->n_ffi_bufs, &c->c_ffi_bufs, 8, sizeof(FfiBuf));
        int bi = c->n_ffi_bufs++;
        c->ffi_bufs[bi].mod  = strdup(mname);
        c->ffi_bufs[bi].name = strdup(bname);
        c->ffi_bufs[bi].size = bsize;
        continue;
      }

      if (!strncmp(dname, "ffi_read_", 9)) {
        if (an < 2) ffi_decl_error(c, s, "`ffi_read_*` needs a name and a byte offset");
        const char *rname = ffi_arg_str(nt, args[0]);
        if (!rname) continue;  /* non-literal name: tolerate */
        int roff = ffi_arg_int(nt, args[1]);
        if (roff < 0) roff = 0;  /* non-literal or negative offset: clamp (pre-existing) */
        const char *kind = dname + 9;  /* a scalar width, or "ptr" */
        /* Reject a typoed or unsupported suffix rather than registering it and
           reading some default width at codegen, which is what the write side
           has always done (#3928). */
        if (!sp_streq(kind, "ptr") && !ffi_scalar_ctype(kind)) continue;
        c->ffi_readers = ffi_grow(c->ffi_readers, c->n_ffi_readers, &c->c_ffi_readers, 8, sizeof(FfiReader));
        int ri = c->n_ffi_readers++;
        c->ffi_readers[ri].mod    = strdup(mname);
        c->ffi_readers[ri].name   = strdup(rname);
        c->ffi_readers[ri].offset = roff;
        c->ffi_readers[ri].kind   = strdup(kind);
        continue;
      }

      /* ffi_callback :name, [arg_specs], ret_spec -- declares a C
         function-pointer type usable as an ffi_func arg spec. */
      if (sp_streq(dname, "ffi_callback") || sp_streq(dname, "callback")) {
        if (an < 3) continue;
        const char *cbname = ffi_arg_str(nt, args[0]);
        const char *arr_ty = nt_type(nt, args[1]);
        const char *ret_spec = ffi_arg_str(nt, args[2]);
        if (!cbname || !ret_spec || !arr_ty || !sp_streq(arr_ty, "ArrayNode")) continue;
        int en = 0; const int *elems = nt_arr(nt, args[1], "elements", &en);
        char **arg_specs = malloc(sizeof(char *) * (size_t)(en + 1));
        if (!arg_specs) { perror("malloc"); exit(1); }
        for (int ei = 0; ei < en; ei++) {
          const char *spec = ffi_arg_str(nt, elems[ei]);
          arg_specs[ei] = strdup(spec ? spec : "");
        }
        arg_specs[en] = NULL;  /* the allocated sentinel slot */
        c->ffi_callbacks = ffi_grow(c->ffi_callbacks, c->n_ffi_callbacks, &c->c_ffi_callbacks, 8, sizeof(FfiCallback));
        int ci = c->n_ffi_callbacks++;
        c->ffi_callbacks[ci].mod       = strdup(mname);
        c->ffi_callbacks[ci].name      = strdup(cbname);
        c->ffi_callbacks[ci].arg_specs = arg_specs;
        c->ffi_callbacks[ci].nargs     = en;
        c->ffi_callbacks[ci].ret_spec  = strdup(ret_spec);
        continue;
      }

      /* ffi_struct :Name, [[:field, :spec], ...] -- a named C struct with
         generated field accessors: Name_new / Name_get_<f> / Name_set_<f>. */
      if (sp_streq(dname, "ffi_struct")) {
        if (an < 2) continue;
        const char *sname = ffi_arg_str(nt, args[0]);
        const char *arr_ty = nt_type(nt, args[1]);
        if (!sname || !arr_ty || !sp_streq(arr_ty, "ArrayNode")) continue;
        int en = 0; const int *elems = nt_arr(nt, args[1], "elements", &en);
        FfiField *fields = malloc(sizeof(FfiField) * (size_t)(en > 0 ? en : 1));
        if (!fields) { perror("malloc"); exit(1); }
        int nf = 0;
        for (int ei = 0; ei < en; ei++) {
          const char *pty = nt_type(nt, elems[ei]);
          if (!pty || !sp_streq(pty, "ArrayNode")) continue;
          int pn = 0; const int *pair = nt_arr(nt, elems[ei], "elements", &pn);
          if (pn < 2) continue;
          const char *fn = ffi_arg_str(nt, pair[0]);
          const char *fs = ffi_arg_str(nt, pair[1]);
          if (!fn || !fs) continue;
          fields[nf].name = strdup(fn);
          fields[nf].spec = strdup(fs);
          nf++;
        }
        if (nf == 0) { free(fields); continue; }
        c->ffi_structs = ffi_grow(c->ffi_structs, c->n_ffi_structs, &c->c_ffi_structs, 8, sizeof(FfiStruct));
        int sidx = c->n_ffi_structs++;
        c->ffi_structs[sidx].mod     = strdup(mname);
        c->ffi_structs[sidx].name    = strdup(sname);
        c->ffi_structs[sidx].fields  = fields;
        c->ffi_structs[sidx].nfields = nf;
        continue;
      }

      /* ffi_write_u32/i32/ptr :name, <offset> -- symmetric to ffi_read_*:
         Module.name(buf, val) stores val at `offset` bytes into buf. */
      if (!strncmp(dname, "ffi_write_", 10)) {
        if (an < 2) continue;
        const char *wname = ffi_arg_str(nt, args[0]);
        if (!wname) continue;
        int woff = ffi_arg_int(nt, args[1]);
        if (woff < 0) woff = 0;
        const char *kind = dname + 10;  /* a scalar width, or "ptr" */
        /* reject a typoed/unsupported suffix rather than silently registering
           it and falling back to some default store at codegen. */
        if (!sp_streq(kind, "ptr") && !ffi_scalar_ctype(kind)) continue;
        c->ffi_writers = ffi_grow(c->ffi_writers, c->n_ffi_writers, &c->c_ffi_writers, 8, sizeof(FfiReader));
        int wi = c->n_ffi_writers++;
        c->ffi_writers[wi].mod    = strdup(mname);
        c->ffi_writers[wi].name   = strdup(wname);
        c->ffi_writers[wi].offset = woff;
        c->ffi_writers[wi].kind   = strdup(kind);
        continue;
      }
    }
  }
}

/* Resolve Module.<method> against ffi_struct declarations. See compiler.h. */
int ffi_struct_method(Compiler *c, const char *mod, const char *method, int *si, int *fi) {
  for (int i = 0; i < c->n_ffi_structs; i++) {
    if (!sp_streq(c->ffi_structs[i].mod, mod)) continue;
    const char *nm = c->ffi_structs[i].name;
    size_t nl = strlen(nm);
    if (strncmp(method, nm, nl) != 0 || method[nl] != '_') continue;
    const char *rest = method + nl + 1;
    if (sp_streq(rest, "new")) { *si = i; *fi = -1; return FFI_SM_NEW; }
    int isget = !strncmp(rest, "get_", 4);
    int isset = !strncmp(rest, "set_", 4);
    if (isget || isset) {
      const char *field = rest + 4;
      for (int f = 0; f < c->ffi_structs[i].nfields; f++)
        if (sp_streq(c->ffi_structs[i].fields[f].name, field)) {
          *si = i; *fi = f; return isget ? FFI_SM_GET : FFI_SM_SET;
        }
    }
  }
  return FFI_SM_NONE;
}

/* Look up an ffi_callback by (module, name). Returns index or -1. */
int ffi_find_callback(Compiler *c, const char *mod, const char *name) {
  for (int i = 0; i < c->n_ffi_callbacks; i++)
    if (sp_streq(c->ffi_callbacks[i].mod, mod) && sp_streq(c->ffi_callbacks[i].name, name))
      return i;
  return -1;
}

/* The IO::Buffer native class, whose instances an ffi_func pointer argument
   takes by base address. Returns its class id, or -1 when the program never
   loads it. */
int ffi_iobuffer_class(Compiler *c) {
  for (int i = 0; i < c->nclasses; i++)
    if (c->classes[i].is_native_class && c->classes[i].c_struct &&
        sp_streq(c->classes[i].c_struct, "sp_IOBuffer"))
      return i;
  return -1;
}

/* Look up an FFI writer by (module, name). Returns index or -1. */
int ffi_find_writer(Compiler *c, const char *mod, const char *name) {
  for (int i = 0; i < c->n_ffi_writers; i++)
    if (sp_streq(c->ffi_writers[i].mod, mod) && sp_streq(c->ffi_writers[i].name, name))
      return i;
  return -1;
}

/* Look up an FFI func by (module, name). Returns index or -1. */
int ffi_find_func(Compiler *c, const char *mod, const char *name) {
  for (int i = 0; i < c->n_ffi_funcs; i++)
    if (sp_streq(c->ffi_funcs[i].mod, mod) && sp_streq(c->ffi_funcs[i].name, name))
      return i;
  return -1;
}

/* Look up an FFI buffer by (module, name). Returns index or -1. */
int ffi_find_buf(Compiler *c, const char *mod, const char *name) {
  for (int i = 0; i < c->n_ffi_bufs; i++)
    if (sp_streq(c->ffi_bufs[i].mod, mod) && sp_streq(c->ffi_bufs[i].name, name))
      return i;
  return -1;
}

/* Look up an FFI reader by (module, name). Returns index or -1. */
int ffi_find_reader(Compiler *c, const char *mod, const char *name) {
  for (int i = 0; i < c->n_ffi_readers; i++)
    if (sp_streq(c->ffi_readers[i].mod, mod) && sp_streq(c->ffi_readers[i].name, name))
      return i;
  return -1;
}

/* `$g = {}` / `$g = Hash.new` followed by `$g[k] = v` elsewhere: an empty hash
   producer has no key/value type of its own, so the global's inferred type
   stays UNKNOWN and it gets no file-scope slot at all -- every reference then
   fails to compile (`gv_g` undeclared). Derive the variant from the global's
   index-writes, defaulting to the widest hash when they are inconclusive so the
   global is always declarable (#3205). Returns UNKNOWN if the global has no
   `[]=` usage (leave the type alone). */
/* The hash variant a global's or a constant's own `[]=` writes (and read-
   modify-write forms) imply for an empty-producer RHS, `rk` being the
   receiver node kind that names the slot and `sname` its name as the read
   node spells it (without the `$` for a global). */
static TyKind slot_hash_variant_from_writes(Compiler *c, NodeKind rk, const char *sname, int dflt) {
  const NodeTable *nt = c->nt;
  TyKind kt = TY_UNKNOWN, vt = TY_UNKNOWN;
  int saw = 0;
  for (int w = 0; w < nt->count; w++) {
    NodeKind wk = nt_kind(nt, w);
    /* `$g[k] += v`, `||=` and `&&=` key the same slot `[]=` does and count for
       the KEY, as the local scan counts them (#3397). Their value lives in a
       `value` ref rather than a second argument and is a read-modify-write of
       what the hash already holds, so it is not taken for the value type. */
    int is_owr = (wk == NK_IndexOperatorWriteNode || wk == NK_IndexOrWriteNode ||
                  wk == NK_IndexAndWriteNode);
    if (wk != NK_CallNode && !is_owr) continue;
    if (!is_owr) {
      const char *wn = nt_str(nt, w, "name");
      if (!wn || (!sp_streq(wn, "[]=") && !sp_streq(wn, "store"))) continue;
    }
    int wr = nt_ref(nt, w, "receiver");
    if (wr < 0 || nt_kind(nt, wr) != rk) continue;
    const char *rn = nt_str(nt, wr, "name");
    if (!rn || !sp_streq(rk == NK_GlobalVariableReadNode ? rn + 1 : rn, sname)) continue;
    int wa = nt_ref(nt, w, "arguments");
    int wan = 0; const int *wav = wa >= 0 ? nt_arr(nt, wa, "arguments", &wan) : NULL;
    if (wan < (is_owr ? 1 : 2)) continue;
    kt = ty_unify(kt, infer_type(c, wav[0]));
    if (!is_owr) vt = ty_unify(vt, infer_type(c, wav[1]));
    saw = 1;
  }
  /* The default is a value the hash answers, so it is part of the value type:
     `Hash.new(0)` filled only by `+=` has no `[]=` to learn Integer from, and a
     default of another kind than the writes widens the value to poly, where
     the boxed default is carried, rather than being cast into an Integer slot. */
  if (dflt >= 0) { vt = ty_unify(vt, hash_default_value_ty(c, dflt)); saw = 1; }
  if (!saw) return TY_UNKNOWN;
  TyKind want = (kt == TY_SYMBOL) ? TY_SYM_POLY_HASH
              : (kt == TY_UNKNOWN) ? TY_POLY_POLY_HASH : ty_hash_of(kt, vt);
  if (!ty_is_hash(want)) want = (kt == TY_STRING) ? TY_STR_POLY_HASH : TY_POLY_POLY_HASH;
  return want;
}

/* `$g = x || {}` (or `Hash.new`): the fallback hash takes the variant the
   slot's own `[]=` writes imply, as a bare `{}` does, and answers it. Else
   TY_UNKNOWN. */
static TyKind cvar_hash_variant_from_writes(Compiler *c, const char *cvname, int dflt);
static TyKind fallback_hash_variant(Compiler *c, int vnode, NodeKind rk, const char *sname, int *changed) {
  int fb = an_or_empty_hash_fallback(c, vnode);
  if (fb < 0 || !c->hash_want || fb >= c->node_cap) return TY_UNKNOWN;
  TyKind hv = rk == NK_ClassVariableReadNode ? cvar_hash_variant_from_writes(c, sname, -1)
                                             : slot_hash_variant_from_writes(c, rk, sname, -1);
  if (!ty_is_hash(hv)) return TY_UNKNOWN;
  if (c->hash_want[fb] != hv) { c->hash_want[fb] = hv; *changed = 1; }
  return hv;
}

/* 1 iff `node` is an empty-hash producer: a bare `{}` or `Hash.new` (with no
   size/default args), which yields no key/value type of its own. */
static int node_is_empty_hash_producer(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return 0;
  NodeKind nk = nt_kind(nt, node);
  if (nk == NK_HashNode || nk == NK_KeywordHashNode) {
    int en = 0; nt_arr(nt, node, "elements", &en);
    return en == 0;
  }
  if (nk == NK_CallNode) {
    const char *nm = nt_str(nt, node, "name");
    if (!nm || !sp_streq(nm, "new")) return 0;
    int r = nt_ref(nt, node, "receiver");
    if (r < 0 || nt_kind(nt, r) != NK_ConstantReadNode) return 0;
    const char *rn = nt_str(nt, r, "name");
    if (!rn || !sp_streq(rn, "Hash")) return 0;
    int a = nt_ref(nt, node, "arguments");
    int an = 0; if (a >= 0) nt_arr(nt, a, "arguments", &an);
    if (nt_ref(nt, node, "block") >= 0) return 0;
    if (an == 0) return 1;
    /* `Hash.new(default)` starts just as empty as `Hash.new`: the argument is
       what a miss answers, not a member, so it decides no key type. Requiring
       no argument left a global assigned one on the poly slot, where every
       call that read it typed as nil and its VALUE was discarded -- `$g =
       Hash.new(0); $g["b"] = 1; $g["b"]` printed nil -- and a constant
       assigned one did not build at all. A keyword-hash argument is not a
       default but `Hash.new(capacity: n)`, which starts empty all the same;
       hash_new_default_arg tells the two apart. */
    return an == 1;
  }
  return 0;
}

/* The type a write of `vt` from `vnode` brings to a slot now typed `slot`. An
   empty `[]` / `{}` has no type of its own yet, and unified it took the
   slot's: `@x = 1; @x = {}` stored the hash pointer in an Integer slot.
   Against a slot of another kind it is a container all the same, so the slot
   boxes, as an if/else with an empty-literal arm does. */
static TyKind empty_container_write(Compiler *c, int vnode, TyKind vt, TyKind slot) {
  if (vt == TY_UNKNOWN && an_empty_container_disagrees(an_empty_container_kind(c, vnode), slot))
    return TY_POLY;
  return vt;
}

/* The type a slot holding `cur` takes when it is also written nil. A slot
   whose C form has a nil of its own (NULL, or the Integer/Float sentinel)
   keeps its type; a bool, a Symbol, a Class, a Rational and a Complex have no
   spare value, so the nil boxes the slot -- the join ty_unify gives a local
   written both. The ivar, cvar and gvar write passes skipped every nil write,
   so such a slot kept the bare type and its nil read back as false (or the
   zero Symbol): `@v.nil?` folded to false and `p @v` printed false. */
/* 1 iff a write of `vt` leaves a slot typed `cur` as it is: a slot an element
   write widened to the general Array takes any array written into it as one
   (the write converts it), where the join of two array kinds is the scalar
   poly box. */
static int keep_general_array(TyKind cur, TyKind vt) {
  return cur == TY_POLY_ARRAY && ty_is_array(vt);
}

static TyKind nil_write_type(TyKind cur) {
  return an_ty_holds_nil(cur) ? cur : TY_POLY;
}

static int user_method_named(Compiler *c, const char *nm) {
  if (nm && sp_streq(nm, "new")) nm = "initialize";
  for (int i = 1; nm && i < c->nscopes; i++)
    if (c->scopes[i].name && sp_streq(c->scopes[i].name, nm)) return 1;
  return 0;
}

/* Could the subtree `n`, run ahead of a global's first assignment, read the
   global? A read or update of it counts, and so does a call that may reach a
   user method outside a class body: the callee may read it. Method bodies do
   not run here. */
static int gvar_read_hazard(Compiler *c, int n, const char *gname, int in_cbody) {
  const NodeTable *nt = c->nt;
  if (n < 0) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_DefNode) return 0;
  if (k == NK_GlobalVariableReadNode || k == NK_GlobalVariableOperatorWriteNode ||
      k == NK_GlobalVariableOrWriteNode || k == NK_GlobalVariableAndWriteNode) {
    const char *nm = nt_str(nt, n, "name");
    if (nm && sp_streq(comp_resolve_gvar(c, nm + 1), gname)) return 1;
  }
  if (k == NK_CallNode && !in_cbody && user_method_named(c, nt_str(nt, n, "name"))) return 1;
  if (k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode) in_cbody = 1;
  for (int i = 0; i < nt_num_refs(nt, n); i++)
    if (gvar_read_hazard(c, nt_ref_at(nt, n, i), gname, in_cbody)) return 1;
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int an = 0; const int *av = nt_arr_at(nt, n, i, &an);
    for (int j = 0; j < an; j++)
      if (gvar_read_hazard(c, av[j], gname, in_cbody)) return 1;
  }
  return 0;
}

/* Is global `gname` assigned a non-nil value by a top-level statement that
   runs before anything could read it? Until its first assignment a global is
   nil, so a slot with no nil of its own has to box unless this holds. */
int gvar_seeded_before_read(Compiler *c, const char *gname) {
  const NodeTable *nt = c->nt;
  int body = c->nscopes > 0 ? c->scopes[0].body : -1;
  if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) return 0;
  int n = 0; const int *st = nt_arr(nt, body, "body", &n);
  for (int i = 0; i < n; i++) {
    for (int w = st[i]; w >= 0 && nt_kind(nt, w) == NK_GlobalVariableWriteNode; w = nt_ref(nt, w, "value")) {
      const char *nm = nt_str(nt, w, "name");
      int v = nt_ref(nt, w, "value");
      if (!nm || !sp_streq(comp_resolve_gvar(c, nm + 1), gname)) continue;
      return v >= 0 && nt_kind(nt, v) != NK_NilNode && comp_nil_chain_bottom(nt, v) < 0 &&
             !gvar_read_hazard(c, v, gname, 0);
    }
    if (gvar_read_hazard(c, st[i], gname, 0)) return 0;
  }
  return 0;
}

/* A global with no nil of its own that something could read before its first
   assignment boxes, as one written nil does: the read is nil. The interpreter's
   own flags keep their preset value. */
static int gvar_implicit_nil_writes(Compiler *c) {
  int changed = 0;
  for (int g = 0; g < c->ngvars; g++) {
    LocalVar *lv = &c->gvars[g];
    if (!lv->name || nil_write_type(lv->type) == lv->type) continue;
    if (comp_gvar_is_interp_flag(lv->name)) continue;
    char g0 = lv->name[0];
    if (!((g0 >= 'a' && g0 <= 'z') || (g0 >= 'A' && g0 <= 'Z') || g0 == '_')) continue;
    if (gvar_seeded_before_read(c, lv->name)) continue;
    lv->type = nil_write_type(lv->type);
    changed = 1;
  }
  return changed;
}

int infer_global_const_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    LocalVar *lv = NULL;
    TyKind vt = TY_UNKNOWN;
    if (sp_streq(ty, "GlobalVariableWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
      if (rn) lv = comp_gvar(c, rn);
      int vnode = nt_ref(nt, id, "value");
      vt = infer_type(c, vnode);
      /* an empty `{}`/`Hash.new` RHS leaves vt UNKNOWN (no element type); adopt
         the variant implied by the global's `[]=` writes so it gets a slot. */
      if (!ty_is_hash(vt) && rn && node_is_empty_hash_producer(c, vnode)) {
        TyKind hv = slot_hash_variant_from_writes(c, NK_GlobalVariableReadNode, rn, hash_new_default_arg(c, vnode));
        if (ty_is_hash(hv)) vt = hv;
      }
      if (rn) {
        TyKind hv = fallback_hash_variant(c, vnode, NK_GlobalVariableReadNode, rn, &changed);
        if (ty_is_hash(hv)) vt = hv;
      }
      /* an empty `[]` RHS leaves vt UNKNOWN (no element type); a global still
         needs a concrete slot to be declared and iterated, so give it a poly
         array (it can hold anything pushed later) (#3263). */
      if (vt == TY_UNKNOWN && rn) {
        const char *vnty = nt_type(nt, vnode);
        if (vnty && sp_streq(vnty, "ArrayNode")) {
          int en = 0; nt_arr(nt, vnode, "elements", &en);
          if (en == 0) vt = TY_POLY_ARRAY;
        }
      }
      if (lv) vt = empty_container_write(c, vnode, vt, lv->type);
      if (vt == TY_NIL) {
        if (lv && nil_write_type(lv->type) != lv->type) { lv->type = nil_write_type(lv->type); changed = 1; }
        continue;
      }
    }
    else if (sp_streq(ty, "GlobalVariableOperatorWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
      if (rn) lv = comp_gvar(c, rn);
      TyKind cur = lv ? lv->type : TY_UNKNOWN;
      TyKind v = infer_type(c, nt_ref(nt, id, "value"));
      if (cur == TY_STRING) vt = TY_STRING;
      else if (ty_is_numeric(cur) && ty_is_numeric(v)) vt = (cur == TY_FLOAT || v == TY_FLOAT) ? TY_FLOAT : TY_INT;
      else vt = cur;
    }
    else if (sp_streq(ty, "GlobalVariableOrWriteNode") || sp_streq(ty, "GlobalVariableAndWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
      if (rn) lv = comp_gvar(c, rn);
      vt = infer_type(c, nt_ref(nt, id, "value"));
      if (vt == TY_NIL) {
        if (lv && nil_write_type(lv->type) != lv->type) { lv->type = nil_write_type(lv->type); changed = 1; }
        continue;
      }
    }
    else if (sp_streq(ty, "ConstantWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm) lv = comp_const(c, nm);
      int vnode = nt_ref(nt, id, "value");
      vt = infer_type(c, vnode);
      /* `COUNTS = Hash.new(0)`: the same empty-producer rule the global arm
         applies. Without it the RHS typed as nothing, the `[]=` writes made
         the constant an int ARRAY, and the C did not build. */
      if (!ty_is_hash(vt) && nm && node_is_empty_hash_producer(c, vnode)) {
        TyKind hv = slot_hash_variant_from_writes(c, NK_ConstantReadNode, nm, hash_new_default_arg(c, vnode));
        if (ty_is_hash(hv)) vt = hv;
      }
    }
    else if (sp_streq(ty, "ConstantOrWriteNode") || sp_streq(ty, "ConstantAndWriteNode") ||
             sp_streq(ty, "ConstantOperatorWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm) lv = comp_const(c, nm);
      int is_orand = !sp_streq(ty, "ConstantOperatorWriteNode");
      /* An or/and-write-only constant has no definite value before its first
         use, so it must default to nil (poly) for the truthiness check. */
      if (is_orand && lv && !lv->const_def_write)
        vt = TY_POLY;
      else
        vt = infer_type(c, nt_ref(nt, id, "value"));
    }
    else if (sp_streq(ty, "ConstantPathWriteNode") || sp_streq(ty, "ConstantPathOrWriteNode") ||
             sp_streq(ty, "ConstantPathAndWriteNode") || sp_streq(ty, "ConstantPathOperatorWriteNode")) {
      int tgt = nt_ref(nt, id, "target");
      const char *nm = tgt >= 0 ? nt_str(nt, tgt, "name") : NULL;
      if (nm) lv = comp_const(c, nm);
      int is_orand = sp_streq(ty, "ConstantPathOrWriteNode") || sp_streq(ty, "ConstantPathAndWriteNode");
      /* An or/and-write-only constant has no definite value before its first
         use, so it must default to nil (poly) for the truthiness check. */
      if (is_orand && lv && !lv->const_def_write)
        vt = TY_POLY;
      else
        vt = infer_type(c, nt_ref(nt, id, "value"));
    }
    else if (sp_streq(ty, "MultiWriteNode")) {
      int ln = 0;
      const int *lefts = nt_arr(nt, id, "lefts", &ln);
      int value = nt_ref(nt, id, "value");
      int en = 0;
      const int *els = masgn_tuple_rhs(nt, value) ? nt_arr(nt, value, "elements", &en) : NULL;
      int rn_count = 0;
      nt_arr(nt, id, "rights", &rn_count);
      for (int i = 0; i < ln; i++) {
        const char *lty2 = nt_type(nt, lefts[i]);
        if (!lty2 || !sp_streq(lty2, "GlobalVariableTargetNode")) continue;
        const char *gnm = nt_str(nt, lefts[i], "name");
        const char *rn2 = gnm ? comp_resolve_gvar(c, gnm + 1) : NULL;
        LocalVar *glv = rn2 ? comp_gvar(c, rn2) : NULL;
        if (!glv) continue;
        TyKind vt2 = (els && i < en) ? infer_type(c, els[i]) : TY_UNKNOWN;
        if (vt2 == TY_NIL || vt2 == TY_UNKNOWN) continue;
        TyKind merged2 = ty_unify(glv->type, vt2);
        if (merged2 != glv->type) { glv->type = merged2; changed = 1; }
      }
      /* handle splat-rest global target (*$rest = ...) */
      int rest_nid2 = nt_ref(nt, id, "rest");
      if (rest_nid2 >= 0) {
        const char *rsty2 = nt_type(nt, rest_nid2);
        int rest_inner2 = (rsty2 && sp_streq(rsty2, "SplatNode")) ? nt_ref(nt, rest_nid2, "expression") : -1;
        const char *rinty2 = rest_inner2 >= 0 ? nt_type(nt, rest_inner2) : NULL;
        if (rinty2 && sp_streq(rinty2, "GlobalVariableTargetNode")) {
          const char *gnm2 = nt_str(nt, rest_inner2, "name");
          const char *rn3 = gnm2 ? comp_resolve_gvar(c, gnm2 + 1) : NULL;
          LocalVar *glv2 = rn3 ? comp_gvar(c, rn3) : NULL;
          if (glv2 && els) {
            TyKind rest_elem = TY_UNKNOWN;
            for (int i = ln; i < en - rn_count; i++)
              rest_elem = ty_unify(rest_elem, infer_type(c, els[i]));
            TyKind rest_arr_t = (rest_elem != TY_UNKNOWN) ? ty_array_of(rest_elem) : TY_UNKNOWN;
            if (rest_arr_t != TY_UNKNOWN) {
              TyKind merged3 = ty_unify(glv2->type, rest_arr_t);
              if (merged3 != glv2->type) { glv2->type = merged3; changed = 1; }
            }
          }
        }
      }
      continue;
    }
    else if (sp_streq(ty, "CallNode")) {
      /* CONST << v / CONST.push(v) / CONST.append(v): infer CONST as an
         array whose element type comes from v's type. Only applies when
         the receiver is a direct ConstantReadNode. */
      const char *cnm = nt_str(nt, id, "name");
      if (!cnm) continue;
      int is_push = (sp_streq(cnm, "<<") || sp_streq(cnm, "push") || sp_streq(cnm, "append"));
      /* `CONST[i] = v` is the other way a constant bound to an empty literal
         gets filled -- the table-building shape (`DISPATCH[opcode] = args`).
         Without it the constant stayed UNKNOWN, which reads as "defined
         nowhere" and raises NameError on every reference (#4051). */
      int is_iset = sp_streq(cnm, "[]=");
      if (!is_push && !is_iset) continue;
      int crecv = nt_ref(nt, id, "receiver");
      if (crecv < 0) continue;
      const char *rty = nt_type(nt, crecv);
      if (!rty || !sp_streq(rty, "ConstantReadNode")) continue;
      const char *cnm2 = nt_str(nt, crecv, "name");
      if (!cnm2) continue;
      lv = comp_const(c, cnm2);
      if (!lv || lv->type != TY_UNKNOWN) continue;
      int cargs = nt_ref(nt, id, "arguments");
      int cac = 0;
      const int *cav = cargs >= 0 ? nt_arr(nt, cargs, "arguments", &cac) : NULL;
      if (cac < (is_iset ? 2 : 1) || !cav) continue;
      /* the stored value is the last argument: `push(v)` / `[]=(i, v)` */
      TyKind et = infer_type(c, cav[is_iset ? 1 : 0]);
      if (et == TY_UNKNOWN || et == TY_NIL) continue;
      vt = ty_array_of(et);
      if (vt == TY_UNKNOWN) vt = TY_POLY_ARRAY;
    }
    else {
      continue;
    }
    if (!lv) continue;
    TyKind merged = keep_general_array(lv->type, vt) ? lv->type : ty_unify(lv->type, vt);
    if (merged != lv->type) { lv->type = merged; changed = 1; }
  }
  changed |= gvar_implicit_nil_writes(c);
  return changed;
}

/* Re-infer constants assigned via multi-write with a call/variable RHS.
   The existing infer_write_types pass widened them to TY_POLY early (before
   block params converged); this pass overrides with the now-stable element
   type once it is known and not poly. */
int infer_multiwrite_const_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  NT_FOREACH_KIND(nt, NK_MultiWriteNode, id) {
    int value = nt_ref(nt, id, "value");
    if (value < 0) continue;
    const char *vty = nt_type(nt, value);
    if (vty && sp_streq(vty, "ArrayNode")) continue; /* literal handled in infer_write_types */
    TyKind st = infer_type(c, value);
    if (!ty_is_array(st)) continue;
    TyKind elem = ty_array_elem(st);
    if (elem == TY_POLY || elem == TY_UNKNOWN) continue; /* not yet settled */
    int ln = 0;
    const int *lefts = nt_arr(nt, id, "lefts", &ln);
    for (int i = 0; i < ln; i++) {
      const char *lty = nt_type(nt, lefts[i]) ? nt_type(nt, lefts[i]) : "";
      if (!sp_streq(lty, "ConstantTargetNode") && !sp_streq(lty, "ConstantPathTargetNode")) continue;
      const char *nm = nt_str(nt, lefts[i], "name");
      LocalVar *cv = nm ? comp_const(c, nm) : NULL;
      if (!cv || cv->type == elem) continue;
      cv->type = elem; changed = 1;
    }
    int rn = 0;
    const int *rights = nt_arr(nt, id, "rights", &rn);
    for (int j = 0; j < rn; j++) {
      const char *rty2 = nt_type(nt, rights[j]) ? nt_type(nt, rights[j]) : "";
      if (!sp_streq(rty2, "ConstantTargetNode") && !sp_streq(rty2, "ConstantPathTargetNode")) continue;
      const char *nm = nt_str(nt, rights[j], "name");
      LocalVar *cv = nm ? comp_const(c, nm) : NULL;
      if (!cv || cv->type == elem) continue;
      cv->type = elem; changed = 1;
    }
  }
  return changed;
}

/* The two redeclarations CRuby refuses to LOAD. Both built here with nothing
   said, and each answered whatever its FIRST declaration was written with, so
   the later one was silently dropped -- bodies and all (#4309).

   A constant declared `class` in one place and `module` in another, and a
   class reopened with a different superclass. Neither can be a running Ruby
   program: CRuby raises TypeError as it loads the file. Reported here, at the
   second declaration, with the position of the first.

   Only an EXPLICIT superclass on both sides counts: a bare reopen (`class Foo`
   with no `<`) is how a class is normally added to, and a computed one
   (`class K < Struct.new(:a)`) has no name to compare. */
/* Is this expression a class or a module -- or something this cannot judge?
   Answers 1 for both of those, and 0 ONLY when the value is certainly neither.
   The asymmetry is deliberate: this drives a refusal, so an expression whose
   kind is not obvious must not be refused. A literal is the certain case. */
static int const_value_is_class_like(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  switch (nt_kind(nt, v)) {
    case NK_IntegerNode: case NK_FloatNode: case NK_StringNode:
    case NK_SymbolNode: case NK_ArrayNode: case NK_HashNode:
    case NK_NilNode: case NK_TrueNode: case NK_FalseNode:
    case NK_RangeNode: case NK_RegularExpressionNode:
    case NK_InterpolatedStringNode:
      return 0;
    default:
      return 1;   /* Struct.new, Data.define, Class.new, another constant, a call */
  }
}

/* A method's `&block` parameter is read-only in spinel. Assigning to it is
   refused at compile time, with the rewrite that does the same thing.

   Why it is refused rather than compiled: a method that yields is inlined
   at each call site, and there its block is not a value -- it is the
   caller's block CODE, pasted in at the yields. Every read of the name is
   answered statically from whether this site passed one, and a write has no
   variable to land in: `b = nil` emitted an assignment to an lv_b that
   nothing declares, and the C build stopped with an error naming it. A
   method that does not yield has a real proc variable, and could take the
   write; but which of the two a method is depends on whether a `yield`
   appears anywhere in its body, so allowing it there would make an
   unrelated `b = ...` line stop compiling the day someone adds a yield
   elsewhere. One rule, checkable on the line itself, is the usable one.

   Little is lost. In CRuby a reassigned block parameter does not change
   what `yield` or `block_given?` see -- only later reads of the local --
   so in a yielding method the write rarely means what it looks like. And
   the idiom it serves, a default block (`b ||= proc { ... }`), is the same
   program written against a fresh local: `blk = b || proc { ... }`.

   A write from inside a nested block (`each { b = nil }`) reaches the
   parameter too, unless that block -- or one between it and the def --
   declares its own `b`, as a block parameter or a block-local (`|b|`,
   `|x; b|`): a block's `locals` lists exactly those, and past one that
   names it the search stops. A nested def is a scope of its own and is not
   searched at all. (The node table does not carry prism's `depth`, which
   would say this directly.) */
/* Does a block's `locals` list (comma-separated) name `nm`? */
int blk_locals_have(const char *locals, const char *nm) {
  if (!locals || !nm) return 0;
  size_t n = strlen(nm);
  const char *p = locals;
  while (*p) {
    const char *e = strchr(p, ',');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len == n && strncmp(p, nm, n) == 0) return 1;
    if (!e) break;
    p = e + 1;
  }
  return 0;
}

static void check_blk_param_writes_in(Compiler *c, int node, const char *bp,
                                      const char *meth) {
  const NodeTable *nt = c->nt;
  if (node < 0) return;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode) return;
  /* a block that declares its own `bp` shadows the parameter for everything
     inside it */
  if ((k == NK_BlockNode || k == NK_LambdaNode) &&
      blk_locals_have(nt_str(nt, node, "locals"), bp)) return;
  if (k == NK_LocalVariableWriteNode || k == NK_LocalVariableOperatorWriteNode ||
      k == NK_LocalVariableOrWriteNode || k == NK_LocalVariableAndWriteNode) {
    const char *wn = nt_str(nt, node, "name");
    if (wn && sp_streq(wn, bp)) {
      char msg[512];
      snprintf(msg, sizeof msg,
               "assignment to the block parameter &%s of `%s` is not supported: "
               "a block parameter is read-only in spinel "
               "(assign to a new local instead, e.g. `blk = %s || proc { ... }`)",
               bp, meth ? meth : "?", bp);
      unsupported_feature(c, node, msg);
    }
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++)
    check_blk_param_writes_in(c, nt_ref_at(nt, node, i), bp, meth);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, node, i, &n);
    for (int j = 0; j < n; j++)
      check_blk_param_writes_in(c, ids[j], bp, meth);
  }
}

static void check_blk_param_writes(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_DefNode) continue;
    int pn = nt_ref(nt, id, "parameters");
    if (pn < 0) continue;
    int bpn = nt_ref(nt, pn, "block");
    if (bpn < 0 || !nt_type(nt, bpn) || !sp_streq(nt_type(nt, bpn), "BlockParameterNode")) continue;
    const char *bn = nt_str(nt, bpn, "name");
    if (!bn || !bn[0]) continue;   /* an anonymous `&` has no name to assign */
    check_blk_param_writes_in(c, nt_ref(nt, id, "body"), bn, nt_str(nt, id, "name"));
  }
}

static void check_class_redeclarations(Compiler *c) {
  const NodeTable *nt = c->nt;
  int n = nt->count;
  /* Both searches below walked the whole table per declaration: the constant
     writes (now their kind list, ascending) and the first declaration of the
     name (now a name -> first id map built on the way). rubys/roundhouse#72 */
  ANameHash decl_names; memset(&decl_names, 0, sizeof decl_names);
  int *decl_first = NULL, ndecl = 0, capdecl = 0;
  int ncw = 0; const int *cws = nt_nodes_of_kind(nt, NK_ConstantWriteNode, &ncw);
  for (int id = 0; id < n; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k != NK_ClassNode && k != NK_ModuleNode) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *nm = cp >= 0 ? nt_str(nt, cp, "name") : nt_str(nt, id, "name");
    /* registered before any skip below: a qualified or unnamed-skip
       declaration still counts as an earlier one, as the scan it replaces
       counted every declaration ahead of this node */
    int first_prev = -1;
    if (nm) {
      int di = anh_find(&decl_names, nm);
      if (di >= 0) first_prev = decl_first[di];
      else {
        if (ndecl == capdecl) { capdecl = capdecl ? capdecl * 2 : 64; decl_first = realloc(decl_first, sizeof(int) * (size_t)capdecl); }
        decl_first[ndecl++] = id;
        anh_add(&decl_names, nm);
      }
    }
    if (!nm || !*nm) continue;
    /* The same name also ASSIGNED a value. CRuby refuses to load the file when
       the value is not the kind the declaration says -- `A = 1` then `class A`
       is "A is not a class" -- and warns, then lets the value win, in the other
       order. spinel said nothing either way, and worse, resolved the name TWO
       ways in one file: `p T` read the constant while `T.inspect` and
       `T.only_class` read the class (#4318).
       Only a disagreement is refused. `Foo = Struct.new(:a)` followed by
       `class Foo` is an ordinary idiom -- the value IS a class and the
       declaration reopens it -- and stays legal, which is why this asks what
       the value is rather than whether one exists. */
    /* Same NAME is not the same constant: `module A; module B; C = 7; end; end`
       and `module M; class C; end; end` share a leaf and nothing else. Compare
       the enclosing class, and stand aside entirely for a qualified
       declaration (`class M::C`), whose namespace this leaf match cannot
       judge. Both shapes are in the suite, and both tripped the first cut. */
    if (cp >= 0 && nt_kind(nt, cp) == NK_ConstantPathNode) continue;
    { int dcid = (c->node_cbody && id < c->node_cap) ? c->node_cbody[id] : -1;
    for (int ci = 0; ci < ncw; ci++) {
      int cw = cws[ci];
      if (cw >= n) break;
      if (nt_kind(nt, cw) != NK_ConstantWriteNode) continue;
      const char *wn = nt_str(nt, cw, "name");
      if (!wn || !sp_streq(wn, nm)) continue;
      if (((c->node_cbody && cw < c->node_cap) ? c->node_cbody[cw] : -1) != dcid) continue;
      int v = nt_ref(nt, cw, "value");
      if (v < 0 || const_value_is_class_like(c, v)) continue;   /* a class/module, or unknown */
      char msg[512];
      snprintf(msg, sizeof msg,
               "%s is not a %s: it is assigned a value on line %d, and declared "
               "with %s on line %d",
               nm, k == NK_ModuleNode ? "module" : "class",
               (int)nt_int(nt, cw, "node_line", 0),
               k == NK_ModuleNode ? "module" : "class",
               (int)nt_int(nt, id, "node_line", 0));
      unsupported_feature(c, cw, msg);
    } }
    /* the FIRST declaration of this name; nothing to say if this is it */
    int first = first_prev;
    if (first < 0) continue;
    int fline = (int)nt_int(nt, first, "node_line", 0);
    if (nt_kind(nt, first) != k) {
      char msg[512];
      snprintf(msg, sizeof msg,
               "%s is not a %s (the first declaration, at line %d, is a %s)",
               nm, k == NK_ModuleNode ? "module" : "class", fline,
               nt_kind(nt, first) == NK_ModuleNode ? "module" : "class");
      unsupported_feature(c, id, msg);
    }
    if (k != NK_ClassNode) continue;
    int sc = nt_ref(nt, id, "superclass"), fs = nt_ref(nt, first, "superclass");
    if (sc < 0 || fs < 0) continue;
    const char *sty = nt_type(nt, sc), *fty = nt_type(nt, fs);
    if (!sty || !fty) continue;
    if (!(sp_streq(sty, "ConstantReadNode") || sp_streq(sty, "ConstantPathNode"))) continue;
    if (!(sp_streq(fty, "ConstantReadNode") || sp_streq(fty, "ConstantPathNode"))) continue;
    const char *sn = nt_str(nt, sc, "name"), *fn = nt_str(nt, fs, "name");
    if (!sn || !fn || sp_streq(sn, fn)) continue;
    char msg[512];
    snprintf(msg, sizeof msg,
             "superclass mismatch for class %s (%s here, %s at line %d)",
             nm, sn, fn, fline);
    unsupported_feature(c, id, msg);
  }
  anh_free(&decl_names); free(decl_first);
}

/* Resolve each class's superclass index from its ClassNode. */
/* Forwardable's def_delegator / def_delegators are rewritten by the parser
   into forwarding defs when their arguments are a literal symbol list
   (spinel_parse.c). A call that is still here is one that rewrite could not
   read -- a splatted or computed name list, a string receiver -- and the
   bundled Forwardable module is empty, so it defined nothing and every
   delegated call raised NoMethodError at run time (#4822). Refuse it where
   it is written instead. */
static void check_unrewritten_delegators(Compiler *c) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    if (nt_ref(nt, id, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, id, "name");
    /* the def_* spellings only: `delegate` is an ordinary name elsewhere */
    if (!nm || (!sp_streq(nm, "def_delegator") && !sp_streq(nm, "def_delegators") &&
                !sp_streq(nm, "def_instance_delegator") &&
                !sp_streq(nm, "def_instance_delegators") &&
                !sp_streq(nm, "def_single_delegator") &&
                !sp_streq(nm, "def_single_delegators")))
      continue;
    /* a program that defines the name itself is calling its own method */
    int user = 0;
    for (int k = 0; k < c->nclasses && !user; k++)
      if (comp_cmethod_in_class(c, k, nm) >= 0 || comp_method_in_class(c, k, nm) >= 0) user = 1;
    if (user) continue;
    char msg[256];
    snprintf(msg, sizeof msg,
             "%s with arguments other than a literal symbol list is not supported "
             "(write the names as symbols: `%s :@target, :name, ...`)", nm, nm);
    unsupported_feature(c, id, msg);
  }
}

void resolve_parents(Compiler *c) {
  check_class_redeclarations(c);
  check_blk_param_writes(c);
  check_unrewritten_delegators(c);
  const NodeTable *nt = c->nt;
  for (int i = 0; i < c->nclasses; i++) {
    int sc = nt_ref(nt, c->classes[i].def_node, "superclass");
    if (sc < 0) continue;
    const char *sty = nt_type(nt, sc);
    /* A module-qualified superclass (`class Child < M::Handler`) is a
       ConstantPathNode whose `name` is the last segment ("Handler").
       Classes are registered under that bare last name, so resolve it the
       same way as an unqualified ConstantReadNode superclass. */
    if (sty && (sp_streq(sty, "ConstantReadNode") || sp_streq(sty, "ConstantPathNode"))) {
      int p = comp_class_index(c, nt_str(nt, sc, "name"));
      if (p >= 0 && p != i) c->classes[i].parent = p;
    }
  }
  /* A `class << self; attr_accessor :x` is a method of the singleton class,
     and a subclass's singleton class inherits it: the accessor answers
     through the subclass, on the subclass's own slot (nil until assigned),
     and a class method the subclass inherits reads that slot bare. Each
     descendant gets the ancestor's names as its own (the adders skip a name
     the class already declares, so an override keeps its own); the storage
     is declared per (class, name), so every class has one. A class method
     the subclass runs -- its own, or one inherited from any ancestor --
     naming `@x` reads the subclass's class-level ivar, so the inherited
     accessor shares that slot (sg_civ), as the declaring class's own does. */
  for (int i = 0; i < c->nclasses; i++) {
    for (int k = c->classes[i].parent, guard = 0; k >= 0 && guard < 256; k = c->classes[k].parent, guard++) {
      ClassInfo *pc = &c->classes[k];
      for (int j = 0; j < pc->nsg_readers + pc->nsg_writers; j++) {
        const char *base = j < pc->nsg_readers ? pc->sg_readers[j] : pc->sg_writers[j - pc->nsg_readers];
        int own = j < pc->nsg_readers ? comp_is_sg_reader(&c->classes[i], base)
                                      : comp_is_sg_writer(&c->classes[i], base);
        if (j < pc->nsg_readers) comp_add_sg_reader(&c->classes[i], base);
        else comp_add_sg_writer(&c->classes[i], base);
        if (!own) comp_add_sg_inh(&c->classes[i], base);
        if (comp_is_sg_civ(&c->classes[i], base)) continue;
        char ivname[256];
        snprintf(ivname, sizeof ivname, "@%s", base);
        for (int a = i, g2 = 0; a >= 0 && g2 < 256; a = c->classes[a].parent, g2++) {
          if (cmethod_names_ivar(c, a, ivname)) { comp_add_sg_civ(&c->classes[i], base); break; }
          if (a == c->classes[a].parent) break;
        }
      }
      if (k == c->classes[k].parent) break;
    }
  }
  resolve_inherited_aliases(c);
}

/* An alias of a method this class only INHERITS names the ancestor's body: a
   redefinition later in the same class, by a def or by another alias, must
   not capture it. Record where the lookup resumes, now that superclasses are
   wired (they are not at the point the alias itself is registered). The
   same-class case -- a definition earlier in this very body -- is already
   handled by renaming that definition. */
void resolve_inherited_aliases(Compiler *c) {
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    if (c->classes[ci].parent < 0) continue;
    for (int a = 0; a < cls->naliases; a++) {
      if (!cls->alias_node || cls->alias_node[a] < 0) continue;
      const char *od = cls->alias_old[a];
      int later = 0, own = 0;
      for (int si = 1; si < c->nscopes; si++) {
        Scope *sc = &c->scopes[si];
        if (sc->class_id != ci || sc->is_cmethod || !sc->name || !sp_streq(sc->name, od)) continue;
        if (sc->def_node >= 0 && sc->def_node < cls->alias_node[a]) { own = 1; break; }
        later = 1;
      }
      for (int b = 0; !own && !later && b < cls->naliases; b++)
        if (b != a && cls->alias_node[b] > cls->alias_node[a] && sp_streq(cls->alias_new[b], od)) later = 1;
      if (own || !later) continue;
      /* the table holds `class << self` aliases too: an ancestor's class
         method is the body such an alias took */
      for (int p = c->classes[ci].parent; p >= 0; p = c->classes[p].parent)
        if (comp_method_in_class(c, p, od) >= 0 || comp_cmethod_in_class(c, p, od) >= 0) {
          cls->alias_cls[a] = p;
          break;
        }
    }
  }
}

/* True if the method scope's body contains a `super` (an explicit-arg SuperNode
   or a bare ForwardingSuperNode). */
static int scope_body_has_super(Compiler *c, int scope_idx) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    if (c->nscope[id] != scope_idx) continue;
    const char *ty = nt_type(nt, id);
    if (ty && (sp_streq(ty, "SuperNode") || sp_streq(ty, "ForwardingSuperNode"))) return 1;
  }
  return 0;
}

/* True when the scope body contains a receiverless instance_exec/instance_eval.
   Such a method rebinds self to the receiver, so when mixed in via `include` its
   body must be re-attributed to the includer scope (cloned + walk_scope'd) rather
   than shared with the module: with a shared body, comp_scope_of resolves the
   block's self to the module, the escape loop cannot mark the includer copy
   inlinable, and the instance_exec splice binds the wrong (module) class. Cloning
   per includer mirrors CRuby, where `include` inserts a per-includer iclass and
   the instance_exec block runs with self = the receiver (the includer). */
static int scope_body_has_receiverless_ie(Compiler *c, int scope_idx) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    if (c->nscope[id] != scope_idx) continue;
    if (nt_kind(nt, id) != NK_CallNode) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, id, "name");
    if (nm && (sp_streq(nm, "instance_exec") || sp_streq(nm, "instance_eval"))) return 1;
  }
  return 0;
}

/* True when the scope body reads or writes an instance variable. A module
   instance method that touches an ivar must be re-attributed (cloned) to the
   includer so the ivar types against the includer's slot, not a separate
   module-owned slot: a module cannot be instantiated, so in CRuby its methods
   always run on the includer's instance and there is a single ivar. With a
   shared body the module slot (typed only from module-side usage) and the
   includer slot diverge, and the transplanted method -- emitted taking the
   includer as self -- reads self's includer-typed ivar through the module type. */
static int scope_body_uses_ivar(Compiler *c, int scope_idx) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    if (c->nscope[id] != scope_idx) continue;
    NodeKind k = nt_kind(nt, id);
    if (k == NK_InstanceVariableReadNode ||
        k == NK_InstanceVariableWriteNode ||
        k == NK_InstanceVariableOperatorWriteNode ||
        k == NK_InstanceVariableOrWriteNode ||
        k == NK_InstanceVariableAndWriteNode ||
        k == NK_InstanceVariableTargetNode)
      return 1;
  }
  return 0;
}

/* A `module_function` method is registered class-level (is_cmethod), so the
   include transplant below skipped it and a receiverless call to it was
   rewritten onto the module (`Rt.peek`). That is sound only for a body that
   does not depend on its receiver, and the rewrite's comment claimed exactly
   that -- wrongly. Through an includer CRuby runs such a method on the
   INSTANCE: `@x` is the instance's ivar, `self` is the instance, and a
   receiverless sibling call dispatches on the instance's class, where the
   includer may override it. Spinel gave the module's own storage (a
   `civ_<Mod>_<name>` global), the Module object, and the module's sibling.

   Which bodies care: an ivar, a self, or a receiverless call. A body with none
   of those runs the same either way and keeps the single shared copy. */
static int module_function_self_dependent(Compiler *c, int scope_idx) {
  const NodeTable *nt = c->nt;
  if (scope_body_uses_ivar(c, scope_idx)) return 1;
  for (int id = 0; id < nt->count; id++) {
    if (c->nscope[id] != scope_idx) continue;
    NodeKind k = nt_kind(nt, id);
    if (k == NK_SelfNode) return 1;
    if (k == NK_CallNode && nt_ref(nt, id, "receiver") < 0) return 1;
  }
  return 0;
}

/* Copy src's parameter names and defaults into the clone dst, and intern
   them as param locals so infer_param_types can update their types. */
static void scope_copy_params(Scope *dst, const Scope *src) {
  dst->nparams = src->nparams;
  /* the posts after a rest are part of the layout: a copy without them laid
     `super(1)` into an included `def m(*r, p1)` out as if the rest came
     last, the 1 into the rest and p1 left its zero */
  dst->npost_rest = src->npost_rest;
  if (src->nparams <= 0) return;
  dst->pnames = malloc(sizeof(char *) * (size_t)src->nparams);
  dst->pdefault = malloc(sizeof(int) * (size_t)src->nparams);
  for (int p = 0; p < src->nparams; p++) {
    dst->pnames[p] = src->pnames[p] ? strdup(src->pnames[p]) : NULL;
    dst->pdefault[p] = src->pdefault ? src->pdefault[p] : -1;
  }
  for (int p = 0; p < src->nparams; p++) {
    if (dst->pnames[p]) {
      LocalVar *lv = scope_local_intern(dst, dst->pnames[p]);
      lv->is_param = 1;
      /* A splat / double-splat parameter is typed by construction when the
         source scope is built (a poly array / a hash), not by any call site,
         so nothing re-derives it for the clone: carry it over. Left UNKNOWN,
         `*args` in a method transplanted by `extend`/`include` typed every
         use of args as nothing and the method's return with it. */
      if (p == src->rest_idx || p == src->kwrest_idx) {
        for (int L = 0; L < src->nlocals; L++)
          if (src->locals[L].name && sp_streq(src->locals[L].name, dst->pnames[p])) {
            if (lv->type == TY_UNKNOWN) lv->type = src->locals[L].type;
            break;
          }
      }
    }
  }
}

/* Give a copied method its own default values: clone each one and walk it
   into the copy. Shared, a default reading an earlier parameter
   (`def m(p1 = 51, p2 = p1)`) was typed against the source method's
   parameter, an Integer nothing widens, while the copy's took the values its
   own callers pass; the default then boxed an sp_RbVal as an sp_int and the
   C compiler refused it. Returns 1 when it cloned any. */
int scope_own_defaults(Compiler *c, int di) {
  NodeTable *nt = (NodeTable *)c->nt;
  int any = 0;
  for (int p = 0; p < c->scopes[di].nparams; p++) {
    int od = c->scopes[di].pdefault ? c->scopes[di].pdefault[p] : -1;
    if (od < 0) continue;
    int nd = nt_clone_subtree(nt, od);
    if (nd < 0) continue;
    comp_grow_node_arrays(c);
    c->scopes[di].pdefault[p] = nd;
    walk_scope(c, nd, di, c->scopes[di].class_id);
    any = 1;
  }
  return any;
}

static void intern_scope_ivars(Compiler *c, int ms, int ci) {
  const NodeTable *nt = c->nt;
  for (int id2 = 0; id2 < nt->count; id2++) {
    if (c->nscope[id2] != ms) continue;
    const char *bty = nt_type(nt, id2);
    if (!bty) continue;
    if (sp_streq(bty, "InstanceVariableWriteNode") ||
        sp_streq(bty, "InstanceVariableReadNode") ||
        sp_streq(bty, "InstanceVariableOperatorWriteNode") ||
        sp_streq(bty, "InstanceVariableOrWriteNode")) {
      const char *ivnm = nt_str(nt, id2, "name");
      if (ivnm) comp_ivar_intern(&c->classes[ci], ivnm);
    }
  }
}

/* Process include calls in a single class body, creating scope copies for each
   included module method. We copy (not mutate) so multiple classes can include
   the same module independently. */
int g_inc_did_clone = 0;
void process_include_body(Compiler *c, int ci, int body_node) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *stmts = body_node >= 0 ? nt_arr(nt, body_node, "body", &n) : NULL;
  for (int k = 0; k < n; k++) {
    int s = stmts[k];
    const char *sty = nt_type(nt, s);
    if (!sty || !sp_streq(sty, "CallNode")) continue;
    const char *nm = nt_str(nt, s, "name");
    if (!nm || !sp_streq(nm, "include")) continue;
    if (nt_ref(nt, s, "receiver") >= 0) continue;
    int anode = nt_ref(nt, s, "arguments");
    int an = 0;
    const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
    for (int j = 0; j < an; j++) {
      const char *aty = nt_type(nt, args[j]);
      const char *mname = NULL;
      if (aty && sp_streq(aty, "ConstantReadNode")) mname = nt_str(nt, args[j], "name");
      else if (aty && sp_streq(aty, "ConstantPathNode")) mname = nt_str(nt, args[j], "name");
      int mod_id = mname ? comp_class_index(c, mname) : -1;
      /* `M = ::M` beside the include: a constant that merely ALIASES the
         module names it just as well, and the collision qualifier rewrites
         the include's own argument to the owner-qualified constant
         (`Consumer__Rt`) the moment two classes hold an alias of that name.
         Resolving through the alias keeps the include registered -- without
         it the module was silently not included at all, and every
         receiverless call to its module_function methods lost its callee
         and was refused as an unsupported call. */
      if (mod_id < 0 && mname) {
        const char *al = resolve_class_alias(c, mname);
        if (al) mod_id = comp_class_index(c, al);
      }
      if (mod_id < 0) {
        /* A module with no class of its own -- a builtin named through its
           path, `include IO::WaitReadable`. The AST name is the leaf, so the
           qualified string is rebuilt from the parent chain; that string is
           what `rescue IO::WaitReadable` compares against, since the module
           match is by name. Silently dropping these left the include with no
           effect at all (#1054, read_nonblock). */
        if (aty && sp_streq(aty, "ConstantPathNode") && mname) {
          char segs[8][64]; int nseg = 0;
          int qpar = nt_ref(nt, args[j], "parent");
          while (qpar >= 0 && nseg < 8) {
            const char *pty = nt_type(nt, qpar);
            const char *pn = nt_str(nt, qpar, "name");
            if (pty && sp_streq(pty, "ConstantReadNode") && pn) {
              snprintf(segs[nseg++], sizeof segs[0], "%s", pn);
              break;
            }
            if (pty && sp_streq(pty, "ConstantPathNode")) {
              if (pn) snprintf(segs[nseg++], sizeof segs[0], "%s", pn);
              qpar = nt_ref(nt, qpar, "parent");
              continue;
            }
            break;
          }
          char qual[256]; qual[0] = 0;
          for (int q = nseg - 1; q >= 0; q--) {
            strncat(qual, segs[q], sizeof qual - strlen(qual) - 1);
            strncat(qual, "::", sizeof qual - strlen(qual) - 1);
          }
          strncat(qual, mname, sizeof qual - strlen(qual) - 1);
          ClassInfo *cif2 = &c->classes[ci];
          int dup = 0;
          for (int m = 0; m < cif2->nincluded_mod_names; m++)
            if (sp_streq(cif2->included_mod_names[m], qual)) { dup = 1; break; }
          if (!dup) {
            if (cif2->nincluded_mod_names >= cif2->cincluded_mod_names) {
              cif2->cincluded_mod_names = cif2->cincluded_mod_names ? cif2->cincluded_mod_names * 2 : 4;
              char **nn = realloc(cif2->included_mod_names,
                                  sizeof(char *) * (size_t)cif2->cincluded_mod_names);
              if (!nn) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
              cif2->included_mod_names = nn;
            }
            cif2->included_mod_names[cif2->nincluded_mod_names++] = strdup(qual);
          }
        }
        continue;
      }
      /* record membership for `rescue M` matching (dedup across reopenings) */
      class_note_included_mod(c, ci, mod_id);
      /* snapshot count before adding new scopes to avoid re-scanning them */
      int snap = c->nscopes;
      for (int ms = 0; ms < snap; ms++) {
        Scope *src = &c->scopes[ms];
        if (src->class_id != mod_id || !src->name) continue;
        /* a module_function method whose body depends on its receiver needs a
           per-includer copy; everything else class-level stays module-side */
        if (src->is_cmethod &&
            !(src->is_module_function && module_function_self_dependent(c, ms)))
          continue;
        const char *dst_name = src->name;
        char inc_shadow[256];
        int own = comp_method_in_class(c, ci, src->name);
        if (own >= 0 && c->scopes[own].is_include_copy) {
          /* An earlier include of another module put this name here. Ruby's MRO
             puts the LAST include first, so this module supersedes it: rename
             the earlier copy to a shadow (carrying its own super target with
             it) and let this one take the real name, with its super, if any,
             reaching the earlier copy (#3731). */
          ClassInfo *cif = &c->classes[ci];
          snprintf(inc_shadow, sizeof inc_shadow, "__inc %d %s",
                   cif->prep_shadow_count++, src->name);
          for (int kk = 0; kk < cif->nprep_chain; kk++)
            if (sp_streq(cif->prep_from[kk], src->name)) {
              free(cif->prep_from[kk]); cif->prep_from[kk] = strdup(inc_shadow);
              break;
            }
          free(c->scopes[own].name);
          c->scopes[own].name = strdup(inc_shadow);
          if (scope_body_has_super(c, ms)) comp_prep_chain_add(cif, src->name, inc_shadow);
          else c->scopes[own].reachable = 0;   /* nothing can reach it now */
          own = -1;
        }
        if (own >= 0) {
          /* The class overrides the module method. If the override calls super,
             the module method is the super target: copy it under a shadow name
             and chain to it so emit_super reaches it via the prepend-super path.
             Otherwise the module method is simply shadowed -- nothing to emit. */
          if (!scope_body_has_super(c, own)) { if (!src->is_module_function) src->is_transplanted_source = 1; continue; }
          const char *existing = comp_prep_chain_target(c, ci, src->name);
          /* spaces keep the shadow unwritable in Ruby source, so an explicit
             `obj.__inc_0_tag` finds nothing and raises; mc() folds them back
             to underscores, so the C symbol is unchanged (#3738) */
          snprintf(inc_shadow, sizeof inc_shadow, "__inc %d %s",
                   c->classes[ci].prep_shadow_count++, src->name);
          if (existing) {
            /* Another included module already supplies the super target for this
               method. A later include takes precedence (Ruby MRO: C -> Mlast ->
               ... -> Mfirst), so retarget the class's super to this module's copy
               and chain this copy to the previously included one:
               name -> new_shadow -> earlier_shadow. */
            char *prev = strdup(existing);  /* stable copy: the slot is freed below */
            ClassInfo *cif = &c->classes[ci];
            for (int kk = 0; kk < cif->nprep_chain; kk++)
              if (sp_streq(cif->prep_from[kk], src->name)) {
                free(cif->prep_to[kk]);
                cif->prep_to[kk] = strdup(inc_shadow);
                break;
              }
            comp_prep_chain_add(cif, inc_shadow, prev);
            free(prev);
          }
          else {
            comp_prep_chain_add(&c->classes[ci], src->name, inc_shadow);
          }
          dst_name = inc_shadow;
        }
        /* Create a new scope sharing the same AST nodes but owned by ci. */
        Scope *dst = comp_scope_new(c, dst_name, src->def_node);
        int dst_idx = c->nscopes - 1;
        /* comp_scope_new may realloc c->scopes; re-derive src pointer. */
        src = &c->scopes[ms];
        /* Clone the body and re-attribute it to the target when either:
           (a) the target is a built-in class, where `self` has a different
           (scalar) type than the module's object self -- otherwise the shared
           SelfNode resolves to the module and e.g. `self.to_s` mis-dispatches; or
           (b) the copied method itself calls `super` (a multi-module chain), so
           its super node resolves to this shadow scope (and thus the class's prep
           chain) rather than to the source module, where the chain isn't set; or
           (c) the body has a receiverless instance_exec/eval, whose block rebinds
           self to the includer -- a shared body would resolve that self to the
           module and mis-splice (see scope_body_has_receiverless_ie); or
           (d) the body touches an ivar, which must type against the includer's
           slot rather than a divergent module-owned slot (scope_body_uses_ivar); or
           (e) the method takes a &block param. A block-taking module method must be
           inlinable per includer so a forwarded block literal threads through the
           call chain and can be spliced (a collector builder `col(&b) = build(tag,
           &b)` forwarding into a self-rebinding instance_exec); shared, it emits as
           a real function that lifts the block to a proc and breaks the chain. */
        /* Clone UNCONDITIONALLY: a shared body carries ONE node-type cache
           and ONE scope attribution across every includer, so when two
           includers' call sites settle the params differently (an --rbs
           StrStr pin in one test class, symbol-keyed fixtures in another)
           the signature is emitted from the emitting includer's scope while
           the body reads the LAST-walked includer's types -- a hard C
           mismatch (#2008). Per-includer cloning is how CRuby's iclass
           semantics resolve self/ivars/blocks anyway (the previous
           conditions); divergent inference makes it necessary for every
           method. */
        if (src->body >= 0) {
          int nb = nt_clone_subtree((NodeTable *)nt, src->body);
          if (nb >= 0) {
            comp_grow_node_arrays(c);
            src = &c->scopes[ms]; dst = &c->scopes[dst_idx];
            dst->body = nb;
            walk_scope(c, nb, dst_idx, ci);
            g_inc_did_clone = 1;
          }
          else dst->body = src->body;
        }
else {
          dst->body = src->body;
        }
        dst->origin_module_ci = src->class_id + 1;  /* #owner names it (#3662) */
        dst->class_id = ci;
        dst->is_cmethod = 0;
        dst->is_include_copy = 1;
        dst->reachable = src->reachable;
        dst->yields = src->yields;
        dst->nrequired = src->nrequired;
        dst->rest_idx = src->rest_idx;
        dst->kwrest_idx = src->kwrest_idx;
        if (src->blk_param) dst->blk_param = strdup(src->blk_param);
        /* ...but a module_function original keeps its module-side spelling:
           `Rt.peek` is a real call whoever also includes Rt, so the source is
           not copied AWAY, only copied FROM. */
        if (!src->is_module_function) src->is_transplanted_source = 1;
        scope_copy_params(dst, src);
        if (scope_own_defaults(c, dst_idx)) g_inc_did_clone = 1;
        src = &c->scopes[ms]; dst = &c->scopes[dst_idx];
        /* Scan source body for ivar accesses and register them in the
           destination class so codegen's struct layout includes them. */
        intern_scope_ivars(c, ms, ci);
      }
    }
  }
}

/* For each class, find `include M` declarations in ALL class bodies
   (including reopenings) and transplant M's instance methods into the
   class so they are reachable via comp_method_in_chain. */
/* The class indices a body's `include` statements name (the ones that resolve
   to a class of the program), for the ordering below. */
static int body_included_mods(Compiler *c, int body_node, int *out, int cap) {
  const NodeTable *nt = c->nt;
  int n = 0, cnt = 0;
  const int *stmts = body_node >= 0 ? nt_arr(nt, body_node, "body", &n) : NULL;
  for (int k = 0; k < n && cnt < cap; k++) {
    int s = stmts[k];
    const char *sty = nt_type(nt, s);
    if (!sty || !sp_streq(sty, "CallNode")) continue;
    const char *nm = nt_str(nt, s, "name");
    if (!nm || !sp_streq(nm, "include") || nt_ref(nt, s, "receiver") >= 0) continue;
    int anode = nt_ref(nt, s, "arguments");
    int an = 0;
    const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
    for (int j = 0; j < an && cnt < cap; j++) {
      const char *aty = nt_type(nt, args[j]);
      const char *mname = (aty && (sp_streq(aty, "ConstantReadNode") || sp_streq(aty, "ConstantPathNode")))
                          ? nt_str(nt, args[j], "name") : NULL;
      int mod_id = mname ? comp_class_index(c, mname) : -1;
      if (mod_id >= 0) out[cnt++] = mod_id;
    }
  }
  return cnt;
}

void register_includes(Compiler *c) {
  const NodeTable *nt = c->nt;
  g_inc_did_clone = 0;
  /* Every class body that can carry an `include`: the definition first, then
     each reopening in source order. A body is processed only once every
     module it includes has had ALL of its own bodies processed, because the
     transplant copies the module's method list as it stands: with
     `module M; include Inner; end` in a reopening that came after
     `class C; include M; end` in the walk, C copied M before M had Inner's
     methods and could not reach them (#4517). A cycle (mutual includes)
     falls back to source order. */
  int *bci, *bnode;
  int nb = class_body_list(c, &bci, &bnode);
  int *remaining = calloc((size_t)c->nclasses, sizeof(int));
  char *done = calloc((size_t)nb, 1);
  for (int b = 0; b < nb; b++) remaining[bci[b]]++;
  int left = nb;
  while (left > 0) {
    int progress = 0;
    for (int b = 0; b < nb; b++) {
      if (done[b]) continue;
      int ready = 1;
      /* this class's earlier bodies first (the definition before a reopening) */
      for (int b2 = 0; b2 < b && ready; b2++)
        if (!done[b2] && bci[b2] == bci[b]) ready = 0;
      int mods[64];
      int nm = ready ? body_included_mods(c, bnode[b], mods, 64) : 0;
      for (int m = 0; m < nm && ready; m++)
        if (mods[m] != bci[b] && remaining[mods[m]] > 0) ready = 0;
      if (!ready) continue;
      process_include_body(c, bci[b], bnode[b]);
      done[b] = 1; remaining[bci[b]]--; left--; progress = 1;
    }
    if (!progress) {   /* a cycle: take the first body left, in source order */
      for (int b = 0; b < nb; b++) {
        if (done[b]) continue;
        process_include_body(c, bci[b], bnode[b]);
        done[b] = 1; remaining[bci[b]]--; left--;
        break;
      }
    }
  }
  free(bci); free(bnode); free(remaining); free(done);
  if (g_inc_did_clone) register_locals(c);
}

int cmethod_needs_specialization(Compiler *c, int mi, int ci, int def_cls, int *has_new);
static void specialize_cmethod_for(Compiler *c, int mi, int def_cls, int ci);

/* attr_reader/attr_accessor/attr_writer and alias_method in a MODULE body
   belong to every class that includes it, just like a plain def. The transplant
   copies method scopes only, so carry the declarative surface across too: the
   reader/writer names (with their backing ivars) and the alias table (#3774). */
void register_include_attrs(Compiler *c) {
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    for (int k = 0; k < cls->nincluded_mods; k++) {
      int mi = cls->included_mods[k];
      if (mi < 0 || mi >= c->nclasses || mi == ci) continue;
      ClassInfo *mod = &c->classes[mi];
      for (int r = 0; r < mod->nreaders; r++) {
        char ivname[256];
        snprintf(ivname, sizeof ivname, "@%s", mod->readers[r]);
        comp_ivar_intern(cls, ivname);
        comp_add_reader(cls, mod->readers[r]);
      }
      for (int w = 0; w < mod->nwriters; w++) {
        char ivname[256];
        snprintf(ivname, sizeof ivname, "@%s", mod->writers[w]);
        comp_ivar_intern(cls, ivname);
        comp_add_writer(cls, mod->writers[w]);
      }
      for (int a = 0; a < mod->naliases; a++)
        comp_add_alias(cls, mod->alias_new[a], mod->alias_old[a]);
    }
  }
}

/* The class a `super` from class `ci`'s method `mname` lands on answers it with
   an attribute: attr_reader/writer/accessor or a Struct member, declared there
   or by a module it includes. A reader or writer is copied down into every
   subclass (inherit_members), so the class that declares it is the highest one
   still holding it; a `def` found on the way up answers first. */
static int super_lands_on_attr(Compiler *c, int ci, const char *base, int want_write) {
  char mname[300];
  snprintf(mname, sizeof mname, want_write ? "%s=" : "%s", base);
  ClassInfo *self_cls = &c->classes[ci];
  for (int k = 0; k < self_cls->nincluded_mods; k++) {
    int mi = self_cls->included_mods[k];
    if (mi < 0 || mi >= c->nclasses || mi == ci) continue;
    ClassInfo *mod = &c->classes[mi];
    if (want_write ? comp_is_writer(mod, base) : comp_is_reader(mod, base)) return 1;
  }
  for (int k = self_cls->parent; k >= 0; k = c->classes[k].parent) {
    if (comp_method_in_class(c, k, mname) >= 0) return 0;
    ClassInfo *kc = &c->classes[k];
    if (!(want_write ? comp_is_writer(kc, base) : comp_is_reader(kc, base))) continue;
    int up = kc->parent;
    if (up < 0 || !(want_write ? comp_is_writer(&c->classes[up], base)
                               : comp_is_reader(&c->classes[up], base)))
      return 1;
  }
  return 0;
}

/* `super` from a method overriding an attribute calls the generated reader or
   writer, which has no function of its own: it reads or writes the backing
   ivar. Rewrite the super into that ivar access in place, so the write types
   the ivar like any other and the value is the written argument. Left alone,
   the super found no method in the chain and raised NoMethodError. */
void rewrite_attr_supers(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    NodeKind nk = nt_kind(nt, id);
    if (nk != NK_SuperNode && nk != NK_ForwardingSuperNode) continue;
    if (nt_ref(nt, id, "block") >= 0) continue;
    Scope *s = comp_scope_of(c, id);
    if (!s || !s->name || s->class_id < 0 || s->is_cmethod) continue;
    ClassInfo *cls = &c->classes[s->class_id];
    if (comp_class_is_module(c, cls)) continue;
    if (comp_prep_chain_target(c, s->class_id, s->name)) continue;
    const char *uname = comp_prep_user_name(s->name);
    char base[256];
    int is_write = name_is_plain_setter(uname);
    if (is_write) { if (!setter_base_name(uname, base, sizeof base)) continue; }
    else if (snprintf(base, sizeof base, "%s", uname) >= (int)sizeof base) continue;
    /* the value a writer's super passes: the one explicit argument, or the
       method's one plain positional parameter for a bare super */
    int val = -1;
    const char *fwd_name = NULL;
    if (nk == NK_SuperNode) {
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an != (is_write ? 1 : 0)) continue;
      if (is_write) {
        NodeKind ak = nt_kind(nt, av[0]);
        const char *aty = nt_type(nt, av[0]);
        if (ak == NK_SplatNode || ak == NK_KeywordHashNode || ak == NK_BlockArgumentNode ||
            (aty && sp_streq(aty, "ForwardingArgumentsNode")))
          continue;
        val = av[0];
      }
    }
    else {
      int dn = s->def_node;
      int params = dn >= 0 ? nt_ref(nt, dn, "parameters") : -1;
      int nkw = 0, nopt = 0, nreq = 0, npost = 0;
      if (params >= 0) {
        nt_arr(nt, params, "keywords", &nkw);
        nt_arr(nt, params, "optionals", &nopt);
        nt_arr(nt, params, "requireds", &nreq);
        nt_arr(nt, params, "posts", &npost);
        if (nt_ref(nt, params, "rest") >= 0 || nt_ref(nt, params, "keyword_rest") >= 0 ||
            nt_ref(nt, params, "block") >= 0 || nkw || npost)
          continue;
      }
      if (nreq + nopt != (is_write ? 1 : 0) || s->nparams != nreq + nopt) continue;
      if (is_write) fwd_name = s->pnames[0];
    }
    if (!super_lands_on_attr(c, s->class_id, base, is_write)) continue;
    char ivn[260];
    snprintf(ivn, sizeof ivn, "@%s", base);
    int line = (int)nt_int(nt, id, "node_line", 0);
    int file = (int)nt_int(nt, id, "node_file", 0);
    if (fwd_name) {
      val = nt_new_node(nt, "LocalVariableReadNode");
      if (val < 0) continue;
      nt_node_set_str(nt, val, "name", fwd_name);
      nt_node_set_int(nt, val, "depth", 0);
      if (line) nt_node_set_int(nt, val, "node_line", line);
      if (file) nt_node_set_int(nt, val, "node_file", file);
      comp_grow_node_arrays(c);
      c->nscope[val] = c->nscope[id];
    }
    nt_node_reset(nt, id, is_write ? "InstanceVariableWriteNode" : "InstanceVariableReadNode");
    nt_node_set_str(nt, id, "name", ivn);
    if (is_write) nt_node_set_ref(nt, id, "value", val);
    if (line) nt_node_set_int(nt, id, "node_line", line);
    if (file) nt_node_set_int(nt, id, "node_file", file);
    comp_ivar_intern(cls, ivn);
  }
}

/* A module method named by `Mod.instance_method(:m)` / `Mod.method(:m)` is
   referenced directly, so its own function must be emitted even though an
   include copied it into a class (which marks the source transplanted and
   skips it -- the Method object then named an undeclared symbol, #3659). */
void unmark_referenced_module_sources(Compiler *c) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "instance_method") && !sp_streq(nm, "method"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    if (nt_kind(nt, recv) != NK_ConstantReadNode && nt_kind(nt, recv) != NK_ConstantPathNode) continue;
    const char *cn = nt_str(nt, recv, "name");
    int ci = cn ? comp_class_index(c, cn) : -1;
    if (ci < 0) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an != 1 || !av || nt_kind(nt, av[0]) != NK_SymbolNode) continue;
    const char *mn = nt_str(nt, av[0], "value");
    if (!mn) continue;
    for (int si = 1; si < c->nscopes; si++) {
      Scope *sc = &c->scopes[si];
      if (sc->class_id != ci || sc->is_cmethod || !sc->name || !sp_streq(sc->name, mn)) continue;
      sc->is_transplanted_source = 0;
      sc->reachable = 1;
    }
  }
}

/* `inherited`: a superclass already extends mod_id, so the module is among
   the singleton's ancestors. CRuby ignores the extend for the lookup: the
   module stays behind the superclass, and it supersedes no earlier module's
   copy here. */
static int extend_class_with(Compiler *c, int ci, int mod_id, int inherited) {
  int did_clone = 0;
  int snap = c->nscopes;
  for (int ms = 0; ms < snap; ms++) {
    Scope *src = &c->scopes[ms];
    /* Only transplant instance methods; self.* on the module stay on it.
       A module_function method is registered class-level on the module
       but is an instance method of it too, so `extend` hands it to the
       extending class like any other; skipped, a bare call to it from
       one of that class's own class methods fell to the INSTANCE copy an
       `include` of the same module had made, with the class object cast
       to an instance pointer (#4648). */
    if (src->class_id != mod_id || (src->is_cmethod && !src->is_module_function) || !src->name) continue;
    int own = comp_cmethod_in_class(c, ci, src->name);
    if (own >= 0 && (inherited || !c->scopes[own].is_extend_copy)) continue;   /* the class's own */
    if (own >= 0) {
      /* An earlier extend put this name here. The later module comes first
         among the singleton's ancestors, so it supersedes: the earlier copy
         takes a shadow name, carrying its own super target with it, and a
         super in this module's copy reaches it, as include does (#3731). */
      ClassInfo *cif = &c->classes[ci];
      char shadow[256], key[320];
      snprintf(shadow, sizeof shadow, "__inc %d %s", cif->prep_shadow_count++, src->name);
      snprintf(key, sizeof key, "self.%s", src->name);
      for (int kk = 0; kk < cif->nprep_chain; kk++)
        if (sp_streq(cif->prep_from[kk], key)) {
          free(cif->prep_from[kk]);
          snprintf(key, sizeof key, "self.%s", shadow);
          cif->prep_from[kk] = strdup(key);
          break;
        }
      free(c->scopes[own].name);
      c->scopes[own].name = strdup(shadow);
      if (scope_body_has_super(c, ms)) comp_cprep_chain_add(cif, src->name, shadow);
      else c->scopes[own].reachable = 0;   /* nothing can reach it now */
      src = &c->scopes[ms];
    }
    /* Always a clone re-walked against `ci`, as include does: a shared
       body kept the module's attribution, so `self`, the block and the
       parameter types resolved against the module, not the class.
       The ivars the body names become class-level ivars of the
       extending class, and their storage is declared from its ivar
       list -- register them even when nothing assigns one there. */
    { const NodeTable *nt2 = c->nt;
      NT_FOREACH_KIND(nt2, NK_InstanceVariableReadNode, ivid)
        if (c->nscope[ivid] == ms && nt_str(nt2, ivid, "name"))
          comp_ivar_intern(&c->classes[ci], nt_str(nt2, ivid, "name"));
      NT_FOREACH_KIND(nt2, NK_InstanceVariableWriteNode, ivid)
        if (c->nscope[ivid] == ms && nt_str(nt2, ivid, "name"))
          comp_ivar_intern(&c->classes[ci], nt_str(nt2, ivid, "name"));
      /* `@memo ||= v` / `&&=` / `+=` name the ivar too, and may be
         the body's only mention of it */
      static const NodeKind ow[] = { NK_InstanceVariableOrWriteNode,
                                     NK_InstanceVariableAndWriteNode,
                                     NK_InstanceVariableOperatorWriteNode };
      for (int k = 0; k < 3; k++)
        NT_FOREACH_KIND(nt2, ow[k], ivid)
          if (c->nscope[ivid] == ms && nt_str(nt2, ivid, "name"))
            comp_ivar_intern(&c->classes[ci], nt_str(nt2, ivid, "name")); }
    specialize_cmethod_for(c, ms, mod_id, ci);
    src = &c->scopes[ms];  /* realloc-safe */
    { int cp = comp_cmethod_in_class(c, ci, src->name);
      if (cp >= 0) c->scopes[cp].is_extend_copy = 1; }
    did_clone = 1;
    /* a module_function stays callable on the module itself
       (`Coordinates.countdown(1)`), so its source is not dead */
    if (!src->is_module_function) src->is_transplanted_source = 1;
  }
  return did_clone;
}

static int class_is_root(Compiler *c, int ci) {
  ClassInfo *k = &c->classes[ci];
  if (k->is_native_class || is_builtin_reopen(k->name) || comp_class_is_module(c, k)) return 0;
  if (k->parent < 0) return 1;
  ClassInfo *p = &c->classes[k->parent];
  return p->is_native_class || is_builtin_reopen(p->name);
}

/* For each class, find `extend M` declarations and transplant M's instance
   methods as class methods (is_cmethod=1) so they are callable as C.m. */
/* The module a constant argument of `extend` names, or -1. */
static int extend_arg_module(Compiler *c, int arg) {
  const NodeTable *nt = c->nt;
  const char *aty = nt_type(nt, arg);
  const char *mname = NULL;
  if (aty && sp_streq(aty, "ConstantReadNode")) mname = nt_str(nt, arg, "name");
  else if (aty && sp_streq(aty, "ConstantPathNode")) mname = nt_str(nt, arg, "name");
  return mname ? comp_class_index(c, mname) : -1;
}

/* Whether class body `cn` has a bare `extend` naming mod_id. */
static int body_extends_module(Compiler *c, int cn, int mod_id) {
  const NodeTable *nt = c->nt;
  int body = class_def_body(c, cn);
  int n = 0;
  const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
  for (int k = 0; k < n; k++) {
    int s = stmts[k];
    if (nt_kind(nt, s) != NK_CallNode || nt_ref(nt, s, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, s, "name");
    if (!nm || !sp_streq(nm, "extend")) continue;
    int anode = nt_ref(nt, s, "arguments");
    int an = 0;
    const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
    for (int j = 0; j < an; j++)
      if (extend_arg_module(c, args[j]) == mod_id) return 1;
  }
  return 0;
}

void register_extends(Compiler *c) {
  const NodeTable *nt = c->nt;
  int did_clone = 0;
  /* Every class and module body, with the class it defines, collected once in
     node order. Asking each class to rescan the whole table and look every
     body's name up again was classes x nodes x (a linear name lookup, the
     index is not frozen yet): the largest single cost of analysis on a
     program with many classes (#4847). The per-class walk below visits the
     same bodies in the same order. Names do not change here, and a class
     this pass creates has no body of its own. */
  int nbody = 0, capbody = 16;
  int *body_node = malloc(sizeof(int) * (size_t)capbody);
  int *body_cls = malloc(sizeof(int) * (size_t)capbody);
  if (!body_node || !body_cls) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int cn = 0; cn < nt->count; cn++) {
    /* the body this class is defined by, named the way every other pass
       reads a ClassNode's name */
    const char *cnm = class_body_name(c, cn);
    int bci = cnm ? comp_class_index(c, cnm) : -1;
    if (bci < 0) continue;
    if (nbody == capbody) {
      capbody *= 2;
      body_node = realloc(body_node, sizeof(int) * (size_t)capbody);
      body_cls = realloc(body_cls, sizeof(int) * (size_t)capbody);
      if (!body_node || !body_cls) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    body_node[nbody] = cn; body_cls[nbody] = bci; nbody++;
  }
  int cls_mod = comp_class_index(c, class_reopen_mod);
  int nseen = 0, capseen = 8;
  int *seen = malloc(sizeof(int) * (size_t)capseen);
  if (!seen) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int ci = 0; ci < c->nclasses; ci++) {
   nseen = 0;
   /* Every body that defines this class, not only the first: `extend M` is
      commonly written in a REOPENING of the class, and reading def_node alone
      never saw it, so the module's methods were never transplanted and a call
      to one did not resolve (#3802). */
   for (int bi = 0; bi < nbody; bi++) {
    if (body_cls[bi] != ci) continue;
    int cn = body_node[bi];
    int body = class_def_body(c, cn);
    int n = 0;
    const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    for (int k = 0; k < n; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty || !sp_streq(sty, "CallNode")) continue;
      const char *nm = nt_str(nt, s, "name");
      if (!nm || !sp_streq(nm, "extend")) continue;
      if (nt_ref(nt, s, "receiver") >= 0) continue;
      int anode = nt_ref(nt, s, "arguments");
      int an = 0;
      const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
      /* `extend A, B` extends B first, so A ends up in front */
      for (int j = an - 1; j >= 0; j--) {
        const char *aty = nt_type(nt, args[j]);
        const char *mname = NULL;
        if (aty && sp_streq(aty, "ConstantReadNode")) mname = nt_str(nt, args[j], "name");
        else if (aty && sp_streq(aty, "ConstantPathNode")) mname = nt_str(nt, args[j], "name");
        int mod_id = mname ? comp_class_index(c, mname) : -1;
        if (mod_id < 0) continue;
        /* extending a module the class already extends is a no-op: it stays
           where it was, and a second copy in front would hide the later one */
        int again = 0;
        for (int e = 0; e < nseen; e++) if (seen[e] == mod_id) again = 1;
        if (again) continue;
        if (nseen == capseen) {
          capseen *= 2;
          seen = realloc(seen, sizeof(int) * (size_t)capseen);
          if (!seen) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
        }
        seen[nseen++] = mod_id;
        int inherited = 0;
        for (int p = c->classes[ci].parent; p >= 0 && !inherited; p = c->classes[p].parent)
          for (int bj = 0; bj < nbody && !inherited; bj++)
            if (body_cls[bj] == p) inherited = body_extends_module(c, body_node[bj], mod_id);
        did_clone |= extend_class_with(c, ci, mod_id, inherited);
      }
    }
   }
   if (cls_mod >= 0 && (ci == cls_mod || class_is_root(c, ci)))
     did_clone |= extend_class_with(c, ci, cls_mod, 0);
  }
  /* The cloned bodies introduced new local nodes, and register_locals ran
     before this pass: a local first assigned in the clone had no slot, so
     it was never declared and every read of it answered nil (#4535). The
     include and inherited-class-method clones re-register the same way. */
  free(body_node); free(body_cls); free(seen);
  if (did_clone) register_locals(c);
}

/* True if class method scope `mi`'s body contains a bare `new` call (which
   must rebind to the calling subclass, not the defining class). */
int cmethod_has_bare_new(Compiler *c, int mi) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    if (c->nscope[id] != mi) continue;
    const char *ty = nt_type(nt, id);
    if (ty && sp_streq(ty, "CallNode") && nt_ref(nt, id, "receiver") < 0 &&
        nt_str(nt, id, "name") && sp_streq(nt_str(nt, id, "name"), "new"))
      return 1;
  }
  return 0;
}

/* Does the inherited cls method `mi` (defined on def_cls), run as a class method
   of `ci`, reach a bare cmethod call that resolves to a DIFFERENT method for ci
   -- directly, or TRANSITIVELY through a non-overriding intermediate cmethod?
   E.g. `last_row` calls `all_rows` (which ci does not override) calls
   `table_name` (which ci does): `last_row` reaches an override and so must be
   specialized for ci too, or its implicit-self chain stays bound to the base and
   the override is skipped (#1451). The depth cap bounds the walk and doubles as a
   cycle guard for mutually-recursive cmethods. */
static int cmethod_reaches_override_walk(Compiler *c, int mi, int ci, int def_cls,
                                         unsigned char *seen) {
  /* A visited set, not a depth cap: the walk branches at every bare call,
     so a depth cap alone is exponential on a class-side DSL whose methods
     call each other (a Rails-shaped app spent 20+ minutes here). Reachability
     of an override is exactly what a DFS with a visited set answers. */
  if (mi < 0 || mi >= c->nscopes || seen[mi]) return 0;
  seen[mi] = 1;
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    if (c->nscope[id] != mi) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;   /* receiverless only */
    const char *nm = nt_str(nt, id, "name");
    if (!nm || sp_streq(nm, "new")) continue;          /* direct `new` is the caller's has_new path */
    int sub_def = -1;
    int mci = comp_cmethod_in_chain(c, ci, nm, NULL);
    int mdef = comp_cmethod_in_chain(c, def_cls, nm, &sub_def);
    if (mci >= 0 && mci != mdef) return 1;            /* ci overrides nm directly */
    /* nm is inherited unchanged by ci -- but its own body may still reach an
       override; descend into the def-chain version. */
    if (mdef >= 0 && sub_def >= 0 &&
        cmethod_reaches_override_walk(c, mdef, ci, sub_def, seen)) return 1;
  }
  return 0;
}

static int cmethod_reaches_override(Compiler *c, int mi, int ci, int def_cls, int depth) {
  (void)depth;
  unsigned char *seen = calloc((size_t)c->nscopes + 1, 1);
  int r = seen ? cmethod_reaches_override_walk(c, mi, ci, def_cls, seen) : 0;
  free(seen);
  return r;
}

/* Does the inherited cls method `mi` (defined on def_cls) name a class-level
   @ivar -- in its own body, or TRANSITIVELY through a bare call to another
   cmethod the subclass inherits unchanged? A class-level @ivar is per-class, so
   whoever ends up reading it has to read the CALLING class's storage; a DSL
   writes one indirectly (`field` calls `fields`, which is the one that says
   `@fields`), and reading only mi's own body left `field` unspecialized and
   still pointed at the base class's slot (#4051). The depth cap bounds the walk
   and doubles as a cycle guard for mutually-recursive cmethods. */
static int cmethod_reaches_class_ivar_walk(Compiler *c, int mi, int def_cls, unsigned char *seen) {
  if (mi < 0 || mi >= c->nscopes || seen[mi]) return 0;
  seen[mi] = 1;
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    if (c->nscope[id] != mi) continue;
    NodeKind k = nt_kind(nt, id);
    if (k == NK_InstanceVariableReadNode || k == NK_InstanceVariableWriteNode ||
        k == NK_InstanceVariableOperatorWriteNode || k == NK_InstanceVariableOrWriteNode ||
        k == NK_InstanceVariableAndWriteNode || k == NK_InstanceVariableTargetNode) return 1;
  }
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    if (c->nscope[id] != mi) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;   /* receiverless only */
    const char *nm = nt_str(nt, id, "name");
    if (!nm || sp_streq(nm, "new")) continue;
    int sub_def = -1;
    int mdef = comp_cmethod_in_chain(c, def_cls, nm, &sub_def);
    if (mdef >= 0 && mdef != mi && sub_def >= 0 &&
        cmethod_reaches_class_ivar_walk(c, mdef, sub_def, seen)) return 1;
  }
  return 0;
}

static int cmethod_reaches_class_ivar(Compiler *c, int mi, int def_cls, int depth) {
  (void)depth;
  unsigned char *seen = calloc((size_t)c->nscopes + 1, 1);
  int r = seen ? cmethod_reaches_class_ivar_walk(c, mi, def_cls, seen) : 0;
  free(seen);
  return r;
}

/* Does the inherited cls method `mi` (defined on def_cls) contain a bare call
   that would resolve differently when run as a class method of `ci`? That is:
   a bare `new` (constructs ci), or a bare cmethod call that resolves to ci's
   own version directly or transitively (cmethod_reaches_override). */
static int cmethod_needs_specialization_d(Compiler *c, int mi, int ci, int def_cls,
                                          int *has_new, int depth);

/* A bare call to a SIBLING class method that itself needs specializing. The
   sibling's `new` constructs the CALLING class, and reaching it through this
   method must not lose that -- but this method's own body has no `new` in it,
   so none of the tests below saw a reason to specialize it, and the generic
   copy was used. Its bare call then resolved in the DEFINING class's chain and
   built the base: `Closed.create_for` -> `create!` -> `new` answered a Room,
   with no diagnostic (#4427). One hop was right because the one hop is what
   the tests below cover.
   specialize_cmethod_for already recurses into such calls once a method has
   been chosen -- its own comment calls that the #1451 fix. What was missing is
   CHOOSING this one. Depth-capped like its neighbours, which is also what
   terminates a pair of cmethods that call each other. */
static int cmethod_calls_specialized_sibling(Compiler *c, int mi, int ci, int def_cls, int depth) {
  if (depth > 8) return 0;
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    if (c->nscope[id] != mi) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || sp_streq(nm, "new")) continue;
    int sdef = -1;
    int smi = comp_cmethod_in_chain(c, def_cls, nm, &sdef);
    if (smi < 0 || smi == mi || sdef < 0) continue;
    if (comp_cmethod_in_class(c, ci, nm) >= 0) continue;   /* ci owns it already */
    int hn = 0;
    if (cmethod_needs_specialization_d(c, smi, ci, sdef, &hn, depth + 1)) return 1;
  }
  return 0;
}

int cmethod_needs_specialization(Compiler *c, int mi, int ci, int def_cls, int *has_new) {
  return cmethod_needs_specialization_d(c, mi, ci, def_cls, has_new, 0);
}

static int cmethod_needs_specialization_d(Compiler *c, int mi, int ci, int def_cls,
                                          int *has_new, int depth) {
  const NodeTable *nt = c->nt;
  int need = 0;
  if (has_new) *has_new = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    if (c->nscope[id] != mi) continue;
    /* `allocate` / `self.allocate` builds the calling class, as a bare `new` does */
    if (allocate_on_own_class(c, id) >= 0) { if (has_new) *has_new = 1; need = 1; continue; }
    if (nt_ref(nt, id, "receiver") >= 0) continue;   /* receiverless only */
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    if (sp_streq(nm, "new")) { if (has_new) *has_new = 1; need = 1; }
  }
  /* A class-level @ivar read/written in the body is NOT inherited in Ruby: each
     class holds its own (civ_<Cls>_<name>), and an inherited class method must
     bind to the CALLING class's storage, not the defining class's. Specializing
     the method for ci re-attributes its body to ci, so its civ_ references key
     on ci (civ_<Sub>_...). Without this, Sub.tag read Base's civ_ -- a value Ruby
     never shares down the hierarchy. @@class-variables ARE shared and use a
     separate mechanism, so only plain @ivar nodes trigger this. */
  if (!need && cmethod_reaches_class_ivar(c, mi, def_cls, 0)) need = 1;
  if (cmethod_reaches_override(c, mi, ci, def_cls, 0)) need = 1;
  if (!need && cmethod_calls_specialized_sibling(c, mi, ci, def_cls, depth)) need = 1;
  return need;
}

/* Clone inherited cls method `mi` (defined on def_cls) as a ci-owned copy whose
   body is re-attributed to ci, so its bare `new` constructs ci and its
   implicit-self cmethod calls resolve in ci's chain. Then recurse: any bare
   cmethod call in mi's body that ci inherits unchanged but that itself needs
   specialization (reaches an override, or does `new`) is cloned for ci too, so
   the cloned body's implicit-self call rebinds to ci's copy instead of staying
   on the base -- that transitive rebind is the #1451 fix. The ci-already-owns
   guard makes this idempotent and terminates mutually-recursive cmethods. */
static int body_tail_is_bare_new(const NodeTable *nt, int body) {
  int n = 0;
  const int *st = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &n) : NULL;
  int tail = st && n > 0 ? st[n - 1] : body;
  if (tail < 0 || nt_kind(nt, tail) != NK_CallNode) return 0;
  const char *nm = nt_str(nt, tail, "name");
  int r = nt_ref(nt, tail, "receiver");
  return nm && sp_streq(nm, "new") && (r < 0 || nt_kind(nt, r) == NK_SelfNode);
}
static void specialize_cmethod_for(Compiler *c, int mi, int def_cls, int ci) {
  if (comp_cmethod_in_class(c, ci, c->scopes[mi].name) >= 0) return;
  NodeTable *nt = (NodeTable *)c->nt;
  int has_new = 0;
  (void)cmethod_needs_specialization(c, mi, ci, def_cls, &has_new);
  int src_body = c->scopes[mi].body;
  int new_body = src_body >= 0 ? nt_clone_subtree(nt, src_body) : -1;
  if (src_body >= 0 && new_body < 0) return;  /* clone failed: skip */
  comp_grow_node_arrays(c);
  Scope *src = &c->scopes[mi];
  Scope *dst = comp_scope_new(c, src->name, src->def_node);
  src = &c->scopes[mi];  /* realloc-safe */
  int dst_idx = c->nscopes - 1;
  dst->body = new_body;
  if (new_body >= 0) walk_scope(c, new_body, dst_idx, ci);
  dst->class_id = ci;
  dst->is_cmethod = 1;
  dst->yields = src->yields;
  dst->nrequired = src->nrequired;
  dst->rest_idx = src->rest_idx;
  dst->kwrest_idx = src->kwrest_idx;
  /* A create method ending in its bare `new` returns the specialized subclass
     instance, so pin its return type. One that only builds an instance on the
     way to some other value (`w = new(p); w.finish`), and every other
     specialization, lets normal return inference compute the type from the
     cloned, ci-attributed body. */
  if (has_new && body_tail_is_bare_new(nt, new_body)) {
    dst->ret = ty_object(ci);
    dst->ret_specialized = 1;
  }
  if (src->blk_param) dst->blk_param = strdup(src->blk_param);
  scope_copy_params(dst, src);
  scope_own_defaults(c, dst_idx);
  src = &c->scopes[mi]; dst = &c->scopes[dst_idx];
  /* Recurse into the inherited intermediates this body reaches. Scan the
     ORIGINAL mi body (cloned nodes are attributed to dst, not mi); a sub-clone
     reallocs c->scopes/c->nscope, so use indices and refetch. */
  int scan_n = nt->count;
  for (int id = 0; id < scan_n; id++) {
    if (c->nscope[id] != mi) continue;
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || sp_streq(nm, "new")) continue;
    int sub_def = -1;
    int sub_mi = comp_cmethod_in_chain(c, ci, nm, &sub_def);
    if (sub_mi < 0 || sub_def == ci) continue;   /* unresolved, or ci-native */
    int sub_new = 0;
    if (cmethod_needs_specialization(c, sub_mi, ci, sub_def, &sub_new))
      specialize_cmethod_for(c, sub_mi, sub_def, ci);
  }
}

/* Is `name` called anywhere on a receiver that is neither a constant nor self,
   one that may hold a Class value known only at run time? */
static int name_called_on_dynamic_recv(Compiler *c, const char *name, int node_count) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < node_count; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, name)) continue;
    int r = nt_ref(nt, id, "receiver");
    if (r < 0) continue;
    NodeKind rk = nt_kind(nt, r);
    if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode || rk == NK_SelfNode) continue;
    return 1;
  }
  return 0;
}

/* The class method a call runs on its receiver: its own name, or for a
   `K.method(:m)` the m the Method binds, which runs on K when called as
   `K.m` does (`Sub.method(:make).call` constructs a Sub). */
static const char *cls_call_name(Compiler *c, int id) {
  const char *nm = nt_str(c->nt, id, "name");
  const char *sym = nm && sp_streq(nm, "method") ? method_sym_arg(c, id) : NULL;
  return sym ? sym : nm;
}

/* `Subclass.create` where `create` is an inherited class method whose body
   does `new(...)`: Ruby's bare `new` constructs the *calling* class, so copy
   the inherited cls method into each calling subclass (the copy's class_id
   makes codegen's `new` resolve to that subclass). The defining-class source
   is DCE'd unless it is itself called directly. Covers #224 / #229 / #1451. */
void specialize_inherited_cls_new(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int snap = c->nscopes;
  int node_count = nt->count;   /* don't scan nodes appended by cloning */
  int did_clone = 0;
  /* self in a class body is the class, so a receiver-less call there names a
     class method of THAT class even though the statement belongs to no class
     method scope. Map each such statement to its class up front; the loop below
     then treats it exactly like the `Klass.m` form, so an inherited DSL method
     (`class User < Model; field :id; end`) specializes for User and writes
     User's class-level ivar instead of Model's (#4051). Direct body statements
     only, matching what codegen runs from a class body. */
  int *body_cls = malloc((size_t)node_count * sizeof(int));
  if (body_cls) {
    for (int i = 0; i < node_count; i++) body_cls[i] = -1;
    for (int cn = 0; cn < node_count; cn++) {
      if (nt_kind(nt, cn) != NK_ClassNode && nt_kind(nt, cn) != NK_ModuleNode) continue;
      int cp = nt_ref(nt, cn, "constant_path");
      const char *cnm = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
      int bci = cnm ? comp_class_index(c, cnm) : -1;
      if (bci < 0) continue;
      int bd = nt_ref(nt, cn, "body");
      int bn = 0;
      const int *bstmts = bd >= 0 ? nt_arr(nt, bd, "body", &bn) : NULL;
      for (int k = 0; k < bn; k++)
        if (bstmts[k] >= 0 && bstmts[k] < node_count && nt_kind(nt, bstmts[k]) == NK_CallNode &&
            nt_ref(nt, bstmts[k], "receiver") < 0)
          body_cls[bstmts[k]] = bci;
    }
  }
  for (int id = 0; id < node_count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    int recv = nt_ref(nt, id, "receiver");
    int ci = -1;
    /* `self.class.m` in an instance method: the receiver is whichever class
       the instance has at run time -- the defining class or any descendant.
       An inherited class method reading class-level @ivars must bind to that
       class's storage, so each descendant gets its own copy (the dispatch
       switches on cls_id to reach it). FFI::Struct's `self.class.layout`
       from `#[]` is the shape: every Struct subclass holds its own layout. */
    if (recv >= 0 && nt_kind(nt, recv) == NK_CallNode) {
      const char *rn = nt_str(nt, recv, "name");
      int rr = nt_ref(nt, recv, "receiver");
      const char *mname = nt_str(nt, id, "name");
      Scope *encl = comp_scope_of(c, id);
      if (rn && sp_streq(rn, "class") && rr >= 0 && nt_kind(nt, rr) == NK_SelfNode &&
          mname && !sp_streq(mname, "new") && encl && !encl->is_cmethod && encl->class_id >= 0 &&
          nt_kind(nt, c->classes[encl->class_id].def_node) != NK_ModuleNode) {
        int base = encl->class_id;
        for (int k = 0; k < c->nclasses; k++) {
          if (k == base || !is_descendant(c, k, base)) continue;
          if (comp_cmethod_in_class(c, k, mname) >= 0) continue;
          int def_cls = -1;
          int mi = comp_cmethod_in_chain(c, k, mname, &def_cls);
          if (mi < 0 || def_cls == k) continue;
          if (mi >= snap) {
            int orig = -1;
            for (int o = 1; o < snap; o++)
              if (c->scopes[o].def_node == c->scopes[mi].def_node && c->scopes[o].is_cmethod) { orig = o; break; }
            if (orig < 0) continue;
            mi = orig; def_cls = c->scopes[orig].class_id;
            if (def_cls == k) continue;
          }
          int has_new = 0;
          if (cmethod_needs_specialization(c, mi, k, def_cls, &has_new))
            specialize_cmethod_for(c, mi, def_cls, k);
        }
      }
      continue;
    }
    if (recv < 0 ||
        (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode"))) {
      /* a bare (or explicit-self: this pass runs before the desugar that
         drops such receivers) call inside a class-method body: self is the
         class there, so an inherited factory's `new` must construct the
         CALLING class. Clone for the enclosing class exactly like the
         Const-receiver form (`def self.upsert; build { |kv| ... }; end`). */
      Scope *encl = comp_scope_of(c, id);
      if (encl && encl->is_cmethod && encl->class_id >= 0) ci = encl->class_id;
      else if (body_cls && body_cls[id] >= 0) ci = body_cls[id];
      else continue;
    }
    else {
      const char *rty = nt_type(nt, recv);
      if (!rty || (!sp_streq(rty, "ConstantReadNode") && !sp_streq(rty, "ConstantPathNode"))) continue;
      const char *cn = nt_str(nt, recv, "name");
      ci = cn ? comp_class_index(c, cn) : -1;
      if (ci < 0) continue;
    }
    const char *mname = cls_call_name(c, id);
    if (!mname || sp_streq(mname, "new")) continue;
    if (comp_cmethod_in_class(c, ci, mname) >= 0) continue;  /* defined on ci */
    int def_cls = -1;
    int mi = comp_cmethod_in_chain(c, ci, mname, &def_cls);
    if (mi < 0 || def_cls == ci) continue;                   /* not inherited */
    /* The nearest inherited copy may itself be a fresh specialization built
       this pass for an INTERMEDIATE subclass (index >= snap); specializing
       from it would re-attribute that intermediate's storage. Remap to the
       ORIGINAL (same def_node, index < snap) so ci binds its own civ_. */
    if (mi >= snap) {
      int orig = -1;
      for (int o = 1; o < snap; o++)
        if (c->scopes[o].def_node == c->scopes[mi].def_node && c->scopes[o].is_cmethod) { orig = o; break; }
      if (orig < 0) continue;
      mi = orig; def_cls = c->scopes[orig].class_id;
      if (def_cls == ci) continue;
    }
    int has_new = 0;
    if (!cmethod_needs_specialization(c, mi, ci, def_cls, &has_new)) continue;
    /* Clone mi for ci and, transitively, the inherited intermediates it reaches
       (#1451). nscopes growth below stands in for the old did_clone flag. */
    specialize_cmethod_for(c, mi, def_cls, ci);
  }
  /* A Class value the analysis cannot pin (`CONTAINERS.fetch(ext).open`)
     runs an inherited class method on whichever class it holds, so its bare
     `new` must construct that class. Each class escaping as a value gets its
     own copy once a call on a non-constant receiver names the method. The
     targets are chosen before any copy exists: a copy made for an
     intermediate class would otherwise hide the original from its
     subclasses. */
  for (int mi = 1; mi < snap; mi++) {
    if (!c->scopes[mi].is_cmethod || !c->scopes[mi].name || c->scopes[mi].class_id < 0) continue;
    int def_cls = c->scopes[mi].class_id;
    const char *mname = c->scopes[mi].name;
    int *tgt = NULL, ntgt = 0;
    for (int k = 0; k < c->nclasses; k++) {
      if (k == def_cls || !is_descendant(c, k, def_cls) || !class_value_escapes(c, k)) continue;
      /* the original, or a copy made above for an intermediate class */
      int kmi = comp_cmethod_in_chain(c, k, mname, NULL);
      if (kmi != mi && (kmi < snap || c->scopes[kmi].def_node != c->scopes[mi].def_node ||
                        c->scopes[kmi].class_id == k)) continue;
      int has_new = 0;
      if (!cmethod_needs_specialization(c, mi, k, def_cls, &has_new)) continue;
      int *nt2 = realloc(tgt, sizeof(int) * (size_t)(ntgt + 1));
      if (!nt2) break;
      tgt = nt2; tgt[ntgt++] = k;
    }
    if (ntgt > 0 && name_called_on_dynamic_recv(c, mname, node_count))
      for (int t = 0; t < ntgt; t++) specialize_cmethod_for(c, mi, def_cls, tgt[t]);
    free(tgt);
  }
  did_clone = (c->nscopes > snap);
  /* Index of every CallNode with a constant receiver, built once: the
     called-direct check below otherwise rescans all nodes per shadowed cmethod
     (O(cmethods * nodes)). */
  int *ccall = malloc((size_t)node_count * sizeof(int));
  int nccall = 0;
  if (ccall) {
    for (int id = 0; id < node_count; id++) {
      if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
      int r = nt_ref(nt, id, "receiver");
      const char *rty = r >= 0 ? nt_type(nt, r) : NULL;
      if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode")))
        ccall[nccall++] = id;
    }
  }
  /* DCE the now-shadowed source cls methods that are never called on their
     own defining class. */
  for (int s = 0; s < snap; s++) {
    Scope *src = &c->scopes[s];
    if (!src->is_cmethod || !src->name || src->class_id < 0) continue;
    /* did we specialize this one into a subclass? (a fresh cmethod copy with
       the same name was appended in a descendant class). Match on the class
       hierarchy, not just the name: an unrelated class's cmethod that merely
       shares the name must not be treated as a transplanted source (and DCE'd). */
    int specialized = 0;
    for (int d = snap; d < c->nscopes; d++)
      if (c->scopes[d].is_cmethod && c->scopes[d].name && c->scopes[d].class_id >= 0 &&
          sp_streq(c->scopes[d].name, src->name) &&
          is_descendant(c, c->scopes[d].class_id, src->class_id)) { specialized = 1; break; }
    if (!specialized) continue;
    /* keep it if some <Class>.<name> call still resolves HERE: the defining
       class itself, or a subclass that got no specialized copy of its own
       (`Sub2.describe` when only Sub needed one) -- that call is emitted
       against this source, so DCEing it left an undefined reference. */
    int called_direct = 0;
    for (int ii = 0; ii < nccall && !called_direct; ii++) {
      int id = ccall[ii];
      if (!cls_call_name(c, id) || !sp_streq(cls_call_name(c, id), src->name)) continue;
      int r = nt_ref(nt, id, "receiver");
      int rc = comp_class_index(c, nt_str(nt, r, "name"));
      if (rc < 0) continue;
      if (rc == src->class_id || comp_cmethod_in_chain(c, rc, src->name, NULL) == s) called_direct = 1;
    }
    /* ... or if a bare call inside some class-method body still resolves to
       this source (a class with no specialized copy of its own) */
    for (int id = 0; id < node_count && !called_direct; id++) {
      if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
      int r2 = nt_ref(nt, id, "receiver");
      if (r2 >= 0 && !(nt_type(nt, r2) && sp_streq(nt_type(nt, r2), "SelfNode"))) continue;
      const char *nm2 = cls_call_name(c, id);
      if (!nm2 || !sp_streq(nm2, src->name)) continue;
      Scope *encl = comp_scope_of(c, id);
      if (!encl || !encl->is_cmethod || encl->class_id < 0) continue;
      if (comp_cmethod_in_chain(c, encl->class_id, nm2, NULL) == s) called_direct = 1;
    }
    /* ... or if a receiver-less call in a class BODY resolves to this source:
       self there is the class, and a class that needed no specialized copy of
       its own emits that call against this one. */
    for (int id = 0; id < node_count && !called_direct; id++) {
      if (!body_cls || body_cls[id] < 0) continue;
      const char *nm3 = nt_str(nt, id, "name");
      if (!nm3 || !sp_streq(nm3, src->name)) continue;
      if (comp_cmethod_in_chain(c, body_cls[id], nm3, NULL) == s) called_direct = 1;
    }
    /* ... or if an `obj.class.<name>` call exists anywhere. That compiles to a
       switch over the receiver's cls_id whose DEFAULT arm calls this source, so
       the source is referenced even though no `Const.<name>` call names it, and
       DCEing it left the arm pointing at a symbol that was never emitted --
       an implicit declaration, mistyped arms, and a link error (#4053). */
    for (int id = 0; id < node_count && !called_direct; id++) {
      if (nt_kind(nt, id) != NK_CallNode) continue;
      const char *nm4 = nt_str(nt, id, "name");
      if (!nm4 || !sp_streq(nm4, src->name)) continue;
      int r4 = nt_ref(nt, id, "receiver");
      if (r4 < 0 || nt_kind(nt, r4) != NK_CallNode) continue;
      const char *rn4 = nt_str(nt, r4, "name");
      if (rn4 && sp_streq(rn4, "class")) called_direct = 1;
    }
    /* ... or if the defining class itself escapes as a value a dynamic
       receiver's call can reach */
    if (!called_direct && class_value_escapes(c, src->class_id) &&
        name_called_on_dynamic_recv(c, src->name, node_count)) called_direct = 1;
    if (!called_direct) src->is_transplanted_source = 1;
  }
  free(ccall);
  free(body_cls);
  /* The cloned bodies introduced new local/ivar nodes; intern them. */
  if (did_clone) register_locals(c);
}

/* For each class, find `prepend M` declarations and transplant M's instance
   methods into the class with shadow-chain renaming so `super` can route
   from M's body to the original (now renamed) class body. */
/* Process `prepend M` calls in a single class body. Split out of
   register_prepends so a REOPEN's body is scanned too: only the def_node's
   was, and a `prepend` in `class B ... end; class B; prepend Guard; end`
   compiled and did nothing -- while being the very form the explicit-receiver
   diagnostic recommends (#4200). */
static void process_prepend_body(Compiler *c, int ci, int body) {
  const NodeTable *nt = c->nt;
  {
    int n = 0;
    const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    for (int k = 0; k < n; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty || !sp_streq(sty, "CallNode")) continue;
      const char *nm = nt_str(nt, s, "name");
      if (!nm || !sp_streq(nm, "prepend")) continue;
      if (nt_ref(nt, s, "receiver") >= 0) continue;
      int anode = nt_ref(nt, s, "arguments");
      int an = 0;
      const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
      for (int j = 0; j < an; j++) {
        const char *aty = nt_type(nt, args[j]);
        const char *mname = NULL;
        if (aty && sp_streq(aty, "ConstantReadNode")) mname = nt_str(nt, args[j], "name");
        else if (aty && sp_streq(aty, "ConstantPathNode")) mname = nt_str(nt, args[j], "name");
        int mod_id = mname ? comp_class_index(c, mname) : -1;
        if (mod_id < 0) continue;
        /* Transplant each instance method of the module into class ci. */
        for (int ms = 0; ms < c->nscopes; ms++) {
          Scope *sc = &c->scopes[ms];
          if (sc->class_id != mod_id || sc->is_cmethod || !sc->name) continue;
          const char *method_name = sc->name;
          int active_mi = comp_method_in_class(c, ci, method_name);
          if (active_mi >= 0) {
            Scope *active = &c->scopes[active_mi];
            char shadow[256];
            snprintf(shadow, sizeof shadow, "__prep_%d_%s",
                     c->classes[ci].prep_shadow_count++, method_name);
            /* Rename any existing chain entry for method_name to use shadow. */
            ClassInfo *cif = &c->classes[ci];
            for (int kk = 0; kk < cif->nprep_chain; kk++) {
              if (sp_streq(cif->prep_from[kk], method_name)) {
                free(cif->prep_from[kk]);
                cif->prep_from[kk] = strdup(shadow);
                break;
              }
            }
            /* Rename the currently active scope to the shadow name. */
            free(active->name);
            active->name = strdup(shadow);
            /* Record the new dispatch chain entry: method_name -> shadow. */
            comp_prep_chain_add(&c->classes[ci], method_name, shadow);
          }
          /* CLONE the module method into class ci rather than MOVING it. The
             same module can be prepended by more than one class, and moving
             gave the first prepender the scope and left every later one with
             nothing to transplant -- its prepend did nothing, silently, and
             the call fell through to the included chain (#4039). The include
             path clones for this reason among others; a prepended module has
             the same need. */
          {
            int ms_i = ms;
            Scope *dst = comp_scope_new(c, method_name, sc->def_node);
            int dst_i = c->nscopes - 1;
            sc = &c->scopes[ms_i];               /* comp_scope_new may realloc */
            if (sc->body >= 0) {
              int nb = nt_clone_subtree((NodeTable *)nt, sc->body);
              comp_grow_node_arrays(c);
              sc = &c->scopes[ms_i]; dst = &c->scopes[dst_i];
              if (nb >= 0) { dst->body = nb; walk_scope(c, nb, dst_i, ci); }
              else dst->body = sc->body;
            }
            else dst->body = sc->body;
            sc = &c->scopes[ms_i]; dst = &c->scopes[dst_i];
            dst->class_id = ci;
            dst->is_cmethod = 0;
            dst->is_include_copy = 1;
            dst->origin_module_ci = mod_id + 1;   /* #owner names the module */
            dst->reachable = sc->reachable;
            dst->yields = sc->yields;
            dst->nrequired = sc->nrequired;
            dst->rest_idx = sc->rest_idx;
            dst->kwrest_idx = sc->kwrest_idx;
            dst->ret = sc->ret;
            if (sc->blk_param) dst->blk_param = strdup(sc->blk_param);
            /* register_locals has already run, so the parameters have to be
               copied across by hand and their locals re-interned -- exactly
               what the include clone does, and the half my first attempt at
               this omitted (the clone came out with no signature at all). */
            scope_copy_params(dst, sc);
            scope_own_defaults(c, dst_i);
            sc = &c->scopes[ms_i]; dst = &c->scopes[dst_i];
            /* the module body's ivars belong to the prepending class's layout */
            intern_scope_ivars(c, ms_i, ci);
            sc = &c->scopes[ms_i];
            sc->is_transplanted_source = 1;
          }
        }
      }
    }
  }
}

/* For each class, find `prepend M` in ALL class bodies, the reopenings
   included, the same two passes register_includes makes (#4200). */
void register_prepends(Compiler *c) {
  int *bci, *bnode;
  int nb = class_body_list(c, &bci, &bnode);
  for (int b = 0; b < nb; b++) process_prepend_body(c, bci[b], bnode[b]);
  free(bci);
  free(bnode);
}

/* Merge inherited ivar/reader/writer NAMES into subclasses so the struct
   layout is [parent ivars..., own ivars...] (cast-compatible). Types are
   propagated later in the fixpoint. Parent-first order. */
void inherit_members(Compiler *c) {
  /* Parent-before-child order, by dependency rather than by index: a class
     whose superclass is an anonymous Struct (`class Kid < Struct.new(:a)`)
     can be registered BEFORE that Struct, and the old index test skipped it,
     leaving the subclass without the members it inherits (#3576). */
  char *done = (char *)calloc((size_t)(c->nclasses > 0 ? c->nclasses : 1), 1);
  if (!done) return;
  for (int i = 0; i < c->nclasses; i++)
    if (c->classes[i].parent < 0) done[i] = 1;
  for (int round = 0; round < c->nclasses + 1; round++) {
    int progressed = 0;
    for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    int p = ci->parent;
    if (done[i]) continue;
    if (p < 0 || p >= c->nclasses || !done[p]) continue;  /* parent not merged yet */
    done[i] = 1; progressed = 1;
    ClassInfo *pc = &c->classes[p];
    /* a subclass of a Struct is a Struct: it keeps the members, the positional
       constructor and the member face (#3576); a subclass of a Data class is
       a Data class */
    if (pc->is_struct && !ci->is_struct && !ci->is_data) {
      ci->is_struct = 1;
      ci->is_data = pc->is_data;
    }

    char **old = ci->ivars; TyKind *oldt = ci->ivar_types; int oldn = ci->nivars;
    /* The per-slot side fields ride along with the names: this rebuild runs
       again after the fixpoint, and dropping them there undid what the
       passes had decided. narrow_object_arrays' pin (ivar_oa_type and
       ivar_int_table) was the one that showed: the ivar kept its narrowed
       type but lost the pin, so the final narrowing no longer saw it as a
       slot, the locals in its component fell back to the poly array, and
       the C did not build (#4642, a controller under a superclass). */
    unsigned char *old_ss = ci->ivar_str_shared, *old_it = ci->ivar_int_table,
                  *old_oc = ci->ivar_oa_conflict, *old_ni = ci->ivar_nullable_int,
                  *old_ne = ci->ivar_nullable_int_elem, *old_ae = ci->ivar_arr_elem_arr_or_nil;
    TyKind *old_oa = ci->ivar_oa_type; int *old_os = ci->ivar_oa_seed;
    ci->ivars = NULL; ci->ivar_types = NULL; ci->ivar_str_shared = NULL; ci->ivar_int_table = NULL;
    ci->ivar_oa_type = NULL; ci->ivar_oa_seed = NULL; ci->ivar_oa_conflict = NULL;
    ci->ivar_nullable_int = NULL; ci->ivar_nullable_int_elem = NULL; ci->ivar_arr_elem_arr_or_nil = NULL;
    ci->nivars = ci->civars = 0;
    #define IV_SIDE_COPY(dst, di, src, si) do { \
      (dst)->ivar_str_shared[di] = (src)->ivar_str_shared[si]; \
      (dst)->ivar_int_table[di] = (src)->ivar_int_table[si]; \
      (dst)->ivar_oa_type[di] = (src)->ivar_oa_type[si]; \
      (dst)->ivar_oa_seed[di] = (src)->ivar_oa_seed[si]; \
      (dst)->ivar_oa_conflict[di] = (src)->ivar_oa_conflict[si]; \
      (dst)->ivar_nullable_int[di] = (src)->ivar_nullable_int[si]; \
      (dst)->ivar_nullable_int_elem[di] = (src)->ivar_nullable_int_elem[si]; \
      (dst)->ivar_arr_elem_arr_or_nil[di] = (src)->ivar_arr_elem_arr_or_nil[si]; \
    } while (0)
    for (int k = 0; k < pc->nivars; k++) {
      int idx = comp_ivar_intern(ci, pc->ivars[k]);
      ci->ivar_types[idx] = pc->ivar_types[k];
      /* the layouts must stay cast-compatible, so a slot the parent narrowed
         is narrowed the same way here */
      IV_SIDE_COPY(ci, idx, pc, k);
    }
    for (int k = 0; k < oldn; k++) {
      int idx = comp_ivar_intern(ci, old[k]);
      /* an --rbs-pinned slot keeps its pinned type verbatim through the
         layout rebuild (this runs again AFTER seeds apply; unifying with the
         parent's inferred type here would overwrite the pin) -- UNLESS an
         ancestor carries the same ivar with a conflicting type. The struct
         layouts must stay cast-compatible (a parent method writes the slot
         through a (Parent*) cast of the child), so a pin that would split
         the layouts yields with a warning instead of corrupting every
         inherited read (#1871: a child's `attr_reader id: Integer` under a
         parent assign writing Hash[String, untyped] values). */
      if (class_ivar_pinned(ci, old[k])) {
        int pidx = comp_ivar_index(pc, old[k]);
        TyKind pt = pidx >= 0 ? pc->ivar_types[pidx] : TY_UNKNOWN;
        if (pidx >= 0 && pt != TY_UNKNOWN && pt != TY_NIL && pt != oldt[k]) {
          fprintf(stderr,
                  "spinel: warning: --rbs ivar pin %s dropped on %s: ancestor %s holds it as %s (layouts must stay cast-compatible)\n",
                  old[k], ci->name, pc->name, ty_name(pt));
          class_unpin_ivar(ci, old[k]);
          ci->ivar_types[idx] = ty_unify(ci->ivar_types[idx], oldt[k]);
        }
        else {
          ci->ivar_types[idx] = oldt[k];
        }
      }
      else
        ci->ivar_types[idx] = ty_unify(ci->ivar_types[idx], oldt[k]);
      /* an own slot keeps its side fields; one the parent also carries took
         the parent's above */
      if (comp_ivar_index(pc, old[k]) < 0 && old_oa) {
        ci->ivar_str_shared[idx] = old_ss[k]; ci->ivar_int_table[idx] = old_it[k];
        ci->ivar_oa_type[idx] = old_oa[k]; ci->ivar_oa_seed[idx] = old_os[k];
        ci->ivar_oa_conflict[idx] = old_oc[k]; ci->ivar_nullable_int[idx] = old_ni[k];
        ci->ivar_nullable_int_elem[idx] = old_ne[k]; ci->ivar_arr_elem_arr_or_nil[idx] = old_ae[k];
      }
      free(old[k]);
    }
    #undef IV_SIDE_COPY
    /* the parent's ivars lead the rebuilt layout, so its members lead them */
    if (pc->is_struct && ci->nmembers < pc->nmembers) ci->nmembers = pc->nmembers;
    free(old); free(oldt); free(old_ss); free(old_it); free(old_oa); free(old_os);
    free(old_oc); free(old_ni); free(old_ne); free(old_ae);

    /* An inherited attribute the child overrides with a `def` stops there: the
       def answers the name for the child and everything below it, so copying
       the flag on let a grandchild read the attribute past the def -- and a
       grandchild that re-declares the attribute looked like it had only
       inherited it, so a `super` below it skipped to the def. An attribute
       the parent undefines stops there too: copied on, it looked declared
       again in the child, and a boxed `sub.x` read it. A Struct's readers
       are its positional members, so they stay. */
    for (int k = 0; k < pc->nreaders; k++) {
      int undeffed = 0;
      for (int j = 0; j < pc->nundefs && !undeffed; j++) undeffed = sp_streq(pc->undefs[j], pc->readers[k]);
      if (comp_method_in_class(c, i, pc->readers[k]) < 0 && (!undeffed || ci->is_struct))
        comp_add_reader(ci, pc->readers[k]);
    }
    for (int k = 0; k < pc->nwriters; k++) {
      char wn[300];
      snprintf(wn, sizeof wn, "%s=", pc->writers[k]);
      if (comp_method_in_class(c, i, wn) < 0) comp_add_writer(ci, pc->writers[k]);
    }
    }
    if (!progressed) break;   /* the rest are cycles or dangling parents */
  }
  free(done);
}

/* Propagate inherited @ivar types parent -> child. */
int infer_inherited_ivars(Compiler *c) {
  int changed = 0;
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (ci->parent < 0) continue;
    ClassInfo *pc = &c->classes[ci->parent];
    for (int k = 0; k < pc->nivars; k++) {
      int idx = comp_ivar_index(ci, pc->ivars[k]);
      if (idx < 0) continue;
      if (class_ivar_pinned(ci, pc->ivars[k])) {
        /* authoritative -- unless the parent's slot settled to a conflicting
           type after the layout rebuild (same cast-compatibility rule as
           inherit_members; the write reaches this slot through a Parent*). */
        TyKind pt2 = pc->ivar_types[k];
        if (pt2 == TY_UNKNOWN || pt2 == TY_NIL || pt2 == ci->ivar_types[idx]) continue;
        fprintf(stderr,
                "spinel: warning: --rbs ivar pin %s dropped on %s: ancestor %s holds it as %s (layouts must stay cast-compatible)\n",
                pc->ivars[k], ci->name, pc->name, ty_name(pt2));
        class_unpin_ivar(ci, pc->ivars[k]);
      }
      TyKind merged = ty_unify(ci->ivar_types[idx], pc->ivar_types[k]);
      if (merged != ci->ivar_types[idx]) { ci->ivar_types[idx] = merged; changed = 1; }
    }
  }
  return changed;
}

/* `@@h = {}` / `@@a = []` followed by `@@h[k] = v` / `@@a.push(x)` elsewhere:
   an empty container literal carries no key/value or element type, so the class
   variable's inferred type stayed UNKNOWN and its file-scope slot was declared
   `sp_int` (the codegen fallback) while the write site emitted a real container
   -- the C compiler rejected the store. Globals (#3205, #3263) and constants
   (#2879) already derive a variant from usage; this is the same rule for class
   variables. Returns UNKNOWN when the RHS is not an empty container. */
/* Does `recv` answer the class variable `cvname`: a read of it, or a
   zero-argument call to a method whose value is that read (`K.h[k] = v`
   through `def self.h = @@h`)? */
static int recv_is_cvar(Compiler *c, int recv, const char *cvname) {
  const NodeTable *nt = c->nt;
  if (recv < 0) return 0;
  if (nt_kind(nt, recv) == NK_ClassVariableReadNode) {
    const char *rn = nt_str(nt, recv, "name");
    return rn && sp_streq(rn, cvname);
  }
  if (nt_kind(nt, recv) != NK_CallNode || nt_ref(nt, recv, "arguments") >= 0 ||
      nt_ref(nt, recv, "block") >= 0) return 0;
  const char *mname = nt_str(nt, recv, "name");
  if (!mname) return 0;
  for (int mi = 0; mi < c->nscopes; mi++) {
    Scope *s = &c->scopes[mi];
    if (!s->name || s->nparams > 0 || !sp_streq(s->name, mname)) continue;
    int last = scope_body_last(c, mi);
    if (last < 0 || nt_kind(nt, last) != NK_ClassVariableReadNode) continue;
    const char *rn = nt_str(nt, last, "name");
    if (rn && sp_streq(rn, cvname)) return 1;
  }
  return 0;
}

static TyKind cvar_hash_variant_from_writes(Compiler *c, const char *cvname, int dflt) {
  const NodeTable *nt = c->nt;
  TyKind kt = TY_UNKNOWN, vt = TY_UNKNOWN;
  int saw = 0;
  for (int w = 0; w < nt->count; w++) {
    NodeKind wk = nt_kind(nt, w);
    /* `h[k] ||= v` / `h[k] &&= v` / `h[k] op= v` store `v` at `k` too */
    int opw = wk == NK_IndexOrWriteNode || wk == NK_IndexAndWriteNode ||
              wk == NK_IndexOperatorWriteNode;
    if (!opw) {
      if (wk != NK_CallNode) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || (!sp_streq(wn, "[]=") && !sp_streq(wn, "store"))) continue;
    }
    if (!recv_is_cvar(c, nt_ref(nt, w, "receiver"), cvname)) continue;
    int wa = nt_ref(nt, w, "arguments");
    int wan = 0; const int *wav = wa >= 0 ? nt_arr(nt, wa, "arguments", &wan) : NULL;
    if (opw ? wan != 1 : wan < 2) continue;
    kt = ty_unify(kt, infer_type(c, wav[0]));
    vt = ty_unify(vt, infer_type(c, opw ? nt_ref(nt, w, "value") : wav[1]));
    saw = 1;
  }
  /* the default is a value the hash answers, as for a global's */
  if (dflt >= 0) { vt = ty_unify(vt, hash_default_value_ty(c, dflt)); saw = 1; }
  /* No resolved index-write: the slot still has to be declarable, so take the
     variant a bare `{}` emits rather than leaving it typeless. */
  if (!saw) return TY_STR_POLY_HASH;
  TyKind want = (kt == TY_SYMBOL) ? TY_SYM_POLY_HASH
              : (kt == TY_UNKNOWN) ? TY_POLY_POLY_HASH : ty_hash_of(kt, vt);
  if (!ty_is_hash(want)) want = (kt == TY_STRING) ? TY_STR_POLY_HASH : TY_POLY_POLY_HASH;
  return want;
}

/* The type an empty-container RHS gives a class variable, or `vt` unchanged. */
static TyKind cvar_empty_container_type(Compiler *c, int vnode, const char *nm, TyKind vt) {
  if (vnode < 0 || !nm) return vt;
  if (!ty_is_hash(vt) && node_is_empty_hash_producer(c, vnode))
    return cvar_hash_variant_from_writes(c, nm, hash_new_default_arg(c, vnode));
  if (vt == TY_UNKNOWN && nt_kind(c->nt, vnode) == NK_ArrayNode) {
    int en = 0; nt_arr(c->nt, vnode, "elements", &en);
    /* an empty `[]` holds whatever is pushed later, so a poly array (#3263) */
    if (en == 0) return TY_POLY_ARRAY;
  }
  return vt;
}

static int is_cvar_write_kind(NodeKind k) {
  return k == NK_ClassVariableWriteNode || k == NK_ClassVariableOrWriteNode ||
         k == NK_ClassVariableAndWriteNode || k == NK_ClassVariableOperatorWriteNode;
}

/* Registers the cvar a write-like node (`=`, `||=`, `&&=`, `op=`) stores into
   and unifies the stored type into its slot. An op-write stores the RHS type
   unless the slot holds an object (the operator method's return) or an array
   the operator combines with its own kind. */
static int cvar_note_write(Compiler *c, int cid, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm) return 0;
  ClassInfo *ci = &c->classes[comp_cvar_owner(c, cid, nm)];
  int old_n = ci->ncvars;
  int idx = comp_cvar_intern(ci, nm);
  int changed = ci->ncvars != old_n;
  int vnode = nt_ref(nt, id, "value");
  TyKind cur = ci->cvar_types[idx];
  TyKind vt;
  if (nt_kind(nt, id) == NK_ClassVariableOperatorWriteNode) {
    vt = infer_type(c, vnode);
    const char *op = nt_str(nt, id, "binary_operator");
    if (ty_is_object(cur)) {
      int mi = op ? comp_method_in_chain(c, ty_object_class(cur), op, NULL) : -1;
      vt = (mi >= 0 && c->scopes[mi].ret != TY_UNKNOWN) ? c->scopes[mi].ret : cur;
    }
    else if ((ty_is_array(cur) || cur == TY_POLY_ARRAY) && op &&
               ((sp_streq(op, "*") && vt == TY_INT) ||
                ((sp_streq(op, "|") || sp_streq(op, "&") || sp_streq(op, "-") ||
                  sp_streq(op, "+")) && vt == cur)))
      vt = cur;
    if (vt == TY_NIL || vt == TY_UNKNOWN) return changed;
  }
  else {
    vt = cvar_empty_container_type(c, vnode, nm, infer_type(c, vnode));
    { TyKind hv = fallback_hash_variant(c, vnode, NK_ClassVariableReadNode, nm, &changed);
      if (ty_is_hash(hv)) vt = hv; }
    if (vt == TY_NIL) vt = nil_write_type(cur);
    if (vt == TY_NIL) return changed;
  }
  TyKind merged = keep_general_array(cur, vt) ? cur : ty_unify(cur, vt);
  if (merged != cur) { ci->cvar_types[idx] = merged; changed = 1; }
  return changed;
}

/* Register each class variable (@@x) in its owning class and infer its type
   from the write sites' RHS. */
int infer_cvar_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  /* Pass 1: class body-level writes (comp_scope_of returns scope 0, class_id=-1,
     so use the class's def_node to find which class owns them). */
  for (int ci = 0; ci < c->nclasses; ci++) {
    int body = nt_ref(nt, c->classes[ci].def_node, "body");
    int n = 0;
    const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    for (int k = 0; k < n; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty) continue;
      if (is_cvar_write_kind(nt_kind(nt, s))) {
        if (cvar_note_write(c, ci, s)) changed = 1;
      }
      else if (sp_streq(sty, "MultiWriteNode")) {
        int mln = 0;
        const int *mlefts = nt_arr(nt, s, "lefts", &mln);
        int mval = nt_ref(nt, s, "value");
        int men = 0;
        const int *mels = masgn_tuple_rhs(nt, mval) ? nt_arr(nt, mval, "elements", &men) : NULL;
        for (int mi = 0; mi < mln; mi++) {
          const char *mlty = nt_type(nt, mlefts[mi]);
          if (!mlty || !sp_streq(mlty, "ClassVariableTargetNode")) continue;
          const char *cnm = nt_str(nt, mlefts[mi], "name");
          if (!cnm) continue;
          ClassInfo *mcl = &c->classes[comp_cvar_owner(c, ci, cnm)];
          int midx = comp_cvar_intern(mcl, cnm);
          TyKind mvt2 = (mels && mi < men) ? infer_type(c, mels[mi]) : TY_UNKNOWN;
          if (mvt2 == TY_NIL || mvt2 == TY_UNKNOWN) continue;
          TyKind mmerged = ty_unify(mcl->cvar_types[midx], mvt2);
          if (mmerged != mcl->cvar_types[midx]) { mcl->cvar_types[midx] = mmerged; changed = 1; }
        }
      }
    }
  }
  /* Pass 2: method-level writes (comp_scope_of has class_id set), and
     body-level writes in a REOPENED class/module body: those sit in scope 0
     like top-level code, but the scope pass recorded the enclosing body's
     class in node_cbody, and the cvar is that class's (not Toplevel's). */
  for (int id = 0; id < nt->count; id++) {
    if (!is_cvar_write_kind(nt_kind(nt, id))) continue;
    Scope *s = comp_scope_of(c, id);
    int wcid = s->class_id;
    if (wcid < 0 && c->node_cbody && id < c->node_cap) wcid = c->node_cbody[id];
    if (wcid < 0) continue;
    if (cvar_note_write(c, wcid, id)) changed = 1;
  }
  /* A multiple-assignment target (`@@a, *@@r = ...`), in a method or a class
     body and on either side of a splat, declares its cvar too; the elements'
     types reach the slot through infer_write_types. */
  NT_FOREACH_KIND(nt, NK_ClassVariableTargetNode, id) {
    Scope *s = comp_scope_of(c, id);
    int cc = s && s->class_id >= 0 ? s->class_id : id < c->node_cap ? c->node_cbody[id] : -1;
    const char *nm = nt_str(nt, id, "name");
    if (cc < 0 || !nm) continue;
    ClassInfo *tcl = &c->classes[comp_cvar_owner(c, cc, nm)];
    int old_n = tcl->ncvars;
    comp_cvar_intern(tcl, nm);
    if (tcl->ncvars != old_n) changed = 1;
  }
  /* Pass 2b: every class-body-level write -- one nested in begin/rescue or
     an if (Pass 1 sees only a body's direct statements), and one in a
     reopening of the class rather than the body that defined it. */
  for (int id = 0; id < nt->count; id++) {
    if (!is_cvar_write_kind(nt_kind(nt, id))) continue;
    Scope *s = comp_scope_of(c, id);
    if (s->class_id >= 0 || id >= c->node_cap || c->node_cbody[id] < 0) continue;
    if (cvar_note_write(c, c->node_cbody[id], id)) changed = 1;
  }
  /* Pass 2.5: `Klass.class_variable_set(:@@name, v)` with a literal name
     DECLARES the cvar when the class has no such write -- CRuby creates it on
     the fly, and the codegen store needs a registered global to hit (#2719). */
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "class_variable_set")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_ConstantReadNode) continue;
    int cci = comp_class_index(c, nt_str(nt, recv, "name"));
    if (cci < 0) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an != 2 || !av) continue;
    const char *aty = nt_type(nt, av[0]);
    const char *cvn = (aty && sp_streq(aty, "SymbolNode")) ? nt_str(nt, av[0], "value")
                    : (aty && sp_streq(aty, "StringNode")) ? nt_str(nt, av[0], "content") : NULL;
    if (!cvn || cvn[0] != '@' || cvn[1] != '@') continue;
    ClassInfo *scl = &c->classes[comp_cvar_owner(c, cci, cvn)];
    int idx = comp_cvar_intern(scl, cvn);
    TyKind vt = infer_type(c, av[1]);
    if (vt == TY_NIL || vt == TY_UNKNOWN) continue;
    TyKind merged = ty_unify(scl->cvar_types[idx], vt);
    if (merged != scl->cvar_types[idx]) { scl->cvar_types[idx] = merged; changed = 1; }
  }
  /* Pass 3: top-level writes (class_id == -1 in scope 0, and not inside any
     class body) -- use Toplevel pseudo-class. */
  for (int id = 0; id < nt->count; id++) {
    if (!is_cvar_write_kind(nt_kind(nt, id))) continue;
    Scope *s = comp_scope_of(c, id);
    if (!nt_str(nt, id, "name") || s->class_id >= 0) continue;
    if (c->node_cbody && id < c->node_cap && c->node_cbody[id] >= 0) continue;
    int tl_idx = comp_class_index(c, "Toplevel");
    if (tl_idx < 0) { comp_class_new(c, "Toplevel", -1); tl_idx = c->nclasses - 1; }
    if (cvar_note_write(c, tl_idx, id)) changed = 1;
  }
  /* Pass 4: a subclass can have interned a name before its superclass
     declared it, when a write in the subclass was reached first (the passes
     above go by kind of write, not by inheritance). That entry goes, with its
     type: the next round registers the same writes at the owner, through
     cvar_note_write's own merge, so the owner's type does not depend on which
     write came first. The other entries keep their order. */
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cl = &c->classes[ci];
    for (int j = 0; j < cl->ncvars; ) {
      if (comp_cvar_owner(c, ci, cl->cvars[j]) == ci) { j++; continue; }
      free(cl->cvars[j]);
      memmove(&cl->cvars[j], &cl->cvars[j + 1], sizeof(char *) * (size_t)(cl->ncvars - j - 1));
      memmove(&cl->cvar_types[j], &cl->cvar_types[j + 1], sizeof(TyKind) * (size_t)(cl->ncvars - j - 1));
      cl->ncvars--;
      changed = 1;
    }
  }
  return changed;
}

/* def_node -> scopes sharing it (transplanted module copies), cached per scope
   count. infer_ivar_types propagates each ivar write to copies of its method in
   other classes; without this it rescanned every scope per ivar write
   (O(ivar_writes * scopes)). Built once per fixpoint run (scope shape is fixed
   there); dn_head is indexed by node id, dn_next chains scopes. */
static int dn_nscopes = -1, dn_count = -1;
static int *dn_head = NULL, *dn_next = NULL;
static void dn_build(Compiler *c) {
  int nc = c->nt->count, ns = c->nscopes;
  free(dn_head); free(dn_next);
  dn_head = malloc((size_t)(nc > 0 ? nc : 1) * sizeof(int));
  dn_next = malloc((size_t)(ns > 0 ? ns : 1) * sizeof(int));
  dn_count = nc; dn_nscopes = ns;
  if (!dn_head || !dn_next) { dn_nscopes = -1; return; }
  for (int i = 0; i < nc; i++) dn_head[i] = -1;
  for (int s = 0; s < ns; s++) {
    int d = c->scopes[s].def_node;
    if (d >= 0 && d < nc) { dn_next[s] = dn_head[d]; dn_head[d] = s; }
    else dn_next[s] = -1;
  }
}

/* `@iv = cond ? nil : <int>` (a literal-nil ternary arm) pins the ivar as a
   nullable int -- the SP_INT_NIL sentinel in an unboxed int slot, the same
   representation a direct `@iv = nil` / `@iv = <int>` pair already yields
   (a bare `@iv = nil` is skipped below, leaving the int writes) -- rather than
   widening to poly. Scoped to the ivar write so the nullable value never
   escapes as a bare ternary expression, where a non-ivar consumer would not be
   sentinel-aware. Returns TY_INT for that shape, else TY_UNKNOWN. */
static TyKind ivar_nullable_int_ternary(Compiler *c, int vnode) {
  int tn, en;
  if (!comp_ternary_arms(c->nt, vnode, &tn, &en)) return TY_UNKNOWN;
  const char *tt = nt_type(c->nt, tn), *et = nt_type(c->nt, en);
  int t_nil = tt && sp_streq(tt, "NilNode");
  int e_nil = et && sp_streq(et, "NilNode");
  if (t_nil == e_nil) return TY_UNKNOWN;  /* exactly one arm a literal nil */
  return infer_type(c, t_nil ? en : tn) == TY_INT ? TY_INT : TY_UNKNOWN;
}

/* The ivars written nil somewhere, as (class, name) pairs. infer_ivar_types
   boxes them after its write sweep, not at the nil write: the re-narrow loop
   re-clears a poly ivar before every sweep, and a nil write met ahead of the
   typed write (`@v = nil` in initialize) would see the cleared slot and box
   nothing, settling the ivar back on the bare type. */
typedef struct { int n, cap; int *cls; const char **nm; } NilWrites;

static void nil_write_note(NilWrites *w, int cls, const char *nm) {
  if (cls < 0 || !nm) return;
  if (w->n == w->cap) {
    w->cap = w->cap ? w->cap * 2 : 16;
    w->cls = realloc(w->cls, sizeof(int) * w->cap);
    w->nm = realloc(w->nm, sizeof(const char *) * w->cap);
  }
  w->cls[w->n] = cls; w->nm[w->n] = nm; w->n++;
}

/* Box each noted slot whose type has no nil of its own, in its class and in
   every ancestor carrying the same ivar: the layouts stay cast-compatible, and
   an inherited method reads the slot a subclass wrote nil into (the child
   side follows through infer_inherited_ivars). */
static int nil_writes_apply(Compiler *c, NilWrites *w) {
  int changed = 0;
  for (int k = 0; k < w->n; k++) {
    for (int a = w->cls[k]; a >= 0 && a < c->nclasses; a = c->classes[a].parent) {
      ClassInfo *ci = &c->classes[a];
      int iv = comp_ivar_index(ci, w->nm[k]);
      if (iv < 0 || class_ivar_pinned(ci, w->nm[k]) || ci->ivar_int_table[iv]) continue;
      TyKind t = nil_write_type(ci->ivar_types[iv]);
      if (t == ci->ivar_types[iv]) continue;
      sp_ivwatch(w->nm[k], "ivar_nil_write", ci->ivar_types[iv], t);
      ci->ivar_types[iv] = t;
      changed = 1;
    }
  }
  free(w->cls); free(w->nm);
  return changed;
}

/* The ivar writes a fresh instance runs before any other method: those in an
   `initialize` and in the methods it calls on self, as (class, name) pairs. A
   write of a bare nil leaves the slot nil, so it does not count. */
static int ctor_scopes_write(Compiler *c, NilWrites *out) {
  const NodeTable *nt = c->nt;
  unsigned char *ctor = calloc((size_t)(c->nscopes ? c->nscopes : 1), 1);
  for (int si = 0; si < c->nscopes; si++) {
    Scope *s = &c->scopes[si];
    if (s->class_id >= 0 && !s->is_cmethod && s->name && sp_streq(s->name, "initialize")) ctor[si] = 1;
  }
  for (int grew = 1; grew; ) {
    grew = 0;
    NT_FOREACH_KIND(nt, NK_CallNode, id) {
      Scope *s = comp_scope_of(c, id);
      if (!s || !ctor[s - c->scopes]) continue;
      int recv = nt_ref(nt, id, "receiver");
      if (recv >= 0 && nt_kind(nt, recv) != NK_SelfNode) continue;
      const char *nm = nt_str(nt, id, "name");
      int mi = nm ? comp_method_in_chain(c, s->class_id, nm, NULL) : -1;
      if (mi < 0 || ctor[mi] || c->scopes[mi].is_cmethod) continue;
      ctor[mi] = 1; grew = 1;
    }
  }
  static const NodeKind kinds[] = {
    NK_InstanceVariableWriteNode, NK_InstanceVariableOperatorWriteNode,
    NK_InstanceVariableOrWriteNode, NK_InstanceVariableAndWriteNode,
    NK_InstanceVariableTargetNode,
  };
  for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
    NT_FOREACH_KIND(nt, kinds[k], id) {
      Scope *s = comp_scope_of(c, id);
      if (!s || !ctor[s - c->scopes]) continue;
      if (kinds[k] == NK_InstanceVariableWriteNode) {
        int v = nt_ref(nt, id, "value");
        if (v >= 0 && (nt_kind(nt, v) == NK_NilNode || comp_nil_chain_bottom(nt, v) >= 0)) continue;
      }
      nil_write_note(out, s->class_id, nt_str(nt, id, "name"));
    }
  }
  free(ctor);
  return out->n;
}

/* An ivar no constructor writes is nil on a fresh instance until some method
   assigns it: an implicit nil write, which boxes a slot with no nil of its own
   the way an explicit one does. Without it a Symbol, bool or Class slot read
   before its first write answered its C zero -- `@v.nil?` false, `p @v`
   false. Only classes something constructs are asked, so an abstract base
   whose subclasses' constructors assign the slot keeps it unboxed. */
static void implicit_nil_writes_note(Compiler *c, NilWrites *w) {
  NilWrites seeded = {0};
  int built = 0;
  for (int k = 0; k < c->nclasses; k++) {
    ClassInfo *ci = &c->classes[k];
    if (!ci->ctor_reachable || ci->is_struct) continue;
    for (int iv = 0; iv < ci->nivars; iv++) {
      if (nil_write_type(ci->ivar_types[iv]) == ci->ivar_types[iv]) continue;
      if (!built) { ctor_scopes_write(c, &seeded); built = 1; }
      int found = 0;
      for (int a = k; a >= 0 && a < c->nclasses && !found; a = c->classes[a].parent)
        for (int j = 0; j < seeded.n && !found; j++)
          found = seeded.cls[j] == a && sp_streq(seeded.nm[j], ci->ivars[iv]);
      if (!found) nil_write_note(w, k, ci->ivars[iv]);
    }
  }
  free(seeded.cls); free(seeded.nm);
}

int class_has_subclass(Compiler *c, int ocid);

static int sg_writer_class(Compiler *c, int id, int recv) {
  const NodeTable *nt = c->nt;
  NodeKind rk = nt_kind(nt, recv);
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) {
    const char *cn = nt_str(nt, recv, "name");
    return cn ? comp_class_index(c, cn) : -1;
  }
  if (rk == NK_SelfNode) {
    Scope *s = comp_scope_of(c, id);
    if (s->is_cmethod) return s->class_id;
    return s->class_id < 0 ? c->node_cbody[id] : -1;
  }
  const char *rn = rk == NK_CallNode ? nt_str(nt, recv, "name") : NULL;
  if (rn && sp_streq(rn, "class") && nt_ref(nt, recv, "arguments") < 0) {
    int robj = nt_ref(nt, recv, "receiver");
    TyKind ot = robj >= 0 ? infer_type(c, robj) : TY_UNKNOWN;
    if (ty_is_object(ot) && !class_has_subclass(c, ty_object_class(ot))) return ty_object_class(ot);
  }
  return -1;
}

/* Unify an ivar slot's type with a written value's, keeping a slot that only
   ever holds an Array an ARRAY.

   ty_unify answers the plain poly SCALAR for two array kinds. That boxes the
   slot and sends every push through sp_poly_shl, where a foreign element was
   silently coerced to the typed array's own kind (`[0, 0]` for a pushed
   "one", #4196). When either side is already the boxed ARRAY the answer is
   the boxed ARRAY: both sides are Arrays, so nothing is lost by saying so.

   Two TYPED kinds still box. Their readers were typed from the writes, and
   the box is what keeps them consistent.

   Either operand may be the boxed one. The slot is the typed array and the
   value the boxed one whenever the #5499 re-narrow has reset the slot: its
   pushes re-fold to `int_array` before the pushed value re-settles, and the
   next write of a boxed array then met it (#5521). */
static TyKind ivar_merge_with_write(TyKind slot, TyKind vt) {
  TyKind m = ty_unify(slot, vt);
  if (m == TY_POLY && ty_is_array(slot) && ty_is_array(vt) &&
      (slot == TY_POLY_ARRAY || vt == TY_POLY_ARRAY))
    return TY_POLY_ARRAY;
  return m;
}

int infer_ivar_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  NilWrites nilw = {0};
  if (dn_nscopes != c->nscopes || dn_count != nt->count) dn_build(c);
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    /* A top-level method that only READS an ivar (never assigned anywhere)
       gets no slot from the write pass and none from register_locals (which
       interns reads only for a real class scope), so the read can't be
       lowered. An unassigned ivar is always nil in Ruby: register a slot for
       it in the Toplevel pseudo-class (or the enclosing class body), leaving
       its type unpinned so it stays a nil-valued poly. */
    /* a multiple assignment's target too: `@a, @b = pair` at the top level
       wrote a slot nothing had registered */
    if (sp_streq(ty, "InstanceVariableReadNode") || sp_streq(ty, "InstanceVariableTargetNode")) {
      Scope *s = comp_scope_of(c, id);
      if (s->class_id >= 0) continue;  /* class/instance reads: register_locals */
      const char *nm = nt_str(nt, id, "name");
      if (!nm) continue;
      int cid = c->node_cbody[id];
      if (cid < 0) {
        int old_nc = c->nclasses;
        cid = comp_class_index(c, "Toplevel");
        if (cid < 0) { comp_class_new(c, "Toplevel", -1); cid = c->nclasses - 1; }
        if (c->nclasses != old_nc) changed = 1;
      }
      ClassInfo *ci = &c->classes[cid];
      int old_ni = ci->nivars;
      comp_ivar_intern(ci, nm);
      if (ci->nivars != old_ni) changed = 1;
      continue;
    }
    if (sp_streq(ty, "InstanceVariableWriteNode") ||
        sp_streq(ty, "InstanceVariableOrWriteNode") ||
        sp_streq(ty, "InstanceVariableAndWriteNode") ||
        sp_streq(ty, "InstanceVariableOperatorWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      int vnode = nt_ref(nt, id, "value");
      TyKind vt = infer_type(c, vnode);
      /* A nil write doesn't pin the ivar type, but it can box it (see
         nil_write_type). `@a = @b = nil`: the chain writes nil to every
         target; don't let the inner slot's unified type (from its other
         writes) pin this ivar. */
      int nil_write = !sp_streq(ty, "InstanceVariableOperatorWriteNode") &&
                      (vt == TY_NIL || comp_nil_chain_bottom(nt, vnode) >= 0);
      if (vt == TY_POLY && ivar_nullable_int_ternary(c, vnode) == TY_INT) vt = TY_INT;
      Scope *s = comp_scope_of(c, id);
      int cls_id2 = s->class_id;
      if (!nm) continue;
      /* A `@ivar = v` directly in a class/module body (not in a method) belongs
         to that class/module object, like its class methods see it -- attribute
         it to the enclosing class-body rather than the Toplevel pseudo-class. */
      if (cls_id2 < 0 && c->node_cbody[id] >= 0) cls_id2 = c->node_cbody[id];
      if (nil_write) {
        nil_write_note(&nilw, cls_id2 >= 0 ? cls_id2 : comp_class_index(c, "Toplevel"), nm);
        /* the transplanted copies of this method (see below) */
        if (s->class_id >= 0 && s->def_node >= 0) {
          int use_idx = dn_head && dn_nscopes == c->nscopes && s->def_node < dn_count;
          int si = use_idx ? dn_head[s->def_node] : 0;
          for (; use_idx ? (si >= 0) : (si < c->nscopes); si = use_idx ? dn_next[si] : si + 1) {
            Scope *ts = &c->scopes[si];
            if (ts->def_node != s->def_node || ts->class_id == s->class_id || ts->class_id < 0) continue;
            nil_write_note(&nilw, ts->class_id, nm);
          }
        }
        continue;
      }
      if (vt == TY_NIL) continue;
      /* Inside an instance_eval/exec body the ivar is the receiver's, whatever
         the enclosing scope is -- a class method's own `@x` would otherwise
         take the write (Phlex's `object.instance_exec { @content = block }`
         in `self.new`). A poly receiver writes each class it can be. */
      /* (A one-class receiver only from a method: at top level the codegen
         keeps reading the Toplevel slot's type for such a write, which it
         has always had.) */
      { int iec = ie_class_of(c, id);
        if (iec >= 0 && s->class_id >= 0) cls_id2 = iec;
        else if (iec < -1 && !sp_streq(ty, "InstanceVariableOperatorWriteNode")) {
          int pk[64], npk = ie_poly_classes_at(c, id, pk, 64);
          for (int q = 0; q < npk; q++) {
            ClassInfo *pci = &c->classes[pk[q]];
            int old_pn = pci->nivars;
            int piv = comp_ivar_intern(pci, nm);
            if (pci->nivars != old_pn) changed = 1;
            if (class_ivar_pinned(pci, nm) || pci->ivar_int_table[piv]) continue;
            TyKind pvt = empty_container_write(c, vnode, vt, pci->ivar_types[piv]);
            TyKind pm = ty_unify(pci->ivar_types[piv], pvt);
            if (pm != pci->ivar_types[piv]) { pci->ivar_types[piv] = pm; changed = 1; }
          }
          if (npk > 0) continue;
        } }
      if (cls_id2 < 0) {
        /* Top-level method: track ivars in the Toplevel pseudo-class */
        int old_nc = c->nclasses;
        cls_id2 = comp_class_index(c, "Toplevel");
        if (cls_id2 < 0) { comp_class_new(c, "Toplevel", -1); cls_id2 = c->nclasses - 1; }
        if (c->nclasses != old_nc) changed = 1;  /* new class created, need another pass */
      }
      ClassInfo *ci = &c->classes[cls_id2];
      int old_ni = ci->nivars;
      int iv = comp_ivar_intern(ci, nm);
      if (ci->nivars != old_ni) changed = 1;  /* new ivar registered, need another pass */
      /* `@x |= v`, `&=`, `^=` on a slot nothing has typed yet: the slot is nil
         there, and NilClass#| answers true, #& false, #^ v's truthiness -- not
         an Integer. Taking v's type made it an int slot, and `nil | 256` ORed
         the nil sentinel's bits into a large negative number (#5470). */
      if (sp_streq(ty, "InstanceVariableOperatorWriteNode") && ci->ivar_types[iv] == TY_UNKNOWN &&
          !class_ivar_pinned(ci, nm)) {
        const char *bo = nt_str(nt, id, "binary_operator");
        if (bo && (sp_streq(bo, "|") || sp_streq(bo, "&") || sp_streq(bo, "^"))) vt = TY_POLY;
      }
      /* For operator-write (@b += rhs), vt is the RHS type, not the result type.
         When the slot holds a user object, the result is the method's return type. */
      if (sp_streq(ty, "InstanceVariableOperatorWriteNode") && ty_is_object(ci->ivar_types[iv])) {
        const char *op2 = nt_str(nt, id, "binary_operator");
        int cid2 = ty_object_class(ci->ivar_types[iv]);
        int mi2 = op2 ? comp_method_in_chain(c, cid2, op2, NULL) : -1;
        if (mi2 >= 0 && c->scopes[mi2].ret != TY_UNKNOWN)
          vt = c->scopes[mi2].ret;
        else
          vt = ci->ivar_types[iv];  /* keep existing type, don't widen */
      }
      /* An Array slot's `*= n` repeats it, and `|=` `&=` `-=` `+=` with an
         array of its own kind combine two of them: the result is the slot's
         own array type. Unifying the RHS instead made `@a *= 2` an Integer
         write, boxed the slot, and every `|=` on it went to the integer
         bit operator (#4833). */
      else if (sp_streq(ty, "InstanceVariableOperatorWriteNode") &&
               (ty_is_array(ci->ivar_types[iv]) || ci->ivar_types[iv] == TY_POLY_ARRAY)) {
        const char *op2 = nt_str(nt, id, "binary_operator");
        if (op2 && ((sp_streq(op2, "*") && vt == TY_INT) ||
                    ((sp_streq(op2, "|") || sp_streq(op2, "&") || sp_streq(op2, "-") ||
                      sp_streq(op2, "+")) && vt == ci->ivar_types[iv])))
          vt = ci->ivar_types[iv];
      }
      if (!sp_streq(ty, "InstanceVariableOperatorWriteNode"))
        vt = empty_container_write(c, vnode, vt, ci->ivar_types[iv]);
      /* A Time slot's `+= n` / `-= n` with a numeric operand answers a Time
         (Time + Integer / Float); unifying the RHS boxed the slot to poly
         and `@t.hour` after it had no arm (the logger gem's Period). */
      else if (sp_streq(ty, "InstanceVariableOperatorWriteNode") && ci->ivar_types[iv] == TY_TIME) {
        const char *op2 = nt_str(nt, id, "binary_operator");
        if (op2 && (sp_streq(op2, "+") || sp_streq(op2, "-")) &&
            (vt == TY_INT || vt == TY_FLOAT || vt == TY_BIGINT))
          vt = TY_TIME;
      }
      /* A narrowed int table is pinned: its own write reads TY_POLY_ARRAY,
         and the two array kinds unify to the plain poly SCALAR -- re-deriving
         it here would replace the narrowed type with something strictly
         worse, and a parameter bound from `@t[k][j]` would take poly for
         good (parameters only widen). */
      if (!class_ivar_pinned(ci, nm) && !ci->ivar_int_table[iv]) {
        TyKind merged = ivar_merge_with_write(ci->ivar_types[iv], vt);
        sp_ivwatch(nm, "ivar_write_merge", ci->ivar_types[iv], merged);
        if (merged != ci->ivar_types[iv]) { ci->ivar_types[iv] = merged; changed = 1; }
      }
      /* Propagate to transplanted copies (module included into a class).
         Body nodes still point to the module scope, so cls_id2 is the module.
         Any scope sharing the same def_node but with a different class_id is
         a transplanted copy that must see the same ivar type. */
      if (s->class_id >= 0 && s->def_node >= 0) {
        int sdef = s->def_node;
        int orig_cid = s->class_id;
        int use_idx = dn_head && dn_nscopes == c->nscopes && sdef < dn_count;
        int si = use_idx ? dn_head[sdef] : 0;
        for (; use_idx ? (si >= 0) : (si < c->nscopes); si = use_idx ? dn_next[si] : si + 1) {
          Scope *ts = &c->scopes[si];
          if (ts->def_node != sdef || ts->class_id == orig_cid || ts->class_id < 0) continue;
          ClassInfo *tc = &c->classes[ts->class_id];
          if (class_ivar_pinned(tc, nm)) continue;
          int tiv = comp_ivar_intern(tc, nm);
          TyKind tmerged = ivar_merge_with_write(tc->ivar_types[tiv], vt);
          sp_ivwatch(nm, "transplant_merge", tc->ivar_types[tiv], tmerged);
          if (tmerged != tc->ivar_types[tiv]) { tc->ivar_types[tiv] = tmerged; changed = 1; }
        }
      }
    }
    else if (sp_streq(ty, "CallNode")) {
      /* instance_variable_set(:@lit, v): CRuby creates the ivar on the spot,
         so register a slot for a brand-new literal name in the receiver's
         class layout (like an `@lit = v` write would), pinning the value's
         type. Without this the write has no field to lower to (#3059). */
      {
        const char *ivsn = nt_str(nt, id, "name");
        if (ivsn && sp_streq(ivsn, "instance_variable_set")) {
          int sargs = nt_ref(nt, id, "arguments"); int san = 0;
          const int *sav = sargs >= 0 ? nt_arr(nt, sargs, "arguments", &san) : NULL;
          const char *a0ty = (san == 2 && sav) ? nt_type(nt, sav[0]) : NULL;
          const char *sym = NULL;
          if (a0ty && sp_streq(a0ty, "SymbolNode")) sym = nt_str(nt, sav[0], "value");
          else if (a0ty && sp_streq(a0ty, "StringNode")) sym = nt_str(nt, sav[0], "content");
          if (sym && sym[0] == '@') {
            int ivrecv = nt_ref(nt, id, "receiver");
            const char *ivrt = ivrecv >= 0 ? nt_type(nt, ivrecv) : NULL;
            int tcid = -1;
            if (ivrecv < 0 || (ivrt && sp_streq(ivrt, "SelfNode"))) {
              Scope *s = comp_scope_of(c, id);
              tcid = s->class_id;
              if (tcid < 0 && c->node_cbody[id] >= 0) tcid = c->node_cbody[id];
            }
            else {
              TyKind rt = comp_ntype(c, ivrecv);
              if (ty_is_object(rt)) tcid = ty_object_class(rt);
              /* a poly receiver may be any class that has the slot: the
                 value's type reaches each of them (the dispatch writes it) */
              else if (rt == TY_POLY) {
                TyKind pvt = infer_type(c, sav[1]);
                for (int k = 0; k < c->nclasses; k++) {
                  ClassInfo *pk = &c->classes[k];
                  int piv = pk->is_struct ? -1 : comp_ivar_index(pk, sym);
                  if (piv < 0) continue;
                  if (pvt == TY_NIL) nil_write_note(&nilw, k, sym);
                  else if (!class_ivar_pinned(pk, sym)) {
                    TyKind pm = ivar_merge_with_write(pk->ivar_types[piv], pvt);
                    if (pm != pk->ivar_types[piv]) { pk->ivar_types[piv] = pm; changed = 1; }
                  }
                }
              }
            }
            if (tcid >= 0 && tcid < c->nclasses) {
              ClassInfo *ci = &c->classes[tcid];
              int old_ni = ci->nivars;
              int iv = comp_ivar_intern(ci, sym);
              if (ci->nivars != old_ni) changed = 1;
              TyKind vt = infer_type(c, sav[1]);
              if (vt == TY_NIL) nil_write_note(&nilw, tcid, sym);
              else if (!class_ivar_pinned(ci, sym)) {
                TyKind merged = ivar_merge_with_write(ci->ivar_types[iv], vt);
                if (merged != ci->ivar_types[iv]) { ci->ivar_types[iv] = merged; changed = 1; }
              }
            }
          }
          continue;
        }
      }
      /* attr-writer assignment: obj.x = v  (CallNode "x=") */
      const char *nm = nt_str(nt, id, "name");
      int recv = nt_ref(nt, id, "receiver");
      size_t ln = nm ? strlen(nm) : 0;
      if (!nm || recv < 0 || ln < 2 || nm[ln - 1] != '=') continue;
      /* not a comparison that happens to end in '=' (==, !=, <=, >=) */
      if (nm[ln - 2] == '=' || nm[ln - 2] == '!' || nm[ln - 2] == '<' || nm[ln - 2] == '>') continue;
      char base[256];
      if (ln - 1 >= sizeof base) continue;
      memcpy(base, nm, ln - 1); base[ln - 1] = '\0';
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 1) continue;
      TyKind vt = infer_type(c, argv[0]);
      char ivname[256];
      snprintf(ivname, sizeof ivname, "@%s", base);
      int sgc = sg_writer_class(c, id, recv);
      if (sgc >= 0 && comp_is_sg_writer(&c->classes[sgc], base)) {
        ClassInfo *ci = &c->classes[sgc];
        int iv = comp_is_sg_civ(ci, base) ? comp_ivar_index(ci, ivname) : -1;
        if (iv < 0 || class_ivar_pinned(ci, ivname)) continue;
        if (vt == TY_NIL) { nil_write_note(&nilw, sgc, ci->ivars[iv]); continue; }
        TyKind merged = ty_unify(ci->ivar_types[iv], empty_container_write(c, argv[0], vt, ci->ivar_types[iv]));
        if (merged != ci->ivar_types[iv]) { ci->ivar_types[iv] = merged; changed = 1; }
        continue;
      }
      TyKind rt = infer_type(c, recv);
      if (vt == TY_NIL) {
        /* a nil write doesn't pin the ivar type, but it can box it: the
           receiver's class, or every class this writer could reach */
        for (int ci2 = 0; ci2 < c->nclasses; ci2++) {
          if (ty_is_object(rt) && ci2 != ty_object_class(rt)) continue;
          int iv = comp_is_writer(&c->classes[ci2], base) ? comp_ivar_index(&c->classes[ci2], ivname) : -1;
          if (iv >= 0) nil_write_note(&nilw, ci2, c->classes[ci2].ivars[iv]);
        }
        continue;
      }
      if (ty_is_object(rt)) {
        /* concrete receiver: attribute to its class. */
        ClassInfo *ci = &c->classes[ty_object_class(rt)];
        if (!comp_is_writer(ci, base)) continue;
        int iv = comp_ivar_index(ci, ivname);
        if (iv < 0 || class_ivar_pinned(ci, ivname)) continue;
        TyKind merged = ty_unify(ci->ivar_types[iv], empty_container_write(c, argv[0], vt, ci->ivar_types[iv]));
        if (merged != ci->ivar_types[iv]) { ci->ivar_types[iv] = merged; changed = 1; }
      }
      else {
        /* Poly/unknown receiver -- e.g. `cell` read from a poly array/hash
           (`@cells.each_value { |cell| cell.neighbours = ... }`). The static
           class is unknown, but if exactly ONE class defines this attr-writer
           with a matching ivar, the runtime object must be of that class, so
           attribute the write to it. (Skip when ambiguous: zero or several
           classes share the attr name -- over-widening an unrelated same-named
           ivar would be unsound to attribute.) ty_unify only widens. */
        int only = -1;
        for (int ci2 = 0; ci2 < c->nclasses; ci2++) {
          if (comp_is_writer(&c->classes[ci2], base) &&
              comp_ivar_index(&c->classes[ci2], ivname) >= 0) {
            if (only >= 0) { only = -2; break; }   /* ambiguous */
            only = ci2;
          }
        }
        if (only < 0) continue;
        ClassInfo *ci = &c->classes[only];
        int iv = comp_ivar_index(ci, ivname);
        if (iv < 0 || class_ivar_pinned(ci, ivname)) continue;
        TyKind merged = ty_unify(ci->ivar_types[iv], empty_container_write(c, argv[0], vt, ci->ivar_types[iv]));
        if (merged != ci->ivar_types[iv]) { ci->ivar_types[iv] = merged; changed = 1; }
      }
    }
  }
  implicit_nil_writes_note(c, &nilw);
  changed |= nil_writes_apply(c, &nilw);
  return changed;
}

/* ---- fixpoint passes ---- */

