/* Compile-time evaluation over the node table (ct_eval.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ct_eval.h"
#include "builtin_names.h"

static void *ct_grow(void *p, int *cap, int need, size_t elem) {
  if (need <= *cap) return p;
  int nc = *cap ? *cap * 2 : 8;
  while (nc < need) nc *= 2;
  void *r = realloc(p, elem * (size_t)nc);
  if (!r) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  *cap = nc;
  return r;
}

static char *ct_strdup(const char *s) {
  char *r = strdup(s);
  if (!r) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  return r;
}

void ct_env_ran(CtEnv *e, const char *cls, const char *meth) {
  for (int i = 0; i < e->nran; i++)
    if (strcmp(e->ran[i].cls, cls) == 0 && strcmp(e->ran[i].meth, meth) == 0) return;
  e->ran = ct_grow(e->ran, &e->cran, e->nran + 1, sizeof *e->ran);
  e->ran[e->nran].cls = ct_strdup(cls);
  e->ran[e->nran].meth = ct_strdup(meth);
  e->nran++;
}

int ct_same_string(const CtVal *a, const CtVal *b) {
  return a->k == CT_VALUE && b->k == CT_VALUE && a->mv.k == MV_STR && b->mv.k == MV_STR && a->mv.s == b->mv.s;
}

void ct_env_relied(CtEnv *e, const char *meth) {
  for (int i = 0; i < e->nrelied; i++)
    if (e->relied[i].after == e->nfx && strcmp(e->relied[i].name, meth) == 0) return;
  e->relied = ct_grow(e->relied, &e->crelied, e->nrelied + 1, sizeof *e->relied);
  e->relied[e->nrelied].name = ct_strdup(meth);
  e->relied[e->nrelied].after = e->nfx;
  e->nrelied++;
}

static CtFx *ct_fx(CtEnv *e, CtFxKind k, int site) {
  e->fx = ct_grow(e->fx, &e->cfx, e->nfx + 1, sizeof *e->fx);
  CtFx *f = &e->fx[e->nfx++];
  memset(f, 0, sizeof *f);
  f->k = k; f->site = site; f->cls = -1; f->frame = e->frame;
  return f;
}

static void ct_fx_clear(CtFx *f) {
  for (int i = 0; i < f->nfold; i++) free(f->foldv[i]);
  free(f->fold); free(f->foldv);
  free(f->name);
  for (int i = 0; i < f->nattrs; i++) free(f->attrs[i]);
  free(f->attrs);
  free(f->snap);
}

/* f's statement runs where it stands: the locals it reads then */
static void ct_fx_snap(const CtEnv *e, CtFx *f) {
  f->snap = malloc(sizeof *f->snap * (size_t)(e->nloc + 1));
  if (!f->snap) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(f->snap, e->loc, sizeof *f->snap * (size_t)e->nloc);
  f->nsnap = e->nloc;
}

static CtLocal *ct_local(CtEnv *e, const char *name) {
  for (int i = 0; i < e->nloc; i++) if (strcmp(e->loc[i].name, name) == 0) return &e->loc[i];
  return NULL;
}

static CtVal *ct_lookup(CtEnv *e, const char *name) {
  CtLocal *l = ct_local(e, name);
  return l ? &l->v : NULL;
}

const CtLocal *ct_frame_local(const CtEnv *e, int frame, const char *name) {
  if (!name || frame < 0 || frame >= e->nframes) return NULL;
  const CtFrame *f = &e->frames[frame];
  for (int i = 0; i < f->nloc; i++) if (strcmp(f->loc[i].name, name) == 0) return &f->loc[i];
  return NULL;
}

/* local name = v, written by statement wr (-1: bound as a parameter) */
static int ct_env_write(CtEnv *e, const char *name, CtVal v, int wr) {
  CtLocal *o = ct_local(e, name);
  if (!o) {
    if (e->nloc >= 64) return 0;
    o = &e->loc[e->nloc++];
    memset(o, 0, sizeof *o);
    o->name = ct_strdup(name);
  }
  o->v = v;
  o->wr = wr;
  o->nw += wr >= 0;
  return 1;
}

int ct_env_set(CtEnv *e, const char *name, CtVal v) { return ct_env_write(e, name, v, -1); }

void ct_env_new_call(CtEnv *e) {
  for (int i = 0; i < e->nloc; i++) free(e->loc[i].name);
  e->nloc = 0;
}

static CtLocal *ct_ivar(CtEnv *e, const char *name) {
  for (int i = 0; name && i < e->nivar; i++) if (strcmp(e->ivar[i].name, name) == 0) return &e->ivar[i];
  return NULL;
}

void ct_env_ivar_set(CtEnv *e, const char *name, CtVal v) {
  CtLocal *l = ct_ivar(e, name);
  if (!l) {
    e->ivar = ct_grow(e->ivar, &e->civar, e->nivar + 1, sizeof *e->ivar);
    l = &e->ivar[e->nivar++];
    memset(l, 0, sizeof *l);
    l->name = ct_strdup(name);
    l->wr = -1;
  }
  l->v = v;
  l->nw++;
}

void ct_env_free(CtEnv *e) {
  for (int i = 0; i < e->nivar; i++) free(e->ivar[i].name);
  free(e->ivar);
  ct_env_new_call(e);
  for (int i = 0; i < e->ncls; i++) free(e->cls[i].name);
  for (int i = 0; i < e->nfx; i++) ct_fx_clear(&e->fx[i]);
  for (int i = 0; i < e->nran; i++) { free((char *)e->ran[i].cls); free((char *)e->ran[i].meth); }
  for (int i = 0; i < e->nrelied; i++) free(e->relied[i].name);
  for (int i = 0; i < e->nframes; i++) {
    for (int j = 0; j < e->frames[i].nloc; j++) free(e->frames[i].loc[j].name);
    free(e->frames[i].loc);
  }
  free(e->cls); free(e->fx); free(e->ran); free(e->relied); free(e->frames);
  memset(e, 0, sizeof *e);
}

