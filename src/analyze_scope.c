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
        lv->type = TY_POLY_ARRAY;
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
        int en = 0; const int *els = nt_arr(nt, av[an - 1], "elements", &en);
        for (int e = 0; e < en; e++) {
          int key = nt_ref(nt, els[e], "key");
          const char *kty = key >= 0 ? nt_type(nt, key) : NULL;
          const char *kn = (kty && sp_streq(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
          if (!kn) continue;
          int dup = 0;
          for (int p = 0; p < s->nparams; p++) if (sp_streq(s->pnames[p], kn)) { dup = 1; break; }
          if (!dup) scope_add_param(s, kn, -1);
        }
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

/* The method `s`'s body forwards `...` to (a `callee(...)` call). Returns the
   callee scope index, or -1 if none/unresolved. */
static int forwarding_target_idx(Compiler *c, Scope *s) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode") || comp_scope_of(c, id) != s) continue;
    int a = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (an != 1 || !nt_type(nt, av[0]) ||
        !sp_streq(nt_type(nt, av[0]), "ForwardingArgumentsNode")) continue;
    const char *cn = nt_str(nt, id, "name");
    if (!cn) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv >= 0) continue;  /* receiver-qualified target: not resolved here */
    int mi = comp_method_index(c, cn);
    if (mi < 0 && s->class_id >= 0) mi = comp_method_in_chain(c, s->class_id, cn, NULL);
    if (mi < 0 && s->class_id >= 0) mi = comp_cmethod_in_chain(c, s->class_id, cn, NULL);
    if (mi >= 0) return mi;
  }
  return -1;
}

/* Chained `...`: a forwarding method called only via another `f(...)` forward
   has no concrete call site, so its call-site arity is 0. Top its synthesized
   positional params up to its forwarding target's arity, to a fixpoint, so
   `def h(...); f(...); end; def f(...); g(a,b); end` propagates g's arity back
   through f and h (#1288). */
