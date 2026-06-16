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
  if (strcmp(want, name)) return;
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
    if (!ty || strcmp(ty, "CallNode")) continue;
    const char *cn = nt_str(nt, id, "name");
    if (!cn || strcmp(cn, mname)) continue;
    int a = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (an == 0) continue;
    /* a `foo(...)` forwarding call is not a concrete arg count */
    if (an == 1 && nt_type(nt, av[0]) && !strcmp(nt_type(nt, av[0]), "ForwardingArgumentsNode")) continue;
    int pos = an;
    if (an > 0 && nt_type(nt, av[an - 1]) && !strcmp(nt_type(nt, av[an - 1]), "KeywordHashNode")) pos = an - 1;
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
    if (rpty && !strcmp(rpty, "RestParameterNode")) {
      const char *rname = nt_str(c->nt, rp, "name");
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
    int dv = !strcmp(pty, "OptionalKeywordParameterNode") ? nt_ref(c->nt, kws[i], "value") : -1;
    if (pname) scope_add_param(s, pname, dv);
  }
  int kwrp = nt_ref(c->nt, pn, "keyword_rest");
  if (kwrp >= 0) {
    const char *kwrpty = nt_type(c->nt, kwrp);
    if (kwrpty && !strcmp(kwrpty, "KeywordRestParameterNode")) {
      const char *kwrname = nt_str(c->nt, kwrp, "name");
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
  if (bp >= 0 && nt_type(c->nt, bp) && !strcmp(nt_type(c->nt, bp), "BlockParameterNode")) {
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
        !strcmp(nt_type(c->nt, kwr), "ForwardingParameterNode") && s->name) {
      int arity = forwarding_call_arity(c, s->name);
      for (int i = 0; i < arity; i++) {
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
        if (!ty || strcmp(ty, "CallNode") || !nt_str(nt, id, "name") ||
            strcmp(nt_str(nt, id, "name"), s->name)) continue;
        int a = nt_ref(nt, id, "arguments");
        int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
        if (an == 0 || !nt_type(nt, av[an - 1]) ||
            strcmp(nt_type(nt, av[an - 1]), "KeywordHashNode")) continue;
        int en = 0; const int *els = nt_arr(nt, av[an - 1], "elements", &en);
        for (int e = 0; e < en; e++) {
          int key = nt_ref(nt, els[e], "key");
          const char *kty = key >= 0 ? nt_type(nt, key) : NULL;
          const char *kn = (kty && !strcmp(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
          if (!kn) continue;
          int dup = 0;
          for (int p = 0; p < s->nparams; p++) if (!strcmp(s->pnames[p], kn)) { dup = 1; break; }
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
         !strcmp(nt_type(c->nt, kwr), "ForwardingParameterNode");
}

/* The method `s`'s body forwards `...` to (a `callee(...)` call). Returns the
   callee scope index, or -1 if none/unresolved. */
static int forwarding_target_idx(Compiler *c, Scope *s) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode") || comp_scope_of(c, id) != s) continue;
    int a = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (an != 1 || !nt_type(nt, av[0]) ||
        strcmp(nt_type(nt, av[0]), "ForwardingArgumentsNode")) continue;
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
  if (!strcmp(ty, "IntegerNode")) {
    char buf[32]; snprintf(buf, sizeof buf, "%lld", (long long)nt_int(nt, lit, "value", 0));
    return strdup(buf);
  }
  if (!strcmp(ty, "StringNode")) {
    const char *s = nt_str(nt, lit, "content");
    if (!s) s = nt_str(nt, lit, "unescaped");
    return s ? strdup(s) : NULL;
  }
  if (!strcmp(ty, "SymbolNode")) { const char *s = nt_str(nt, lit, "value"); return s ? strdup(s) : NULL; }
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
  if (!strcmp(ty, "StringNode")) {
    const char *s = nt_str(nt, node, "content");
    if (!s) s = nt_str(nt, node, "unescaped");
    return s ? strdup(s) : NULL;
  }
  if (!strcmp(ty, "SymbolNode")) { const char *s = nt_str(nt, node, "value"); return s ? strdup(s) : NULL; }
  if (!strcmp(ty, "LocalVariableReadNode")) {
    const char *nm = nt_str(nt, node, "name");
    if (nm && bv && !strcmp(nm, bv)) return dm_lit_str(c, lit);
    return NULL;
  }
  if (!strcmp(ty, "EmbeddedStatementsNode")) {
    int body = nt_ref(nt, node, "statements");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    if (bn != 1) return NULL;
    return dm_eval_name(c, bb[0], bv, lit);
  }
  if (!strcmp(ty, "InterpolatedStringNode") || !strcmp(ty, "InterpolatedSymbolNode")) {
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
  if (!strcmp(ty, "IntegerNode")) return TY_INT;
  if (!strcmp(ty, "StringNode"))  return TY_STRING;
  if (!strcmp(ty, "SymbolNode"))  return TY_SYMBOL;
  return TY_UNKNOWN;
}

/* Detect `[lit, ...].each { |v| define_method("m_#{v}") { body } }` in a
   class body and synthesize one method scope per literal element, each with
   a compile-time substitution of `v`. Returns 1 if handled. */
int collect_dm_each_unroll(Compiler *c, int id, int class_id) {
  const NodeTable *nt = c->nt;
  if (class_id < 0) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || strcmp(nm, "each")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || strcmp(nt_type(nt, recv), "ArrayNode")) return 0;
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
  if (!nt_type(nt, dc) || strcmp(nt_type(nt, dc), "CallNode")) return 0;
  const char *dcn = nt_str(nt, dc, "name");
  if (!dcn || strcmp(dcn, "define_method") || nt_ref(nt, dc, "receiver") >= 0) return 0;
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
const char *resolve_class_alias(Compiler *c, const char *cname) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "ConstantWriteNode")) continue;
    const char *n = nt_str(nt, id, "name");
    if (!n || strcmp(n, cname)) continue;
    int v = nt_ref(nt, id, "value");
    if (v < 0) return NULL;
    const char *vty = nt_type(nt, v);
    if (vty && (!strcmp(vty, "ConstantReadNode") || !strcmp(vty, "ConstantPathNode"))) {
      const char *vn = nt_str(nt, v, "name");
      if (vn && (comp_class_index(c, vn) >= 0 || is_builtin_class_name(vn))) return vn;
    }
    if (vty && !strcmp(vty, "CallNode") && nt_str(nt, v, "name") &&
        !strcmp(nt_str(nt, v, "name"), "class")) {
      int r = nt_ref(nt, v, "receiver");
      if (r >= 0) return builtin_class_of_type(infer_type(c, r));
    }
    return NULL;
  }
  return NULL;
}

/* compiler_state_* class macros (see legacy/compiler_helpers.rb): declare a
   bag of typed instance variables and auto-synthesize init/dump/set methods.
   The CRuby shim that would define these via define_method is dead code under
   RUBY_ENGINE != "ruby"; spinel recognizes the macros natively here. */
const char *cs_macro_kind(const char *nm) {
  if (!nm) return NULL;
  if (!strcmp(nm, "compiler_state_int")) return "int";
  if (!strcmp(nm, "compiler_state_str")) return "str";
  if (!strcmp(nm, "compiler_state_sa"))  return "sa";
  if (!strcmp(nm, "compiler_state_ia"))  return "ia";
  return NULL;
}
static TyKind cs_field_type(const char *kind) {
  if (!strcmp(kind, "str")) return TY_STRING;
  if (!strcmp(kind, "sa"))  return TY_STR_ARRAY;
  if (!strcmp(kind, "ia"))  return TY_INT_ARRAY;
  return TY_INT;
}
/* CS_SYNTH_* markers; mirrored in codegen. */
enum { CS_INIT = 1, CS_DUMP, CS_SET_INT, CS_SET_STR, CS_SET_SA, CS_SET_IA };
static void cs_synth_method(Compiler *c, int class_id, int def_node, const char *name,
                            int cs_synth, TyKind ret, const char **pnames,
                            const TyKind *ptypes, int nparams) {
  for (int s = 0; s < c->nscopes; s++)
    if (c->scopes[s].class_id == class_id && c->scopes[s].name &&
        !strcmp(c->scopes[s].name, name)) return;  /* already present */
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
    if (!aty || strcmp(aty, "SymbolNode")) continue;
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

void walk_scope(Compiler *c, int id, int scope_idx, int class_id) {
  if (id < 0 || id >= c->nt->count) return;
  c->nscope[id] = scope_idx;
  c->node_cbody[id] = g_cbody_class_id;
  const char *ty = nt_type(c->nt, id);
  int child = scope_idx;
  int child_class = class_id;

  /* `class << self; def X; ...; end; end` — treat body defs as class methods. */
  if (ty && !strcmp(ty, "SingletonClassNode")) {
    int sbody = nt_ref(c->nt, id, "body");
    if (sbody >= 0) {
      int n = 0;
      const int *stmts = nt_arr(c->nt, sbody, "body", &n);
      for (int k = 0; k < n; k++) {
        int s = stmts[k];
        const char *sty = nt_type(c->nt, s);
        if (!sty) continue;
        if (!strcmp(sty, "DefNode")) {
          const char *name = nt_str(c->nt, s, "name");
          if (!name) continue;
          Scope *sc = comp_scope_new(c, name, s);
          int new_idx = c->nscopes - 1;
          sc->body = nt_ref(c->nt, s, "body");
          sc->class_id = class_id;
          sc->is_cmethod = 1;
          collect_def_params(c, s, sc);
          /* Assign scope to the def node and its body */
          c->nscope[s] = new_idx;
          if (sc->body >= 0) walk_scope(c, sc->body, new_idx, class_id);
        }
        else {
          walk_scope(c, s, scope_idx, class_id);
        }
      }
      c->nscope[id] = scope_idx;
      c->nscope[sbody] = scope_idx;
    }
    return;
  }

  if (ty && (!strcmp(ty, "ClassNode") || !strcmp(ty, "ModuleNode"))) {
    int cp = nt_ref(c->nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(c->nt, cp, "name") : NULL;
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
    }
  }
  else if (ty && !strcmp(ty, "DefNode")) {
    const char *name = nt_str(c->nt, id, "name");
    Scope *s = comp_scope_new(c, name, id);
    int new_idx = c->nscopes - 1;
    s->body = nt_ref(c->nt, id, "body");
    s->class_id = class_id;   /* instance method of the enclosing class */
    /* `def self.foo` / `def Klass.foo`: a class (singleton) method. */
    if (nt_ref(c->nt, id, "receiver") >= 0) s->is_cmethod = 1;
    collect_def_params(c, id, s);
    child = new_idx;
  }
  else if (ty && !strcmp(ty, "CallNode")) {
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
    if (dm_cn && !strcmp(dm_cn, "define_method") && dm_recv < 0) {
      int dm_args = nt_ref(c->nt, id, "arguments");
      int dm_na = 0;
      const int *dm_argv = dm_args >= 0 ? nt_arr(c->nt, dm_args, "arguments", &dm_na) : NULL;
      if (dm_na >= 1) {
        const char *dm_aty = nt_type(c->nt, dm_argv[0]);
        const char *dm_mname = NULL;
        if (dm_aty && !strcmp(dm_aty, "SymbolNode"))
          dm_mname = nt_str(c->nt, dm_argv[0], "value");
        else if (dm_aty && !strcmp(dm_aty, "StringNode"))
          dm_mname = nt_str(c->nt, dm_argv[0], "content");
        int dm_blk = nt_ref(c->nt, id, "block");
        if (dm_mname && dm_blk >= 0) {
          Scope *dm_s = comp_scope_new(c, dm_mname, id);
          int dm_new_idx = c->nscopes - 1;
          dm_s->body = nt_ref(c->nt, dm_blk, "body");
          dm_s->class_id = class_id;
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
  if (child_class >= 0) g_cbody_class_id = child_class;

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
}

/* Mark methods following `module_function` in a module body as class-level
   (is_cmethod=1, no self param). This lets them be called as bare functions
   when their module is included at the top level. */
void register_module_functions(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int ci = 0; ci < c->nclasses; ci++) {
    int dn = c->classes[ci].def_node;
    const char *dt = dn >= 0 ? nt_type(nt, dn) : NULL;
    if (!dt || strcmp(dt, "ModuleNode")) continue;
    int body = nt_ref(nt, dn, "body");
    if (body < 0) continue;
    int bn = 0;
    const int *stmts = nt_arr(nt, body, "body", &bn);
    int in_module_function = 0;
    for (int k = 0; k < bn; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty) continue;
      if (!strcmp(sty, "CallNode") && nt_ref(nt, s, "receiver") < 0) {
        const char *nm = nt_str(nt, s, "name");
        if (nm && !strcmp(nm, "module_function")) {
          /* `module_function :m1, :m2` form: mark named methods */
          int an = 0;
          int anode = nt_ref(nt, s, "arguments");
          const int *aargs = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
          if (an == 0) { in_module_function = 1; continue; }
          for (int ai = 0; ai < an; ai++) {
            const char *aty = nt_type(nt, aargs[ai]);
            const char *aval = NULL;
            if (aty && !strcmp(aty, "SymbolNode")) aval = nt_str(nt, aargs[ai], "value");
            if (!aval) continue;
            for (int mi = 0; mi < c->nscopes; mi++) {
              if (c->scopes[mi].class_id == ci && !c->scopes[mi].is_cmethod &&
                  c->scopes[mi].name && !strcmp(c->scopes[mi].name, aval))
                c->scopes[mi].is_cmethod = 1;
            }
          }
          continue;
        }
      }
      if (!strcmp(sty, "DefNode") && in_module_function) {
        const char *mname = nt_str(nt, s, "name");
        if (!mname) continue;
        for (int mi = 0; mi < c->nscopes; mi++) {
          if (c->scopes[mi].def_node == s) { c->scopes[mi].is_cmethod = 1; break; }
        }
      }
    }
  }
}

void register_locals(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (!strcmp(ty, "LocalVariableWriteNode") ||
        !strcmp(ty, "LocalVariableTargetNode") ||
        !strcmp(ty, "LocalVariableReadNode") ||
        !strcmp(ty, "LocalVariableOperatorWriteNode") ||
        !strcmp(ty, "LocalVariableOrWriteNode") ||
        !strcmp(ty, "LocalVariableAndWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm) scope_local_intern(comp_scope_of(c, id), nm);
    }
    if (!strcmp(ty, "InstanceVariableWriteNode") ||
        !strcmp(ty, "InstanceVariableReadNode") ||
        !strcmp(ty, "InstanceVariableOperatorWriteNode")) {
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
  if (val < 0 || !nt_type(nt, val) || strcmp(nt_type(nt, val), "CallNode")) return 0;
  const char *mn = nt_str(nt, val, "name");
  int vr = nt_ref(nt, val, "receiver");
  const char *rn = vr >= 0 && nt_type(nt, vr) && !strcmp(nt_type(nt, vr), "ConstantReadNode")
                   ? nt_str(nt, vr, "name") : NULL;
  return rn && ((!strcmp(rn, "Struct") && mn && !strcmp(mn, "new")) ||
                (!strcmp(rn, "Data") && mn && !strcmp(mn, "define")));
}

/* Register the symbol members of a Struct.new(...) call onto `cls`. */
void register_struct_members(Compiler *c, ClassInfo *cls, int val) {
  const NodeTable *nt = c->nt;
  cls->is_struct = 1;
  int args = nt_ref(nt, val, "arguments");
  int an = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  for (int a = 0; a < an; a++) {
    if (!nt_type(nt, argv[a]) || strcmp(nt_type(nt, argv[a]), "SymbolNode")) continue;
    const char *m = nt_str(nt, argv[a], "value");
    if (!m) continue;
    char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", m);
    comp_ivar_intern(cls, ivn);
    comp_add_reader(cls, m);
    comp_add_writer(cls, m);
  }
}

void register_structs(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    /* Const = Struct.new(:a, :b) */
    if (!strcmp(ty, "ConstantWriteNode")) {
      const char *cname = nt_str(nt, id, "name");
      int val = nt_ref(nt, id, "value");
      if (!cname || !is_c_ident(cname) || !is_struct_call(c, val)) continue;
      if (comp_class_index(c, cname) >= 0) continue;
      register_struct_members(c, comp_class_new(c, cname, id), val);
    }
    /* class X < Struct.new(:a, :b); ... end */
    else if (!strcmp(ty, "ClassNode")) {
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
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "ConstantWriteNode")) continue;
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
      if (!sty || strcmp(sty, "DefNode")) continue;
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
  int accessor = !strcmp(nm, "attr_accessor") ||
                 !strcmp(nm, "attribute") || !strcmp(nm, "attributes");
  int reader = !strcmp(nm, "attr_reader") || accessor;
  int writer = !strcmp(nm, "attr_writer") || accessor;
  if (!reader && !writer) return;
  int args = nt_ref(nt, s, "arguments");
  int an = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  for (int a = 0; a < an; a++) {
    const char *aty = nt_type(nt, argv[a]);
    if (!aty || strcmp(aty, "SymbolNode")) continue;
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
    if (!strcmp(sty, "CallNode")) {
      register_attr_call(c, cls, s, 0);
    }
    else if (!strcmp(sty, "SingletonClassNode")) {
      /* class << self; attr_accessor :x; end */
      int sbody = nt_ref(nt, s, "body");
      if (sbody < 0) continue;
      int sn = 0;
      const int *sstmts = nt_arr(nt, sbody, "body", &sn);
      for (int j = 0; j < sn; j++) {
        int ss = sstmts[j];
        const char *ssty = nt_type(nt, ss);
        if (ssty && !strcmp(ssty, "CallNode"))
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
    if (!ty || (strcmp(ty, "ClassNode") && strcmp(ty, "ModuleNode"))) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cname) continue;
    int ci = comp_class_index(c, cname);
    if (ci < 0) continue;
    if (id == c->classes[ci].def_node) continue;  /* already handled above */
    register_attrs_body(c, &c->classes[ci], nt_ref(nt, id, "body"));
  }
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
    if (!strcmp(sty, "AliasMethodNode")) {
      int nn = nt_ref(nt, s, "new_name");
      int on = nt_ref(nt, s, "old_name");
      const char *nw = nn >= 0 ? nt_str(nt, nn, "value") : NULL;
      const char *od = on >= 0 ? nt_str(nt, on, "value") : NULL;
      comp_add_alias(cls, nw, od);
    }
    else if (!strcmp(sty, "CallNode")) {
      const char *nm = nt_str(nt, s, "name");
      if (!nm || strcmp(nm, "alias_method")) continue;
      int args = nt_ref(nt, s, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an >= 2 && nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "SymbolNode") &&
          nt_type(nt, argv[1]) && !strcmp(nt_type(nt, argv[1]), "SymbolNode"))
        comp_add_alias(cls, nt_str(nt, argv[0], "value"), nt_str(nt, argv[1], "value"));
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
    if (!ty || (strcmp(ty, "ClassNode") && strcmp(ty, "ModuleNode"))) continue;
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
    if (!sty || strcmp(sty, "UndefNode")) continue;
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
    if (!ty || (strcmp(ty, "ClassNode") && strcmp(ty, "ModuleNode"))) continue;
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
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "AliasGlobalVariableNode")) continue;
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
    if (!strcmp(ty, "GlobalVariableWriteNode") || !strcmp(ty, "GlobalVariableReadNode") ||
        !strcmp(ty, "GlobalVariableOperatorWriteNode") || !strcmp(ty, "GlobalVariableTargetNode") ||
        !strcmp(ty, "GlobalVariableOrWriteNode") || !strcmp(ty, "GlobalVariableAndWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      /* skip alias names - they resolve to the original and need no separate slot */
      if (nm && nm[0] == '$' && is_c_ident(nm + 1) &&
          !strcmp(nm + 1, comp_resolve_gvar(c, nm + 1)))
        comp_gvar_intern(c, nm + 1);
    }
    else if (!strcmp(ty, "AliasGlobalVariableNode")) {
      /* already handled in pass 1 */
    }
    else if (!strcmp(ty, "ConstantTargetNode")) {
      /* target in a multi-write: A, B = expr */
      const char *nm = nt_str(nt, id, "name");
      if (nm && is_c_ident(nm) && comp_class_index(c, nm) < 0)
        comp_const_intern(c, nm);
    }
    else if (!strcmp(ty, "ConstantWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      /* a constant bound to a regex literal is resolved at compile time to a
         precompiled pattern, not stored as a runtime value */
      int rv = nt_ref(nt, id, "value");
      if (rv >= 0 && nt_type(nt, rv) && !strcmp(nt_type(nt, rv), "CallNode") &&
          nt_str(nt, rv, "name") && !strcmp(nt_str(nt, rv, "name"), "freeze"))
        rv = nt_ref(nt, rv, "receiver");
      int is_regex_const = rv >= 0 && nt_type(nt, rv) && !strcmp(nt_type(nt, rv), "RegularExpressionNode");
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
        /* `CONST = SomeClass.new(...)`: reads of CONST during the new()
           (i.e. inside initialize or anything it calls) must raise
           NameError, since CONST is not yet bound. */
        int v = nt_ref(nt, id, "value");
        const char *vty = v >= 0 ? nt_type(nt, v) : NULL;
        if (vty && !strcmp(vty, "CallNode") && nt_str(nt, v, "name") &&
            !strcmp(nt_str(nt, v, "name"), "new")) {
          int vr = nt_ref(nt, v, "receiver");
          if (vr >= 0 && nt_type(nt, vr) && !strcmp(nt_type(nt, vr), "ConstantReadNode") &&
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
  if (!strcmp(ty, "SymbolNode")) return nt_str(nt, nid, "value");
  if (!strcmp(ty, "StringNode")) return nt_str(nt, nid, "content");
  return NULL;
}

/* Extract an integer literal value, or -1. */
int ffi_arg_int(const NodeTable *nt, int nid) {
  if (nid < 0) return -1;
  const char *ty = nt_type(nt, nid);
  if (!ty) return -1;
  if (!strcmp(ty, "IntegerNode")) return (int)nt_int(nt, nid, "value", 0);
  return -1;
}

/* Map an FFI spec string to the Spinel TyKind used for return types. */
TyKind ffi_spec_to_ty(const char *spec) {
  if (!spec) return TY_UNKNOWN;
  if (!strcmp(spec,"int")||!strcmp(spec,"uint32")||!strcmp(spec,"int32")||
      !strcmp(spec,"uint16")||!strcmp(spec,"int16")||!strcmp(spec,"uint8")||
      !strcmp(spec,"size_t")||!strcmp(spec,"long")||!strcmp(spec,"int64"))
    return TY_INT;
  if (!strcmp(spec,"float")||!strcmp(spec,"double")) return TY_FLOAT;
  if (!strcmp(spec,"str")) return TY_STRING;
  if (!strcmp(spec,"bool")) return TY_BOOL;
  if (!strcmp(spec,"ptr")) return TY_POLY;
  if (!strcmp(spec,"void")) return TY_NIL;
  if (!strcmp(spec,"float_array")) return TY_FLOAT_ARRAY;
  if (!strcmp(spec,"int_array")) return TY_INT_ARRAY;
  return TY_UNKNOWN;
}

/* Register a ffi_func / ffi_const / ffi_buffer / ffi_read_* declared in
   module bodies. Called during analyze_program before fixpoint. */
void register_ffi_decls(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "ModuleNode")) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *mname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!mname) continue;
    int body = nt_ref(nt, id, "body");
    int sn = 0;
    const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &sn) : NULL;
    for (int k = 0; k < sn; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty || strcmp(sty, "CallNode")) continue;
      if (nt_ref(nt, s, "receiver") >= 0) continue;
      const char *dname = nt_str(nt, s, "name");
      if (!dname) continue;
      int anode = nt_ref(nt, s, "arguments");
      int an = 0;
      const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;

      if (!strcmp(dname, "ffi_lib")) {
        if (an < 1) continue;
        const char *libname = ffi_arg_str(nt, args[0]);
        if (!libname) continue;
        /* find or create lib entry */
        int mi = -1;
        for (int li = 0; li < c->n_ffi_libs; li++)
          if (!strcmp(c->ffi_lib_mods[li], mname)) { mi = li; break; }
        if (mi < 0) {
          if (c->n_ffi_libs >= c->c_ffi_libs) {
            c->c_ffi_libs = c->c_ffi_libs ? c->c_ffi_libs * 2 : 8;
            c->ffi_lib_mods  = realloc(c->ffi_lib_mods,  sizeof(char*) * (size_t)c->c_ffi_libs);
            c->ffi_lib_names = realloc(c->ffi_lib_names, sizeof(char*) * (size_t)c->c_ffi_libs);
          }
          c->ffi_lib_mods[c->n_ffi_libs]  = strdup(mname);
          c->ffi_lib_names[c->n_ffi_libs] = strdup(libname);
          mi = c->n_ffi_libs++;
        }
        else {
          /* append with semicolon */
          size_t old_len = strlen(c->ffi_lib_names[mi]);
          size_t new_len = old_len + 1 + strlen(libname) + 1;
          char *merged = malloc(new_len);
          snprintf(merged, new_len, "%s;%s", c->ffi_lib_names[mi], libname);
          free(c->ffi_lib_names[mi]);
          c->ffi_lib_names[mi] = merged;
        }
        continue;
      }

      if (!strcmp(dname, "ffi_cflags")) {
        if (an < 1) continue;
        const char *cflag = ffi_arg_str(nt, args[0]);
        if (!cflag) continue;
        /* find or create cflag entry, semicolon-merged per module */
        int mi = -1;
        for (int ci = 0; ci < c->n_ffi_cflags; ci++)
          if (!strcmp(c->ffi_cflag_mods[ci], mname)) { mi = ci; break; }
        if (mi < 0) {
          if (c->n_ffi_cflags >= c->c_ffi_cflags) {
            c->c_ffi_cflags = c->c_ffi_cflags ? c->c_ffi_cflags * 2 : 8;
            c->ffi_cflag_mods = realloc(c->ffi_cflag_mods, sizeof(char*) * (size_t)c->c_ffi_cflags);
            c->ffi_cflag_vals = realloc(c->ffi_cflag_vals, sizeof(char*) * (size_t)c->c_ffi_cflags);
          }
          c->ffi_cflag_mods[c->n_ffi_cflags] = strdup(mname);
          c->ffi_cflag_vals[c->n_ffi_cflags] = strdup(cflag);
          mi = c->n_ffi_cflags++;
        }
        else {
          size_t old_len = strlen(c->ffi_cflag_vals[mi]);
          size_t new_len = old_len + 1 + strlen(cflag) + 1;
          char *merged = malloc(new_len);
          snprintf(merged, new_len, "%s;%s", c->ffi_cflag_vals[mi], cflag);
          free(c->ffi_cflag_vals[mi]);
          c->ffi_cflag_vals[mi] = merged;
        }
        continue;
      }

      if (!strcmp(dname, "ffi_func")) {
        if (an < 3) continue;
        const char *fname = ffi_arg_str(nt, args[0]);
        if (!fname) continue;
        /* arg type array */
        int arr_id = args[1];
        const char *arr_ty = nt_type(nt, arr_id);
        if (!arr_ty || strcmp(arr_ty, "ArrayNode")) continue;
        int en = 0;
        const int *elems = nt_arr(nt, arr_id, "elements", &en);
        char **arg_specs = malloc(sizeof(char*) * (size_t)(en + 1));
        for (int ei = 0; ei < en; ei++) {
          const char *spec = ffi_arg_str(nt, elems[ei]);
          arg_specs[ei] = strdup(spec ? spec : "");
        }
        const char *ret_spec = ffi_arg_str(nt, args[2]);
        if (!ret_spec) { free(arg_specs); continue; }
        /* grow array */
        if (c->n_ffi_funcs >= c->c_ffi_funcs) {
          c->c_ffi_funcs = c->c_ffi_funcs ? c->c_ffi_funcs * 2 : 16;
          c->ffi_func_mods   = realloc(c->ffi_func_mods,   sizeof(char*) * (size_t)c->c_ffi_funcs);
          c->ffi_func_names  = realloc(c->ffi_func_names,  sizeof(char*) * (size_t)c->c_ffi_funcs);
          c->ffi_func_ret    = realloc(c->ffi_func_ret,    sizeof(char*) * (size_t)c->c_ffi_funcs);
          c->ffi_func_args   = realloc(c->ffi_func_args,   sizeof(char**) * (size_t)c->c_ffi_funcs);
          c->ffi_func_nargs  = realloc(c->ffi_func_nargs,  sizeof(int) * (size_t)c->c_ffi_funcs);
        }
        int fi = c->n_ffi_funcs++;
        c->ffi_func_mods[fi]  = strdup(mname);
        c->ffi_func_names[fi] = strdup(fname);
        c->ffi_func_ret[fi]   = strdup(ret_spec);
        c->ffi_func_args[fi]  = arg_specs;
        c->ffi_func_nargs[fi] = en;
        continue;
      }

      if (!strcmp(dname, "ffi_const")) {
        if (an < 2) continue;
        const char *kname = ffi_arg_str(nt, args[0]);
        if (!kname) continue;
        int val = ffi_arg_int(nt, args[1]);
        if (c->n_ffi_consts >= c->c_ffi_consts) {
          c->c_ffi_consts = c->c_ffi_consts ? c->c_ffi_consts * 2 : 16;
          c->ffi_const_mods  = realloc(c->ffi_const_mods,  sizeof(char*) * (size_t)c->c_ffi_consts);
          c->ffi_const_names = realloc(c->ffi_const_names, sizeof(char*) * (size_t)c->c_ffi_consts);
          c->ffi_const_vals  = realloc(c->ffi_const_vals,  sizeof(int) * (size_t)c->c_ffi_consts);
        }
        int ci2 = c->n_ffi_consts++;
        c->ffi_const_mods[ci2]  = strdup(mname);
        c->ffi_const_names[ci2] = strdup(kname);
        c->ffi_const_vals[ci2]  = val;
        continue;
      }

      if (!strcmp(dname, "ffi_buffer")) {
        if (an < 2) continue;
        const char *bname = ffi_arg_str(nt, args[0]);
        if (!bname) continue;
        int bsize = ffi_arg_int(nt, args[1]);
        if (bsize <= 0) continue;
        if (c->n_ffi_bufs >= c->c_ffi_bufs) {
          c->c_ffi_bufs = c->c_ffi_bufs ? c->c_ffi_bufs * 2 : 8;
          c->ffi_buf_mods  = realloc(c->ffi_buf_mods,  sizeof(char*) * (size_t)c->c_ffi_bufs);
          c->ffi_buf_names = realloc(c->ffi_buf_names, sizeof(char*) * (size_t)c->c_ffi_bufs);
          c->ffi_buf_sizes = realloc(c->ffi_buf_sizes, sizeof(int) * (size_t)c->c_ffi_bufs);
        }
        int bi = c->n_ffi_bufs++;
        c->ffi_buf_mods[bi]  = strdup(mname);
        c->ffi_buf_names[bi] = strdup(bname);
        c->ffi_buf_sizes[bi] = bsize;
        continue;
      }

      if (!strncmp(dname, "ffi_read_", 9)) {
        if (an < 2) continue;
        const char *rname = ffi_arg_str(nt, args[0]);
        if (!rname) continue;
        int roff = ffi_arg_int(nt, args[1]);
        if (roff < 0) roff = 0;
        const char *kind = dname + 9;  /* "u32", "i32", "ptr" */
        if (c->n_ffi_readers >= c->c_ffi_readers) {
          c->c_ffi_readers = c->c_ffi_readers ? c->c_ffi_readers * 2 : 8;
          c->ffi_reader_mods    = realloc(c->ffi_reader_mods,    sizeof(char*) * (size_t)c->c_ffi_readers);
          c->ffi_reader_names   = realloc(c->ffi_reader_names,   sizeof(char*) * (size_t)c->c_ffi_readers);
          c->ffi_reader_offsets = realloc(c->ffi_reader_offsets, sizeof(int) * (size_t)c->c_ffi_readers);
          c->ffi_reader_kinds   = realloc(c->ffi_reader_kinds,   sizeof(char*) * (size_t)c->c_ffi_readers);
        }
        int ri = c->n_ffi_readers++;
        c->ffi_reader_mods[ri]    = strdup(mname);
        c->ffi_reader_names[ri]   = strdup(rname);
        c->ffi_reader_offsets[ri] = roff;
        c->ffi_reader_kinds[ri]   = strdup(kind);
        continue;
      }
    }
  }
}

/* Look up an FFI func by (module, name). Returns index or -1. */
int ffi_find_func(Compiler *c, const char *mod, const char *name) {
  for (int i = 0; i < c->n_ffi_funcs; i++)
    if (!strcmp(c->ffi_func_mods[i], mod) && !strcmp(c->ffi_func_names[i], name))
      return i;
  return -1;
}

/* Look up an FFI buffer by (module, name). Returns index or -1. */
int ffi_find_buf(Compiler *c, const char *mod, const char *name) {
  for (int i = 0; i < c->n_ffi_bufs; i++)
    if (!strcmp(c->ffi_buf_mods[i], mod) && !strcmp(c->ffi_buf_names[i], name))
      return i;
  return -1;
}

/* Look up an FFI reader by (module, name). Returns index or -1. */
int ffi_find_reader(Compiler *c, const char *mod, const char *name) {
  for (int i = 0; i < c->n_ffi_readers; i++)
    if (!strcmp(c->ffi_reader_mods[i], mod) && !strcmp(c->ffi_reader_names[i], name))
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
    if (!strcmp(ty, "GlobalVariableWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
      if (rn) lv = comp_gvar(c, rn);
      vt = infer_type(c, nt_ref(nt, id, "value"));
      if (vt == TY_NIL) continue;
    }
    else if (!strcmp(ty, "GlobalVariableOperatorWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
      if (rn) lv = comp_gvar(c, rn);
      TyKind cur = lv ? lv->type : TY_UNKNOWN;
      TyKind v = infer_type(c, nt_ref(nt, id, "value"));
      if (cur == TY_STRING) vt = TY_STRING;
      else if (ty_is_numeric(cur) && ty_is_numeric(v)) vt = (cur == TY_FLOAT || v == TY_FLOAT) ? TY_FLOAT : TY_INT;
      else vt = cur;
    }
    else if (!strcmp(ty, "GlobalVariableOrWriteNode") || !strcmp(ty, "GlobalVariableAndWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
      if (rn) lv = comp_gvar(c, rn);
      vt = infer_type(c, nt_ref(nt, id, "value"));
      if (vt == TY_NIL) continue;
    }
    else if (!strcmp(ty, "ConstantWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm) lv = comp_const(c, nm);
      vt = infer_type(c, nt_ref(nt, id, "value"));
    }
    else if (!strcmp(ty, "MultiWriteNode")) {
      int ln = 0;
      const int *lefts = nt_arr(nt, id, "lefts", &ln);
      int value = nt_ref(nt, id, "value");
      const char *vty = nt_type(nt, value);
      int en = 0;
      const int *els = (vty && !strcmp(vty, "ArrayNode")) ? nt_arr(nt, value, "elements", &en) : NULL;
      int rn_count = 0;
      nt_arr(nt, id, "rights", &rn_count);
      for (int i = 0; i < ln; i++) {
        const char *lty2 = nt_type(nt, lefts[i]);
        if (!lty2 || strcmp(lty2, "GlobalVariableTargetNode")) continue;
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
        int rest_inner2 = (rsty2 && !strcmp(rsty2, "SplatNode")) ? nt_ref(nt, rest_nid2, "expression") : -1;
        const char *rinty2 = rest_inner2 >= 0 ? nt_type(nt, rest_inner2) : NULL;
        if (rinty2 && !strcmp(rinty2, "GlobalVariableTargetNode")) {
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
    else if (!strcmp(ty, "CallNode")) {
      /* CONST << v / CONST.push(v) / CONST.append(v): infer CONST as an
         array whose element type comes from v's type. Only applies when
         the receiver is a direct ConstantReadNode. */
      const char *cnm = nt_str(nt, id, "name");
      if (!cnm) continue;
      int is_push = (!strcmp(cnm, "<<") || !strcmp(cnm, "push") || !strcmp(cnm, "append"));
      if (!is_push) continue;
      int crecv = nt_ref(nt, id, "receiver");
      if (crecv < 0) continue;
      const char *rty = nt_type(nt, crecv);
      if (!rty || strcmp(rty, "ConstantReadNode")) continue;
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
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "MultiWriteNode")) continue;
    int value = nt_ref(nt, id, "value");
    if (value < 0) continue;
    const char *vty = nt_type(nt, value);
    if (vty && !strcmp(vty, "ArrayNode")) continue; /* literal handled in infer_write_types */
    TyKind st = infer_type(c, value);
    if (!ty_is_array(st)) continue;
    TyKind elem = ty_array_elem(st);
    if (elem == TY_POLY || elem == TY_UNKNOWN) continue; /* not yet settled */
    int ln = 0;
    const int *lefts = nt_arr(nt, id, "lefts", &ln);
    for (int i = 0; i < ln; i++) {
      const char *lty = nt_type(nt, lefts[i]) ? nt_type(nt, lefts[i]) : "";
      if (strcmp(lty, "ConstantTargetNode")) continue;
      const char *nm = nt_str(nt, lefts[i], "name");
      LocalVar *cv = nm ? comp_const(c, nm) : NULL;
      if (!cv || cv->type == elem) continue;
      cv->type = elem; changed = 1;
    }
    int rn = 0;
    const int *rights = nt_arr(nt, id, "rights", &rn);
    for (int j = 0; j < rn; j++) {
      const char *rty2 = nt_type(nt, rights[j]) ? nt_type(nt, rights[j]) : "";
      if (strcmp(rty2, "ConstantTargetNode")) continue;
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
    if (sty && (!strcmp(sty, "ConstantReadNode") || !strcmp(sty, "ConstantPathNode"))) {
      int p = comp_class_index(c, nt_str(nt, sc, "name"));
      if (p >= 0 && p != i) c->classes[i].parent = p;
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
    if (!sty || strcmp(sty, "CallNode")) continue;
    const char *nm = nt_str(nt, s, "name");
    if (!nm || strcmp(nm, "include")) continue;
    if (nt_ref(nt, s, "receiver") >= 0) continue;
    int anode = nt_ref(nt, s, "arguments");
    int an = 0;
    const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
    for (int j = 0; j < an; j++) {
      const char *aty = nt_type(nt, args[j]);
      const char *mname = NULL;
      if (aty && !strcmp(aty, "ConstantReadNode")) mname = nt_str(nt, args[j], "name");
      else if (aty && !strcmp(aty, "ConstantPathNode")) mname = nt_str(nt, args[j], "name");
      int mod_id = mname ? comp_class_index(c, mname) : -1;
      if (mod_id < 0) continue;
      /* snapshot count before adding new scopes to avoid re-scanning them */
      int snap = c->nscopes;
      for (int ms = 0; ms < snap; ms++) {
        Scope *src = &c->scopes[ms];
        if (src->class_id != mod_id || src->is_cmethod || !src->name) continue;
        if (comp_method_in_class(c, ci, src->name) >= 0) continue;
        /* Create a new scope sharing the same AST nodes but owned by ci. */
        Scope *dst = comp_scope_new(c, src->name, src->def_node);
        int dst_idx = c->nscopes - 1;
        /* comp_scope_new may realloc c->scopes; re-derive src pointer. */
        src = &c->scopes[ms];
        /* When the target is a built-in class, `self` has a different (scalar)
           type than the module's object self, so clone the body and re-attribute
           it to the target -- otherwise the shared SelfNode resolves to the
           module and e.g. `self.to_s` mis-dispatches. */
        if (is_builtin_class_name(c->classes[ci].name) && src->body >= 0) {
          int nb = nt_clone_subtree((NodeTable *)nt, src->body);
          if (nb >= 0) {
            comp_grow_node_arrays(c);
            src = &c->scopes[ms]; dst = &c->scopes[dst_idx];
            dst->body = nb;
            walk_scope(c, nb, dst_idx, ci);
            g_inc_did_clone = 1;
          } else dst->body = src->body;
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
          if (!strcmp(bty, "InstanceVariableWriteNode") ||
              !strcmp(bty, "InstanceVariableReadNode") ||
              !strcmp(bty, "InstanceVariableOperatorWriteNode") ||
              !strcmp(bty, "InstanceVariableOrWriteNode")) {
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
    if (!ty || (strcmp(ty, "ClassNode") && strcmp(ty, "ModuleNode"))) continue;
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
      if (!sty || strcmp(sty, "CallNode")) continue;
      const char *nm = nt_str(nt, s, "name");
      if (!nm || strcmp(nm, "extend")) continue;
      if (nt_ref(nt, s, "receiver") >= 0) continue;
      int anode = nt_ref(nt, s, "arguments");
      int an = 0;
      const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
      for (int j = 0; j < an; j++) {
        const char *aty = nt_type(nt, args[j]);
        const char *mname = NULL;
        if (aty && !strcmp(aty, "ConstantReadNode")) mname = nt_str(nt, args[j], "name");
        else if (aty && !strcmp(aty, "ConstantPathNode")) mname = nt_str(nt, args[j], "name");
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
    if (ty && !strcmp(ty, "CallNode") && nt_ref(nt, id, "receiver") < 0 &&
        nt_str(nt, id, "name") && !strcmp(nt_str(nt, id, "name"), "new"))
      return 1;
  }
  return 0;
}

/* Does the inherited cls method `mi` (defined on def_cls) contain a bare call
   that would resolve differently when run as a class method of `ci`? That is:
   a bare `new` (constructs ci), or a bare cmethod call `m` that ci defines but
   def_cls does not (so the inherited body's `m` means ci's version). */
int cmethod_needs_specialization(Compiler *c, int mi, int ci, int def_cls, int *has_new) {
  const NodeTable *nt = c->nt;
  int need = 0;
  if (has_new) *has_new = 0;
  for (int id = 0; id < nt->count; id++) {
    if (c->nscope[id] != mi) continue;
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;   /* receiverless only */
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    if (!strcmp(nm, "new")) { if (has_new) *has_new = 1; need = 1; continue; }
    /* a bare cmethod call that resolves to a different method for ci than for
       the defining class (ci adds or overrides it) */
    int mci = comp_cmethod_in_chain(c, ci, nm, NULL);
    int mdef = comp_cmethod_in_chain(c, def_cls, nm, NULL);
    if (mci >= 0 && mci != mdef) need = 1;
  }
  return need;
}

int is_descendant(Compiler *c, int k, int anc);  /* codegen_fold.c */

/* `Subclass.create` where `create` is an inherited class method whose body
   does `new(...)`: Ruby's bare `new` constructs the *calling* class, so copy
   the inherited cls method into each calling subclass (the copy's class_id
   makes codegen's `new` resolve to that subclass). The defining-class source
   is DCE'd unless it is itself called directly. Covers #224 / #229. */
void specialize_inherited_cls_new(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int snap = c->nscopes;
  int node_count = nt->count;   /* don't scan nodes appended by cloning */
  int did_clone = 0;
  for (int id = 0; id < node_count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || (strcmp(rty, "ConstantReadNode") && strcmp(rty, "ConstantPathNode"))) continue;
    const char *cn = nt_str(nt, recv, "name");
    int ci = cn ? comp_class_index(c, cn) : -1;
    if (ci < 0) continue;
    const char *mname = nt_str(nt, id, "name");
    if (!mname || !strcmp(mname, "new")) continue;
    if (comp_cmethod_in_class(c, ci, mname) >= 0) continue;  /* defined on ci */
    int def_cls = -1;
    int mi = comp_cmethod_in_chain(c, ci, mname, &def_cls);
    if (mi < 0 || def_cls == ci || mi >= snap) continue;     /* not inherited */
    int has_new = 0;
    if (!cmethod_needs_specialization(c, mi, ci, def_cls, &has_new)) continue;

    /* Copy the inherited cls method scope, owned by ci, with its OWN clone of
       the body AST so locals/dispatches inside resolve against ci (the bare
       `new` constructs ci, and `instance = new` is typed obj_ci). */
    int src_body = c->scopes[mi].body;
    int new_body = src_body >= 0 ? nt_clone_subtree(nt, src_body) : -1;
    if (src_body >= 0 && new_body < 0) continue;  /* clone failed: skip */
    comp_grow_node_arrays(c);
    did_clone = 1;
    Scope *src = &c->scopes[mi];
    Scope *dst = comp_scope_new(c, src->name, src->def_node);
    src = &c->scopes[mi];  /* realloc-safe */
    int dst_idx = c->nscopes - 1;
    dst->body = new_body;
    /* attribute the cloned subtree to the new scope */
    if (new_body >= 0) walk_scope(c, new_body, dst_idx, ci);
    dst->class_id = ci;
    dst->is_cmethod = 1;
    dst->yields = src->yields;
    dst->nrequired = src->nrequired;
    dst->rest_idx = src->rest_idx;
    dst->kwrest_idx = src->kwrest_idx;
    /* A bare-`new` create method returns the specialized subclass instance, so
       pin its return type. Other specializations (a body calling a subclass-
       specific class method) let the normal return inference compute the type
       from the cloned, ci-attributed body. */
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
  }
  /* DCE the now-shadowed source cls methods that are never called on their
     own defining class. */
  for (int s = 0; s < snap; s++) {
    Scope *src = &c->scopes[s];
    if (!src->is_cmethod || !src->name || src->class_id < 0) continue;
    /* did we specialize this one into a subclass? A specialization clones the
       inherited cmethod into a *subclass*, so the appended copy's class_id is a
       proper descendant of src's defining class. Match on name AND lineage: a
       same-named cmethod in an unrelated class must NOT count. Without the
       lineage test, e.g. `Base.find` specialized into `Article`/`Comment` also
       flags the unrelated `SqliteAdapter.find` (same name "find"); that source
       is then DCE'd while its devirtualized call site survives, leaving a call
       to an undefined function (#1314). */
    int specialized = 0;
    for (int d = snap; d < c->nscopes && !specialized; d++) {
      if (!c->scopes[d].is_cmethod || !c->scopes[d].name ||
          strcmp(c->scopes[d].name, src->name) != 0) continue;
      int dcls = c->scopes[d].class_id;
      if (dcls != src->class_id && is_descendant(c, dcls, src->class_id))
        specialized = 1;
    }
    if (!specialized) continue;
    /* keep it if called directly as <DefiningClass>.<name> */
    int called_direct = 0;
    for (int id = 0; id < nt->count && !called_direct; id++) {
      if (!nt_type(nt, id) || strcmp(nt_type(nt, id), "CallNode")) continue;
      if (!nt_str(nt, id, "name") || strcmp(nt_str(nt, id, "name"), src->name)) continue;
      int r = nt_ref(nt, id, "receiver");
      if (r < 0 || !nt_type(nt, r)) continue;
      if (strcmp(nt_type(nt, r), "ConstantReadNode") && strcmp(nt_type(nt, r), "ConstantPathNode")) continue;
      if (comp_class_index(c, nt_str(nt, r, "name")) == src->class_id) called_direct = 1;
    }
    if (!called_direct) src->is_transplanted_source = 1;
  }
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
      if (!sty || strcmp(sty, "CallNode")) continue;
      const char *nm = nt_str(nt, s, "name");
      if (!nm || strcmp(nm, "prepend")) continue;
      if (nt_ref(nt, s, "receiver") >= 0) continue;
      int anode = nt_ref(nt, s, "arguments");
      int an = 0;
      const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
      for (int j = 0; j < an; j++) {
        const char *aty = nt_type(nt, args[j]);
        const char *mname = NULL;
        if (aty && !strcmp(aty, "ConstantReadNode")) mname = nt_str(nt, args[j], "name");
        else if (aty && !strcmp(aty, "ConstantPathNode")) mname = nt_str(nt, args[j], "name");
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
              if (!strcmp(cif->prep_from[kk], method_name)) {
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
      if (!strcmp(sty, "ClassVariableWriteNode")) {
        const char *nm = nt_str(nt, s, "name");
        if (!nm) continue;
        int idx = comp_cvar_intern(&c->classes[ci], nm);
        TyKind vt = infer_type(c, nt_ref(nt, s, "value"));
        if (vt == TY_NIL) continue;
        TyKind merged = ty_unify(c->classes[ci].cvar_types[idx], vt);
        if (merged != c->classes[ci].cvar_types[idx]) { c->classes[ci].cvar_types[idx] = merged; changed = 1; }
      }
      else if (!strcmp(sty, "MultiWriteNode")) {
        int mln = 0;
        const int *mlefts = nt_arr(nt, s, "lefts", &mln);
        int mval = nt_ref(nt, s, "value");
        const char *mvty = nt_type(nt, mval);
        int men = 0;
        const int *mels = (mvty && !strcmp(mvty, "ArrayNode")) ? nt_arr(nt, mval, "elements", &men) : NULL;
        for (int mi = 0; mi < mln; mi++) {
          const char *mlty = nt_type(nt, mlefts[mi]);
          if (!mlty || strcmp(mlty, "ClassVariableTargetNode")) continue;
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
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "ClassVariableWriteNode")) continue;
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
  /* Pass 3: top-level writes (class_id == -1 in scope 0) -- use Toplevel pseudo-class. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "ClassVariableWriteNode")) continue;
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

int infer_ivar_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (!strcmp(ty, "InstanceVariableWriteNode") ||
        !strcmp(ty, "InstanceVariableOrWriteNode") ||
        !strcmp(ty, "InstanceVariableAndWriteNode") ||
        !strcmp(ty, "InstanceVariableOperatorWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
      if (vt == TY_NIL) continue;  /* nil write doesn't pin the ivar type */
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
      if (!strcmp(ty, "InstanceVariableOperatorWriteNode") && ty_is_object(ci->ivar_types[iv])) {
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
        for (int si = 0; si < c->nscopes; si++) {
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
    else if (!strcmp(ty, "CallNode")) {
      /* attr-writer assignment: obj.x = v  (CallNode "x=") */
      const char *nm = nt_str(nt, id, "name");
      int recv = nt_ref(nt, id, "receiver");
      size_t ln = nm ? strlen(nm) : 0;
      if (!nm || recv < 0 || ln < 2 || nm[ln - 1] != '=') continue;
      TyKind rt = infer_type(c, recv);
      if (!ty_is_object(rt)) continue;
      ClassInfo *ci = &c->classes[ty_object_class(rt)];
      char base[256];
      if (ln - 1 >= sizeof base) continue;
      memcpy(base, nm, ln - 1); base[ln - 1] = '\0';
      if (!comp_is_writer(ci, base)) continue;
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 1) continue;
      char ivname[256];
      snprintf(ivname, sizeof ivname, "@%s", base);
      int iv = comp_ivar_index(ci, ivname);
      if (iv < 0 || class_ivar_pinned(ci, ivname)) continue;
      TyKind vt = infer_type(c, argv[0]);
      TyKind merged = ty_unify(ci->ivar_types[iv], vt);
      if (merged != ci->ivar_types[iv]) { ci->ivar_types[iv] = merged; changed = 1; }
    }
  }
  return changed;
}

/* ---- fixpoint passes ---- */