/* the core class whose methods a value's calls run */
static const char *ct_class_of(Mv v) {
  switch (v.k) {
  case MV_NIL: return "NilClass";
  case MV_TRUE: return "TrueClass";
  case MV_FALSE: return "FalseClass";
  case MV_INT: return "Integer";
  case MV_STR: return "String";
  case MV_SYM: return "Symbol";
  case MV_ARR: return "Array";
  default: return NULL;
  }
}

/* a plain value (not a constant or a class) */
static int ct_value(const NodeTable *nt, CtEnv *e, int node, Mv *out) {
  CtVal v;
  if (!ct_eval(nt, e, node, &v) || v.k != CT_VALUE) return 0;
  *out = v.mv;
  return 1;
}

/* the arguments of call `call`, when every one is a plain positional one */
static const int *ct_args(const NodeTable *nt, int call, int *argc) {
  int an = nt_ref(nt, call, "arguments");
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", argc) : NULL;
  if (!av) { *argc = 0; return NULL; }
  for (int i = 0; i < *argc; i++) {
    NodeKind k = nt_kind(nt, av[i]);
    if (k == NK_SplatNode || k == NK_KeywordHashNode || k == NK_BlockArgumentNode) { *argc = -1; return NULL; }
  }
  return av;
}

/* the one statement of a StatementsNode, or -1 */
static int ct_single(const NodeTable *nt, int stmts) {
  int n = 0;
  const int *b = stmts >= 0 && nt_kind(nt, stmts) == NK_StatementsNode ? nt_arr(nt, stmts, "body", &n) : NULL;
  return b && n == 1 ? b[0] : -1;
}

/* the pattern argument of gsub / sub: a regexp literal with no options, or
   an ASCII String */
static int ct_pattern(const NodeTable *nt, CtEnv *e, int node, MvPat *pt) {
  memset(pt, 0, sizeof *pt);
  if (nt_kind(nt, node) == NK_RegularExpressionNode) {
    if (nt_regexp_opts(nt, node) & (NT_RE_IGNORE_CASE | NT_RE_EXTENDED | NT_RE_MULTI_LINE | NT_RE_ONCE)) return 0;
    const char *src = nt_str(nt, node, "unescaped");
    return src && mv_pat_regexp(src, src + strlen(src), pt);
  }
  Mv v;
  if (!ct_value(nt, e, node, &v) || v.k != MV_STR || !mv_ascii(v.s) || !v.s[0]) return 0;
  pt->lit = 1; pt->text = v.s; pt->tn = strlen(v.s);
  return 1;
}

/* the text of a String or Symbol's parts */
static int ct_parts(const NodeTable *nt, CtEnv *e, int node, MvBuf *b) {
  int pn = 0;
  const int *parts = nt_arr(nt, node, "parts", &pn);
  for (int i = 0; i < pn; i++) {
    NodeKind k = nt_kind(nt, parts[i]);
    if (k == NK_StringNode) { const char *t = nt_str(nt, parts[i], "content"); mvb_puts(b, t ? t : ""); }
    else if (k == NK_EmbeddedStatementsNode) {
      Mv v;
      int st = ct_single(nt, nt_ref(nt, parts[i], "statements"));
      if (st < 0 || !ct_value(nt, e, st, &v)) return 0;
      if (v.k == MV_ARR) {
        if (!mv_inspect_exact(v)) return 0;
        ct_env_ran(e, "Array", "inspect");
        mv_inspect(v, b);
      }
      else {
        if (ct_class_of(v)) ct_env_ran(e, ct_class_of(v), "to_s");
        mv_to_s(v, b);
      }
    }
    else if (k == NK_InterpolatedStringNode) { if (!ct_parts(nt, e, parts[i], b)) return 0; }
    else return 0;
  }
  return 1;
}

/* is interpolated String `node` adjacent literals only ("a" "b") */
static int ct_literal_fold(const NodeTable *nt, int node) {
  int pn = 0;
  const int *parts = nt_arr(nt, node, "parts", &pn);
  for (int i = 0; i < pn; i++)
    if (nt_kind(nt, parts[i]) != NK_StringNode &&
        (nt_kind(nt, parts[i]) != NK_InterpolatedStringNode || !ct_literal_fold(nt, parts[i]))) return 0;
  return 1;
}

/* `Class.new(Constant)` without a block: a class of the evaluation's own */
static int ct_class_new(const NodeTable *nt, CtEnv *e, int call, CtVal *out) {
  int rv = nt_ref(nt, call, "receiver"), argc = 0;
  const int *av = ct_args(nt, call, &argc);
  if (rv < 0 || nt_kind(nt, rv) != NK_ConstantReadNode || !is_class_const_name(nt_str(nt, rv, "name")) ||
      nt_ref(nt, call, "block") >= 0 || argc != 1) return 0;
  CtVal pv;
  if (!ct_eval(nt, e, av[0], &pv) || pv.k != CT_CONST) return 0;
  ct_env_ran(e, "Class", "new");
  e->cls = ct_grow(e->cls, &e->ccls, e->ncls + 1, sizeof *e->cls);
  e->cls[e->ncls].parent = pv.node;
  e->cls[e->ncls].name = NULL;
  memset(out, 0, sizeof *out);
  out->k = CT_CLASS; out->cls = e->ncls++;
  CtFx *f = ct_fx(e, CT_FX_CLASS_NEW, call);
  f->cls = out->cls;
  return 1;
}

/* is call `call` on self (no receiver, or `self`), a core method of it the
   frame lets run: macro mode only */
static int ct_self_core(const NodeTable *nt, CtEnv *e, int call, const char *name) {
  int rv = nt_ref(nt, call, "receiver");
  if (!(e->self_name && name && (rv < 0 || nt_kind(nt, rv) == NK_SelfNode) && e->self_core(e->host, name))) return 0;
  ct_env_relied(e, name);
  return 1;
}