void topup_forwarding_arity(Compiler *c) {
  int changed = 1;
  for (int iter = 0; iter < 32 && changed; iter++) {
    changed = 0;
    for (int s = 1; s < c->nscopes; s++) {
      Scope *sc = &c->scopes[s];
      if (!scope_is_forwarding(c, sc)) continue;
      int tgt = forwarding_target_idx(c, sc);
      if (tgt < 0 || tgt == s) continue;
      int want = c->scopes[tgt].nparams;
      while (sc->nparams < want) {
        char nm[24]; snprintf(nm, sizeof nm, "__fwd_%d", sc->nparams);
        scope_add_param(sc, nm, -1);
        changed = 1;
      }
    }
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

/* If `id` is a receiverless `define_method(:lit) { }` that walk_scope will
   register as a method scope, return that literal method name; else NULL.
   walk_scope only registers when the name is a literal symbol/string AND a block
   is present, so this mirrors that exact gate. Keeping class_eval_reopen_class's
   purity test in lockstep with it prevents a `define_method` that the registrar
   silently skips (blockless, or a dynamic name) from making the block look like a
   pure reopen -- which would no-op the whole call and drop it without a diagnostic. */
static const char *dm_registerable_name(const NodeTable *nt, int id) {
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode")) return NULL;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "define_method") || nt_ref(nt, id, "receiver") >= 0) return NULL;
  if (nt_ref(nt, id, "block") < 0) return NULL;
  int args = nt_ref(nt, id, "arguments");
  int na = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &na) : NULL;
  if (na < 1) return NULL;
  const char *aty = nt_type(nt, argv[0]);
  if (aty && sp_streq(aty, "SymbolNode")) return nt_str(nt, argv[0], "value");
  if (aty && sp_streq(aty, "StringNode")) return nt_str(nt, argv[0], "content");
  return NULL;
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

void walk_scope(Compiler *c, int id, int scope_idx, int class_id) {
  if (id < 0 || id >= c->nt->count) return;
  c->nscope[id] = scope_idx;
  c->node_cbody[id] = g_cbody_class_id;
  const char *ty = nt_type(c->nt, id);
  int child = scope_idx;
  int child_class = class_id;

  /* `class << self; def X; ...; end; end` — treat body defs as class methods. */
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
          if (sp_streq(sty, "DefNode")) {
            const char *name = nt_str(c->nt, s, "name");
            if (!name) continue;
            Scope *sc = comp_scope_new(c, name, s);
            int new_idx = c->nscopes - 1;
            sc->body = nt_ref(c->nt, s, "body");
            sc->class_id = target_class;
            sc->is_cmethod = 1;
            collect_def_params(c, s, sc);
            /* Assign scope to the def node and its body */
            c->nscope[s] = new_idx;
            if (sc->body >= 0) walk_scope(c, sc->body, new_idx, target_class);
          }
          else {
            walk_scope(c, s, scope_idx, target_class);
          }
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
    int dm_cmethod = 0, dm_cls = class_id, dm_ok = dm_is_dm, dm_defer = 0;
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

/* Mark methods following `module_function` in a module body as class-level
   (is_cmethod=1, no self param). This lets them be called as bare functions
   when their module is included at the top level. */
void register_module_functions(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int ci = 0; ci < c->nclasses; ci++) {
    int dn = c->classes[ci].def_node;
    const char *dt = dn >= 0 ? nt_type(nt, dn) : NULL;
    if (!dt || !sp_streq(dt, "ModuleNode")) continue;
    int body = nt_ref(nt, dn, "body");
    if (body < 0) continue;
    int bn = 0;
    const int *stmts = nt_arr(nt, body, "body", &bn);
    int in_module_function = 0;
    for (int k = 0; k < bn; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty) continue;
      if (sp_streq(sty, "CallNode") && nt_ref(nt, s, "receiver") < 0) {
        const char *nm = nt_str(nt, s, "name");
        if (nm && sp_streq(nm, "module_function")) {
          /* `module_function :m1, :m2` form: mark named methods */
          int an = 0;
          int anode = nt_ref(nt, s, "arguments");
          const int *aargs = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
          if (an == 0) { in_module_function = 1; continue; }
          for (int ai = 0; ai < an; ai++) {
            const char *aty = nt_type(nt, aargs[ai]);
            const char *aval = NULL;
            if (aty && sp_streq(aty, "SymbolNode")) aval = nt_str(nt, aargs[ai], "value");
            if (!aval) continue;
            for (int mi = 0; mi < c->nscopes; mi++) {
              if (c->scopes[mi].class_id == ci && !c->scopes[mi].is_cmethod &&
                  c->scopes[mi].name && sp_streq(c->scopes[mi].name, aval))
                c->scopes[mi].is_cmethod = 1;
            }
          }
          continue;
        }
      }
      if (sp_streq(sty, "DefNode") && in_module_function) {
        const char *mname = nt_str(nt, s, "name");
        if (!mname) continue;
        for (int mi = 0; mi < c->nscopes; mi++) {
          if (c->scopes[mi].def_node == s) { c->scopes[mi].is_cmethod = 1; break; }
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
static void vis_apply_attr(Compiler *c, ClassInfo *cls, int call, int kind) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, call, "name");
  if (!nm) return;
  int reader = sp_streq(nm, "attr_reader") || sp_streq(nm, "attr_accessor");
  int writer = sp_streq(nm, "attr_writer") || sp_streq(nm, "attr_accessor");
  if (!reader && !writer) return;
  int args = nt_ref(nt, call, "arguments");
  int an = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  for (int i = 0; i < an; i++) {
    const char *base = vis_arg_name(nt, argv[i]);
    if (!base) continue;
    if (reader) comp_method_vis_set(cls, base, kind);
    if (writer) {
      char buf[256];
      snprintf(buf, sizeof buf, "%s=", base);
      comp_method_vis_set(cls, buf, kind);
    }
  }
}

/* Walk one class/module body in lexical order, recording each method's
   visibility (default public). Handles a bare `private`/`protected`/`public`
   (switches the mode for following defs/attrs), the `private :a, :b` /
   `private def m;end` / `private attr_reader :x` argument forms, and plain
   `def`/`attr_*` declarations under the active mode. Class (`def self.x`)
   methods are a separate axis and left alone. */
static void register_method_visibility_body(Compiler *c, ClassInfo *cls, int body) {
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
      if (mname && nt_ref(nt, s, "receiver") < 0)
        comp_method_vis_set(cls, mname, cur);
      continue;
    }
    if (!sp_streq(sty, "CallNode") || nt_ref(nt, s, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, s, "name");
    if (!nm) continue;
    int kind = sp_streq(nm, "private")   ? SP_VIS_PRIVATE   :
               sp_streq(nm, "protected") ? SP_VIS_PROTECTED :
               sp_streq(nm, "public")    ? SP_VIS_PUBLIC : -1;
    if (kind >= 0) {
      int args = nt_ref(nt, s, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an == 0) { cur = kind; continue; }  /* bare: switch the section mode */
      for (int i = 0; i < an; i++) {
        const char *aty = nt_type(nt, argv[i]);
        const char *mn = vis_arg_name(nt, argv[i]);
        if (mn) { comp_method_vis_set(cls, mn, kind); continue; }
        if (aty && sp_streq(aty, "DefNode")) {
          const char *dn = nt_str(nt, argv[i], "name");
          if (dn && nt_ref(nt, argv[i], "receiver") < 0)
            comp_method_vis_set(cls, dn, kind);
        }
        else if (aty && sp_streq(aty, "CallNode")) {
          vis_apply_attr(c, cls, argv[i], kind);  /* private attr_reader :x */
        }
      }
      continue;
    }
    /* Record attr visibility unconditionally (like a plain `def`), so a public
       attr in a subclass overrides an inherited private/protected method rather
       than resolving up the chain to the ancestor's visibility. */
    if (sp_streq(nm, "attr_reader") || sp_streq(nm, "attr_writer") ||
        sp_streq(nm, "attr_accessor"))
      vis_apply_attr(c, cls, s, cur);
  }
}

/* Record per-method visibility for every class/module body, including reopened
   bodies (each `class Foo ... end` opening starts public, like CRuby). */
void register_method_visibility(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    register_method_visibility_body(c, cls, nt_ref(nt, cls->def_node, "body"));
  }
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || (!sp_streq(ty, "ClassNode") && !sp_streq(ty, "ModuleNode"))) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cname) continue;
    int ci = comp_class_index(c, cname);
    if (ci < 0) continue;
    if (id == c->classes[ci].def_node) continue;  /* canonical body already done */
    register_method_visibility_body(c, &c->classes[ci], nt_ref(nt, id, "body"));
  }
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
      if (nm) scope_local_intern(comp_scope_of(c, id), nm);
    }
    if (sp_streq(ty, "InstanceVariableWriteNode") ||
        sp_streq(ty, "InstanceVariableReadNode") ||
        sp_streq(ty, "InstanceVariableOperatorWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      Scope *s = comp_scope_of(c, id);
      if (nm && s->class_id >= 0) comp_ivar_intern(&c->classes[s->class_id], nm);
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

/* Register the symbol members of a Struct.new(...) call onto `cls`. */
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
    if (!nt_type(nt, argv[a]) || !sp_streq(nt_type(nt, argv[a]), "SymbolNode")) continue;
    const char *m = nt_str(nt, argv[a], "value");
    if (!m) continue;
    char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", m);
    comp_ivar_intern(cls, ivn);
    comp_add_reader(cls, m);
    comp_add_writer(cls, m);
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
typedef struct { int *wkey, *wci, n, cap; } SgMap;
static int sg_get_or_make(Compiler *c, SgMap *m, int wnode, int parent_ci) {
  for (int i = 0; i < m->n; i++) if (m->wkey[i] == wnode) return m->wci[i];
  const NodeTable *nt = c->nt;
  char snm[96];
  snprintf(snm, sizeof snm, "%s__sg_%d", c->classes[parent_ci].name ? c->classes[parent_ci].name : "Obj", wnode);
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

/* Copy module `mod_ci`'s instance methods onto subclass `newci` (obj.extend). */
static void sg_transplant_module(Compiler *c, int mod_ci, int newci) {
  const NodeTable *nt = c->nt;
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
    dst->kwrest_idx = src->kwrest_idx;
    src->is_transplanted_source = 1;   /* the module original is copied away */
    if (src->blk_param) dst->blk_param = strdup(src->blk_param);
    dst->nparams = src->nparams;
    if (src->nparams > 0) {
      dst->pnames = malloc(sizeof(char *) * (size_t)src->nparams);
      dst->pdefault = malloc(sizeof(int) * (size_t)src->nparams);
      for (int p = 0; p < src->nparams; p++) {
        dst->pnames[p] = src->pnames[p] ? strdup(src->pnames[p]) : NULL;
        dst->pdefault[p] = src->pdefault ? src->pdefault[p] : -1;
      }
    }
  }
}

void register_singleton_defs(Compiler *c) {
  const NodeTable *nt = c->nt;
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
    if (wnode < 0) continue;   /* not statically traceable: leave as today */
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
      int newci = sg_get_or_make(c, &m, wnode, parent_ci);
      for (int j = 0; j < an; j++)
        sg_transplant_module(c, comp_class_index(c, nt_str(nt, args[j], "name")), newci);
      continue;
    }
    int newci = sg_get_or_make(c, &m, wnode, parent_ci);
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
  }
  free(m.wkey); free(m.wci);
}

void register_structs(Compiler *c) {
  const NodeTable *nt = c->nt;
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
      } else {
        register_struct_members(c, comp_class_new(c, cname, id), val);
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
      snprintf(an, sizeof an, "StructAnon_%d", id);
      ClassInfo *cls = comp_class_new(c, an, id);
      cls->is_anon_struct = 1;
      register_struct_members(c, cls, val);
    }
    /* Inline `Data.define(...).method(...)` / `Struct.new(...).method(...)`: the
       struct/data call is the receiver of another call, with no name to hold it.
       Synthesize an anon class keyed to the call node itself so the receiver
       resolution (.new / .members / .class) can find it. #2682 */
    else if (sp_streq(ty, "CallNode") && is_struct_call(c, id)) {
      int is_recv = 0;
      for (int p = 0; p < nt->count && !is_recv; p++)
        if (nt_kind(nt, p) == NK_CallNode && nt_ref(nt, p, "receiver") == id) is_recv = 1;
      if (is_recv) {
        char an[48];
        snprintf(an, sizeof an, "StructAnon_%d", id);
        ClassInfo *cls = comp_class_new(c, an, id);
        cls->is_anon_struct = 1;
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
  NT_FOREACH_KIND(nt, NK_ConstantWriteNode, id) {
    const char *cname = nt_str(nt, id, "name");
    int val = nt_ref(nt, id, "value");
    if (!cname || val < 0 || !is_struct_call(c, val)) continue;
    int blk = nt_ref(nt, val, "block");
    if (blk < 0) continue;
    int ci = comp_class_index(c, cname);
    if (ci < 0) continue;
    /* Walk the block body and fix any DefNode scopes */
    int bbody = nt_ref(nt, blk, "body");
    if (bbody < 0) continue;
    int bn = 0;
    const int *stmts = nt_arr(nt, bbody, "body", &bn);
    for (int k = 0; k < bn; k++) {
      const char *sty = nt_type(nt, stmts[k]);
      if (!sty || !sp_streq(sty, "DefNode")) continue;
      int dn = stmts[k];
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
  int reader = sp_streq(nm, "attr_reader") || accessor;
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
    }
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
      }
    }
  }
}