static int ct_run_macro_as(const NodeTable *nt, CtEnv *e, int call, int def, CtVal *value);

/* A macro on self called for its value (`kind` reading what `kind :box`
   kept): its frame runs, and must have no effect but its own locals'
   writes, which no statement of the call carries */
static int ct_macro_value(const NodeTable *nt, CtEnv *e, int call, int def, CtVal *out) {
  int nfx = e->nfx, ok = ct_run_macro_as(nt, e, call, def, out);
  for (int i = nfx; ok && i < e->nfx; i++) ok = e->fx[i].k == CT_FX_LOCAL;
  while (e->nfx > nfx) ct_fx_clear(&e->fx[--e->nfx]);
  return ok;
}

static int ct_eval_call(const NodeTable *nt, CtEnv *e, int call, CtVal *out) {
  const char *name = nt_str(nt, call, "name");
  int rv = nt_ref(nt, call, "receiver"), argc = 0;
  const int *av = ct_args(nt, call, &argc);
  if (e->self_macro && name && nt_ref(nt, call, "block") < 0 && (rv < 0 || nt_kind(nt, rv) == NK_SelfNode)) {
    int def = e->self_macro(e->host, name);
    if (def >= 0) {
      ct_env_relied(e, name);
      return ct_macro_value(nt, e, call, def, out);
    }
  }
  /* the frame's class's name, which Module#name answers frozen */
  if (name && argc == 0 && nt_ref(nt, call, "block") < 0 && is_name_reader(name) && ct_self_core(nt, e, call, name)) {
    memset(out, 0, sizeof *out);
    out->mv = mv_str(e->self_name, strlen(e->self_name), MV_STR);
    out->mv.frozen = is_class_name_name(name);
    return 1;
  }
  if (!name || rv < 0 || nt_kind(nt, rv) == NK_SelfNode || argc < 0 || nt_ref(nt, call, "block") >= 0) return 0;
  if (is_new_name(name)) return ct_class_new(nt, e, call, out);
  Mv r, a0;
  memset(out, 0, sizeof *out);
  if (!ct_value(nt, e, rv, &r)) return 0;
  if (argc == 2 && is_substitution(name) && r.k == MV_STR && mv_ascii(r.s)) {
    MvPat pt; Mv rep;
    if (!ct_pattern(nt, e, av[0], &pt) || !ct_value(nt, e, av[1], &rep) || rep.k != MV_STR) return 0;
    ct_env_ran(e, "String", name);
    return mv_sub(r, &pt, rep.s, name[0] == 'g', &out->mv);
  }
  memset(&a0, 0, sizeof a0);
  if (argc > 1 || (argc == 1 && !ct_value(nt, e, av[0], &a0))) return 0;
  if (!mv_call(name, r, argc, a0, &out->mv)) return 0;
  if (ct_class_of(r)) ct_env_ran(e, ct_class_of(r), name);
  return 1;
}

int ct_eval(const NodeTable *nt, CtEnv *e, int node, CtVal *out) {
  memset(out, 0, sizeof *out);
  if (node < 0) { out->mv = mv_nil(); return 1; }
  switch (nt_kind(nt, node)) {
  case NK_NilNode: out->mv = mv_nil(); return 1;
  case NK_TrueNode: out->mv = mv_nil(); out->mv.k = MV_TRUE; return 1;
  case NK_FalseNode: out->mv = mv_nil(); out->mv.k = MV_FALSE; return 1;
  case NK_IntegerNode:
    if (nt_str(nt, node, "bigval")) return 0;   /* a Bignum */
    out->mv = mv_nil(); out->mv.k = MV_INT; out->mv.i = nt_int(nt, node, "value", 0);
    return 1;
  case NK_StringNode: case NK_SymbolNode: {
    int str = nt_kind(nt, node) == NK_StringNode;
    const char *t = nt_str(nt, node, str ? "content" : "value");
    if (!t) return 0;
    out->mv = mv_str(t, strlen(t), str ? MV_STR : MV_SYM);
    /* a frozen literal is the literal of its bytes; an unfrozen one (the
       file's pragma off) a new String each time it runs */
    out->mv.frozen = out->mv.lit = str && nt_int(nt, node, "fzl", 0) != 0;
    return 1;
  }
  case NK_InterpolatedStringNode: case NK_InterpolatedSymbolNode: {
    MvBuf b = {0};
    int ok = ct_parts(nt, e, node, &b);
    if (ok) out->mv = mv_str(b.p ? b.p : "", b.len, nt_kind(nt, node) == NK_InterpolatedStringNode ? MV_STR : MV_SYM);
    free(b.p);
    /* adjacent literals ("a" "b") are one literal, frozen as a literal is */
    if (ok && nt_kind(nt, node) == NK_InterpolatedStringNode && ct_literal_fold(nt, node))
      out->mv.frozen = out->mv.lit = nt_int(nt, node, "fzl", 0) != 0;
    return ok;
  }
  case NK_ArrayNode: {
    int en = 0;
    const int *el = nt_arr(nt, node, "elements", &en);
    out->mv = mv_nil(); out->mv.k = MV_ARR; out->mv.n = en;
    out->mv.a = calloc((size_t)en + 1, sizeof(Mv));
    if (!out->mv.a) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < en; i++)
      if (nt_kind(nt, el[i]) == NK_SplatNode || !ct_value(nt, e, el[i], &out->mv.a[i])) return 0;
    return 1;
  }
  case NK_ConstantReadNode: case NK_ConstantPathNode:
    out->k = CT_CONST; out->node = node;
    return 1;
  case NK_LocalVariableReadNode: {
    const char *nm = nt_str(nt, node, "name");
    CtVal *v = nm && nt_int(nt, node, "depth", 0) == 0 ? ct_lookup(e, nm) : NULL;
    if (!v) return 0;
    *out = *v;
    return 1;
  }
  case NK_ParenthesesNode: {
    int b = nt_ref(nt, node, "body");
    if (b >= 0 && nt_kind(nt, b) == NK_StatementsNode) b = ct_single(nt, b);
    return b >= 0 && ct_eval(nt, e, b, out);
  }
  case NK_AndNode: case NK_OrNode: {
    Mv l;
    if (!ct_value(nt, e, nt_ref(nt, node, "left"), &l)) return 0;
    if ((nt_kind(nt, node) == NK_AndNode) != mv_truthy(l)) { out->mv = l; return 1; }
    return ct_eval(nt, e, nt_ref(nt, node, "right"), out);
  }
  case NK_IfNode: {
    /* `c ? a : b`, and an if whose arms are one expression each */
    Mv pv;
    if (!ct_value(nt, e, nt_ref(nt, node, "predicate"), &pv)) return 0;
    int arm = nt_ref(nt, node, "statements");
    if (!mv_truthy(pv)) {
      int el = nt_ref(nt, node, "subsequent");
      if (el < 0) { out->mv = mv_nil(); return 1; }
      if (nt_kind(nt, el) != NK_ElseNode) return ct_eval(nt, e, el, out);
      arm = nt_ref(nt, el, "statements");
    }
    if (arm < 0) { out->mv = mv_nil(); return 1; }
    int one = ct_single(nt, arm);
    return one >= 0 && ct_eval(nt, e, one, out);
  }
  case NK_CallNode: return ct_eval_call(nt, e, node, out);
  case NK_InstanceVariableReadNode: {
    /* self's, in a macro's frame: as the class body and the run left it */
    if (!e->self_name) return 0;
    const CtLocal *l = ct_ivar(e, nt_str(nt, node, "name"));
    if (l ? l->v.k == CT_VALUE && l->v.mv.k == MV_UNDEF : e->ivar_open) return 0;
    if (l) *out = l->v;
    else out->mv = mv_nil();
    return 1;
  }
  case NK_DefinedNode: {
    /* defined?(@x) of self's, in a macro's frame */
    int v = nt_ref(nt, node, "value");
    if (!e->self_name || v < 0 || nt_kind(nt, v) != NK_InstanceVariableReadNode) return 0;
    const CtLocal *l = ct_ivar(e, nt_str(nt, v, "name"));
    if (!l && e->ivar_open) return 0;
    out->mv = l ? mv_str("instance-variable", 17, MV_STR) : mv_nil();
    if (l) out->mv.frozen = 1;
    return 1;
  }
  default: return 0;
  }
}