void register_attrs(Compiler *c) {
  const NodeTable *nt = c->nt;
  /* Pass 1: process primary definition bodies. */
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    register_attrs_body(c, cls, nt_ref(nt, cls->def_node, "body"));
  }
  /* Pass 2: scan all ClassNode/ModuleNode reopenings. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || (!sp_streq(ty, "ClassNode") && !sp_streq(ty, "ModuleNode"))) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cname) continue;
    int ci = comp_class_index(c, cname);
    if (ci < 0) continue;
    if (id == c->classes[ci].def_node) continue;  /* already handled above */
    register_attrs_body(c, &c->classes[ci], nt_ref(nt, id, "body"));
  }
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
      comp_add_alias(cls, nw, od);
    }
    else if (sp_streq(sty, "CallNode")) {
      const char *nm = nt_str(nt, s, "name");
      if (!nm || !sp_streq(nm, "alias_method")) continue;
      int args = nt_ref(nt, s, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an >= 2 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "SymbolNode") &&
          nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "SymbolNode"))
        comp_add_alias(cls, nt_str(nt, argv[0], "value"), nt_str(nt, argv[1], "value"));
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
  /* Pass 1: primary definition bodies. */
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    register_aliases_body(c, cls, nt_ref(nt, cls->def_node, "body"));
  }
  /* Pass 2: reopened class/module bodies. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || (!sp_streq(ty, "ClassNode") && !sp_streq(ty, "ModuleNode"))) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cname) continue;
    int ci = comp_class_index(c, cname);
    if (ci < 0) continue;
    if (id == c->classes[ci].def_node) continue;
    register_aliases_body(c, &c->classes[ci], nt_ref(nt, id, "body"));
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
  const NodeTable *nt = c->nt;
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    register_undefs_body(c, cls, nt_ref(nt, cls->def_node, "body"));
  }
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || (!sp_streq(ty, "ClassNode") && !sp_streq(ty, "ModuleNode"))) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cname) continue;
    int ci = comp_class_index(c, cname);
    if (ci < 0) continue;
    if (id == c->classes[ci].def_node) continue;
    register_undefs_body(c, &c->classes[ci], nt_ref(nt, id, "body"));
  }
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
    }
    else if (sp_streq(ty, "AliasGlobalVariableNode")) {
      /* already handled in pass 1 */
    }
    else if (sp_streq(ty, "ConstantTargetNode")) {
      /* target in a multi-write: A, B = expr (a definite write) */
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
      } else {
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
      /* the source file's directory (same convention as the codegen fold) */
      const char *sf = nt->source_file;
      char dir[1024];
      if (sf && strrchr(sf, '/')) {
        size_t n = (size_t)(strrchr(sf, '/') - sf);
        if (n >= sizeof dir) n = sizeof dir - 1;
        if (n == 0) { dir[0] = '/'; dir[1] = 0; }
        else { memcpy(dir, sf, n); dir[n] = 0; }
      }
      else { dir[0] = '.'; dir[1] = 0; }
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

/* Register a ffi_func / ffi_const / ffi_buffer / ffi_read_* declared in
   module bodies. Called during analyze_program before fixpoint. */
void register_ffi_decls(Compiler *c) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_ModuleNode, id) {
    int cp = nt_ref(nt, id, "constant_path");
    const char *mname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!mname) continue;
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
       bind to its class index, regardless of declaration order. */
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
      if (na < 2) break;
      const char *clsname = ffi_arg_str(nt, av[0]);
      const char *cstruct = ffi_arg_str(nt, av[1]);
      const char *freesym = na >= 3 ? ffi_arg_str(nt, av[2]) : NULL;
      if (!clsname || !cstruct) break;
      int ex = comp_class_index(c, clsname);
      if (ex >= 0) native_cid = ex;
      else { comp_class_new(c, clsname, -1); native_cid = c->nclasses - 1; }
      ClassInfo *nc = &c->classes[native_cid];
      nc->is_native_class = 1;
      free(nc->c_struct); nc->c_struct = strdup(cstruct);
      if (freesym) { free(nc->native_free); nc->native_free = strdup(freesym); }
      /* NOTE: no `break` -- a module may declare many native classes (one
         native_struct per socket class), and every one must be tagged so
         codegen routes their constructor/methods to the package. */
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
        c->native_methods[mi].nargs = en;
        continue;
      }
      /* native_struct is handled in the pre-scan above; skip it here. */
      /* native_struct is handled in the pre-scan above, but the pre-scan leaves
         `native_cid` pinned to the LAST native class it saw -- so without
         re-resolving here, every native_new/native_method below would bind to
         that one class instead of its own. Re-derive native_cid from the
         native_struct's class name (mirroring the pre-scan). */
      if (sp_streq(dname, "native_struct")) {
        if (an >= 2) {
          const char *clsname2 = ffi_arg_str(nt, args[0]);
          if (clsname2) {
            int ex2 = comp_class_index(c, clsname2);
            if (ex2 >= 0) native_cid = ex2;
          }
        }
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
          if (c->n_ffi_libs >= c->c_ffi_libs) {
            c->c_ffi_libs = c->c_ffi_libs ? c->c_ffi_libs * 2 : 8;
            FfiLib *tmp = realloc(c->ffi_libs, sizeof(FfiLib) * (size_t)c->c_ffi_libs);
            if (!tmp) { perror("realloc"); exit(1); }
            c->ffi_libs = tmp;
          }
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
          if (c->n_ffi_cflags >= c->c_ffi_cflags) {
            c->c_ffi_cflags = c->c_ffi_cflags ? c->c_ffi_cflags * 2 : 8;
            FfiCflag *tmp = realloc(c->ffi_cflags, sizeof(FfiCflag) * (size_t)c->c_ffi_cflags);
            if (!tmp) { perror("realloc"); exit(1); }
            c->ffi_cflags = tmp;
          }
          c->ffi_cflags[c->n_ffi_cflags].mod = strdup(mname);
          c->ffi_cflags[c->n_ffi_cflags].val = strdup(cflag);
          c->n_ffi_cflags++;
        }
        else ffi_semi_append(&c->ffi_cflags[mi].val, cflag);
        free(cflag);
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
        int a_arr = args[1], a_ret = args[2], a_csym = -1;
        if (sp_streq(dname, "attach_function") && an >= 4) {
          a_csym = args[1]; a_arr = args[2]; a_ret = args[3];
        }
        const char *fname = ffi_arg_str(nt, args[0]);
        if (!fname) continue;  /* non-literal name: tolerate */
        /* arg type array */
        const char *arr_ty = nt_type(nt, a_arr);
        if (!arr_ty || !sp_streq(arr_ty, "ArrayNode")) continue;
        int en = 0;
        const int *elems = nt_arr(nt, a_arr, "elements", &en);
        char **arg_specs = malloc(sizeof(char*) * (size_t)(en + 1));
        if (!arg_specs) { perror("malloc"); exit(1); }
        for (int ei = 0; ei < en; ei++) {
          const char *spec = ffi_arg_str(nt, elems[ei]);
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
        if (c->n_ffi_funcs >= c->c_ffi_funcs) {
          c->c_ffi_funcs = c->c_ffi_funcs ? c->c_ffi_funcs * 2 : 16;
          FfiFunc *tmp = realloc(c->ffi_funcs, sizeof(FfiFunc) * (size_t)c->c_ffi_funcs);
          if (!tmp) { perror("realloc"); exit(1); }
          c->ffi_funcs = tmp;
        }
        int fi = c->n_ffi_funcs++;
        c->ffi_funcs[fi].mod  = strdup(mname);
        c->ffi_funcs[fi].name = strdup(fname);
        c->ffi_funcs[fi].csym = csym ? strdup(csym) : NULL;
        c->ffi_funcs[fi].ret   = strdup(ret_spec);
        c->ffi_funcs[fi].args  = arg_specs;
        c->ffi_funcs[fi].nargs = en;
        continue;
      }

      if (sp_streq(dname, "ffi_const")) {
        if (an < 2) ffi_decl_error(c, s, "`ffi_const` needs a name and an integer value");
        const char *kname = ffi_arg_str(nt, args[0]);
        if (!kname) continue;  /* non-literal name: tolerate */
        int val = ffi_arg_int(nt, args[1]);
        if (c->n_ffi_consts >= c->c_ffi_consts) {
          c->c_ffi_consts = c->c_ffi_consts ? c->c_ffi_consts * 2 : 16;
          FfiConst *tmp = realloc(c->ffi_consts, sizeof(FfiConst) * (size_t)c->c_ffi_consts);
          if (!tmp) { perror("realloc"); exit(1); }
          c->ffi_consts = tmp;
        }
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
        if (c->n_ffi_bufs >= c->c_ffi_bufs) {
          c->c_ffi_bufs = c->c_ffi_bufs ? c->c_ffi_bufs * 2 : 8;
          FfiBuf *tmp = realloc(c->ffi_bufs, sizeof(FfiBuf) * (size_t)c->c_ffi_bufs);
          if (!tmp) { perror("realloc"); exit(1); }
          c->ffi_bufs = tmp;
        }
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
        const char *kind = dname + 9;  /* "u32", "i32", "ptr" */
        if (c->n_ffi_readers >= c->c_ffi_readers) {
          c->c_ffi_readers = c->c_ffi_readers ? c->c_ffi_readers * 2 : 8;
          FfiReader *tmp = realloc(c->ffi_readers, sizeof(FfiReader) * (size_t)c->c_ffi_readers);
          if (!tmp) { perror("realloc"); exit(1); }
          c->ffi_readers = tmp;
        }
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
        if (c->n_ffi_callbacks >= c->c_ffi_callbacks) {
          c->c_ffi_callbacks = c->c_ffi_callbacks ? c->c_ffi_callbacks * 2 : 8;
          FfiCallback *grown = realloc(c->ffi_callbacks, sizeof(FfiCallback) * (size_t)c->c_ffi_callbacks);
          if (!grown) { perror("realloc"); exit(1); }
          c->ffi_callbacks = grown;
        }
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
        if (c->n_ffi_structs >= c->c_ffi_structs) {
          c->c_ffi_structs = c->c_ffi_structs ? c->c_ffi_structs * 2 : 8;
          FfiStruct *grown = realloc(c->ffi_structs, sizeof(FfiStruct) * (size_t)c->c_ffi_structs);
          if (!grown) { perror("realloc"); exit(1); }
          c->ffi_structs = grown;
        }
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
        const char *kind = dname + 10;  /* "u32", "i32", "ptr" */
        /* reject a typoed/unsupported suffix rather than silently registering
           it and falling back to some default store at codegen. */
        if (!sp_streq(kind, "u32") && !sp_streq(kind, "i32") && !sp_streq(kind, "ptr")) continue;
        if (c->n_ffi_writers >= c->c_ffi_writers) {
          c->c_ffi_writers = c->c_ffi_writers ? c->c_ffi_writers * 2 : 8;
          FfiReader *grown = realloc(c->ffi_writers, sizeof(FfiReader) * (size_t)c->c_ffi_writers);
          if (!grown) { perror("realloc"); exit(1); }
          c->ffi_writers = grown;
        }
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
      vt = infer_type(c, nt_ref(nt, id, "value"));
      if (vt == TY_NIL) continue;
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
      if (vt == TY_NIL) continue;
    }
    else if (sp_streq(ty, "ConstantWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm) lv = comp_const(c, nm);
      vt = infer_type(c, nt_ref(nt, id, "value"));
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
      const char *vty = nt_type(nt, value);
      int en = 0;
      const int *els = (vty && sp_streq(vty, "ArrayNode")) ? nt_arr(nt, value, "elements", &en) : NULL;
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
      if (!is_push) continue;
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
      if (cac < 1 || !cav) continue;
      TyKind et = infer_type(c, cav[0]);
      if (et == TY_UNKNOWN || et == TY_NIL) continue;
      vt = ty_array_of(et);
      if (vt == TY_UNKNOWN) vt = TY_POLY_ARRAY;
    }
    else {
      continue;
    }
    if (!lv) continue;
    TyKind merged = ty_unify(lv->type, vt);
    if (merged != lv->type) { lv->type = merged; changed = 1; }
  }
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
      if (!sp_streq(lty, "ConstantTargetNode")) continue;
      const char *nm = nt_str(nt, lefts[i], "name");
      LocalVar *cv = nm ? comp_const(c, nm) : NULL;
      if (!cv || cv->type == elem) continue;
      cv->type = elem; changed = 1;
    }
    int rn = 0;
    const int *rights = nt_arr(nt, id, "rights", &rn);
    for (int j = 0; j < rn; j++) {
      const char *rty2 = nt_type(nt, rights[j]) ? nt_type(nt, rights[j]) : "";
      if (!sp_streq(rty2, "ConstantTargetNode")) continue;
      const char *nm = nt_str(nt, rights[j], "name");
      LocalVar *cv = nm ? comp_const(c, nm) : NULL;
      if (!cv || cv->type == elem) continue;
      cv->type = elem; changed = 1;
    }
  }
  return changed;
}

/* Resolve each class's superclass index from its ClassNode. */
void resolve_parents(Compiler *c) {
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
      if (mod_id < 0) continue;
      /* record membership for `rescue M` matching (dedup across reopenings) */
      {
        ClassInfo *cif = &c->classes[ci];
        int seen = 0;
        for (int m = 0; m < cif->nincluded_mods; m++)
          if (cif->included_mods[m] == mod_id) { seen = 1; break; }
        if (!seen) {
          if (cif->nincluded_mods >= cif->cincluded_mods) {
            cif->cincluded_mods = cif->cincluded_mods ? cif->cincluded_mods * 2 : 4;
            int *nm2 = realloc(cif->included_mods, sizeof(int) * (size_t)cif->cincluded_mods);
            if (!nm2) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
            cif->included_mods = nm2;
          }
          cif->included_mods[cif->nincluded_mods++] = mod_id;
        }
      }
      /* snapshot count before adding new scopes to avoid re-scanning them */
      int snap = c->nscopes;
      for (int ms = 0; ms < snap; ms++) {
        Scope *src = &c->scopes[ms];
        if (src->class_id != mod_id || src->is_cmethod || !src->name) continue;
        const char *dst_name = src->name;
        char inc_shadow[256];
        int own = comp_method_in_class(c, ci, src->name);
        if (own >= 0) {
          /* The class overrides the module method. If the override calls super,
             the module method is the super target: copy it under a shadow name
             and chain to it so emit_super reaches it via the prepend-super path.
             Otherwise the module method is simply shadowed -- nothing to emit. */
          if (!scope_body_has_super(c, own)) continue;
          const char *existing = comp_prep_chain_target(c, ci, src->name);
          snprintf(inc_shadow, sizeof inc_shadow, "__inc_%d_%s",
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
        dst->class_id = ci;
        dst->is_cmethod = 0;
        dst->reachable = src->reachable;
        dst->yields = src->yields;
        dst->nrequired = src->nrequired;
        dst->rest_idx = src->rest_idx;
        dst->kwrest_idx = src->kwrest_idx;
        if (src->blk_param) dst->blk_param = strdup(src->blk_param);
        src->is_transplanted_source = 1;
        /* Copy parameter names and defaults. */
        dst->nparams = src->nparams;
        if (src->nparams > 0) {
          dst->pnames = malloc(sizeof(char *) * (size_t)src->nparams);
          dst->pdefault = malloc(sizeof(int) * (size_t)src->nparams);
          for (int p = 0; p < src->nparams; p++) {
            dst->pnames[p] = src->pnames[p] ? strdup(src->pnames[p]) : NULL;
            dst->pdefault[p] = src->pdefault ? src->pdefault[p] : -1;
          }
          /* Register param locals so infer_param_types can update types. */
          for (int p = 0; p < src->nparams; p++) {
            if (dst->pnames[p]) {
              LocalVar *lv = scope_local_intern(dst, dst->pnames[p]);
              lv->is_param = 1;
            }
          }
        }
        /* Scan source body for ivar accesses and register them in the
           destination class so codegen's struct layout includes them. */
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
    }
  }
}

/* For each class, find `include M` declarations in ALL class bodies
   (including reopenings) and transplant M's instance methods into the
   class so they are reachable via comp_method_in_chain. */
void register_includes(Compiler *c) {
  const NodeTable *nt = c->nt;
  g_inc_did_clone = 0;
  /* First pass: process def_node bodies (first class definition). */
  for (int ci = 0; ci < c->nclasses; ci++) {
    int body = nt_ref(nt, c->classes[ci].def_node, "body");
    process_include_body(c, ci, body);
  }
  /* Second pass: scan all ClassNode/ModuleNode in the AST for reopenings. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || (!sp_streq(ty, "ClassNode") && !sp_streq(ty, "ModuleNode"))) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cname) continue;
    int ci = comp_class_index(c, cname);
    if (ci < 0) continue;
    if (id == c->classes[ci].def_node) continue;  /* already processed above */
    int body = nt_ref(nt, id, "body");
    process_include_body(c, ci, body);
  }
  if (g_inc_did_clone) register_locals(c);
}