/* does `when` value w match v */
static int ct_when_matches(const NodeTable *nt, CtEnv *e, int w, Mv v, int *hit) {
  if (nt_kind(nt, w) == NK_RangeNode) {
    Mv lo, hi;
    int l = nt_ref(nt, w, "left"), r = nt_ref(nt, w, "right");
    if (l < 0 || r < 0 || !ct_value(nt, e, l, &lo) || !ct_value(nt, e, r, &hi) || lo.k != MV_INT || hi.k != MV_INT) return 0;
    ct_env_ran(e, "Range", "===");
    *hit = v.k == MV_INT && v.i >= lo.i && (nt_range_excl(nt, w) ? v.i < hi.i : v.i <= hi.i);
    return 1;
  }
  Mv wv;
  if (!ct_value(nt, e, w, &wv) || (wv.k != MV_INT && wv.k != MV_STR && wv.k != MV_SYM)) return 0;
  ct_env_ran(e, ct_class_of(wv), "===");
  *hit = mv_eq(v, wv);
  return 1;
}

/* a name attr_* accepts: an identifier, without ? ! or = (CRuby's
   NameError otherwise) */
static int ct_attr_name(const char *s) {
  size_t n = s ? strlen(s) : 0;
  return n && mv_sym_plain(s) && !strchr("?!=", s[n - 1]);
}

/* is rv self's singleton class: `singleton_class` on self, Kernel's, or
   `(class << self; self; end)`, which desugar_singleton_body_self spells
   `(class << self; end; self.singleton_class)` */
static int ct_self_singleton(const NodeTable *nt, CtEnv *e, int rv) {
  int bn = 0;
  const int *bb = NULL;
  switch (nt_kind(nt, rv)) {
  case NK_CallNode:
    return is_singleton_class_name(nt_str(nt, rv, "name")) && nt_ref(nt, rv, "arguments") < 0 &&
           nt_ref(nt, rv, "block") < 0 && ct_self_core(nt, e, rv, nt_str(nt, rv, "name"));
  case NK_SingletonClassNode:
    return nt_kind(nt, nt_ref(nt, rv, "expression")) == NK_SelfNode && nt_kind(nt, ct_single(nt, nt_ref(nt, rv, "body"))) == NK_SelfNode;
  case NK_ParenthesesNode:
    if (nt_kind(nt, nt_ref(nt, rv, "body")) == NK_StatementsNode) bb = nt_arr(nt, nt_ref(nt, rv, "body"), "body", &bn);
    for (int i = 0; i + 1 < bn; i++) {   /* an empty `class << self; end` before it runs nothing */
      int sb = nt_ref(nt, bb[i], "body"), sn = 0;
      if (sb >= 0 && nt_kind(nt, sb) == NK_StatementsNode) nt_arr(nt, sb, "body", &sn);
      if (nt_kind(nt, bb[i]) != NK_SingletonClassNode || nt_kind(nt, nt_ref(nt, bb[i], "expression")) != NK_SelfNode ||
          (sb >= 0 && (nt_kind(nt, sb) != NK_StatementsNode || sn))) return 0;
    }
    return bn > 0 && ct_self_singleton(nt, e, bb[bn - 1]);
  default: return 0;
  }
}

enum { CT_ARGS = 16, CT_MACRO_DEPTH = 16 };