/* For each class, find `extend M` declarations and transplant M's instance
   methods as class methods (is_cmethod=1) so they are callable as C.m. */
void register_extends(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int ci = 0; ci < c->nclasses; ci++) {
    int body = nt_ref(nt, c->classes[ci].def_node, "body");
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
      for (int j = 0; j < an; j++) {
        const char *aty = nt_type(nt, args[j]);
        const char *mname = NULL;
        if (aty && sp_streq(aty, "ConstantReadNode")) mname = nt_str(nt, args[j], "name");
        else if (aty && sp_streq(aty, "ConstantPathNode")) mname = nt_str(nt, args[j], "name");
        int mod_id = mname ? comp_class_index(c, mname) : -1;
        if (mod_id < 0) continue;
        int snap = c->nscopes;
        for (int ms = 0; ms < snap; ms++) {
          Scope *src = &c->scopes[ms];
          /* Only transplant instance methods; self.* on the module stay on it. */
          if (src->class_id != mod_id || src->is_cmethod || !src->name) continue;
          if (comp_cmethod_in_class(c, ci, src->name) >= 0) continue;
          Scope *dst = comp_scope_new(c, src->name, src->def_node);
          src = &c->scopes[ms];
          dst->body = src->body;
          dst->class_id = ci;
          dst->is_cmethod = 1;  /* transplanted as a class method */
          dst->reachable = src->reachable;
          dst->yields = src->yields;
          dst->nrequired = src->nrequired;
          dst->rest_idx = src->rest_idx;
          dst->kwrest_idx = src->kwrest_idx;
          if (src->blk_param) dst->blk_param = strdup(src->blk_param);
          dst->nparams = src->nparams;
          if (src->nparams > 0) {
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
              }
            }
          }
          src->is_transplanted_source = 1;
        }
      }
    }
  }
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
static int cmethod_reaches_override(Compiler *c, int mi, int ci, int def_cls, int depth) {
  if (depth > 64) return 0;
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
        cmethod_reaches_override(c, mdef, ci, sub_def, depth + 1)) return 1;
  }
  return 0;
}

/* Does the inherited cls method `mi` (defined on def_cls) contain a bare call
   that would resolve differently when run as a class method of `ci`? That is:
   a bare `new` (constructs ci), or a bare cmethod call that resolves to ci's
   own version directly or transitively (cmethod_reaches_override). */
int cmethod_needs_specialization(Compiler *c, int mi, int ci, int def_cls, int *has_new) {
  const NodeTable *nt = c->nt;
  int need = 0;
  if (has_new) *has_new = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    if (c->nscope[id] != mi) continue;
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
  if (!need) {
    for (int id = 0; id < nt->count; id++) {
      if (c->nscope[id] != mi) continue;
      NodeKind k = nt_kind(nt, id);
      if (k == NK_InstanceVariableReadNode || k == NK_InstanceVariableWriteNode ||
          k == NK_InstanceVariableOperatorWriteNode || k == NK_InstanceVariableOrWriteNode ||
          k == NK_InstanceVariableAndWriteNode || k == NK_InstanceVariableTargetNode) { need = 1; break; }
    }
  }
  if (cmethod_reaches_override(c, mi, ci, def_cls, 0)) need = 1;
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
  /* A bare-`new` create method returns the specialized subclass instance, so
     pin its return type. Other specializations let normal return inference
     compute the type from the cloned, ci-attributed body. */
  if (has_new) {
    dst->ret = ty_object(ci);
    dst->ret_specialized = 1;
  }
  if (src->blk_param) dst->blk_param = strdup(src->blk_param);
  dst->nparams = src->nparams;
  if (src->nparams > 0) {
    dst->pnames = malloc(sizeof(char *) * (size_t)src->nparams);
    dst->pdefault = malloc(sizeof(int) * (size_t)src->nparams);
    for (int p = 0; p < src->nparams; p++) {
      dst->pnames[p] = src->pnames[p] ? strdup(src->pnames[p]) : NULL;
      dst->pdefault[p] = src->pdefault ? src->pdefault[p] : -1;
      if (dst->pnames[p]) { LocalVar *lv = scope_local_intern(dst, dst->pnames[p]); lv->is_param = 1; }
    }
  }
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
  for (int id = 0; id < node_count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    int recv = nt_ref(nt, id, "receiver");
    int ci = -1;
    if (recv < 0 ||
        (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode"))) {
      /* a bare (or explicit-self: this pass runs before the desugar that
         drops such receivers) call inside a class-method body: self is the
         class there, so an inherited factory's `new` must construct the
         CALLING class. Clone for the enclosing class exactly like the
         Const-receiver form (`def self.upsert; build { |kv| ... }; end`). */
      Scope *encl = comp_scope_of(c, id);
      if (!encl || !encl->is_cmethod || encl->class_id < 0) continue;
      ci = encl->class_id;
    }
    else {
      const char *rty = nt_type(nt, recv);
      if (!rty || (!sp_streq(rty, "ConstantReadNode") && !sp_streq(rty, "ConstantPathNode"))) continue;
      const char *cn = nt_str(nt, recv, "name");
      ci = cn ? comp_class_index(c, cn) : -1;
      if (ci < 0) continue;
    }
    const char *mname = nt_str(nt, id, "name");
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
    /* keep it if called directly as <DefiningClass>.<name> */
    int called_direct = 0;
    for (int ii = 0; ii < nccall && !called_direct; ii++) {
      int id = ccall[ii];
      if (!nt_str(nt, id, "name") || !sp_streq(nt_str(nt, id, "name"), src->name)) continue;
      int r = nt_ref(nt, id, "receiver");
      if (comp_class_index(c, nt_str(nt, r, "name")) == src->class_id) called_direct = 1;
    }
    /* ... or if a bare call inside some class-method body still resolves to
       this source (a class with no specialized copy of its own) */
    for (int id = 0; id < node_count && !called_direct; id++) {
      if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
      int r2 = nt_ref(nt, id, "receiver");
      if (r2 >= 0 && !(nt_type(nt, r2) && sp_streq(nt_type(nt, r2), "SelfNode"))) continue;
      const char *nm2 = nt_str(nt, id, "name");
      if (!nm2 || !sp_streq(nm2, src->name)) continue;
      Scope *encl = comp_scope_of(c, id);
      if (!encl || !encl->is_cmethod || encl->class_id < 0) continue;
      if (comp_cmethod_in_chain(c, encl->class_id, nm2, NULL) == s) called_direct = 1;
    }
    if (!called_direct) src->is_transplanted_source = 1;
  }
  free(ccall);
  /* The cloned bodies introduced new local/ivar nodes; intern them. */
  if (did_clone) register_locals(c);
}

/* For each class, find `prepend M` declarations and transplant M's instance
   methods into the class with shadow-chain renaming so `super` can route
   from M's body to the original (now renamed) class body. */
void register_prepends(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int ci = 0; ci < c->nclasses; ci++) {
    int body = nt_ref(nt, c->classes[ci].def_node, "body");
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
          /* Transplant the module scope into class ci. */
          sc->class_id = ci;
        }
      }
    }
  }
}

/* Merge inherited ivar/reader/writer NAMES into subclasses so the struct
   layout is [parent ivars..., own ivars...] (cast-compatible). Types are
   propagated later in the fixpoint. Parent-first order. */
void inherit_members(Compiler *c) {
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    int p = ci->parent;
    if (p < 0 || p >= i) continue;  /* parent defined earlier; already merged */
    ClassInfo *pc = &c->classes[p];

    char **old = ci->ivars; TyKind *oldt = ci->ivar_types; int oldn = ci->nivars;
    ci->ivars = NULL; ci->ivar_types = NULL; ci->nivars = ci->civars = 0;
    for (int k = 0; k < pc->nivars; k++) {
      int idx = comp_ivar_intern(ci, pc->ivars[k]);
      ci->ivar_types[idx] = pc->ivar_types[k];
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
      free(old[k]);
    }
    free(old); free(oldt);

    for (int k = 0; k < pc->nreaders; k++) comp_add_reader(ci, pc->readers[k]);
    for (int k = 0; k < pc->nwriters; k++) comp_add_writer(ci, pc->writers[k]);
  }
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