/* The values of call `call`'s arguments in the caller's frame: the
   positional ones into pv (*np), a trailing keyword hash's into kn / kv
   (*nk), CT_ARGS of each at most. 0 for one not known, or of another kind
   (a splat, `**`, a block argument, a repeated key) */
static int ct_call_values(const NodeTable *nt, CtEnv *e, int call, CtVal *pv, int *np, const char **kn, CtVal *kv, int *nk) {
  int an = nt_ref(nt, call, "arguments"), ac = 0;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  *np = *nk = 0;
  for (int i = 0; i < ac; i++) {
    NodeKind k = nt_kind(nt, av[i]);
    if (k == NK_KeywordHashNode && i == ac - 1) {
      int en = 0;
      const int *el = nt_arr(nt, av[i], "elements", &en);
      for (int j = 0; j < en; j++) {
        int key = nt_kind(nt, el[j]) == NK_AssocNode ? nt_ref(nt, el[j], "key") : -1;
        const char *kname = nt_kind(nt, key) == NK_SymbolNode ? nt_str(nt, key, "value") : NULL;
        if (!kname || *nk >= CT_ARGS || !ct_eval(nt, e, nt_ref(nt, el[j], "value"), &kv[*nk]) || kv[*nk].k != CT_VALUE) return 0;
        for (int q = 0; q < *nk; q++) if (strcmp(kn[q], kname) == 0) return 0;
        kn[(*nk)++] = kname;
      }
      continue;
    }
    if (k == NK_SplatNode || k == NK_KeywordHashNode || k == NK_BlockArgumentNode || *np >= CT_ARGS ||
        !ct_eval(nt, e, av[i], &pv[*np]) || pv[*np].k != CT_VALUE) return 0;
    (*np)++;
  }
  return 1;
}

/* Bind def's parameters in the running frame, as CRuby does: required
   ones, then optional ones (a default evaluated in the frame, after the
   parameters before it), then keywords (a required one must be passed, and
   every key passed must be one); a rest takes the positionals left over as
   an Array, each a value the evaluator knows (`def forward(*names, to:)`).
   0 for a parameter of another kind (post, `**`, block, `...`) or a call
   it does not take. */
static int ct_bind_params(const NodeTable *nt, CtEnv *e, int def, const CtVal *pv, int np, const char **kn, const CtVal *kv, int nk) {
  int ps = nt_ref(nt, def, "parameters"), rn = 0, on = 0, kw = 0, pn = 0, used = 0;
  const int *req = ps >= 0 ? nt_arr(nt, ps, "requireds", &rn) : NULL;
  const int *opt = ps >= 0 ? nt_arr(nt, ps, "optionals", &on) : NULL;
  const int *kws = ps >= 0 ? nt_arr(nt, ps, "keywords", &kw) : NULL;
  if (ps >= 0) nt_arr(nt, ps, "posts", &pn);
  int rest = ps >= 0 ? nt_ref(nt, ps, "rest") : -1;
  const char *rname = rest >= 0 && nt_kind(nt, rest) == NK_RestParameterNode ? nt_str(nt, rest, "name") : NULL;
  if (pn || (rest >= 0 && !rname) || (ps >= 0 && (nt_ref(nt, ps, "keyword_rest") >= 0 || nt_ref(nt, ps, "block") >= 0)) ||
      np < rn || (np > rn + on && rest < 0) || (nk && !kw))
    return 0;
  for (int i = 0; i < rn; i++)
    if (nt_kind(nt, req[i]) != NK_RequiredParameterNode || !nt_str(nt, req[i], "name") ||
        !ct_env_set(e, nt_str(nt, req[i], "name"), pv[i])) return 0;
  for (int i = 0; i < on; i++) {
    CtVal d;
    const char *pname = nt_str(nt, opt[i], "name");
    if (rn + i < np) d = pv[rn + i];
    else if (!ct_eval(nt, e, nt_ref(nt, opt[i], "value"), &d) || d.k != CT_VALUE) return 0;
    if (nt_kind(nt, opt[i]) != NK_OptionalParameterNode || !pname || !ct_env_set(e, pname, d)) return 0;
  }
  if (rname) {
    int from = rn + on, cnt = np > from ? np - from : 0;
    CtVal r; memset(&r, 0, sizeof r);
    r.mv = mv_nil(); r.mv.k = MV_ARR; r.mv.n = cnt;
    r.mv.a = calloc((size_t)cnt + 1, sizeof(Mv));
    if (!r.mv.a) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < cnt; i++) {
      if (pv[from + i].k != CT_VALUE) return 0;
      r.mv.a[i] = pv[from + i].mv;
    }
    if (!ct_env_set(e, rname, r)) return 0;
  }
  for (int i = 0; i < kw; i++) {
    const char *kname = nt_str(nt, kws[i], "name");
    int q = 0;
    while (kname && q < nk && strcmp(kn[q], kname) != 0) q++;
    CtVal d;
    if (!kname) return 0;
    if (q < nk) { d = kv[q]; used++; }
    else if (nt_kind(nt, kws[i]) != NK_OptionalKeywordParameterNode ||
             !ct_eval(nt, e, nt_ref(nt, kws[i], "value"), &d) || d.k != CT_VALUE) return 0;
    if (!ct_env_set(e, kname, d)) return 0;
  }
  return used == nk;
}

/* Run macro def `def` for call `call` in a frame of its own; the caller's
   locals wait meanwhile, and the frame keeps its locals as its body left
   them, which the blocks it hands out read */
static int ct_run_macro_as(const NodeTable *nt, CtEnv *e, int call, int def, CtVal *value);
static int ct_run_macro(const NodeTable *nt, CtEnv *e, int call, int def) { return ct_run_macro_as(nt, e, call, def, NULL); }

/* run the macro def for call in a frame of its own; with value, its value
   too: what a `return` gave, or its last statement's, which the frame's
   locals answer (0 when neither is known) */
static int ct_run_macro_as(const NodeTable *nt, CtEnv *e, int call, int def, CtVal *value) {
  int body = nt_ref(nt, def, "body"), np = 0, nk = 0;
  CtVal pv[CT_ARGS], kv[CT_ARGS];
  const char *kn[CT_ARGS];
  if (e->depth >= CT_MACRO_DEPTH || (body >= 0 && nt_kind(nt, body) != NK_StatementsNode) ||
      !ct_call_values(nt, e, call, pv, &np, kn, kv, &nk)) return 0;
  CtLocal *saved = malloc(sizeof *saved * (size_t)(e->nloc + 1));
  if (!saved) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int nsaved = e->nloc, caller = e->frame;
  memcpy(saved, e->loc, sizeof *saved * (size_t)nsaved);
  e->nloc = 0;
  e->frames = ct_grow(e->frames, &e->cframes, e->nframes + 1, sizeof *e->frames);
  int f = e->nframes++;
  e->frames[f].def = def;
  e->frames[f].call = call;
  e->frames[f].parent = e->depth ? caller : -1;
  e->frames[f].loc = NULL;
  e->frames[f].nloc = 0;
  e->frame = f;
  e->depth++;
  int returning = e->returning;
  CtVal ret = e->ret;
  e->returning = 0;
  int ok = ct_bind_params(nt, e, def, pv, np, kn, kv, nk) && ct_exec(nt, e, body);
  if (ok && value) {
    int sn = 0;
    const int *ss = body >= 0 ? nt_arr(nt, body, "body", &sn) : NULL;
    if (e->returning) *value = e->ret;
    else if (!sn) { memset(value, 0, sizeof *value); value->mv = mv_nil(); }
    else ok = ct_eval(nt, e, ss[sn - 1], value);
  }
  e->returning = returning;
  e->ret = ret;
  e->depth--;
  e->frame = caller;
  e->frames[f].loc = malloc(sizeof(CtLocal) * (size_t)(e->nloc + 1));
  if (!e->frames[f].loc) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(e->frames[f].loc, e->loc, sizeof(CtLocal) * (size_t)e->nloc);
  e->frames[f].nloc = e->nloc;
  memcpy(e->loc, saved, sizeof *saved * (size_t)nsaved);
  e->nloc = nsaved;
  free(saved);
  return ok;
}

int ct_macro_call(const NodeTable *nt, CtEnv *e, int call) {
  const char *name = nt_str(nt, call, "name");
  int rv = nt_ref(nt, call, "receiver");
  if (!e->self_macro || !name || nt_ref(nt, call, "block") >= 0 ||
      (rv >= 0 && nt_kind(nt, rv) != NK_SelfNode && !nt_int(nt, call, "self_call", 0))) return 0;
  int def = e->self_macro(e->host, name);
  return def >= 0 && ct_run_macro(nt, e, call, def);
}

/* The sends on self in code `id` whose method name the run knows: each
   name argument is noted (f->fold), to be written as the Symbol where the
   code runs, which the send then names at compile time */
static void ct_fold_sends(const NodeTable *nt, CtEnv *e, CtFx *f, int id) {
  if (id < 0) return;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_BlockNode || k == NK_LambdaNode || k == NK_DefNode) return;
  if (k == NK_CallNode && is_send_family(nt_str(nt, id, "name")) && nt_ref(nt, id, "block") < 0) {
    int rv = nt_ref(nt, id, "receiver"), argc = 0;
    const int *av = ct_args(nt, id, &argc);
    Mv mn;
    if ((rv < 0 || nt_kind(nt, rv) == NK_SelfNode) && argc >= 1 && nt_kind(nt, av[0]) != NK_SymbolNode &&
        ct_value(nt, e, av[0], &mn) && (mn.k == MV_SYM || mn.k == MV_STR) && mn.s[0] && mv_sym_plain(mn.s)) {
      f->fold = realloc(f->fold, sizeof *f->fold * (size_t)(f->nfold + 1));
      f->foldv = realloc(f->foldv, sizeof *f->foldv * (size_t)(f->nfold + 1));
      if (!f->fold || !f->foldv) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      f->fold[f->nfold] = av[0];
      f->foldv[f->nfold++] = ct_strdup(mn.s);
    }
  }
  for (int i = 0, nr = nt_num_refs(nt, id); i < nr; i++) ct_fold_sends(nt, e, f, nt_ref_at(nt, id, i));
  for (int i = 0, na = nt_num_arrs(nt, id); i < na; i++) {
    int n = 0;
    const int *v = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++) ct_fold_sends(nt, e, f, v[j]);
  }
}

/* a constant name const_set accepts and the program could have written */
static int ct_const_name(Mv n) {
  if ((n.k != MV_STR && n.k != MV_SYM) || !n.s[0] || n.s[0] < 'A' || n.s[0] > 'Z' || !mv_sym_plain(n.s)) return 0;
  char last = n.s[strlen(n.s) - 1];
  return last != '?' && last != '!' && last != '=';
}