/* @ivar types from their assignments across the class's methods. */
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
      if (sp_streq(sty, "ClassVariableWriteNode")) {
        const char *nm = nt_str(nt, s, "name");
        if (!nm) continue;
        int idx = comp_cvar_intern(&c->classes[ci], nm);
        TyKind vt = infer_type(c, nt_ref(nt, s, "value"));
        if (vt == TY_NIL) continue;
        TyKind merged = ty_unify(c->classes[ci].cvar_types[idx], vt);
        if (merged != c->classes[ci].cvar_types[idx]) { c->classes[ci].cvar_types[idx] = merged; changed = 1; }
      }
      else if (sp_streq(sty, "MultiWriteNode")) {
        int mln = 0;
        const int *mlefts = nt_arr(nt, s, "lefts", &mln);
        int mval = nt_ref(nt, s, "value");
        const char *mvty = nt_type(nt, mval);
        int men = 0;
        const int *mels = (mvty && sp_streq(mvty, "ArrayNode")) ? nt_arr(nt, mval, "elements", &men) : NULL;
        for (int mi = 0; mi < mln; mi++) {
          const char *mlty = nt_type(nt, mlefts[mi]);
          if (!mlty || !sp_streq(mlty, "ClassVariableTargetNode")) continue;
          const char *cnm = nt_str(nt, mlefts[mi], "name");
          if (!cnm) continue;
          int midx = comp_cvar_intern(&c->classes[ci], cnm);
          TyKind mvt2 = (mels && mi < men) ? infer_type(c, mels[mi]) : TY_UNKNOWN;
          if (mvt2 == TY_NIL || mvt2 == TY_UNKNOWN) continue;
          TyKind mmerged = ty_unify(c->classes[ci].cvar_types[midx], mvt2);
          if (mmerged != c->classes[ci].cvar_types[midx]) { c->classes[ci].cvar_types[midx] = mmerged; changed = 1; }
        }
      }
    }
  }
  /* Pass 2: method-level writes (comp_scope_of has class_id set). */
  NT_FOREACH_KIND(nt, NK_ClassVariableWriteNode, id) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    if (!nm || s->class_id < 0) continue;
    ClassInfo *ci = &c->classes[s->class_id];
    int idx = comp_cvar_intern(ci, nm);
    TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
    if (vt == TY_NIL) continue;
    TyKind merged = ty_unify(ci->cvar_types[idx], vt);
    if (merged != ci->cvar_types[idx]) { ci->cvar_types[idx] = merged; changed = 1; }
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
    int idx = comp_cvar_intern(&c->classes[cci], cvn);
    TyKind vt = infer_type(c, av[1]);
    if (vt == TY_NIL || vt == TY_UNKNOWN) continue;
    TyKind merged = ty_unify(c->classes[cci].cvar_types[idx], vt);
    if (merged != c->classes[cci].cvar_types[idx]) { c->classes[cci].cvar_types[idx] = merged; changed = 1; }
  }
  /* Pass 3: top-level writes (class_id == -1 in scope 0) -- use Toplevel pseudo-class. */
  NT_FOREACH_KIND(nt, NK_ClassVariableWriteNode, id) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    if (!nm || s->class_id >= 0) continue;
    int tl_idx = comp_class_index(c, "Toplevel");
    if (tl_idx < 0) { comp_class_new(c, "Toplevel", -1); tl_idx = c->nclasses - 1; }
    ClassInfo *ci = &c->classes[tl_idx];
    int idx = comp_cvar_intern(ci, nm);
    TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
    if (vt == TY_NIL) continue;
    TyKind merged = ty_unify(ci->cvar_types[idx], vt);
    if (merged != ci->cvar_types[idx]) { ci->cvar_types[idx] = merged; changed = 1; }
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

int infer_ivar_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
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
    if (sp_streq(ty, "InstanceVariableReadNode")) {
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
      if (vt == TY_NIL) continue;  /* nil write doesn't pin the ivar type */
      /* `@a = @b = nil`: the chain writes nil to every target; don't let the
         inner slot's unified type (from its other writes) pin this ivar. */
      if (comp_nil_chain_bottom(nt, vnode) >= 0) continue;
      if (vt == TY_POLY && ivar_nullable_int_ternary(c, vnode) == TY_INT) vt = TY_INT;
      Scope *s = comp_scope_of(c, id);
      int cls_id2 = s->class_id;
      if (!nm) continue;
      /* A `@ivar = v` directly in a class/module body (not in a method) belongs
         to that class/module object, like its class methods see it -- attribute
         it to the enclosing class-body rather than the Toplevel pseudo-class. */
      if (cls_id2 < 0 && c->node_cbody[id] >= 0) cls_id2 = c->node_cbody[id];
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
      if (!class_ivar_pinned(ci, nm)) {
        TyKind merged = ty_unify(ci->ivar_types[iv], vt);
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
          TyKind tmerged = ty_unify(tc->ivar_types[tiv], vt);
          sp_ivwatch(nm, "transplant_merge", tc->ivar_types[tiv], tmerged);
          if (tmerged != tc->ivar_types[tiv]) { tc->ivar_types[tiv] = tmerged; changed = 1; }
        }
      }
    }
    else if (sp_streq(ty, "CallNode")) {
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
      if (vt == TY_NIL) continue;  /* a nil write doesn't pin the ivar type */
      char ivname[256];
      snprintf(ivname, sizeof ivname, "@%s", base);
      TyKind rt = infer_type(c, recv);
      if (ty_is_object(rt)) {
        /* concrete receiver: attribute to its class. */
        ClassInfo *ci = &c->classes[ty_object_class(rt)];
        if (!comp_is_writer(ci, base)) continue;
        int iv = comp_ivar_index(ci, ivname);
        if (iv < 0 || class_ivar_pinned(ci, ivname)) continue;
        TyKind merged = ty_unify(ci->ivar_types[iv], vt);
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
        TyKind merged = ty_unify(ci->ivar_types[iv], vt);
        if (merged != ci->ivar_types[iv]) { ci->ivar_types[iv] = merged; changed = 1; }
      }
    }
  }
  return changed;
}

/* ---- fixpoint passes ---- */