/* the statement call `call`: one of the effects the evaluator follows */
static int ct_exec_call(const NodeTable *nt, CtEnv *e, int call) {
  const char *name = nt_str(nt, call, "name");
  int rv = nt_ref(nt, call, "receiver"), argc = 0;
  const int *av = ct_args(nt, call, &argc);
  int blk = nt_ref(nt, call, "block");
  /* the method define_method makes on self, by a name the frame computes;
     its block is the caller's to take. In a method body the scope walk
     makes such a call a refusal (define_method_name 1), which keeps what
     the call was (dm_name, dm_args, dm_block) */
  if (e->self_name && nt_int(nt, call, "define_method_name", 0) == 1) {
    int da = (int)nt_int(nt, call, "dm_args", -1);
    name = nt_str(nt, call, "dm_name");
    av = da >= 0 ? nt_arr(nt, da, "arguments", &argc) : NULL;
    blk = (int)nt_int(nt, call, "dm_block", -1);
  }
  int sg = is_define_singleton_method_name(name);
  /* `define_method(name, -> { })`: the lambda is the body, as a block is */
  int lam = name && argc == 2 && blk < 0 && nt_kind(nt, av[1]) == NK_LambdaNode;
  if (lam) { blk = av[1]; argc = 1; }
  if (name && argc == 1 && blk >= 0 && (nt_kind(nt, blk) == NK_BlockNode || lam) && (is_define_method_name(name) || sg) &&
      ct_self_core(nt, e, call, name)) {
    Mv mn;
    if (!ct_value(nt, e, av[0], &mn) || (mn.k != MV_SYM && mn.k != MV_STR) || !mn.s[0]) return 0;
    CtFx *f = ct_fx(e, CT_FX_DEFINE_METHOD, call);
    f->name = ct_strdup(mn.s);
    f->block = blk;
    f->sg = sg;
    f->lam = lam;
    return 1;
  }
  if (lam) { blk = -1; argc = 2; }
  /* another macro on self: what its frame does, this one does */
  if (e->self_macro && name && blk < 0 && (rv < 0 || nt_kind(nt, rv) == NK_SelfNode)) {
    int def = e->self_macro(e->host, name);
    if (def >= 0) {
      ct_env_relied(e, name);
      return ct_run_macro(nt, e, call, def);
    }
  }
  /* attr_* of self or of its singleton class, by names the frame computes */
  if (e->self_name && name && argc > 0 && blk < 0 && (is_attr_reader_family(name) || is_attr_writer_family(name))) {
    sg = rv >= 0 && nt_kind(nt, rv) != NK_SelfNode;
    if (sg ? !ct_self_singleton(nt, e, rv) : !ct_self_core(nt, e, call, name)) return 0;
    char **v = calloc((size_t)argc, sizeof *v);
    if (!v) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    int ok = 1;
    for (int i = 0; i < argc && ok; i++) {
      Mv mn;
      ok = ct_value(nt, e, av[i], &mn) && (mn.k == MV_SYM || mn.k == MV_STR) && ct_attr_name(mn.s);
      if (ok) v[i] = ct_strdup(mn.s);
    }
    if (!ok) {
      for (int i = 0; i < argc; i++) free(v[i]);
      free(v);
      return 0;
    }
    if (sg) ct_env_ran(e, "Class", name);   /* the singleton class's own: a Class's */
    CtFx *f = ct_fx(e, CT_FX_ATTR, call);
    f->name = ct_strdup(name);
    f->attrs = v;
    f->nattrs = argc;
    f->sg = sg;
    return 1;
  }
  /* self's body's code, as text the frame computes; the caller parses it
     with the call's file and line */
  if (e->self_macro && name && argc >= 1 && argc <= 3 && blk < 0 && is_class_eval_text_name(name) &&
      ct_self_core(nt, e, call, name)) {
    Mv t;
    if (!ct_value(nt, e, av[0], &t) || t.k != MV_STR) return 0;
    ct_fx(e, CT_FX_EVAL, call)->name = ct_strdup(t.s);
    return 1;
  }
  if (!name || argc < 0 || blk >= 0) return 0;
  int self_recv = rv < 0 || nt_kind(nt, rv) == NK_SelfNode;
  if (self_recv && argc == 2 && is_const_set_name(name)) {
    Mv cn; CtVal v;
    if (!ct_value(nt, e, av[0], &cn) || !ct_const_name(cn)) return 0;
    /* a macro's value the evaluator does not know is its code, run where
       the call stands (vcode) */
    int known = ct_eval(nt, e, av[1], &v);
    if (!known && !e->self_name) return 0;
    ct_env_ran(e, "Class", name);
    CtFx *f = ct_fx(e, CT_FX_CONST_SET, call);
    f->name = ct_strdup(cn.s);
    if (!known) { f->vcode = av[1] + 1; ct_fx_snap(e, f); ct_fold_sends(nt, e, f, av[1]); return 1; }
    f->val = v;
    /* an anonymous class takes the first name it is given */
    if (v.k == CT_CLASS && !e->cls[v.cls].name) { e->cls[v.cls].name = ct_strdup(cn.s); f->names = 1; }
    return 1;
  }
  if (!self_recv && argc == 2 && is_ivar_set_name(name)) {
    CtVal r, v; Mv iv;
    if (!ct_eval(nt, e, rv, &r) || r.k != CT_CLASS || !ct_value(nt, e, av[0], &iv) ||
        (iv.k != MV_SYM && iv.k != MV_STR) || iv.s[0] != '@' || iv.s[1] == '@' || !mv_sym_plain(iv.s + 1) ||
        !ct_eval(nt, e, av[1], &v)) return 0;
    ct_env_ran(e, "Class", name);
    CtFx *f = ct_fx(e, CT_FX_IVAR_SET, call);
    f->cls = r.cls; f->name = ct_strdup(iv.s); f->val = v;
    return 1;
  }
  if (!self_recv && argc == 2 && is_index_assign(name) &&
      (nt_kind(nt, rv) == NK_ConstantReadNode || nt_kind(nt, rv) == NK_ConstantPathNode)) {
    CtVal k, v;
    if (!ct_eval(nt, e, av[0], &k) || !ct_eval(nt, e, av[1], &v)) return 0;
    CtFx *f = ct_fx(e, CT_FX_INDEX_SET, call);
    f->recv.k = CT_CONST; f->recv.node = rv; f->key = k; f->val = v;
    return 1;
  }
  if (!self_recv && argc == 0 && is_freeze_name(name)) {
    /* a value the code holds: a String frozen here keeps its literal (lit).
       One the evaluation made may be held by other locals too, which this
       local's value would not tell: not followed. */
    CtVal r;
    if (!ct_eval(nt, e, rv, &r) || r.k != CT_VALUE || (r.mv.k != MV_STR && r.mv.k != MV_INT && r.mv.k != MV_SYM) ||
        (r.mv.k == MV_STR && !r.mv.lit && !r.mv.frozen)) return 0;
    ct_env_ran(e, ct_class_of(r.mv), name);
    if (nt_kind(nt, rv) == NK_LocalVariableReadNode && nt_str(nt, rv, "name")) {
      CtVal *lv = ct_lookup(e, nt_str(nt, rv, "name"));
      if (lv) lv->mv.frozen = 1;
    }
    return 1;
  }
  return 0;
}

static int ct_exec_one(const NodeTable *nt, CtEnv *e, int n) {
  switch (nt_kind(nt, n)) {
  case NK_InstanceVariableWriteNode: {
    /* self's, in a macro's frame: the run follows the value, and the
       statement runs as written where the call stands (a residual) */
    CtVal v;
    if (!e->self_name || !nt_str(nt, n, "name")) return 0;
    if (!ct_eval(nt, e, nt_ref(nt, n, "value"), &v)) { memset(&v, 0, sizeof v); v.mv.k = MV_UNDEF; }
    ct_env_ivar_set(e, nt_str(nt, n, "name"), v);
    return 0;
  }
  case NK_ReturnNode: {   /* a macro's frame's: it answers the frame's value */
    if (!e->self_macro) return 0;
    int args = nt_ref(nt, n, "arguments"), an = 0;
    const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an > 1 || (an == 1 && nt_kind(nt, av[0]) == NK_SplatNode)) return 0;
    CtVal v;
    memset(&v, 0, sizeof v);
    v.mv = mv_nil();
    if (an == 1 && !ct_eval(nt, e, av[0], &v)) return 0;
    e->ret = v;
    e->returning = 1;
    return 1;
  }
  case NK_LocalVariableWriteNode: {
    const char *nm = nt_str(nt, n, "name");
    CtVal v;
    if (!nm || nt_int(nt, n, "depth", 0) != 0) return 0;   /* a local of the scope around the block */
    if (!ct_eval(nt, e, nt_ref(nt, n, "value"), &v)) return 0;
    if (e->self_macro) {   /* the caller may run it as written */
      CtFx *f = ct_fx(e, CT_FX_LOCAL, n);
      f->name = ct_strdup(nm);
      f->val = v;
      ct_fx_snap(e, f);
    }
    return ct_env_write(e, nm, v, n);
  }
  case NK_IfNode: case NK_UnlessNode: {
    int is_if = nt_kind(nt, n) == NK_IfNode;
    Mv pv;
    if (!ct_value(nt, e, nt_ref(nt, n, "predicate"), &pv)) return 0;
    if (mv_truthy(pv) == is_if) return ct_exec(nt, e, nt_ref(nt, n, "statements"));
    int el = nt_ref(nt, n, is_if ? "subsequent" : "else_clause");
    if (el < 0) return 1;
    if (nt_kind(nt, el) == NK_ElseNode) return ct_exec(nt, e, nt_ref(nt, el, "statements"));
    return ct_exec_one(nt, e, el);
  }
  case NK_CaseNode: {
    Mv v;
    int pred = nt_ref(nt, n, "predicate"), wn = 0;
    if (pred < 0 || !ct_value(nt, e, pred, &v)) return 0;
    const int *ws = nt_arr(nt, n, "conditions", &wn);
    for (int i = 0; i < wn; i++) {
      if (nt_kind(nt, ws[i]) != NK_WhenNode) return 0;
      int cn = 0;
      const int *cs = nt_arr(nt, ws[i], "conditions", &cn);
      for (int j = 0; j < cn; j++) {
        int hit = 0;
        if (!ct_when_matches(nt, e, cs[j], v, &hit)) return 0;
        if (hit) return ct_exec(nt, e, nt_ref(nt, ws[i], "statements"));
      }
    }
    int el = nt_ref(nt, n, "else_clause");
    return el >= 0 ? ct_exec(nt, e, nt_ref(nt, el, "statements")) : 1;
  }
  case NK_CallNode: {
    if (ct_exec_call(nt, e, n)) return 1;
    /* a core method's answer on a value (`[..].join("_")`, a body's value
       read by its caller): followed when the evaluator knows it */
    int rv = nt_ref(nt, n, "receiver");
    CtVal v;
    return e->self_macro && rv >= 0 && nt_kind(nt, rv) != NK_SelfNode && ct_eval(nt, e, n, &v) && v.k == CT_VALUE;
  }
  /* an expression that does nothing but answer (a body's value, read by
     the caller), followed when the evaluator knows it */
  case NK_InstanceVariableReadNode: case NK_LocalVariableReadNode: case NK_DefinedNode:
  case NK_NilNode: case NK_TrueNode: case NK_FalseNode: case NK_IntegerNode: case NK_StringNode: case NK_SymbolNode: {
    CtVal v;
    return e->self_macro && ct_eval(nt, e, n, &v);
  }
  default: return 0;
  }
}

int ct_exec(const NodeTable *nt, CtEnv *e, int stmts) {
  if (stmts < 0) return 1;
  if (nt_kind(nt, stmts) != NK_StatementsNode) return 0;
  int n = 0;
  const int *b = nt_arr(nt, stmts, "body", &n);
  for (int i = 0; i < n && !e->returning; i++) {
    int nfx = e->nfx;
    if (ct_exec_one(nt, e, b[i])) continue;
    if (!e->self_macro || nt_kind(nt, b[i]) == NK_ReturnNode) return 0;   /* (a return is the frame's, not the class body's) */
    /* in a macro's frame, a statement not followed runs as written where
       the call stands, if the caller can let it: what it did here is undone */
    while (e->nfx > nfx) ct_fx_clear(&e->fx[--e->nfx]);
    ct_fx_snap(e, ct_fx(e, CT_FX_RESIDUAL, b[i]));
  }
  return 1;
}
