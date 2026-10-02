#include "compiler.h"
#include "analyze.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int comp_ternary_arms(const NodeTable *nt, int id, int *then_node, int *else_node) {
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "IfNode")) return 0;
  int then_b = nt_ref(nt, id, "statements");
  int sub = nt_ref(nt, id, "subsequent");
  if (then_b < 0 || sub < 0) return 0;
  const char *subty = nt_type(nt, sub);
  if (!subty || !sp_streq(subty, "ElseNode")) return 0;
  int else_stmts = nt_ref(nt, sub, "statements");
  if (else_stmts < 0) return 0;
  int tn = 0, en = 0;
  const int *tb = nt_arr(nt, then_b, "body", &tn);
  const int *eb = nt_arr(nt, else_stmts, "body", &en);
  if (tn != 1 || en != 1 || !tb || !eb) return 0;
  *then_node = tb[0];
  *else_node = eb[0];
  return 1;
}

int comp_nil_chain_bottom(const NodeTable *nt, int v) {
  /* `a = b = ... = nil` writes nil to EVERY target, but a nested write node
     evaluates to its slot's unified type, so the inner slot's other writes
     would leak into the outer target's type. Detect the chain so analyze can
     treat the outer write as a nil write and codegen can give each target its
     own typed nil. Only plain local/ivar writes chain; or-/and-/op-writes
     have their own value semantics. Returns the terminal NilNode, or -1. */
  int depth = 0;
  while (v >= 0 && depth < 64) {
    const char *t = nt_type(nt, v);
    if (!t) return -1;
    if (sp_streq(t, "NilNode")) return depth > 0 ? v : -1;
    if (!sp_streq(t, "LocalVariableWriteNode") &&
        !sp_streq(t, "InstanceVariableWriteNode")) return -1;
    v = nt_ref(nt, v, "value");
    depth++;
  }
  return -1;
}

/* The same chain, ending in any literal whose identity a program cannot
   observe -- a number, a symbol, nil or a boolean. `a = b = 0` writes 0 to
   both, so each target should take it in its OWN slot type; taking the inner
   write's value instead assigns b's slot to a, and where b widened to Bignum
   that is an sp_Bigint * going into a's sp_int. A String literal is left out
   on purpose: `a = b = "x"` makes a and b the same object, which re-emitting
   the literal per target would not preserve. Returns the terminal literal
   node, or -1. */
int comp_scalar_literal_chain_bottom(const NodeTable *nt, int v) {
  int depth = 0;
  while (v >= 0 && depth < 64) {
    const char *t = nt_type(nt, v);
    if (!t) return -1;
    if (sp_streq(t, "IntegerNode") || sp_streq(t, "FloatNode") ||
        sp_streq(t, "SymbolNode") || sp_streq(t, "NilNode") ||
        sp_streq(t, "TrueNode") || sp_streq(t, "FalseNode"))
      return depth > 0 ? v : -1;
    if (!sp_streq(t, "LocalVariableWriteNode") &&
        !sp_streq(t, "InstanceVariableWriteNode")) return -1;
    v = nt_ref(nt, v, "value");
    depth++;
  }
  return -1;
}

Compiler *comp_new(const NodeTable *nt) {
  Compiler *c = calloc(1, sizeof(Compiler));
  if (!c) return NULL;
  c->nt = nt;
  int n = nt->count > 0 ? nt->count : 1;
  c->ntype = calloc((size_t)n, sizeof(TyKind));
  c->norigin = malloc((size_t)n * sizeof(int));
  for (int i = 0; i < n; i++) c->norigin[i] = -1;
  c->nilnarrow = calloc((size_t)n, sizeof(TyKind));
  c->strbuf_box = calloc((size_t)n, 1);
  c->strbuf_handle_demand = calloc((size_t)n, 1);
  c->strbuf_read_raw = calloc((size_t)n, 1);
  c->poly_strbuf_lift = calloc((size_t)n, 1);
  c->nscope = calloc((size_t)n, sizeof(int));   /* default scope 0 */
  c->node_cbody = malloc((size_t)n * sizeof(int));   /* enclosing class-body, -1 = none */
  for (int i = 0; i < n; i++) c->node_cbody[i] = -1;
  c->empty_arr_recv = calloc((size_t)n, 1);
  c->empty_hash_recv = calloc((size_t)n, 1);
  c->empty_hash_arg = calloc((size_t)n, 1);
  c->store_misfit_arg = calloc((size_t)n, 1);
  c->ivar_widen_src = calloc((size_t)n, 1);
  c->hash_want = calloc((size_t)n, sizeof(TyKind));
  c->arr_want = calloc((size_t)n, sizeof(TyKind));
  c->poly_builtin_ty = calloc((size_t)n, sizeof(TyKind));
  c->node_cap = n;
  comp_node_ord(c, 0, NULL);   /* number the parsed nodes before any rewrite */
  c->node_ord_parsed = nt->count;
  return c;
}

/* The number a name the compiler invents from a node carries (`__fwdc_12`,
   `x__bp12`), in place of the node id. The builtins are spliced into the
   source ahead of the program, so every node a builtin gained or lost moved
   the id of every node after it, and with it a name in programs that never
   call that builtin. A program node is numbered by its place in table order
   among the program's nodes. A node the parser stamped as spliced from
   builtins/ (`node_bi`, which a clone carries along) is numbered by its place
   among the nodes of its base, the builtin method it sits in, so an edit to
   one builtin method renames nothing outside it. A number once given is
   kept: the table is extended over the nodes appended since, never refilled,
   so a node a rewrite resets keeps its number and cannot hand it to another. */
static void comp_node_base_key(Compiler *c, int b) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, b, "name");
  if (!nm || !*nm) nm = nt_type(nt, b) ? nt_type(nt, b) : "node";
  /* a C-safe spelling of at most 24 characters, so the names stay short */
  char key[40]; size_t o = 0;
  for (const char *q = nm; *q && o < 24; q++) {
    if ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_')
      key[o++] = *q;
    else if (o + 3 <= 24) o += (size_t)snprintf(key + o, 4, "x%02x", (unsigned char)*q);
    else break;
  }
  key[o] = '\0';
  /* a second base of the same name (a reopened module, a method defined
     twice) is told apart by how many came before it */
  int dup = 0;
  for (int k = 0; k < b; k++)
    if (c->bi_base_key[k] && !strncmp(c->bi_base_key[k], key, o) &&
        (c->bi_base_key[k][o] == '\0' || !strncmp(c->bi_base_key[k] + o, "_d", 2))) dup++;
  if (dup) snprintf(key + o, sizeof key - o, "_d%d", dup);
  c->bi_base_key[b] = strdup(key);
}

/* The base a node counts in, or -1 for a program node: its own stamp, or,
   for a node a rewrite made with none, the builtin method whose copy it
   was made in. */
static int comp_node_base(Compiler *c, int k) {
  const NodeTable *nt = c->nt;
  int b = (int)nt_int(nt, k, "node_bi", 0) - 1;
  if (b < 0 && k >= c->node_ord_parsed && c->nscope && k < c->node_cap) {
    int si = c->nscope[k];
    int dn = si > 0 && si < c->nscopes ? c->scopes[si].def_node : -1;
    if (dn >= 0 && dn < nt->count) b = (int)nt_int(nt, dn, "node_bi", 0) - 1;
  }
  return b >= 0 && b < nt->count ? b : -1;
}

static void comp_node_ord_assign(Compiler *c, int k) {
  int b = comp_node_base(c, k);
  c->node_base[k] = b;
  if (b < 0) { c->node_ord[k] = c->node_ord_prog++ << 1; return; }
  if (b >= c->bi_base_cap) {
    int cap = c->bi_base_cap ? c->bi_base_cap : 64;
    while (cap <= b) cap *= 2;
    int *gc = realloc(c->bi_base_cnt, sizeof(int) * (size_t)cap);
    char **gk = realloc(c->bi_base_key, sizeof(char *) * (size_t)cap);
    if (!gc || !gk) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int j = c->bi_base_cap; j < cap; j++) { gc[j] = 0; gk[j] = NULL; }
    c->bi_base_cnt = gc; c->bi_base_key = gk; c->bi_base_cap = cap;
  }
  if (!c->bi_base_key[b]) comp_node_base_key(c, b);
  c->node_ord[k] = (c->bi_base_cnt[b]++ << 1) | 1;
}

/* The parsed nodes are numbered in table order when the compiler is made. A
   node a rewrite appends is numbered when a name is first asked of it, in the
   order the names are asked for: the builtins' rewrites append nodes as
   well, and counting those in table order moved every later program name. */
int comp_node_ord(Compiler *c, int id, int *builtin) {
  const NodeTable *nt = c->nt;
  if (nt->count > c->node_ord_n) {
    int *g = realloc(c->node_ord, sizeof(int) * (size_t)nt->count);
    int *gb = realloc(c->node_base, sizeof(int) * (size_t)nt->count);
    if (!g || !gb) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    c->node_ord = g; c->node_base = gb;
    for (int k = c->node_ord_n; k < nt->count; k++) {
      c->node_ord[k] = -1;
      if (!c->node_ord_parsed) comp_node_ord_assign(c, k);
    }
    c->node_ord_n = nt->count;
  }
  if (id < 0 || id >= c->node_ord_n) { if (builtin) *builtin = 0; return 0; }
  if (c->node_ord[id] < 0) comp_node_ord_assign(c, id);
  int v = c->node_ord[id];
  if (builtin) *builtin = v & 1;
  return v >> 1;
}

/* comp_node_ord as the text a name carries: "12" for a program node, and
   for a builtin's "q", its method's name and its number there ("qmin_by_12"),
   so the two never name the same thing. The text lives until eight more have
   been asked for. */
const char *comp_node_tag(Compiler *c, int id) {
  static char ring[8][200];
  static int next = 0;
  char *t = ring[next++ & 7];
  int b = 0;
  int ord = comp_node_ord(c, id, &b);
  int base = b ? c->node_base[id] : -1;
  if (base >= 0 && base < c->bi_base_cap && c->bi_base_key[base])
    snprintf(t, sizeof ring[0], "q%s_%d", c->bi_base_key[base], ord);
  else snprintf(t, sizeof ring[0], "%d", ord);
  return t;
}

/* Resize the per-node arrays after the node table grew (e.g. an AST subtree
   was cloned). New entries default to TY_UNKNOWN / scope 0. */
void comp_grow_node_arrays(Compiler *c) {
  int n = c->nt->count;
  if (n <= c->node_cap) return;
  c->ntype = realloc(c->ntype, sizeof(TyKind) * (size_t)n);
  c->norigin = realloc(c->norigin, sizeof(int) * (size_t)n);
  c->nilnarrow = realloc(c->nilnarrow, sizeof(TyKind) * (size_t)n);
  c->strbuf_box = realloc(c->strbuf_box, (size_t)n);
  c->strbuf_handle_demand = realloc(c->strbuf_handle_demand, (size_t)n);
  c->strbuf_read_raw = realloc(c->strbuf_read_raw, (size_t)n);
  c->poly_strbuf_lift = realloc(c->poly_strbuf_lift, (size_t)n);
  c->nscope = realloc(c->nscope, sizeof(int) * (size_t)n);
  c->node_cbody = realloc(c->node_cbody, sizeof(int) * (size_t)n);
  c->empty_arr_recv = realloc(c->empty_arr_recv, (size_t)n);
  c->empty_hash_recv = realloc(c->empty_hash_recv, (size_t)n);
  c->empty_hash_arg = realloc(c->empty_hash_arg, (size_t)n);
  c->store_misfit_arg = realloc(c->store_misfit_arg, (size_t)n);
  c->ivar_widen_src = realloc(c->ivar_widen_src, (size_t)n);
  c->hash_want = realloc(c->hash_want, sizeof(TyKind) * (size_t)n);
  c->arr_want = realloc(c->arr_want, sizeof(TyKind) * (size_t)n);
  c->poly_builtin_ty = realloc(c->poly_builtin_ty, sizeof(TyKind) * (size_t)n);
  for (int i = c->node_cap; i < n; i++) { c->ntype[i] = TY_UNKNOWN; c->norigin[i] = -1; c->nilnarrow[i] = TY_UNKNOWN; c->nscope[i] = 0; c->node_cbody[i] = -1; c->empty_arr_recv[i] = 0; c->empty_hash_recv[i] = 0; c->empty_hash_arg[i] = 0; c->store_misfit_arg[i] = 0; c->ivar_widen_src[i] = 0; c->hash_want[i] = TY_UNKNOWN; c->arr_want[i] = TY_UNKNOWN; c->poly_builtin_ty[i] = TY_UNKNOWN; c->strbuf_box[i] = 0; c->strbuf_handle_demand[i] = 0; c->strbuf_read_raw[i] = 0; c->poly_strbuf_lift[i] = 0; }
  c->node_cap = n;
}

void comp_free(Compiler *c) {
  if (!c) return;
  fwd_analysis_free(c);
  free(c->hash_default_arg_memo);
  c->hash_default_arg_memo = NULL;
  free(c->blk_body_map);
  free(c->node_ord); free(c->node_base);
  for (int k = 0; k < c->bi_base_cap; k++) free(c->bi_base_key[k]);
  free(c->bi_base_key); free(c->bi_base_cnt);
  c->blk_body_map = NULL;
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    free(sc->name);
    for (int i = 0; i < sc->nlocals; i++) free(sc->locals[i].name);
    free(sc->locals);
    for (int i = 0; i < sc->nparams; i++) free(sc->pnames[i]);
    free(sc->pnames);
    free(sc->pdefault);
  }
  free(c->scopes);
  for (int i = 0; i < c->nsymbols; i++) free(c->symbols[i]);
  free(c->symbols);
  for (int i = 0; i < c->nclasses; i++) {
    free(c->classes[i].name);
    for (int j = 0; j < c->classes[i].nivars; j++) free(c->classes[i].ivars[j]);
    free(c->classes[i].ivars);
    free(c->classes[i].ivar_types);
    for (int j = 0; j < c->classes[i].n_rbs_pin_ivars; j++) free(c->classes[i].rbs_pin_ivars[j]);
    free(c->classes[i].rbs_pin_ivars);
    for (int j = 0; j < c->classes[i].nreaders; j++) free(c->classes[i].readers[j]);
    free(c->classes[i].readers);
    for (int j = 0; j < c->classes[i].nwriters; j++) free(c->classes[i].writers[j]);
    free(c->classes[i].writers);
    for (int j = 0; j < c->classes[i].nundefs; j++) free(c->classes[i].undefs[j]);
    free(c->classes[i].undefs);
    for (int j = 0; j < c->classes[i].nsg_readers; j++) free(c->classes[i].sg_readers[j]);
    free(c->classes[i].sg_readers);
    for (int j = 0; j < c->classes[i].nsg_writers; j++) free(c->classes[i].sg_writers[j]);
    free(c->classes[i].sg_writers);
    for (int j = 0; j < c->classes[i].nprep_chain; j++) {
      free(c->classes[i].prep_from[j]);
      free(c->classes[i].prep_to[j]);
    }
    free(c->classes[i].prep_from);
    free(c->classes[i].prep_to);
  }
  free(c->classes);
  for (int i = 0; i < c->ngvars; i++) free(c->gvars[i].name);
  free(c->gvars);
  for (int i = 0; i < c->nconsts; i++) free(c->consts[i].name);
  free(c->consts);
  free(c->toplevel_includes);
  for (int i = 0; i < c->n_ffi_sources; i++) {
    free(c->ffi_sources[i].mod);
    free(c->ffi_sources[i].val);
  }
  free(c->ffi_sources);
  free(c->nscope);
  free(c->ntype);
  free(c->norigin);
  free(c->node_cbody);
  free(c->empty_arr_recv);
  free(c->empty_hash_recv);
  free(c->store_misfit_arg);
  free(c->ivar_widen_src);
  free(c->hash_want);
  free(c->arr_want);
  free(c->poly_builtin_ty);
  free(c);
}

static LocalVar *lv_find(LocalVar *arr, int n, const char *name) {
  for (int i = 0; i < n; i++) if (sp_streq(arr[i].name, name)) return &arr[i];
  return NULL;
}
static LocalVar *lv_intern(LocalVar **arr, int *n, int *cap, const char *name) {
  LocalVar *lv = lv_find(*arr, *n, name);
  if (lv) return lv;
  if (*n >= *cap) { *cap = *cap ? *cap * 2 : 8; *arr = realloc(*arr, sizeof(LocalVar) * (size_t)*cap); }
  lv = &(*arr)[(*n)++];
  memset(lv, 0, sizeof(*lv));
  lv->name = strdup(name);
  lv->type = TY_UNKNOWN;
  return lv;
}
LocalVar *comp_gvar(Compiler *c, const char *name) { return lv_find(c->gvars, c->ngvars, name); }
LocalVar *comp_gvar_intern(Compiler *c, const char *name) { return lv_intern(&c->gvars, &c->ngvars, &c->cgvars, name); }
const char *comp_resolve_gvar(Compiler *c, const char *name) {
  for (int i = 0; i < c->ngvar_aliases; i++)
    if (sp_streq(c->gvar_alias_from[i], name)) return c->gvar_alias_to[i];
  return name;
}
int comp_gvar_is_interp_flag(const char *name) {
  return name && (sp_streq(name, "VERBOSE") || sp_streq(name, "DEBUG"));
}
void comp_add_gvar_alias(Compiler *c, const char *from, const char *to) {
  for (int i = 0; i < c->ngvar_aliases; i++)
    if (sp_streq(c->gvar_alias_from[i], from)) return; /* already recorded */
  c->gvar_alias_from = realloc(c->gvar_alias_from, sizeof(char*) * (size_t)(c->ngvar_aliases + 1));
  c->gvar_alias_to   = realloc(c->gvar_alias_to,   sizeof(char*) * (size_t)(c->ngvar_aliases + 1));
  c->gvar_alias_from[c->ngvar_aliases] = strdup(from);
  c->gvar_alias_to[c->ngvar_aliases]   = strdup(to);
  c->ngvar_aliases++;
}
/* Constants are looked up by name on every read the analysis types, and a
   large program has thousands: a hash over the names, rebuilt when the table
   grows or moves (its names are unique, interned by comp_const_intern). */
static int *cst_idx; static int cst_cap, cst_n = -1; static const LocalVar *cst_base;
LocalVar *comp_const(Compiler *c, const char *name) {
  if (!name) return NULL;
  if (c->nconsts < 16) return lv_find(c->consts, c->nconsts, name);
  if (cst_n != c->nconsts || cst_base != c->consts) {
    int cap = 64;
    while (cap < c->nconsts * 2) cap *= 2;
    free(cst_idx);
    cst_idx = malloc(sizeof(int) * (size_t)cap);
    for (int i = 0; i < cap; i++) cst_idx[i] = -1;
    for (int i = 0; i < c->nconsts; i++) {
      unsigned j = sp_strhash(c->consts[i].name) & (unsigned)(cap - 1);
      while (cst_idx[j] >= 0) j = (j + 1) & (unsigned)(cap - 1);
      cst_idx[j] = i;
    }
    cst_cap = cap; cst_n = c->nconsts; cst_base = c->consts;
  }
  unsigned j = sp_strhash(name) & (unsigned)(cst_cap - 1);
  for (; cst_idx[j] >= 0; j = (j + 1) & (unsigned)(cst_cap - 1))
    if (sp_streq(c->consts[cst_idx[j]].name, name)) return &c->consts[cst_idx[j]];
  return NULL;
}
LocalVar *comp_const_intern(Compiler *c, const char *name) { return lv_intern(&c->consts, &c->nconsts, &c->cconsts, name); }

/* Intern a symbol name of a known BYTE length. A symbol's name may hold a NUL
   -- `:"a\0b"` -- and the node table carries it, so the compiler has to as
   well: strdup and sp_streq would both end the name at that byte, conflating
   `:"a\0b"` with `:a` and emitting the short form. */
int comp_sym_intern_n(Compiler *c, const char *name, size_t len) {
  for (int i = 0; i < c->nsymbols; i++)
    if (c->symbol_lens[i] == len && memcmp(c->symbols[i], name, len) == 0) return i;
  if (c->nsymbols >= c->csymbols) {
    c->csymbols = c->csymbols ? c->csymbols * 2 : 8;
    c->symbols = realloc(c->symbols, sizeof(char *) * (size_t)c->csymbols);
    c->symbol_lens = realloc(c->symbol_lens, sizeof(size_t) * (size_t)c->csymbols);
  }
  char *cp = malloc(len + 1);
  if (!cp) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(cp, name, len); cp[len] = 0;
  c->symbols[c->nsymbols] = cp;
  c->symbol_lens[c->nsymbols] = len;
  return c->nsymbols++;
}
/* The name is a plain C string (a method name, a kind label): no NUL by
   construction. */
int comp_sym_intern(Compiler *c, const char *name) {
  return comp_sym_intern_n(c, name, name ? strlen(name) : 0);
}

Scope *comp_scope_new(Compiler *c, const char *name, int def_node) {
  if (c->nscopes >= c->cscopes) {
    c->cscopes = c->cscopes ? c->cscopes * 2 : 8;
    c->scopes = realloc(c->scopes, sizeof(Scope) * (size_t)c->cscopes);
  }
  Scope *s = &c->scopes[c->nscopes++];
  memset(s, 0, sizeof(*s));
  s->name = name ? strdup(name) : NULL;
  s->def_node = def_node;
  s->body = -1;
  s->class_id = -1;
  s->rest_idx = -1;
  s->kwrest_idx = -1;
  s->ret = TY_UNKNOWN;
  s->ret_noblock = TY_UNKNOWN;
  why_reset(&s->ret_why);
  s->dm_subst_node = -1;
  return s;
}

int g_infer_round = 0;

int ty_degraded(TyKind t) {
  return t == TY_POLY || t == TY_POLY_ARRAY || t == TY_POLY_POLY_HASH ||
         t == TY_SYM_POLY_HASH || t == TY_STR_POLY_HASH;
}

void why_reset(SlotWhy *w) {
  w->node = -1; w->other = -1; w->prev = TY_UNKNOWN; w->then = TY_UNKNOWN; w->round = 0; w->reason = NULL;
}

void slot_rule(Compiler *c, LocalVar *lv, TyKind t, int node, const char *reason) {
  (void)c;
  if (ty_degraded(t) && !ty_degraded(lv->type) && lv->why.node < 0 && !lv->why.reason) {
    lv->why.node = node; lv->why.other = -1; lv->why.prev = lv->type; lv->why.then = t;
    lv->why.round = g_infer_round; lv->why.reason = reason;
  }
  lv->type = t;
}

int slot_set(Compiler *c, LocalVar *lv, TyKind merged, TyKind t, int node) {
  (void)c;
  if (merged == lv->type) {
    if (!ty_degraded(merged) && merged != TY_UNKNOWN && node >= 0) lv->last_src = node;
    return 0;
  }
  if (ty_degraded(merged) && !ty_degraded(lv->type)) {
    lv->why.node = node;
    lv->why.other = lv->last_src;
    lv->why.prev = lv->type;
    lv->why.then = t;
    lv->why.round = g_infer_round;
    lv->why.reason = NULL;   /* a value did this, whatever rule did before a reset */
  }
  else if (!ty_degraded(merged)) {
    /* re-derived concrete (the round's reset, or the re-narrow): the old
       why is stale */
    why_reset(&lv->why);
    if (node >= 0) lv->last_src = node;
  }
  lv->type = merged;
  return 1;
}

int slot_take(Compiler *c, LocalVar *lv, TyKind t, int node) {
  return slot_set(c, lv, ty_unify(lv->type, t), t, node);
}

/* Runtime typedefs `sp_<X>` a user class name could redefine. A user class whose
   name matches one gets a `u_`-prefixed C stem (`sp_u_<name>`) so its emitted
   struct/typedef/methods never collide with the runtime's own type. (Builtin
   class names like Range/Complex are reopened via the __oc_ path and never emit
   a fresh sp_<name> struct, so listing them here is merely harmless.) */
static int sp_name_collides_runtime(const char *n) {
  /* ONLY runtime-internal type names that are NOT Ruby classes. A builtin class
     (String, Range, Complex, Proc, ...) is reopened via the primitive/__oc_ path
     and never emits a fresh `sp_<name>` struct, so it does not collide -- and it
     must NOT be mangled, or the builtin-name checks (`sp_streq(dcn,"String")`)
     that share the C stem would stop matching. */
  static const char *const reserved[] = {
    "RbVal", "Val", "Bigint",
    "IntArray", "FloatArray", "StrArray", "PolyArray", "PtrArray",
    "IntIntHash", "IntStrHash", "StrIntHash", "StrStrHash", "SymPolyHash",
    "StrPolyHash", "PolyPolyHash", "BoundMethod", "Curry", "ProcCompose",
    "StrBuf", "Argf", "Argv", "condvar", "mutex", "queue", "thread",
    /* Random is a builtin handled specially (TY_RANDOM), never a user
       ClassInfo; a USER class/module named Random (`OpenSSL::Random`) would
       emit sp_Random_s and clash with the runtime sp_Random typedef. */
    "Random",
    /* runtime value types with no Ruby class of the same name: Process.times'
       struct (a `Benchmark::Tms` is a user class of that name), the range,
       rational and socket-option carriers, IO::Buffer's and Process::Status's */
    "Tms", "StrRange", "FloatRange", "BigRational", "RbValue", "SockOpt",
    "ProcessStatus", "IOBuffer",
    NULL };
  for (int i = 0; reserved[i]; i++) if (sp_streq(n, reserved[i])) return 1;
  return 0;
}
/* The C-identifier stem for a class: the name, disambiguated if it would clash
   with a runtime typedef. Caller owns the returned strdup'd string. */
static char *sp_class_c_name(const char *name) {
  if (!name) return NULL;
  if (!sp_name_collides_runtime(name)) return strdup(name);
  size_t n = strlen(name);
  char *m = malloc(n + 3);
  if (!m) return strdup(name);
  m[0] = 'u'; m[1] = '_'; memcpy(m + 2, name, n + 1);
  return m;
}
ClassInfo *comp_class_new(Compiler *c, const char *name, int def_node) {
  if (c->nclasses >= c->cclasses) {
    c->cclasses = c->cclasses ? c->cclasses * 2 : 8;
    c->classes = realloc(c->classes, sizeof(ClassInfo) * (size_t)c->cclasses);
  }
  ClassInfo *ci = &c->classes[c->nclasses++];
  c->anon_struct_ids_valid = 0;
  memset(ci, 0, sizeof(*ci));
  ci->ctor_reachable = 1;   /* conservatively, until compute_instantiated's early pass has looked */
  ci->name = name ? strdup(name) : NULL;
  ci->c_name = sp_class_c_name(name);
  ci->def_node = def_node;
  ci->parent = -1;
  ci->enclosing_class = -1;
  return ci;
}

/* name -> class index, frozen index (shares the scope-index freeze signal; see
   comp_scope_index_set_frozen). comp_class_index is a linear scan over classes
   called per node during the fixpoint -- O(lookups * classes). Built descending
   so the chain head is the lowest class index (first definition wins, matching
   the forward scan). While unfrozen (walk_scope / register passes add and rename
   classes with the count unchanged), fall back to the linear scan. */
static int ci_frozen = 0, ci_nclasses = -1, ci_buckets = 0;
static int *ci_next = NULL, *ci_head = NULL;
static void ci_build(Compiler *c) {
  int nc = c->nclasses;
  free(ci_next); free(ci_head);
  ci_buckets = nc > 0 ? nc : 1;
  ci_next = malloc((size_t)(nc > 0 ? nc : 1) * sizeof(int));
  ci_head = malloc((size_t)ci_buckets * sizeof(int));
  ci_nclasses = nc;
  if (!ci_next || !ci_head) { ci_buckets = 0; return; }
  for (int i = 0; i < ci_buckets; i++) ci_head[i] = -1;
  for (int i = nc - 1; i >= 0; i--) {
    if (!c->classes[i].name) continue;
    unsigned b = sp_strhash(c->classes[i].name) % (unsigned)ci_buckets;
    ci_next[i] = ci_head[b]; ci_head[b] = i;
  }
}
int comp_class_index(Compiler *c, const char *name) {
  if (!name) return -1;
  if (!ci_frozen) {
    for (int i = 0; i < c->nclasses; i++)
      if (c->classes[i].name && sp_streq(c->classes[i].name, name)) return i;
    return -1;
  }
  if (ci_nclasses != c->nclasses) ci_build(c);
  if (!ci_buckets) return -1;
  for (int i = ci_head[sp_strhash(name) % (unsigned)ci_buckets]; i >= 0; i = ci_next[i])
    if (c->classes[i].name && sp_streq(c->classes[i].name, name)) return i;
  return -1;
}

int comp_ivar_index(ClassInfo *ci, const char *name) {
  for (int i = 0; i < ci->nivars; i++)
    if (sp_streq(ci->ivars[i], name)) return i;
  return -1;
}

int comp_ivar_intern(ClassInfo *ci, const char *name) {
  int idx = comp_ivar_index(ci, name);
  if (idx >= 0) return idx;
  if (ci->nivars >= ci->civars) {
    ci->civars = ci->civars ? ci->civars * 2 : 8;
    ci->ivars = realloc(ci->ivars, sizeof(char *) * (size_t)ci->civars);
    ci->ivar_types = realloc(ci->ivar_types, sizeof(TyKind) * (size_t)ci->civars);
    ci->ivar_str_shared = realloc(ci->ivar_str_shared, (size_t)ci->civars);
    ci->ivar_int_table = realloc(ci->ivar_int_table, (size_t)ci->civars);
    ci->ivar_oa_type = realloc(ci->ivar_oa_type, sizeof(TyKind) * (size_t)ci->civars);
    ci->ivar_oa_seed = realloc(ci->ivar_oa_seed, sizeof(int) * (size_t)ci->civars);
    ci->ivar_oa_conflict = realloc(ci->ivar_oa_conflict, (size_t)ci->civars);
    ci->ivar_nullable_int = realloc(ci->ivar_nullable_int, (size_t)ci->civars);
    ci->ivar_nullable_int_elem = realloc(ci->ivar_nullable_int_elem, (size_t)ci->civars);
    ci->ivar_arr_elem_arr_or_nil = realloc(ci->ivar_arr_elem_arr_or_nil, (size_t)ci->civars);
  }
  ci->ivars[ci->nivars] = strdup(name);
  ci->ivar_types[ci->nivars] = TY_UNKNOWN;
  ci->ivar_str_shared[ci->nivars] = 0;
  ci->ivar_int_table[ci->nivars] = 0;
  ci->ivar_oa_type[ci->nivars] = TY_UNKNOWN;
  ci->ivar_oa_seed[ci->nivars] = 0;
  ci->ivar_oa_conflict[ci->nivars] = 0;
  ci->ivar_nullable_int[ci->nivars] = 0;
  ci->ivar_nullable_int_elem[ci->nivars] = 0;
  ci->ivar_arr_elem_arr_or_nil[ci->nivars] = 0;
  return ci->nivars++;
}

/* Intern a Struct/Data member's backing ivar as the next member: members sit
   at ivars[0..nmembers), in declaration order, ahead of any other ivar the
   class already interned (an attr's, or one a method assigns). */
int comp_member_intern(ClassInfo *ci, const char *name) {
  int idx = comp_ivar_intern(ci, name);
  if (idx < ci->nmembers) return idx;
  int m = ci->nmembers;
  if (idx != m) {
#define IV_SWAP(arr, T) do { T _t = ci->arr[idx]; ci->arr[idx] = ci->arr[m]; ci->arr[m] = _t; } while (0)
    IV_SWAP(ivars, char *);
    IV_SWAP(ivar_types, TyKind);
    IV_SWAP(ivar_str_shared, unsigned char);
    IV_SWAP(ivar_int_table, unsigned char);
    IV_SWAP(ivar_oa_type, TyKind);
    IV_SWAP(ivar_oa_seed, int);
    IV_SWAP(ivar_oa_conflict, unsigned char);
    IV_SWAP(ivar_nullable_int, unsigned char);
    IV_SWAP(ivar_nullable_int_elem, unsigned char);
    IV_SWAP(ivar_arr_elem_arr_or_nil, unsigned char);
#undef IV_SWAP
  }
  return ci->nmembers++;
}

/* The member index of a Struct/Data's `@name`, or -1 for a name that is no
   member (an attr's or a method's own ivar is not one). */
int comp_member_index(ClassInfo *ci, const char *name) {
  int idx = comp_ivar_index(ci, name);
  return idx < ci->nmembers ? idx : -1;
}

int comp_cvar_index(ClassInfo *ci, const char *name) {
  for (int i = 0; i < ci->ncvars; i++)
    if (sp_streq(ci->cvars[i], name)) return i;
  return -1;
}

/* A subclass shares the class variable a superclass declares, so the slot (one
   file-scope static per declaring class) belongs to the topmost ancestor that
   has an entry for `name`, or to `cid` itself when none does. Every reader and
   writer of a class variable goes through here before it looks the entry up or
   builds the slot's name. */
int comp_cvar_owner(const Compiler *c, int cid, const char *name) {
  int owner = cid;
  if (cid < 0 || !name) return cid;
  for (int p = c->classes[cid].parent; p >= 0; p = c->classes[p].parent)
    if (comp_cvar_index(&c->classes[p], name) >= 0) owner = p;
  return owner;
}

int comp_cvar_intern(ClassInfo *ci, const char *name) {
  int idx = comp_cvar_index(ci, name);
  if (idx >= 0) return idx;
  if (ci->ncvars >= ci->ccvars) {
    ci->ccvars = ci->ccvars ? ci->ccvars * 2 : 8;
    ci->cvars = realloc(ci->cvars, sizeof(char *) * (size_t)ci->ccvars);
    ci->cvar_types = realloc(ci->cvar_types, sizeof(TyKind) * (size_t)ci->ccvars);
    ci->cvar_nullable_int = realloc(ci->cvar_nullable_int, (size_t)ci->ccvars);
  }
  ci->cvars[ci->ncvars] = strdup(name);
  ci->cvar_types[ci->ncvars] = TY_UNKNOWN;
  ci->cvar_nullable_int[ci->ncvars] = 0;
  return ci->ncvars++;
}

/* Scopes moved to another class for an instance_exec block, each with the
   class it was defined in (comp_scope_move_begin). The move is for the
   block's self and ivars; which methods a class defines does not change
   with it. The index below is dropped on every comp_scope_index_set_frozen,
   frozen or not, so it can be rebuilt during a move, and keyed by the
   current class it filed a moved Evaluator.label under Plain#label's key: a
   poly dispatch of `label` in the block then called the class method for a
   Plain and raised ArgumentError. So the index keys every scope by its own
   class, and a lookup skips a scope while it is moved: under its own class
   it would be named by the class it was moved to. A scope moved twice (a
   block in a block) keeps its first record. */
static int *mv_s = NULL, *mv_cls = NULL, *mv_cm = NULL;
static int mv_n = 0, mv_cap = 0;
static Compiler *mv_c = NULL;
void comp_scope_move_begin(Compiler *c, int s) {
  if (mv_n == mv_cap) {
    mv_cap = mv_cap ? mv_cap * 2 : 8;
    mv_s = realloc(mv_s, sizeof(int) * (size_t)mv_cap);
    mv_cls = realloc(mv_cls, sizeof(int) * (size_t)mv_cap);
    mv_cm = realloc(mv_cm, sizeof(int) * (size_t)mv_cap);
    if (!mv_s || !mv_cls || !mv_cm) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  mv_s[mv_n] = s; mv_cls[mv_n] = c->scopes[s].class_id; mv_cm[mv_n] = c->scopes[s].is_cmethod;
  mv_n++; mv_c = c;
}
void comp_scope_move_end(void) { if (mv_n > 0) mv_n--; }
int comp_scope_move_depth(void) { return mv_n; }
/* A refusal longjmps out of the emission (a unit abandoned, a probe's arm
   dropped) past the code that moves a scope back, which left the scope in
   the other class: the class method a later unit called was then not found
   and refused as well. The recovery puts back every move made since the
   depth it saved, the latest first, so each scope ends with the class it had
   before its first move. */
void comp_scope_move_unwind(int depth) {
  while (mv_n > depth && mv_n > 0) {
    mv_n--;
    mv_c->scopes[mv_s[mv_n]].class_id = mv_cls[mv_n];
    mv_c->scopes[mv_s[mv_n]].is_cmethod = mv_cm[mv_n];
  }
}
int comp_scope_own_class(const Compiler *c, int s, int *is_cmethod) {
  for (int i = 0; i < mv_n; i++)
    if (mv_s[i] == s) { if (is_cmethod) *is_cmethod = mv_cm[i]; return mv_cls[i]; }
  if (is_cmethod) *is_cmethod = c->scopes[s].is_cmethod;
  return c->scopes[s].class_id;
}
static int sm_moved(int s) {
  for (int i = 0; i < mv_n; i++) if (mv_s[i] == s) return 1;
  return 0;
}
/* scope `s` is a method of (class_id, is_cm) a lookup may answer: not while
   it is moved */
static int sm_owns(const Compiler *c, int s, int class_id, int is_cm) {
  return c->scopes[s].class_id == class_id && (int)c->scopes[s].is_cmethod == is_cm &&
         (!mv_n || !sm_moved(s));
}
/* the same for a top-level method */
static int sm_toplevel(const Compiler *c, int s) {
  return c->scopes[s].class_id < 0 && (!mv_n || !sm_moved(s));
}

/* (class_id, name, is_cmethod) -> scope index, cached per scope count. Both
   lookups below otherwise scan all scopes in reverse, and they are called many
   times per node during the inference fixpoint (O(lookups * scopes)). The chain
   is head-inserted in ascending scope order, so it is in descending order and
   the first matching entry is the highest scope index -- the same "later
   definition wins" semantics as the reverse linear scan. Rebuilt when the scope
   count changes (no scopes are added during the fixpoint). */
static unsigned sm_hash(int class_id, const char *name, int is_cm) {
  unsigned h = 2166136261u;
  for (const char *p = name; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  h ^= (unsigned)class_id * 2654435761u;
  h ^= (unsigned)(is_cm ? 0x9e3779b9u : 0);
  return h;
}
static int sm_nscopes = -1, sm_buckets = 0, sm_frozen = 0;
static int *sm_next = NULL, *sm_head = NULL;
/* Parallel index for top-level methods (class_id < 0), keyed by name. Built in
   descending scope order so the chain head is the lowest scope index -- matching
   comp_method_index's "first definition wins" forward scan. */
static int *tm_next = NULL, *tm_head = NULL;
/* The index is only safe once scope shape (count + each scope's class_id/name/
   is_cmethod) stops changing: walk_scope and the register_* / prepend passes
   create and *rename* scopes, and a rename leaves the count unchanged, so a
   count-keyed cache would go stale. analyze_program freezes the index just
   before the inference fixpoint (where lookups are hottest and scope shape is
   fixed) and unfreezes at entry. While unfrozen, fall back to the linear scan. */
/* Bumped on every freeze/unfreeze transition so consumers that cache a result
   which is only stable while scope shape is fixed (e.g. hash_new_default_arg's
   per-node memo) can stamp their cache and discard it when the epoch changes. */
static unsigned sm_gen = 0;
void comp_scope_index_set_frozen(int f) { if (sm_frozen != f) sm_gen++; sm_frozen = f; sm_nscopes = -1; ci_frozen = f; ci_nclasses = -1; }
int comp_scope_index_is_frozen(void) { return sm_frozen; }
unsigned comp_scope_index_gen(void) { return sm_gen; }
static void sm_build(Compiler *c) {
  int ns = c->nscopes;
  free(sm_next); free(sm_head); free(tm_next); free(tm_head);
  sm_buckets = ns > 0 ? ns : 1;
  sm_next = malloc((size_t)(ns > 0 ? ns : 1) * sizeof(int));
  sm_head = malloc((size_t)sm_buckets * sizeof(int));
  tm_next = malloc((size_t)(ns > 0 ? ns : 1) * sizeof(int));
  tm_head = malloc((size_t)sm_buckets * sizeof(int));
  sm_nscopes = ns;
  if (!sm_next || !sm_head || !tm_next || !tm_head) { sm_buckets = 0; return; }
  for (int i = 0; i < sm_buckets; i++) { sm_head[i] = -1; tm_head[i] = -1; }
  for (int s = 0; s < ns; s++) {
    if (!c->scopes[s].name) continue;
    int cm, cls = comp_scope_own_class(c, s, &cm);
    if (cls >= 0) {
      unsigned b = sm_hash(cls, c->scopes[s].name, cm) % (unsigned)sm_buckets;
      sm_next[s] = sm_head[b]; sm_head[b] = s;
    }
  }
  /* top-level methods: ascending so the highest scope index ends at the head.
     A redefined top-level method is its LAST def, as for a class (the
     class buckets above are built the same way) */
  for (int s = 0; s < ns; s++) {
    if (comp_scope_own_class(c, s, NULL) >= 0 || !c->scopes[s].name) continue;
    unsigned b = sm_hash(-1, c->scopes[s].name, 0) % (unsigned)sm_buckets;
    tm_next[s] = tm_head[b]; tm_head[b] = s;
  }
}
static int sm_lookup(Compiler *c, int class_id, const char *name, int is_cm) {
  if (!name) return -1;
  if (!sm_frozen) {
    /* scope shape may still change: scan in reverse so a later (reopened /
       transplanted) definition wins, matching the frozen index's ordering. */
    for (int s = c->nscopes - 1; s >= 0; s--)
      if (sm_owns(c, s, class_id, is_cm) &&
          c->scopes[s].name && sp_streq(c->scopes[s].name, name)) return s;
    return -1;
  }
  if (sm_nscopes != c->nscopes) sm_build(c);
  if (!sm_buckets) return -1;
  unsigned b = sm_hash(class_id, name, is_cm) % (unsigned)sm_buckets;
  for (int s = sm_head[b]; s >= 0; s = sm_next[s])
    if (sm_owns(c, s, class_id, is_cm) &&
        c->scopes[s].name && sp_streq(c->scopes[s].name, name)) return s;
  return -1;
}

int comp_method_in_class(Compiler *c, int class_id, const char *name) {
  return sm_lookup(c, class_id, name, 0);
}

int comp_cmethod_in_class(Compiler *c, int class_id, const char *name) {
  return sm_lookup(c, class_id, name, 1);
}
int comp_cmethod_in_chain(Compiler *c, int class_id, const char *name, int *def_class) {
  int start = class_id;
  name = comp_resolve_alias_at(c, class_id, name, &start);
  for (int cid = start; cid >= 0; cid = c->classes[cid].parent) {
    int mi = comp_cmethod_in_class(c, cid, name);
    if (mi >= 0) { if (def_class) *def_class = cid; return mi; }
  }
  return -1;
}

int comp_method_in_chain(Compiler *c, int class_id, const char *name, int *def_class) {
  int start = class_id;
  name = comp_resolve_alias_at(c, class_id, name, &start);
  for (int cid = start; cid >= 0; cid = c->classes[cid].parent) {
    int mi = comp_method_in_class(c, cid, name);
    if (mi >= 0) { if (def_class) *def_class = cid; return mi; }
  }
  return -1;
}

/* The method a program's reopen of a builtin kind's own class defines under
   `name` (`class Array; def first ...`), or -1: the class itself, not an
   ancestor, since an Object reopen does not displace Array#first. */
int comp_builtin_kind_reopen_mi(Compiler *c, TyKind t, const char *name) {
  const char *cn = t == TY_INT ? "Integer" : t == TY_FLOAT ? "Float"
                 : t == TY_STRING ? "String" : t == TY_SYMBOL ? "Symbol"
                 : (ty_is_array(t) || ty_is_obj_array(t)) ? "Array"
                 : ty_is_hash(t) ? "Hash" : NULL;
  if (!cn || !name) return -1;
  int ci = comp_class_index(c, cn);
  if (ci < 0) return -1;
  int dc = -1, mi = comp_method_in_chain(c, ci, name, &dc);
  /* under the name itself: an alias resolving to the reopen's name is not it */
  return mi >= 0 && dc == ci && c->scopes[mi].name && sp_streq(c->scopes[mi].name, name) ? mi : -1;
}

static void vis_table_set(char ***names, int **kinds, int *n, int *cap, const char *name, int kind) {
  if (!name) return;
  for (int i = 0; i < *n; i++)
    if (sp_streq((*names)[i], name)) { (*kinds)[i] = kind; return; }
  if (*n >= *cap) {
    *cap = *cap ? *cap * 2 : 8;
    char **nn = realloc(*names, sizeof(char *) * (size_t)*cap);
    int *nk = realloc(*kinds, sizeof(int) * (size_t)*cap);
    if (!nn || !nk) { fprintf(stderr, "out of memory\n"); exit(1); }
    *names = nn; *kinds = nk;
  }
  (*names)[*n] = strdup(name);
  (*kinds)[*n] = kind;
  (*n)++;
}

void comp_method_vis_set(ClassInfo *ci, const char *name, int kind) {
  vis_table_set(&ci->vis_names, &ci->vis_kinds, &ci->nvis, &ci->cvis, name, kind);
}

void comp_cmethod_vis_set(ClassInfo *ci, const char *name, int kind) {
  vis_table_set(&ci->cm_vis_names, &ci->cm_vis_kinds, &ci->ncm_vis, &ci->ccm_vis, name, kind);
}

int comp_cmethod_vis_declared(Compiler *c, int class_id, const char *name, int *at) {
  if (!name) return SP_VIS_PUBLIC;
  for (int cid = class_id; cid >= 0; cid = c->classes[cid].parent) {
    ClassInfo *ci = &c->classes[cid];
    for (int i = 0; i < ci->ncm_vis; i++)
      if (sp_streq(ci->cm_vis_names[i], name)) { if (at) *at = cid; return ci->cm_vis_kinds[i]; }
  }
  return SP_VIS_PUBLIC;
}

int comp_method_vis(ClassInfo *ci, const char *name) {
  if (!name) return SP_VIS_PUBLIC;
  for (int i = 0; i < ci->nvis; i++)
    if (sp_streq(ci->vis_names[i], name)) return ci->vis_kinds[i];
  return SP_VIS_PUBLIC;
}

int comp_method_vis_in_chain(Compiler *c, int class_id, const char *name) {
  return comp_method_vis_declared(c, class_id, name, NULL);
}
/* The same, reporting the class that declared the visibility in *at. A
   method copied in from an included module carries the module's declaration:
   the copy's scope names the module (origin_module_ci) and the module's own
   table holds the `private`/`protected` its body said. */
static int vis_declared_in_chain(Compiler *c, int class_id, const char *name, int *at) {
  for (int cid = class_id; cid >= 0; cid = c->classes[cid].parent) {
    ClassInfo *ci = &c->classes[cid];
    for (int i = 0; i < ci->nvis; i++)
      if (sp_streq(ci->vis_names[i], name)) { if (at) *at = cid; return ci->vis_kinds[i]; }
  }
  return -1;
}

/* An alias is a method of its own: `alias pz z; private :pz` makes only pz
   private, and the alias carries the visibility its target had at the alias
   (register_method_visibility records it), so the alias's own name is looked
   up before the name it resolves to. */
int comp_method_vis_declared(Compiler *c, int class_id, const char *name, int *at) {
  const char *alias = name;
  int v = vis_declared_in_chain(c, class_id, alias, at);
  if (v >= 0) return v;
  name = comp_resolve_alias(c, class_id, name);
  if (name != alias && (v = vis_declared_in_chain(c, class_id, name, at)) >= 0) return v;
  int mi = comp_method_in_chain(c, class_id, name, NULL);
  if (mi >= 0 && mi < c->nscopes && c->scopes[mi].origin_module_ci > 0) {
    int mci = c->scopes[mi].origin_module_ci - 1;
    if (mci >= 0 && mci < c->nclasses) {
      ClassInfo *mi_ci = &c->classes[mci];
      for (int pass = 0; pass < 2; pass++) {
        const char *nm = pass ? name : alias;
        for (int i = 0; i < mi_ci->nvis; i++)
          if (sp_streq(mi_ci->vis_names[i], nm)) { if (at) *at = mci; return mi_ci->vis_kinds[i]; }
      }
    }
  }
  return SP_VIS_PUBLIC;
}

/* Detect an instance_eval/exec trampoline method: a method whose body is a
   single `instance_eval(&block)` / `instance_exec(args, &block)` that forwards
   the method's own `&block` parameter. A call `recv.M(args) { ... }` to such a
   method is compiled exactly like `recv.instance_eval/exec(args) { ... }`.
   Returns 1 for an instance_eval trampoline, 2 for instance_exec, 0 otherwise.
   When non-zero and def_class is non-NULL, it receives the defining class. */
int comp_trampoline_kind(Compiler *c, int class_id, const char *name, int *def_class) {
  const NodeTable *nt = c->nt;
  int dc = -1;
  int mi = comp_method_in_chain(c, class_id, name, &dc);
  if (mi < 0) return 0;
  Scope *s = &c->scopes[mi];
  if (!s->blk_param || !s->blk_param[0]) return 0;  /* needs a named &block */
  if (s->body < 0) return 0;
  int bn = 0; const int *bb = nt_arr(nt, s->body, "body", &bn);
  if (bn != 1 || !bb) return 0;
  int call = bb[0];
  const char *ct = nt_type(nt, call);
  if (!ct || !sp_streq(ct, "CallNode")) return 0;
  if (nt_ref(nt, call, "receiver") >= 0) return 0;  /* must be receiverless */
  const char *cn = nt_str(nt, call, "name");
  if (!cn) return 0;
  int kind = sp_streq(cn, "instance_eval") ? 1 : sp_streq(cn, "instance_exec") ? 2 : 0;
  if (!kind) return 0;
  /* The block arg must forward the method's own &block parameter. */
  int barg = nt_ref(nt, call, "block");
  if (barg < 0) return 0;
  int bexpr = nt_ref(nt, barg, "expression");
  if (bexpr < 0) return 0;
  const char *bvn = nt_str(nt, bexpr, "name");
  if (!bvn || !sp_streq(bvn, s->blk_param)) return 0;
  if (def_class) *def_class = dc;
  return kind;
}

/* Stage-1 static fold for a module-level singleton accessor that holds a
   constant: if `Class.base = SomeConst` appears exactly once program-wide and
   every write is the same constant-resolvable class/module, return that
   class's index; otherwise -1 (polymorphic / non-constant -> runtime path). */
/* Collect the distinct constant class indices assigned to `Class.base = Const`
   across the whole program (deduped, in first-seen order). Returns the count, or
   -1 if any write's RHS is not a constant-resolvable class. */
/* Index of constant-setter CallNodes (`name` ends in '=', constant receiver),
   cached per node table. comp_sg_const_candidates is called once per singleton
   accessor read during the inference fixpoint; scanning every node each time
   made it O(reads * nodes * iterations). The structural shape indexed here is
   stable across the pass; per-call filters (base, receiver class, arg) still
   run fresh. */
static const NodeTable *sgc_nt = NULL;
static int *sgc_ids = NULL;
static int sgc_n = 0, sgc_ntc = -1;
int comp_sg_const_candidates(Compiler *c, int class_id, const char *base, int *out, int max) {
  const NodeTable *nt = c->nt;
  const char *cls_name = c->classes[class_id].name;
  size_t blen = strlen(base);
  int count = 0;
  if (sgc_nt != nt || sgc_ntc != nt->count) {
    free(sgc_ids);
    sgc_ids = malloc((size_t)nt->count * sizeof(int));
    sgc_n = 0;
    if (sgc_ids) {
      for (int id = 0; id < nt->count; id++) {
        const char *ty = nt_type(nt, id);
        if (!ty || !sp_streq(ty, "CallNode")) continue;
        const char *nm = nt_str(nt, id, "name");
        if (!nm) continue;
        size_t nl = strlen(nm);
        if (nl == 0 || nm[nl - 1] != '=') continue;
        int recv = nt_ref(nt, id, "receiver");
        if (recv < 0) continue;
        const char *rty = nt_type(nt, recv);
        if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode")))
          sgc_ids[sgc_n++] = id;
      }
    }
    sgc_nt = nt;
    sgc_ntc = nt->count;
  }
  for (int ii = 0; ii < sgc_n; ii++) {
    int id = sgc_ids[ii];
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    size_t nl = strlen(nm);
    if (nl != blen + 1 || nm[nl - 1] != '=' || strncmp(nm, base, blen)) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || !sp_streq(rn, cls_name)) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an != 1) return -1;
    const char *vty = nt_type(nt, av[0]);
    if (!vty || (!sp_streq(vty, "ConstantReadNode") && !sp_streq(vty, "ConstantPathNode"))) return -1;
    int rci = comp_class_index(c, nt_str(nt, av[0], "name"));
    if (rci < 0) return -1;
    int seen = 0;
    for (int j = 0; j < count; j++) if (out[j] == rci) { seen = 1; break; }
    if (!seen && count < max) out[count++] = rci;
  }
  return count;
}

/* Stage-1 fold: the accessor holds a single distinct constant program-wide. */
int comp_sg_const_binding(Compiler *c, int class_id, const char *base) {
  int cand[32];
  int n = comp_sg_const_candidates(c, class_id, base, cand, 32);
  return n == 1 ? cand[0] : -1;
}

/* If `call_id` reads a module singleton accessor (`Class.reader`) whose value
   folds to a single constant via comp_sg_const_binding, return that constant's
   class index; otherwise -1. */
int comp_sg_reader_const(Compiler *c, int call_id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, call_id);
  if (!ty || !sp_streq(ty, "CallNode")) return -1;
  if (nt_ref(nt, call_id, "block") >= 0) return -1;
  if (nt_ref(nt, call_id, "arguments") >= 0) return -1;
  const char *nm = nt_str(nt, call_id, "name");
  if (!nm) return -1;
  size_t nl = strlen(nm);
  if (nl > 0 && nm[nl - 1] == '=') return -1;
  int recv = nt_ref(nt, call_id, "receiver");
  if (recv < 0) return -1;
  const char *rty = nt_type(nt, recv);
  if (!rty || (!sp_streq(rty, "ConstantReadNode") && !sp_streq(rty, "ConstantPathNode"))) return -1;
  const char *rn = nt_str(nt, recv, "name");
  int ci = rn ? comp_class_index(c, rn) : -1;
  if (ci < 0 || !comp_is_sg_reader(&c->classes[ci], nm)) return -1;
  return comp_sg_const_binding(c, ci, nm);
}

/* If `call_id` reads a module singleton accessor written with 2+ distinct
   constants (Stage-2), fill out[] with those class indices and return the
   count; otherwise 0 (or -1 if a non-constant RHS makes it un-resolvable). */
int comp_sg_reader_candidates(Compiler *c, int call_id, int *out, int max) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, call_id);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  if (nt_ref(nt, call_id, "block") >= 0) return 0;
  if (nt_ref(nt, call_id, "arguments") >= 0) return 0;
  const char *nm = nt_str(nt, call_id, "name");
  if (!nm) return 0;
  size_t nl = strlen(nm);
  if (nl > 0 && nm[nl - 1] == '=') return 0;
  int recv = nt_ref(nt, call_id, "receiver");
  if (recv < 0) return 0;
  const char *rty = nt_type(nt, recv);
  if (!rty || (!sp_streq(rty, "ConstantReadNode") && !sp_streq(rty, "ConstantPathNode"))) return 0;
  const char *rn = nt_str(nt, recv, "name");
  int ci = rn ? comp_class_index(c, rn) : -1;
  if (ci < 0 || !comp_is_sg_reader(&c->classes[ci], nm)) return 0;
  return comp_sg_const_candidates(c, ci, nm, out, max);
}

/* A constant name that resolves to something at compile time: a value
   constant, a registered class/module, or a well-known builtin the
   DefinedNode emit answers "constant" for (keep the list in sync with the
   ConstantReadNode arm in codegen_expr.c). */
/* Is `cn` defined at the TOP LEVEL (a `::cn` anchor's requirement)? The
   class/constant tables are flat by short name -- a module nested inside
   another module registers under its own name -- so `defined?(::Rails)`
   inside `module Underscore::Rails` must not resolve through the nested
   entry. Walks the AST: a Class/Module/ConstantWrite of that name counts
   only when not enclosed by another class/module body. Builtins count. */
static int const_top_level_walk(Compiler *c, int node, const char *cn, int depth) {
  const NodeTable *nt = c->nt;
  if (node < 0 || !nt_type(nt, node)) return 0;
  const char *ty = nt_type(nt, node);
  int is_mod = sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode");
  if (is_mod || sp_streq(ty, "ConstantWriteNode")) {
    const char *nm = NULL;
    if (is_mod) {
      int cp = nt_ref(nt, node, "constant_path");
      /* a qualified definition (`module A::B`) defines B under A, so only
         an unqualified path can define the TOP-LEVEL name */
      if (cp >= 0 && nt_type(nt, cp) && sp_streq(nt_type(nt, cp), "ConstantReadNode"))
        nm = nt_str(nt, cp, "name");
    }
    else nm = nt_str(nt, node, "name");
    if (nm && depth == 0 && sp_streq(nm, cn)) return 1;
  }
  int ndepth = depth + (is_mod ? 1 : 0);
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++)
    if (const_top_level_walk(c, nt_ref_at(nt, node, i), cn, ndepth)) return 1;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, node, i, &n);
    for (int k = 0; k < n; k++)
      if (const_top_level_walk(c, ids[k], cn, ndepth)) return 1;
  }
  return 0;
}
int comp_is_wellknown_const(const char *cn) {
  static const char *const wellknown[] = {
    "Object", "BasicObject", "Kernel", "Module", "Class", "Array", "Hash",
    "String", "Integer", "Float", "Symbol", "Regexp", "Range", "NilClass",
    "TrueClass", "FalseClass", "Numeric", "Comparable", "Enumerable",
    "IO", "File", "Dir", "Math", "GC", "Process", "ENV", "ARGV",
    "STDOUT", "STDERR", "STDIN", "RUBY_VERSION", "RUBY_ENGINE", "RUBY_ENGINE_VERSION",
    "RUBY_PLATFORM", "RUBY_RELEASE_DATE", "RUBY_REVISION", "RUBY_COPYRIGHT", "RUBY_DESCRIPTION",
    "RUBY_PATCHLEVEL", NULL };
  for (int bi = 0; wellknown[bi]; bi++) if (sp_streq(cn, wellknown[bi])) return 1;
  return 0;
}
int const_name_resolves_top_level(Compiler *c, const char *cn) {
  if (!cn) return 0;
  if (comp_is_wellknown_const(cn)) return 1;
  return const_top_level_walk(c, c->nt->root_id, cn, 0);
}
static int const_name_resolves(Compiler *c, const char *cn) {
  if (!cn) return 0;
  if (comp_const(c, cn) || comp_class_index(c, cn) >= 0) return 1;
  if (comp_is_wellknown_const(cn)) return 1;
  return 0;
}

/* Statically-false `defined?(Const)` guard: the predicate is a DefinedNode
   (or a `defined?(Const) && ...` AndNode, whose left arm short-circuits the
   whole conjunction to nil) over a constant / constant path that resolves to
   nothing at compile time. A constant path (A::B::C) needs every segment to
   resolve for the path to possibly exist; one unresolved segment makes the
   whole path -- and the guard -- statically false (`RubyVM::YJIT` folds on
   RubyVM, and `MissingParent::String` folds on MissingParent despite its
   builtin tail). Such an if-branch is compile-time dead -- doom's
   `if defined?(RubyVM::YJIT) && RubyVM::YJIT.enabled?` MRI hack -- so call
   reachability must not walk it and codegen must not emit it. Deliberately
   narrow: only constants/constant paths, no dynamic defined? forms. */
int comp_defined_guard_false(Compiler *c, int pred) {
  const NodeTable *nt = c->nt;
  if (pred < 0) return 0;
  const char *pt = nt_type(nt, pred);
  if (!pt) return 0;
  /* `defined?(X) && rest`: falseness of the left arm decides the whole
     predicate (recurses to cover chained `&&`s, which nest leftward). */
  if (sp_streq(pt, "AndNode"))
    return comp_defined_guard_false(c, nt_ref(nt, pred, "left"));
  if (!sp_streq(pt, "DefinedNode")) return 0;
  int v = nt_ref(nt, pred, "value");
  if (v < 0) return 0;
  const char *vt = nt_type(nt, v);
  if (vt && sp_streq(vt, "ConstantReadNode"))
    return !const_name_resolves(c, nt_str(nt, v, "name"));
  if (vt && sp_streq(vt, "ConstantPathNode")) {
    /* Walk tail-to-root; any unresolved segment kills the whole path. */
    for (int seg = v; ; ) {
      if (!const_name_resolves(c, nt_str(nt, seg, "name"))) return 1;
      int par = nt_ref(nt, seg, "parent");
      if (par < 0) return !const_name_resolves_top_level(c, nt_str(nt, seg, "name"));
      const char *pty = nt_type(nt, par);
      if (pty && sp_streq(pty, "ConstantPathNode")) { seg = par; continue; }
      if (pty && sp_streq(pty, "ConstantReadNode"))
        return !const_name_resolves(c, nt_str(nt, par, "name"));
      return 0;                                  /* dynamic parent: no fold */
    }
  }
  return 0;
}

/* The dual: a defined? guard that is statically TRUE -- every segment of the
   constant / constant path resolves at compile time. Lets value-position
   `defined?(K) ? K : fallback` drop its dead fallback arm (which may not
   type-unify with the live one). Deliberately narrow like
   comp_defined_guard_false: plain constants / constant paths only, no `&&`
   chains (a truthy left arm doesn't decide the conjunction). */
int comp_defined_guard_true(Compiler *c, int pred) {
  const NodeTable *nt = c->nt;
  if (pred < 0) return 0;
  const char *pt = nt_type(nt, pred);
  if (!pt || !sp_streq(pt, "DefinedNode")) return 0;
  int v = nt_ref(nt, pred, "value");
  if (v < 0) return 0;
  const char *vt = nt_type(nt, v);
  if (vt && sp_streq(vt, "ConstantReadNode"))
    return const_name_resolves(c, nt_str(nt, v, "name"));
  if (vt && sp_streq(vt, "ConstantPathNode")) {
    for (int seg = v; ; ) {
      if (!const_name_resolves(c, nt_str(nt, seg, "name"))) return 0;
      int par = nt_ref(nt, seg, "parent");
      /* `::Root` anchor: the root must be defined at the TOP level, not
         through a same-named nested module (#3320) */
      if (par < 0) return const_name_resolves_top_level(c, nt_str(nt, seg, "name"));
      const char *pty = nt_type(nt, par);
      if (pty && sp_streq(pty, "ConstantPathNode")) { seg = par; continue; }
      if (pty && sp_streq(pty, "ConstantReadNode"))
        return const_name_resolves(c, nt_str(nt, par, "name"));
      return 0;                                  /* dynamic parent: no fold */
    }
  }
  return 0;
}

/* A literal ArrayNode whose elements are all integer literals (or empty). */
static int is_int_array_literal(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty || !sp_streq(ty, "ArrayNode")) return 0;
  int en = 0; const int *els = nt_arr(nt, node, "elements", &en);
  for (int i = 0; i < en; i++) {
    const char *et = nt_type(nt, els[i]);
    if (!et || !sp_streq(et, "IntegerNode")) return 0;
  }
  return 1;
}

/* A literal nested array `[[..ints..], ...]` -- every element is itself an
   int-array literal. Used to type the inner arrays of a poly-array for fold
   operations (e.g. inject(&:&) set intersection over arrays of int arrays). */
int comp_is_nested_int_array_literal(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty || !sp_streq(ty, "ArrayNode")) return 0;
  int en = 0; const int *els = nt_arr(nt, node, "elements", &en);
  if (en == 0) return 0;
  for (int i = 0; i < en; i++)
    if (!is_int_array_literal(c, els[i])) return 0;
  return 1;
}

static int name_in(char **list, int n, const char *name) {
  for (int i = 0; i < n; i++) if (sp_streq(list[i], name)) return 1;
  return 0;
}
static void name_add(char ***list, int *n, int *cap, const char *name) {
  if (name_in(*list, *n, name)) return;
  if (*n >= *cap) {
    *cap = *cap ? *cap * 2 : 4;
    *list = realloc(*list, sizeof(char *) * (size_t)*cap);
  }
  (*list)[(*n)++] = strdup(name);
}
unsigned comp_table_gen = 0;   /* bumped when a reader or alias table grows (poly-candidate memo stamp) */
void comp_add_reader(ClassInfo *ci, const char *name) {
  comp_table_gen++;
  name_add(&ci->readers, &ci->nreaders, &ci->creaders, name);
}
void comp_add_writer(ClassInfo *ci, const char *name) {
  name_add(&ci->writers, &ci->nwriters, &ci->cwriters, name);
}
int comp_is_reader(ClassInfo *ci, const char *name) { return name_in(ci->readers, ci->nreaders, name); }
int comp_is_writer(ClassInfo *ci, const char *name) { return name_in(ci->writers, ci->nwriters, name); }

/* A plain setter name: `x=`, but not the operators that also end in `=`
   (`==`, `!=`, `<=`, `>=`, `===`) and not `[]=`, whose value form is its own. */
int name_is_plain_setter(const char *name) {
  size_t ln = name ? strlen(name) : 0;
  if (ln < 2 || name[ln - 1] != '=') return 0;
  char p = name[ln - 2];
  return p != '=' && p != '!' && p != '<' && p != '>' && p != ']';
}
/* Is CallNode `id` a setter written as an assignment (`obj.x = v`), whose
   value is the argument whatever the writer returns? A call the send desugar
   retargeted (`obj.send(:x=, v)`, marked send_blind / vis_enforce) is a plain
   method call: its value is what the method returns (#4921). */
int call_is_setter_assign(const NodeTable *nt, int id) {
  if (id < 0 || !name_is_plain_setter(nt_str(nt, id, "name"))) return 0;
  const char *sb = nt_str(nt, id, "send_blind");
  const char *ve = nt_str(nt, id, "vis_enforce");
  return !(sb && sb[0] == '1') && !(ve && ve[0] == '1');
}
/* The view a `parameters(lambda: v)` call asks for when v is the literal
   true (1), false (0) or nil (-1, the receiver's own): -2 for any other
   argument list. */
int proc_parameters_lambda_mode(const NodeTable *nt, int argc, const int *argv) {
  if (argc != 1 || !nt_type(nt, argv[0]) || !sp_streq(nt_type(nt, argv[0]), "KeywordHashNode"))
    return -2;
  int en = 0; const int *elems = nt_arr(nt, argv[0], "elements", &en);
  if (en != 1) return -2;
  int key = nt_ref(nt, elems[0], "key");
  const char *kn = key >= 0 ? nt_str(nt, key, "unescaped") : NULL;
  if (!kn && key >= 0) kn = nt_str(nt, key, "value");
  int val = nt_ref(nt, elems[0], "value");
  const char *vty = val >= 0 ? nt_type(nt, val) : NULL;
  if (!kn || !sp_streq(kn, "lambda") || !vty) return -2;
  if (sp_streq(vty, "TrueNode")) return 1;
  if (sp_streq(vty, "FalseNode")) return 0;
  if (sp_streq(vty, "NilNode")) return -1;
  return -2;
}
/* The attribute a setter name writes: "x=" -> "x". 0 when the name is not a
   plain setter or does not fit. */
int setter_base_name(const char *name, char *out, size_t cap) {
  if (!name_is_plain_setter(name)) return 0;
  size_t ln = strlen(name) - 1;
  if (ln >= cap) return 0;
  memcpy(out, name, ln); out[ln] = '\0';
  return 1;
}
void comp_add_undef(ClassInfo *ci, const char *name) {
  name_add(&ci->undefs, &ci->nundefs, &ci->cundefs, name);
}
int comp_is_undeffed_in_chain(Compiler *c, int class_id, const char *name) {
  for (int cid = class_id; cid >= 0; cid = c->classes[cid].parent) {
    if (name_in(c->classes[cid].undefs, c->classes[cid].nundefs, name)) return 1;
    if (comp_method_in_class(c, cid, name) >= 0) return 0;
  }
  return 0;
}
void comp_add_sg_reader(ClassInfo *ci, const char *name) {
  name_add(&ci->sg_readers, &ci->nsg_readers, &ci->csg_readers, name);
}
void comp_add_sg_writer(ClassInfo *ci, const char *name) {
  name_add(&ci->sg_writers, &ci->nsg_writers, &ci->csg_writers, name);
}
void comp_add_sg_civ(ClassInfo *ci, const char *name) {
  if (name) name_add(&ci->sg_civ, &ci->nsg_civ, &ci->csg_civ, name);
}
int comp_is_sg_civ(ClassInfo *ci, const char *name) { return name_in(ci->sg_civ, ci->nsg_civ, name); }
int comp_is_sg_inh(ClassInfo *ci, const char *name) { return name_in(ci->sg_inh, ci->nsg_inh, name); }
void comp_add_sg_inh(ClassInfo *ci, const char *name) {
  if (name_in(ci->sg_inh, ci->nsg_inh, name)) return;
  if (ci->nsg_inh >= ci->csg_inh) {
    ci->csg_inh = ci->csg_inh ? ci->csg_inh * 2 : 4;
    ci->sg_inh = realloc(ci->sg_inh, sizeof(char *) * (size_t)ci->csg_inh);
  }
  ci->sg_inh[ci->nsg_inh++] = strdup(name);
}
int comp_is_sg_reader(ClassInfo *ci, const char *name) { return name_in(ci->sg_readers, ci->nsg_readers, name); }
int comp_is_sg_writer(ClassInfo *ci, const char *name) { return name_in(ci->sg_writers, ci->nsg_writers, name); }

void comp_add_alias_from(ClassInfo *ci, const char *new_name, const char *old_name, int alias_node) {
  comp_table_gen++;
  if (!new_name || !old_name) return;
  /* A name aliased again is the later alias, so both entries stay, in
     program order, and lookups read the last; only the same statement
     registered twice (or the same unplaced pair) is a repeat. */
  for (int i = 0; i < ci->naliases; i++)
    if (sp_streq(ci->alias_new[i], new_name) &&
        (alias_node >= 0 ? ci->alias_node[i] == alias_node
                         : (ci->alias_node[i] < 0 && sp_streq(ci->alias_old[i], old_name)))) return;
  if (ci->naliases >= ci->caliases) {
    ci->caliases = ci->caliases ? ci->caliases * 2 : 4;
    ci->alias_new = realloc(ci->alias_new, sizeof(char *) * (size_t)ci->caliases);
    ci->alias_old = realloc(ci->alias_old, sizeof(char *) * (size_t)ci->caliases);
    ci->alias_cls = realloc(ci->alias_cls, sizeof(int) * (size_t)ci->caliases);
    ci->alias_node = realloc(ci->alias_node, sizeof(int) * (size_t)ci->caliases);
    ci->alias_builtin = realloc(ci->alias_builtin, sizeof(int) * (size_t)ci->caliases);
  }
  ci->alias_new[ci->naliases] = strdup(new_name);
  ci->alias_old[ci->naliases] = strdup(old_name);
  ci->alias_cls[ci->naliases] = -1;
  ci->alias_node[ci->naliases] = alias_node;
  ci->alias_builtin[ci->naliases] = 0;
  ci->naliases++;
}

void comp_add_alias(ClassInfo *ci, const char *new_name, const char *old_name) {
  comp_add_alias_from(ci, new_name, old_name, -1);
}

/* An attribute and an explicit method can both own one name. CRuby's
   attr_accessor defines an ordinary method, so a `def` at an equal-or-more-
   derived class replaces it: the more-derived definition wins, and a same-class
   tie goes to the method. Every emission site used to compose this policy from
   the chain queries itself, and the sites that composed only half of it emitted
   a call to the override typed as the attribute, or a switch with no arm for
   the method at all (#3907, #3909, #3910). */
int comp_resolve_member(Compiler *c, int class_id, const char *name, int want_write,
                        int *def_class, int *method_index) {
  if (def_class) *def_class = -1;
  if (method_index) *method_index = -1;
  if (!name || class_id < 0 || class_id >= c->nclasses) return SP_MEMBER_NONE;
  char mbuf[300];
  const char *mname = name;
  if (want_write) { snprintf(mbuf, sizeof mbuf, "%s=", name); mname = mbuf; }
  int adc = -1, mdc = -1;
  int has_attr = want_write ? comp_writer_in_chain(c, class_id, name, &adc)
                            : comp_reader_in_chain(c, class_id, name, &adc);
  int mi = comp_method_in_chain(c, class_id, mname, &mdc);
  if (mi < 0 && !has_attr) return SP_MEMBER_NONE;
  int attr_wins = has_attr;
  if (has_attr && mi >= 0) {
    for (int k = class_id; k >= 0; k = c->classes[k].parent) {
      if (k == mdc) { attr_wins = 0; break; }
      if (k == adc) { attr_wins = 1; break; }
    }
  }
  if (attr_wins) { if (def_class) *def_class = adc; return SP_MEMBER_ATTR; }
  if (def_class) *def_class = mdc;
  if (method_index) *method_index = mi;
  return SP_MEMBER_METHOD;
}

const char *comp_resolve_alias_ex(Compiler *c, int class_id, const char *name, int *start_cls, int *builtin) {
  if (builtin) *builtin = 0;
  if (!name) return name;
  /* Follow alias links (chain-aware), guarding against cycles. A name means
     its LAST alias, and an alias names what its target meant where the
     alias appeared: the hop from it reads only the aliases of that class
     before it. `alias_method :plus_without, :+` then
     `alias_method :+, :plus_with` leaves plus_without on the old `+`;
     reading the later alias made plus_with call itself. */
  int lim_cls = -1, lim = 0;
  for (int hops = 0; hops < 32; hops++) {
    const char *next = NULL;
    for (int cid = class_id; cid >= 0 && !next; cid = c->classes[cid].parent) {
      ClassInfo *ci = &c->classes[cid];
      for (int i = (cid == lim_cls ? lim : ci->naliases) - 1; i >= 0; i--)
        if (sp_streq(ci->alias_new[i], name)) {
          next = ci->alias_old[i];
          /* An alias of an INHERITED method names the body that was in effect
             where the alias appeared, so a redefinition in this class must not
             capture it: resume the lookup at the ancestor that owned the name
             (#3873). */
          if (ci->alias_cls && ci->alias_cls[i] >= 0) {
            if (start_cls) *start_cls = ci->alias_cls[i];
            /* ... and so does the rest of the chase: this class's later
               aliases of the name are not the body the alias took */
            class_id = ci->alias_cls[i];
          }
          lim_cls = cid; lim = i;
          /* it captured a primitive's builtin: that is where it ends */
          if (ci->alias_builtin && ci->alias_builtin[i]) {
            if (builtin) *builtin = 1;
            return next;
          }
          break;
        }
    }
    if (!next) return name;
    name = next;
  }
  return name;
}

const char *comp_resolve_alias_at(Compiler *c, int class_id, const char *name, int *start_cls) {
  return comp_resolve_alias_ex(c, class_id, name, start_cls, NULL);
}

const char *comp_resolve_alias(Compiler *c, int class_id, const char *name) {
  return comp_resolve_alias_at(c, class_id, name, NULL);
}

/* native-binding registry (Path B): a native_func maps a Ruby Module.method to
   a C symbol, with spinel-typed args/return. Lookup and the small type-spec
   vocabulary live here so both analyze and codegen can reach them. */
int comp_native_find(Compiler *c, const char *mod, const char *name) {
  if (!mod || !name) return -1;
  for (int i = 0; i < c->n_native_funcs; i++)
    if (sp_streq(c->native_funcs[i].mod, mod) && sp_streq(c->native_funcs[i].name, name))
      return i;
  return -1;
}

TyKind native_spec_to_ty(const char *spec) {
  if (!spec) return TY_UNKNOWN;
  if (sp_streq(spec, "any"))    return TY_POLY;
  if (sp_streq(spec, "string")) return TY_STRING;
  if (sp_streq(spec, "text"))   return TY_STRING; /* write payload: String, or anything's #to_s */
  if (sp_streq(spec, "string?")) return TY_POLY;  /* nullable string -> boxed */
  if (sp_streq(spec, "nstring")) return TY_STRING; /* NULL-able string, unboxed */
  if (sp_streq(spec, "cstring")) return TY_STRING; /* borrowed C string (static buffer): call site dups */
  if (sp_streq(spec, "cbinstr")) return TY_STRING; /* borrowed C bytes + sp_ffi_bin_len: call site dups exactly */
  if (sp_streq(spec, "regexp")) return TY_REGEX;   /* regex-literal arg -> sp_re_pat_<n> */
  if (sp_streq(spec, "int"))    return TY_INT;
  if (sp_streq(spec, "float"))  return TY_FLOAT;
  if (sp_streq(spec, "bool"))   return TY_BOOL;
  if (sp_streq(spec, "nil") || sp_streq(spec, "void")) return TY_NIL;
  return TY_UNKNOWN;
}

/* Find a native method binding on a class: kind 0 = instance method, 1 =
   constructor. Arity-keyed -- prefer an exact nargs==argc match, else the first
   same-name binding. Returns the index in c->native_methods, or -1. */
int comp_ffi_const_at(Compiler *c, int node, int *out) {
  const NodeTable *nt = c->nt;
  if (node < 0) return 0;
  const char *ty = nt_type(nt, node);
  if (!ty || !sp_streq(ty, "ConstantPathNode")) return 0;
  const char *leaf = nt_str(nt, node, "name");
  int par = nt_ref(nt, node, "parent");
  const char *pty = par >= 0 ? nt_type(nt, par) : NULL;
  const char *mod = (pty && (sp_streq(pty, "ConstantReadNode") || sp_streq(pty, "ConstantPathNode")))
                    ? nt_str(nt, par, "name") : NULL;
  if (!leaf || !mod) return 0;
  for (int i = 0; i < c->n_ffi_consts; i++) {
    if (sp_streq(c->ffi_consts[i].mod, mod) && sp_streq(c->ffi_consts[i].name, leaf)) {
      if (out) *out = c->ffi_consts[i].val;
      return 1;
    }
  }
  return 0;
}
int comp_native_method_find(Compiler *c, int class_id, const char *name, int argc, int kind) {
  return comp_native_method_find_typed(c, class_id, name, argc, kind, NULL);
}

/* Whether class `k` contributes a poly-dispatch arm for instance calls of
   `name`. A native class contributes only through its declared BINDINGS
   (#4504): a Ruby-side def on it (IO::Buffer#values, StringIO#each_line) is
   reachable through a typed receiver alone and gets no arm in the cls_id
   switch -- so a candidate counter that tallied those stood the builtin
   arms down for every poly receiver, and merely loading IO::Buffer turned
   `h.values` on a poly Hash into NoMethodError. Every "does any user class
   define this name" loop that feeds the poly dispatch asks through here so
   the analyzer, the emitters, and the switch count the same arms. */
int comp_poly_arm_defines(Compiler *c, int k, const char *name) {
  if (c->classes[k].is_native_class)
    return comp_native_method_find(c, k, name, 0, 0) >= 0;
  return comp_method_in_chain(c, k, name, NULL) >= 0;
}

/* The arity-pinned form, for the builtin-arm gates that know the call's
   argc: a native binding is an arm only when it takes exactly that many
   arguments, the same filter the dispatch arm emission applies. (Ruby
   chains stay arity-loose, as everywhere: optional and rest parameters
   make a def's acceptance a runtime question.) StringIO's zero-argument
   `getbyte` binding otherwise counted for `s.getbyte(i)` on a poly String
   and stood the builtin arm down again (#4432). */
int comp_poly_arm_defines_n(Compiler *c, int k, const char *name, int argc) {
  if (c->classes[k].is_native_class) {
    int nmi = comp_native_method_find(c, k, name, argc, 0);
    return nmi >= 0 && native_takes(&c->native_methods[nmi], argc);
  }
  return comp_method_in_chain(c, k, name, NULL) >= 0;
}

/* IO::Buffer's type-symbol table, index-compatible with lib/sp_iobuffer.h's
   SP_IOB_TY_* enum. Shared by the analyzer (a literal-symbol get_value's
   return type follows the symbol) and the codegen fold that lowers such a
   call to the typed accessor. */
static const char *const iob_ty_names[] = {
  "U8", "S8", "u16", "s16", "U16", "S16", "u32", "s32", "U32", "S32",
  "u64", "s64", "U64", "S64", "f32", "f64", "F32", "F64", NULL
};
int comp_iob_sym_type(const char *name) {
  if (!name) return -1;
  for (int i = 0; iob_ty_names[i]; i++)
    if (sp_streq(name, iob_ty_names[i])) return i;
  return -1;
}
int comp_iob_ty_is_float(int t) { return t >= 14 && t <= 17; }
/* the 64-bit integer types stay BOXED through the fold: u64 values above
   2^63-1 are Bignums, and an s64 load of INT64_MIN would collide with the
   runtime's SP_INT_NIL sentinel in an unboxed slot */
int comp_iob_ty_is_64(int t) {
  extern int sp_target_int_bits;
  /* on a 32-bit target the 32-bit types are in the same position: a U32
     above 2**31-1 is a Bignum there, an S32 of INT32_MIN the sentinel (#4647) */
  if (sp_target_int_bits == 32 && t >= 6 && t <= 9) return 1;   /* u32/s32/U32/S32 */
  return t >= 10 && t <= 13;    /* u64/s64/U64/S64 */
}

/* Type-keyed variant: among same-name same-arity bindings, prefer one whose
   arg specs match the call's inferred arg types (putc(65) -> the [:int]
   binding, putc("A") -> [:string]). argtys may be NULL (arity-only). */
int comp_native_method_find_typed(Compiler *c, int class_id, const char *name, int argc, int kind,
                                  const TyKind *argtys) {
  if (class_id < 0 || !name) return -1;
  int arity_match = -1, loose = -1, typed = -1, rest_match = -1;
  for (int i = 0; i < c->n_native_methods; i++) {
    NativeMethod *m = &c->native_methods[i];
    if (m->class_id != class_id || m->kind != kind || !sp_streq(m->name, name)) continue;
    /* a :rest binding answers the arities no fixed binding declares */
    if (m->nargs != argc && native_takes(m, argc) && rest_match < 0) rest_match = i;
    if (m->nargs == argc) {
      if (argtys) {
        /* a binding whose every slot takes the actual as-is wins outright;
           one that merely tolerates a boxed or unknown actual in a typed
           slot is kept as the typed fallback, so `io.puts(x)` with a boxed
           x reaches the [:any] binding declared after the [:string] one */
        int all = 1, exact = 1;
        for (int a = 0; a < argc; a++) {
          TyKind want = native_spec_to_ty(m->args[a]);
          if (want == TY_POLY || argtys[a] == want) continue;
          exact = 0;
          if (argtys[a] != TY_UNKNOWN && argtys[a] != TY_POLY) { all = 0; break; }
        }
        if (all && exact) return i;
        if (all && typed < 0) typed = i;
      }
      if (arity_match < 0) arity_match = i;
    }
    if (loose < 0) loose = i;
  }
  if (typed >= 0) return typed;
  if (arity_match >= 0) return arity_match;
  return rest_match >= 0 ? rest_match : loose;
}

int comp_reader_in_chain(Compiler *c, int class_id, const char *name, int *def_class) {
  name = comp_resolve_alias(c, class_id, name);
  for (int cid = class_id; cid >= 0; cid = c->classes[cid].parent)
    if (comp_is_reader(&c->classes[cid], name)) { if (def_class) *def_class = cid; return 1; }
  return 0;
}
int comp_writer_in_chain(Compiler *c, int class_id, const char *name, int *def_class) {
  name = comp_resolve_alias(c, class_id, name);
  for (int cid = class_id; cid >= 0; cid = c->classes[cid].parent)
    if (comp_is_writer(&c->classes[cid], name)) { if (def_class) *def_class = cid; return 1; }
  return 0;
}

/* ---- Poly-dispatch candidates by method name ----
   A call on a poly receiver asks every class whether it answers `name`, and
   asks again for the same name at every such call site, every fixpoint round:
   (poly call sites x classes x chain depth) per round, the N^2 term of the
   front end (lobsters: 543M of 792M chain walks from two sites). The answer
   depends only on `name` while the scope index is frozen -- scope shape, the
   parent chains, the reader and alias tables are all fixed there -- so it is
   computed once per name and re-read. The memo is stamped with the scope-index
   epoch, the scope and class counts and the table generation; any change drops
   it. Unfrozen, nothing is memoized and the caller pays the scan as before.
   The list holds every class in ascending order, so a consumer iterating it
   sees the same classes in the same order as the loop it replaces; per-class
   conditions that vary per call (ctor_reachable, an_builtin_only, native arity)
   stay with the consumer. A native class is always listed: its answer depends
   on the call's arity, which the consumer checks. */
struct pc_entry { char *name; PolyCand *cands; int n; struct pc_entry *next; };
#define PC_BUCKETS 4096
static struct pc_entry *pc_tab[PC_BUCKETS];
static struct pc_entry *cc_tab[PC_BUCKETS];   /* class methods: comp_cmethod_candidates */
static unsigned pc_gen_stamp; static int pc_nscopes_stamp, pc_nclasses_stamp; static unsigned pc_table_stamp;
/* Invalidation never frees: a consumer may be iterating a list when a nested
   inference call invalidates the memo (bind_call_params -> infer_type -> this).
   Retired entries are kept until comp_poly_candidates_reset, at the start of a
   compile. */
static struct pc_entry *pc_retired;
static void pc_clear(void) {
  for (int b = 0; b < PC_BUCKETS; b++) {
    for (struct pc_entry *e = pc_tab[b]; e; ) { struct pc_entry *nx = e->next; e->next = pc_retired; pc_retired = e; e = nx; }
    pc_tab[b] = NULL;
  }
}
void comp_poly_candidates_reset(void) {
  pc_clear();
  for (int b = 0; b < PC_BUCKETS; b++) {
    for (struct pc_entry *e = cc_tab[b]; e; ) { struct pc_entry *nx = e->next; e->next = pc_retired; pc_retired = e; e = nx; }
    cc_tab[b] = NULL;
  }
  for (struct pc_entry *e = pc_retired; e; ) { struct pc_entry *nx = e->next; free(e->name); free(e->cands); free(e); e = nx; }
  pc_retired = NULL;
}
static void pc_build(Compiler *c, const char *name, PolyCand **out, int *n_out) {
  PolyCand *v = NULL; int n = 0, cap = 0;
  for (int k = 0; k < c->nclasses; k++) {
    PolyCand pc; pc.cls = k; pc.rdcls = -1; pc.native = c->classes[k].is_native_class;
    pc.mi = comp_method_in_chain(c, k, name, NULL);
    if (!pc.native && pc.mi < 0 && !comp_reader_in_chain(c, k, name, &pc.rdcls)) continue;
    if (n == cap) { cap = cap ? cap * 2 : 8; v = realloc(v, sizeof *v * (size_t)cap); }
    v[n++] = pc;
  }
  *out = v; *n_out = n;
}
const PolyCand *comp_poly_candidates(Compiler *c, const char *name, int *n) {
  if (!name) { *n = 0; return NULL; }
  if (!sm_frozen) {
    /* scope shape may still change: answer fresh, and keep nothing */
    struct pc_entry *e = calloc(1, sizeof *e);
    pc_build(c, name, &e->cands, &e->n);
    e->next = pc_retired; pc_retired = e;
    *n = e->n; return e->cands;
  }
  if (pc_gen_stamp != sm_gen || pc_nscopes_stamp != c->nscopes || pc_nclasses_stamp != c->nclasses || pc_table_stamp != comp_table_gen) {
    pc_clear(); pc_gen_stamp = sm_gen; pc_nscopes_stamp = c->nscopes; pc_nclasses_stamp = c->nclasses; pc_table_stamp = comp_table_gen;
  }
  unsigned b = sp_strhash(name) % PC_BUCKETS;
  for (struct pc_entry *e = pc_tab[b]; e; e = e->next)
    if (sp_streq(e->name, name)) {
      *n = e->n; return e->cands;
    }
  struct pc_entry *e = calloc(1, sizeof *e);
  /* While a scope is moved for an instance_exec block (comp_scope_move_begin)
     the lookups leave the moved method out, so a list made then would miss
     it after the move: answer it, but keep it off the memo. */
  if (mv_n) {
    pc_build(c, name, &e->cands, &e->n);
    e->next = pc_retired; pc_retired = e;
    *n = e->n; return e->cands;
  }
  e->name = strdup(name);
  pc_build(c, name, &e->cands, &e->n);
  e->next = pc_tab[b]; pc_tab[b] = e;
  *n = e->n; return e->cands;
}

/* ---- Class-method candidates by name ----
   The same question for class methods: a boxed receiver that may hold a Class
   dispatches `name` on the class tag, and three inference sites ask every
   class's chain for it at each call, every round (lobsters: 240M chain walks,
   #4965). Same memo, same stamps, same never-free invalidation; kept in its
   own table so the two lists never alias. */
static unsigned cc_gen_stamp; static int cc_nscopes_stamp, cc_nclasses_stamp; static unsigned cc_table_stamp;
static void cc_build(Compiler *c, const char *name, PolyCand **out, int *n_out) {
  PolyCand *v = NULL; int n = 0, cap = 0;
  for (int k = 0; k < c->nclasses; k++) {
    int mi = comp_cmethod_in_chain(c, k, name, NULL);
    if (mi < 0) continue;
    if (n == cap) { cap = cap ? cap * 2 : 8; v = realloc(v, sizeof *v * (size_t)cap); }
    v[n].cls = k; v[n].mi = mi; v[n].rdcls = -1; v[n].native = 0; n++;
  }
  *out = v; *n_out = n;
}
const PolyCand *comp_cmethod_candidates(Compiler *c, const char *name, int *n) {
  if (!name) { *n = 0; return NULL; }
  if (!sm_frozen) {
    struct pc_entry *e = calloc(1, sizeof *e);
    cc_build(c, name, &e->cands, &e->n);
    e->next = pc_retired; pc_retired = e;
    *n = e->n; return e->cands;
  }
  if (cc_gen_stamp != sm_gen || cc_nscopes_stamp != c->nscopes || cc_nclasses_stamp != c->nclasses || cc_table_stamp != comp_table_gen) {
    for (int b = 0; b < PC_BUCKETS; b++) {
      for (struct pc_entry *e = cc_tab[b]; e; ) { struct pc_entry *nx = e->next; e->next = pc_retired; pc_retired = e; e = nx; }
      cc_tab[b] = NULL;
    }
    cc_gen_stamp = sm_gen; cc_nscopes_stamp = c->nscopes; cc_nclasses_stamp = c->nclasses; cc_table_stamp = comp_table_gen;
  }
  unsigned b = sp_strhash(name) % PC_BUCKETS;
  for (struct pc_entry *e = cc_tab[b]; e; e = e->next)
    if (sp_streq(e->name, name)) { *n = e->n; return e->cands; }
  struct pc_entry *e = calloc(1, sizeof *e);
  if (mv_n) {   /* see comp_poly_candidates */
    cc_build(c, name, &e->cands, &e->n);
    e->next = pc_retired; pc_retired = e;
    *n = e->n; return e->cands;
  }
  e->name = strdup(name);
  cc_build(c, name, &e->cands, &e->n);
  e->next = cc_tab[b]; cc_tab[b] = e;
  *n = e->n; return e->cands;
}

/* ---- Descendants of a class ----
   Five passes ask "which classes descend from X" by walking every class's
   parent chain, for every call node they visit, every fixpoint round:
   (call nodes x classes x chain depth). Parent links are set while classes
   are collected and never after, so the answer is fixed for the whole of
   inference; it is computed once per class -- every proper descendant, in
   ascending order, the order the loops it replaces visited them in -- and
   rebuilt only when the class count changes. */
static int **desc_lists; static int *desc_counts; static int desc_nclasses = -1;
const int *comp_descendants(Compiler *c, int cid, int *n) {
  if (cid < 0 || cid >= c->nclasses) { *n = 0; return NULL; }
  if (desc_nclasses != c->nclasses) {
    for (int i = 0; i < desc_nclasses; i++) free(desc_lists[i]);
    free(desc_lists); free(desc_counts);
    desc_lists = calloc((size_t)c->nclasses, sizeof *desc_lists);
    desc_counts = calloc((size_t)c->nclasses, sizeof *desc_counts);
    desc_nclasses = c->nclasses;
  }
  if (!desc_lists[cid]) {
    int *v = NULL, cnt = 0, cap = 0;
    for (int k = 0; k < c->nclasses; k++) {
      int is_desc = 0;
      for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
        if (p == cid) { is_desc = 1; break; }
      if (!is_desc) continue;
      if (cnt == cap) { cap = cap ? cap * 2 : 8; v = realloc(v, sizeof *v * (size_t)cap); }
      v[cnt++] = k;
    }
    if (!v) v = malloc(sizeof *v);   /* a non-NULL sentinel for "computed, empty" */
    desc_lists[cid] = v; desc_counts[cid] = cnt;
  }
  *n = desc_counts[cid]; return desc_lists[cid];
}
void comp_descendants_reset(void) {
  for (int i = 0; i < desc_nclasses; i++) free(desc_lists[i]);
  free(desc_lists); free(desc_counts); desc_lists = NULL; desc_counts = NULL; desc_nclasses = -1;
}

Scope *comp_scope_of(Compiler *c, int node_id) {
  if (node_id < 0 || node_id >= c->nt->count) return &c->scopes[0];
  int idx = c->nscope[node_id];
  if (idx < 0 || idx >= c->nscopes) idx = 0;
  return &c->scopes[idx];
}

int comp_is_local_write(NodeKind k) {
  return k == NK_LocalVariableWriteNode || k == NK_LocalVariableTargetNode ||
         k == NK_LocalVariableOrWriteNode || k == NK_LocalVariableAndWriteNode ||
         k == NK_LocalVariableOperatorWriteNode;
}
static int comp_chain_alloc(int **head, int **next, int nb, int n, int *built) {
  *head = malloc((size_t)nb * sizeof(int));
  *next = malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
  if (*head && *next) return 1;
  free(*head); free(*next);
  *head = *next = NULL;
  *built = 0;
  return 0;
}
static void lvw_build(Compiler *c) {
  free(c->lvw_head); free(c->lvw_next);
  int n = c->nt->count;
  int nb = 16;
  while (nb < n && nb < (1 << 22)) nb <<= 1;
  if (!comp_chain_alloc(&c->lvw_head, &c->lvw_next, nb, n, &c->lvw_built)) return;
  c->lvw_nbuckets = nb;
  c->lvw_count = n;
  for (int b = 0; b < nb; b++) c->lvw_head[b] = -1;
  for (int w = 0; w < n; w++) {
    c->lvw_next[w] = -1;
    if (!comp_is_local_write(nt_kind(c->nt, w))) continue;
    const char *wn = nt_str(c->nt, w, "name");
    if (!wn) continue;
    unsigned b = sp_strhash(wn) & (unsigned)(nb - 1);
    c->lvw_next[w] = c->lvw_head[b];
    c->lvw_head[b] = w;
  }
  c->lvw_version = c->nt->version;
  c->lvw_built = 1;
}
int comp_lvw_first(Compiler *c, const char *name) {
  if (!c->lvw_built || c->lvw_version != c->nt->version) lvw_build(c);
  if (!c->lvw_built) return -1;
  return c->lvw_head[sp_strhash(name) & (unsigned)(c->lvw_nbuckets - 1)];
}
int comp_lvw_next(const Compiler *c, int w) {
  return (w >= 0 && w < c->lvw_count) ? c->lvw_next[w] : -1;
}

/* The (scope, name)-keyed sibling of the index above. Machine-generated
   programs reuse a handful of local names across thousands of scopes, so the
   name-keyed chain degenerates to a near-full walk for the common names; the
   scope-salted bucket keeps each chain the handful of writes the caller is
   actually after. Chains still carry hash collisions: callers keep their
   name/scope filters. */
static void lvws_build(Compiler *c) {
  free(c->lvws_head); free(c->lvws_next);
  int n = c->nt->count;
  int nb = 16;
  while (nb < n && nb < (1 << 22)) nb <<= 1;
  if (!comp_chain_alloc(&c->lvws_head, &c->lvws_next, nb, n, &c->lvws_built)) return;
  c->lvws_nbuckets = nb;
  c->lvws_count = n;
  for (int b = 0; b < nb; b++) c->lvws_head[b] = -1;
  for (int w = 0; w < n; w++) {
    c->lvws_next[w] = -1;
    if (!comp_is_local_write(nt_kind(c->nt, w))) continue;
    const char *wn = nt_str(c->nt, w, "name");
    if (!wn) continue;
    int si = (w < c->nt->count && c->nscope) ? c->nscope[w] : 0;
    unsigned b = (sp_strhash(wn) ^ ((unsigned)si * 2654435761u)) & (unsigned)(nb - 1);
    c->lvws_next[w] = c->lvws_head[b];
    c->lvws_head[b] = w;
  }
  c->lvws_version = c->nt->version;
  c->lvws_built = 1;
}
int comp_lvw_first_sc(Compiler *c, int scope_idx, const char *name) {
  if (!c->lvws_built || c->lvws_version != c->nt->version) lvws_build(c);
  if (!c->lvws_built) return -1;
  unsigned b = (sp_strhash(name) ^ ((unsigned)scope_idx * 2654435761u)) & (unsigned)(c->lvws_nbuckets - 1);
  return c->lvws_head[b];
}
int comp_lvw_next_sc(const Compiler *c, int w) {
  return (w >= 0 && w < c->lvws_count) ? c->lvws_next[w] : -1;
}

/* Every CallNode in a scope, chained. The strbuf shape checks walked the
   whole node table per candidate local to find the calls of one scope; this
   walks them once. */
static void scall_build(Compiler *c) {
  free(c->scall_head); free(c->scall_next);
  int n = c->nt->count;
  int ns = c->nscopes > 0 ? c->nscopes : 1;
  if (!comp_chain_alloc(&c->scall_head, &c->scall_next, ns, n, &c->scall_built)) return;
  c->scall_nscopes = ns;
  c->scall_count = n;
  for (int s = 0; s < ns; s++) c->scall_head[s] = -1;
  for (int u = n - 1; u >= 0; u--) {   /* reverse: chains run in node order */
    c->scall_next[u] = -1;
    if (nt_kind(c->nt, u) != NK_CallNode) continue;
    int si = c->nscope ? c->nscope[u] : 0;
    if (si < 0 || si >= ns) si = 0;
    c->scall_next[u] = c->scall_head[si];
    c->scall_head[si] = u;
  }
  c->scall_version = c->nt->version;
  c->scall_built = 1;
}
int comp_scall_first(Compiler *c, int scope_idx) {
  if (!c->scall_built || c->scall_version != c->nt->version ||
      c->scall_nscopes < c->nscopes) scall_build(c);
  if (!c->scall_built || scope_idx < 0 || scope_idx >= c->scall_nscopes) return -1;
  return c->scall_head[scope_idx];
}
int comp_scall_next(const Compiler *c, int u) {
  return (u >= 0 && u < c->scall_count) ? c->scall_next[u] : -1;
}

/* Every node of one kind, chained in node order. The string-promotion passes
   walked the whole table per fixpoint round with a kind filter as the first
   test; these chains hand them just the matching nodes. */
static void kind_build(Compiler *c) {
  free(c->kind_head); free(c->kind_next);
  int n = c->nt->count;
  int nk = 0;
  for (int i = 0; i < n; i++) if ((int)nt_kind(c->nt, i) >= nk) nk = (int)nt_kind(c->nt, i) + 1;
  if (nk < 1) nk = 1;
  if (!comp_chain_alloc(&c->kind_head, &c->kind_next, nk, n, &c->kind_built)) return;
  c->kind_nkinds = nk;
  c->kind_count = n;
  for (int k = 0; k < nk; k++) c->kind_head[k] = -1;
  for (int i = n - 1; i >= 0; i--) {   /* reverse: chains run in node order */
    c->kind_next[i] = c->kind_head[(int)nt_kind(c->nt, i)];
    c->kind_head[(int)nt_kind(c->nt, i)] = i;
  }
  c->kind_version = c->nt->version;
  c->kind_built = 1;
}
int comp_kind_first(Compiler *c, int kind) {
  if (!c->kind_built || c->kind_version != c->nt->version) kind_build(c);
  if (!c->kind_built || kind < 0 || kind >= c->kind_nkinds) return -1;
  return c->kind_head[kind];
}
int comp_kind_next(const Compiler *c, int id) {
  return (id >= 0 && id < c->kind_count) ? c->kind_next[id] : -1;
}

/* Whether a bare `gets` may answer ARGF's next line, as `ARGF.gets` does.
   Not when the program has a `gets` of its own anywhere: a def, an alias, a
   symbol or string spelling the name (define_method, send, attr_reader), or a
   symbol built at run time, which may reach the call through include, extend
   or a reopened Object or Kernel. Nor when it reads `$_`, which Kernel#gets
   sets and this does not: by name, through a `print` with no arguments (a
   symbol or string spelling `print` counts too), or through a unary `~`. Nor
   when it names BasicObject, whose instances have no Kernel#gets: the call
   may run with one as self, through instance_eval, from places the arm
   cannot see. Such a program keeps its NameError rather than a silent wrong
   answer. Syntax only, over the whole node table (a required package's
   source too), so it is asked once per version of the table and again when
   the table grows. */
static int bare_gets_scan(const NodeTable *nt) {
  for (int i = 0; i < nt->count; i++) {
    const char *ty = nt_type(nt, i);
    if (!ty) continue;
    const char *v = NULL;
    if (sp_streq(ty, "DefNode")) v = nt_str(nt, i, "name");
    else if (sp_streq(ty, "SymbolNode")) {
      v = nt_str(nt, i, "value");
      if (!v) v = nt_str(nt, i, "unescaped");
    }
    else if (sp_streq(ty, "StringNode")) {
      v = nt_str(nt, i, "content");
      if (!v) v = nt_str(nt, i, "unescaped");
    }
    else if (sp_streq(ty, "InterpolatedSymbolNode")) return 0;
    else if (sp_streq(ty, "ConstantReadNode") || sp_streq(ty, "ConstantPathNode")) {
      const char *cn = nt_str(nt, i, "name");
      if (cn && sp_streq(cn, "BasicObject")) return 0;
    }
    else if (sp_streq(ty, "GlobalVariableReadNode")) {
      const char *gn = nt_str(nt, i, "name");
      if (gn && (sp_streq(gn, "$_") || sp_streq(gn, "$LAST_READ_LINE"))) return 0;
    }
    else if (sp_streq(ty, "CallNode")) {
      const char *cn = nt_str(nt, i, "name");
      int ac = 0, args = nt_ref(nt, i, "arguments");
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
      int splats_only = 1;
      for (int k = 0; k < ac; k++)
        if (nt_kind(nt, av[k]) != NK_SplatNode) splats_only = 0;
      if (cn && sp_streq(cn, "print") && splats_only) return 0;
      if (cn && sp_streq(cn, "~") && ac == 0) return 0;
    }
    if (v && (sp_streq(v, "gets") || sp_streq(v, "print"))) return 0;
  }
  return 1;
}
int comp_bare_gets_is_argf(Compiler *c) {
  static const NodeTable *memo_nt = NULL;
  static unsigned memo_ver = 0;
  static int memo_count = -1, memo_ans = 0;
  if (memo_nt != c->nt || memo_ver != c->nt->version || memo_count != c->nt->count) {
    memo_nt = c->nt; memo_ver = c->nt->version; memo_count = c->nt->count;
    memo_ans = bare_gets_scan(c->nt);
  }
  return memo_ans;
}

static int comp_method_index_direct(Compiler *c, const char *name);
int comp_method_index(Compiler *c, const char *name) {
  int mi = comp_method_index_direct(c, name);
  if (mi >= 0) return mi;
  /* Only when nothing owns the name: a top-level `alias b a` registers on the
     Toplevel pseudo-class, while the methods it names are free functions, so
     the lookup has to consult it here or the alias resolves to nothing
     (#3730). */
  {
    int tl = name ? comp_class_index(c, "Toplevel") : -1;
    if (tl >= 0) {
      const char *res = comp_resolve_alias(c, tl, name);
      if (res && !sp_streq(res, name)) return comp_method_index_direct(c, res);
    }
  }
  return -1;
}
static int comp_method_index_direct(Compiler *c, const char *name) {
  if (!name) return -1;
  if (!sm_frozen) {
    /* in reverse, so a redefinition wins, matching the frozen index */
    for (int s = c->nscopes - 1; s >= 0; s--)
      if (sm_toplevel(c, s) && c->scopes[s].name &&
          sp_streq(c->scopes[s].name, name)) return s;
    return -1;
  }
  if (sm_nscopes != c->nscopes) sm_build(c);
  if (!sm_buckets) return -1;
  unsigned b = sm_hash(-1, name, 0) % (unsigned)sm_buckets;
  for (int s = tm_head[b]; s >= 0; s = tm_next[s])
    if (sm_toplevel(c, s) && c->scopes[s].name && sp_streq(c->scopes[s].name, name)) return s;
  return -1;
}

/* Does class ci descend from an explicit `< BasicObject` (a blank slate)?
   Such an instance answers only BasicObject's own methods and what the user
   defined; the Object/Kernel default arms must NOT serve it (#2703). */
int class_is_blank_slate(Compiler *c, int ci) {
  for (int k = ci; k >= 0; k = c->classes[k].parent) {
    int sc = nt_ref(c->nt, c->classes[k].def_node, "superclass");
    const char *sn = sc >= 0 ? nt_str(c->nt, sc, "name") : NULL;
    if (sn && sp_streq(sn, "BasicObject")) return 1;
    if (sn && c->classes[k].parent < 0) return 0;   /* rooted at another builtin */
  }
  return 0;
}

/* Find a method named `name` in any top-level included module.
   module_function methods are class-level (is_cmethod=1), so check both.
   Iterate in reverse so the last include wins (Ruby semantics). An include
   brings in the module's INSTANCE methods, so those are asked first: a
   module that also defines `def self.name` answered the singleton for a bare
   `name`, and the program printed that method's value instead.
   An include at the top level adds the module to Object. An instance of a
   class that descends from BasicObject is not an Object, so a bare call in
   one of its instance methods, or in a block inside one (`call_id`, -1 when
   there is no call site), does not reach the module: CRuby raises
   NoMethodError there, and resolving it ran the module's method instead. A
   class method's self is a Class, which is an Object, so it still does. */
int comp_included_method_index(Compiler *c, const char *name, int call_id) {
  if (!name) return -1;
  if (call_id >= 0) {
    Scope *cs = comp_scope_of(c, call_id);
    if (cs && cs->class_id >= 0 && !cs->is_cmethod &&
        class_is_blank_slate(c, cs->class_id)) return -1;
  }
  for (int k = c->ntoplevel_includes - 1; k >= 0; k--) {
    int ci = c->toplevel_includes[k];
    int mi = comp_method_in_chain(c, ci, name, NULL);
    if (mi >= 0) return mi;
    mi = comp_cmethod_in_chain(c, ci, name, NULL);
    if (mi >= 0) return mi;
  }
  return -1;
}

LocalVar *scope_local(Scope *s, const char *name) {
  for (int i = 0; i < s->nlocals; i++)
    if (sp_streq(s->locals[i].name, name)) return &s->locals[i];
  return NULL;
}

LocalVar *scope_local_intern(Scope *s, const char *name) {
  LocalVar *lv = scope_local(s, name);
  if (lv) return lv;
  if (s->nlocals >= s->clocals) {
    s->clocals = s->clocals ? s->clocals * 2 : 8;
    s->locals = realloc(s->locals, sizeof(LocalVar) * (size_t)s->clocals);
  }
  lv = &s->locals[s->nlocals++];
  memset(lv, 0, sizeof *lv);
  lv->name = strdup(name);
  lv->type = TY_UNKNOWN;
  why_reset(&lv->why);
  lv->last_src = -1;
  lv->gc_root = 0;
  lv->is_param = 0;
  lv->is_block_param = 0;
  lv->proc_ret = TY_UNKNOWN;
  lv->is_cell = 0;
  lv->cell_shadow = 0;
  lv->cell_outlives = 0;
  lv->byref_out = 0;
  lv->inline_alias = 0;
  lv->init_guarded = 0;
  lv->rbs_seeded = 0;
  lv->rbs_type = TY_UNKNOWN;
  lv->push_widened = 0;
  lv->poly_dispatch_widened = 0;
  lv->or_written = 0;
  lv->maybe_unset = 0;
  lv->str_shared = 0;
  lv->str_append = 0;
  lv->poly_hash_pin = 0;
  lv->poly_array_pin = 0;
  lv->nullable_int = 0;
  lv->nil_passed = 0;
  lv->bounded_counter = 0;
  lv->nullable_int_elem = 0;
  lv->arr_or_nil = 0;
  lv->poly_ctr = 0;
  lv->oa_pin = TY_UNKNOWN;
  lv->boxed_push_elem = TY_UNKNOWN;
  lv->boxed_known_elem = TY_UNKNOWN;
  lv->boxed_store_key = TY_UNKNOWN;
  lv->boxed_store_val = TY_UNKNOWN;
  lv->store_key_src = 0;
  lv->store_val_src = 0;
  lv->store_rest_src = 0;
  return lv;
}

void comp_prep_chain_add(ClassInfo *ci, const char *from, const char *to) {
  if (ci->nprep_chain >= ci->cprep_chain) {
    ci->cprep_chain = ci->cprep_chain ? ci->cprep_chain * 2 : 4;
    ci->prep_from = realloc(ci->prep_from, sizeof(char *) * (size_t)ci->cprep_chain);
    ci->prep_to   = realloc(ci->prep_to,   sizeof(char *) * (size_t)ci->cprep_chain);
  }
  ci->prep_from[ci->nprep_chain] = strdup(from);
  ci->prep_to[ci->nprep_chain]   = strdup(to);
  ci->nprep_chain++;
}

const char *comp_super_shadow(Compiler *c, const Scope *s) {
  if (!s || !s->name) return NULL;
  if (!s->is_cmethod) return comp_prep_chain_target(c, s->class_id, s->name);
  char key[320];
  snprintf(key, sizeof key, "self.%s", s->name);
  return comp_prep_chain_target(c, s->class_id, key);
}

void comp_cprep_chain_add(ClassInfo *ci, const char *from, const char *to) {
  char key[320];
  snprintf(key, sizeof key, "self.%s", from);
  comp_prep_chain_add(ci, key, to);
}

const char *comp_prep_chain_target(Compiler *c, int class_id, const char *name) {
  if (class_id < 0 || class_id >= c->nclasses || !name) return NULL;
  ClassInfo *ci = &c->classes[class_id];
  for (int k = 0; k < ci->nprep_chain; k++)
    if (sp_streq(ci->prep_from[k], name)) return ci->prep_to[k];
  return NULL;
}

/* Is this `super` (with or without arguments) Class#new? Inside a class
   method `new` whose ancestors define no class method of that name, the
   parent is Class itself: `def self.new(x) = super(x * 10)` allocates the
   receiving class and runs its initialize. The class-method chain answered
   "no superclass method", and so did every call that reached one. */
/* The class a `super` in class_id's methods starts its lookup in: the
   parent, and past the top of an explicit chain, Object -- where a method the
   program defines on Object is every class's superclass method. A module's
   super is its includer's, and a class method's chain has no such end. */
int comp_super_parent(Compiler *c, int class_id, int is_cmethod) {
  if (class_id < 0) return -1;
  int p = c->classes[class_id].parent;
  if (p >= 0 || is_cmethod) return p;
  int obj = comp_class_index(c, "Object");
  if (obj < 0 || obj == class_id) return -1;
  int dn = c->classes[class_id].def_node;
  if (dn >= 0 && dn < c->nt->count && nt_kind(c->nt, dn) == NK_ModuleNode) return -1;
  return obj;
}

int comp_super_is_class_new(Compiler *c, int id) {
  NodeKind k = nt_kind(c->nt, id);
  if (k != NK_SuperNode && k != NK_ForwardingSuperNode) return 0;
  Scope *s = comp_scope_of(c, id);
  if (!s || !s->is_cmethod || s->class_id < 0 || !s->name) return 0;
  if (!sp_streq(comp_prep_user_name(s->name), "new")) return 0;
  if (comp_super_shadow(c, s)) return 0;
  int p = c->classes[s->class_id].parent;
  return p < 0 || comp_cmethod_in_chain(c, p, "new", NULL) < 0;
}

const char *comp_prep_user_name(const char *name) {
  if (name && strncmp(name, "__inc ", 6) == 0) return strchr(name + 6, ' ') + 1;
  if (!name || strncmp(name, "__prep_", 7) != 0) return name;
  const char *p = name + 7;
  while (*p >= '0' && *p <= '9') p++;
  return (*p == '_') ? p + 1 : name;
}

/* The name `super` resolves under from the scope named `name` whose class's
   parent is `parent`. A yielding method's proc-form clone (make_yield_proc_forms)
   is named "<m>#pf", and its super wants the ancestor's own clone when that
   ancestor yields too -- but an ancestor that does not yield has no clone, so
   the clone of `def on(&) = super(&)` behind a poly receiver raised
   "no superclass method 'on#pf'". It falls back to the ancestor's plain method,
   which is callable, since only a yielding method is inlined away. */
const char *comp_super_name(Compiler *c, int parent, const char *name, int is_cmethod) {
  const char *u = comp_prep_user_name(name);
  if (!u) return u;
  size_t n = strlen(u);
  if (n <= 3 || strcmp(u + n - 3, "#pf") != 0) return u;
  /* a class whose parent is builtin (Object, StandardError) supers into the
     plain method, which the super emitter answers for itself */
  if (parent < 0) goto plain;
  /* The nearest ancestor that defines the method decides: its own clone when
     it yields, else its plain method. Asking the chain for the clone found a
     grandparent's and skipped a parent that overrides without yielding
     (CodeRabbit on #4996). */
  char base[256];
  if (n - 3 >= sizeof base) return u;
  memcpy(base, u, n - 3); base[n - 3] = '\0';
  int dc = -1;
  int bm = is_cmethod ? comp_cmethod_in_chain(c, parent, base, &dc)
                      : comp_method_in_chain(c, parent, base, &dc);
  int hit = bm < 0 || dc < 0 ? -1
          : is_cmethod ? comp_cmethod_in_class(c, dc, u) : comp_method_in_class(c, dc, u);
  if (hit >= 0 || bm < 0) {
    if (bm >= 0) return u;
    /* no ancestor defines the plain name: keep the old answer, the clone
       found anywhere up the chain */
    int any = is_cmethod ? comp_cmethod_in_chain(c, parent, u, NULL)
                         : comp_method_in_chain(c, parent, u, NULL);
    if (any >= 0) return u;
  }
plain:;
  static struct pf_base { char *from, *to; struct pf_base *next; } *cache;
  for (struct pf_base *e = cache; e; e = e->next)
    if (strcmp(e->from, u) == 0) return e->to;
  struct pf_base *e = (struct pf_base *)malloc(sizeof *e);
  char *to = strndup(u, n - 3), *from = strdup(u);
  if (!e || !to || !from) { free(e); free(to); free(from); return u; }
  e->from = from; e->to = to; e->next = cache; cache = e;
  return to;
}

/* The sp_poly_enum_proc op for a block-carrying Enumerable name, or NULL.
   These are the names a poly dispatch can serve from a builtin Array/Hash
   receiver by driving the materialized block proc over the elements; only the
   one-value-per-element family, since the runtime helper passes one. */
/* A call that hands out ONE element of a container: the same object `[]`
   yields, so a mutation through the result has to reach the container (the
   shared-mutable-string machinery keys off this). `first`/`last` qualify only
   in their zero-argument form -- with a count they answer a new Array, not an
   element (#4013). One predicate, because the analysis probes and the emitter
   have to agree on the set: they did not, and `b.first[1] = "*"` raised
   NoMethodError while `b[0][1] = "*"` worked. */
int container_elem_read_p(const NodeTable *nt, int id) {
  if (id < 0 || nt_kind(nt, id) != NK_CallNode) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm) return 0;
  if (sp_streq(nm, "[]") || sp_streq(nm, "fetch") || sp_streq(nm, "dig")) return 1;
  if (sp_streq(nm, "first") || sp_streq(nm, "last")) {
    int a = nt_ref(nt, id, "arguments");
    int n = 0;
    if (a >= 0) nt_arr(nt, a, "arguments", &n);
    return n == 0;
  }
  return 0;
}

const char *poly_enum_op_for(const char *name) {
  static const struct { const char *nm, *op; } PEN[] = {
    {"each","SP_PENUM_EACH"}, {"each_with_index","SP_PENUM_EACH_WITH_INDEX"},
    {"map","SP_PENUM_MAP"}, {"collect","SP_PENUM_MAP"},
    {"select","SP_PENUM_SELECT"}, {"filter","SP_PENUM_SELECT"},
    {"find_all","SP_PENUM_SELECT"}, {"reject","SP_PENUM_REJECT"},
    {"find","SP_PENUM_FIND"}, {"detect","SP_PENUM_FIND"},
    {"find_index","SP_PENUM_FIND_INDEX"},
    {"sort_by","SP_PENUM_SORT_BY"},
    {"count","SP_PENUM_COUNT"},
    {"sum","SP_PENUM_SUM"}, {"any?","SP_PENUM_ANY"}, {"all?","SP_PENUM_ALL"},
    {"none?","SP_PENUM_NONE"},
    /* each_entry walks what each walks on every builtin receiver; the rest
       are the Hash-only walks, the reversed one and uniq. Without them a poly
       receiver reaching a user class's yielding method of the name was folded
       to the builtin walk, which reads the object as an empty container and
       never runs the method. */
    {"each_entry","SP_PENUM_EACH"}, {"each_pair","SP_PENUM_EACH_PAIR"},
    {"each_key","SP_PENUM_EACH_KEY"}, {"each_value","SP_PENUM_EACH_VALUE"},
    {"reverse_each","SP_PENUM_REVERSE_EACH"}, {"uniq","SP_PENUM_UNIQ"}, {NULL,NULL}
  };
  if (!name) return NULL;
  for (int i = 0; PEN[i].nm; i++) if (sp_streq(name, PEN[i].nm)) return PEN[i].op;
  return NULL;
}


/* Reads whose answer a builtin Array or Hash gives differently from any user
   method of the same name, and which the poly builtin surface can actually
   serve. On a poly receiver that provably carries a container, a call to one of
   these is a union of the user return and the builtin answer, so its type has
   to be poly -- pinning it to the user return left the dispatch with no room
   for the builtin arm, which then raised NoMethodError on a genuine Array or
   Hash (#3459). Names the surface cannot serve are left out: widening them
   would change the type without changing the answer. */
/* The same idea for the NUMERIC surface, minus the container precondition: a
   boxed receiver can always be a number, so a class defining `abs` (or `round`,
   `succ`, ...) owns the name's dispatch and an Integer arriving there answers
   for itself through the runtime helper. The call's type is that union, not
   the user method's return -- typing it as the user's made the builtin arm's
   boxed answer read as that object, and the program segfaulted (#4012). */
int poly_numeric_read_p(const char *name) {
  static const char *const N[] = {
    "abs", "round", "succ", "next", "pred", "ceil", "floor", "truncate",
    "numerator", "denominator", "nonzero?", NULL };
  if (!name) return 0;
  for (int i = 0; N[i]; i++) if (sp_streq(name, N[i])) return 1;
  return 0;
}

int poly_container_read_p(const char *name) {
  static const char *const N[] = {
    "first", "last", "keys", "values", "min", "max", "sum", "sort",
    "reverse", "index",
    /* the surface serves these now: each ends the dispatch in a runtime
       helper that lets the receiver answer for itself, so the call's type is
       the union rather than whichever user method owns the name */
    "delete", "dig", "values_at",
    /* a blockless each answers an Enumerator over the container; a class
       with a Ruby each in the program left an Array on the raise default */
    "each",
    /* an Array's pop and shift answer through sp_poly_pop / sp_poly_shift,
       which mutate the container behind the boxed pointer in place: a user
       class owning the name left the call typed from that method alone, and
       a genuine Array's answer was dropped or raised (#5099) */
    "pop", "shift", NULL };
  if (!name) return 0;
  for (int i = 0; N[i]; i++) if (sp_streq(name, N[i])) return 1;
  return 0;
}

/* The read-only String surface a poly receiver can be served from. Same idea
   as poly_container_read_p, for the other builtin a boxed value can be: when a
   user class happens to own one of these names, the dispatch's switch has arms
   for the user classes and none for a String, so a genuine String reaching it
   answered NoMethodError for a method String has (#4816 arrived with `getbyte`
   and `bytesize`, which openssl's buffering.rb defines, and the same shape
   breaks three dozen more).

   Reads only. The poly arm hands the value over as a `const char *`, so a
   method that MUTATES its receiver would write through a copy and the
   assignment would be lost -- `prepend`, `concat`, `insert`, `replace` and
   `setbyte` are left to the switch, where they still raise.

   Names the Array / Hash / Integer surface also owns are left out (`index`,
   `count`, `sum`, `first`, `length`, ...): the re-entered emission picks its
   arm by NAME, so for those it would emit the container's helper inside a
   String-tagged arm. They keep whatever the container arms already give them. */
int poly_string_read_p(const char *name) {
  static const char *const N[] = {
    "ascii_only?", "b", "byteindex", "byterindex", "byteslice", "bytesize",
    "casecmp", "casecmp?", "center", "codepoints", "crypt",
    "delete_prefix", "delete_suffix", "dump", "encode", "encoding",
    "end_with?", "getbyte", "gsub", "hex", "intern",
    "lines", "ljust", "lstrip", "match", "match?", "oct",
    "partition", "rjust", "rpartition", "rstrip", "scan", "scrub",
    "squeeze", "start_with?", "sub", "to_str", "to_sym",
    "tr", "tr_s", "undump", "unicode_normalize", "unpack", "unpack1",
    "valid_encoding?", NULL };
  if (!name) return 0;
  for (int i = 0; N[i]; i++) if (sp_streq(name, N[i])) return 1;
  return 0;
}

/* The class `self.class` at `recv` names when only one class can answer it:
   an instance method of a class no other class inherits from. In a
   superclass or a module it is whichever class the receiver has at run time.
   -1 when recv is not `self.class` or names no single class. */
int self_class_static_ci(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  if (recv < 0 || nt_kind(nt, recv) != NK_CallNode) return -1;
  const char *cn = nt_str(nt, recv, "name");
  int cr = nt_ref(nt, recv, "receiver");
  if (!cn || !sp_streq(cn, "class") || cr < 0 || nt_kind(nt, cr) != NK_SelfNode) return -1;
  Scope *s = comp_scope_of(c, recv);
  int cid = s ? s->class_id : -1;
  if (cid < 0 || nt_kind(nt, c->classes[cid].def_node) == NK_ModuleNode) return -1;
  for (int j = 0; j < c->nclasses; j++) if (c->classes[j].parent == cid) return -1;
  return cid;
}

/* `allocate` on the class the method runs for: bare or `self.allocate` in a
   class method, `self.class.allocate` in an instance method. Returns that
   class -- the base of what it builds, since a subclass receiving the class
   method, or a subclass instance, builds its own class -- or -1. */
int allocate_on_own_class(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (id < 0 || nt_kind(nt, id) != NK_CallNode) return -1;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "allocate") || nt_ref(nt, id, "block") >= 0) return -1;
  int an = 0; nt_arr(nt, nt_ref(nt, id, "arguments"), "arguments", &an);
  if (an) return -1;
  Scope *s = comp_scope_of(c, id);
  int cid = s ? s->class_id : -1;
  if (cid < 0 || cid >= c->nclasses || nt_kind(nt, c->classes[cid].def_node) == NK_ModuleNode) return -1;
  int recv = nt_ref(nt, id, "receiver");
  if (s->is_cmethod) return (recv < 0 || nt_kind(nt, recv) == NK_SelfNode) ? cid : -1;
  if (recv < 0 || nt_kind(nt, recv) != NK_CallNode) return -1;
  const char *rn = nt_str(nt, recv, "name");
  int rr = nt_ref(nt, recv, "receiver");
  return rn && sp_streq(rn, "class") && rr >= 0 && nt_kind(nt, rr) == NK_SelfNode ? cid : -1;
}

/* A Class-valued receiver that carries its class only at run time: a variable,
   or a call whose result is a class (`Job.set(1).run(2)` -- ActiveJob's chained
   `set`). Excludes a constant receiver and an accessor call, which resolve
   statically through their own dispatch. A method returning a class value was
   left with nowhere to dispatch, so the chained call was rejected outright
   even though the value at run time was right (#3415). */
int class_recv_is_dynamic(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  const char *rty = recv >= 0 ? nt_type(nt, recv) : NULL;
  if (!rty) return 0;
  if (sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "InstanceVariableReadNode"))
    return 1;
  /* A global or class variable, or a conditional picking the class
     (`(bare ? A : B).new(x)`), carries it only at run time as well; with
     arguments they had no emitter and the call was refused. */
  if (sp_streq(rty, "GlobalVariableReadNode") || sp_streq(rty, "ClassVariableReadNode") ||
      sp_streq(rty, "IfNode") || sp_streq(rty, "UnlessNode") || sp_streq(rty, "CaseNode") ||
      sp_streq(rty, "AndNode") || sp_streq(rty, "OrNode"))
    return 1;
  if (!sp_streq(rty, "CallNode")) return 0;
  /* `self.class` in a class with no subclass resolves to that class
     statically and has its own arms on both sides; treating it as dynamic
     would steal them. */
  if (self_class_static_ci(c, recv) >= 0) return 0;
  if (comp_sg_reader_const(c, recv) >= 0) return 0;
  { int cand[4]; if (comp_sg_reader_candidates(c, recv, cand, 4) >= 2) return 0; }
  return 1;
}


/* Does module `mod` -- or a module including it -- take part in class `ci`'s
   singleton ancestors, through an `extend` of `ci` or of a superclass? Then
   `ci.is_a?(mod)` is true. */
static int comp_module_includes(Compiler *c, int m, int mod, int depth) {
  if (m == mod) return 1;
  if (m < 0 || m >= c->nclasses || depth > 16) return 0;
  ClassInfo *mi = &c->classes[m];
  for (int k = 0; k < mi->nincluded_mods; k++)
    if (comp_module_includes(c, mi->included_mods[k], mod, depth + 1)) return 1;
  return 0;
}
int comp_class_singleton_has_module(Compiler *c, int ci, int mod) {
  for (int k = ci, d = 0; k >= 0 && k < c->nclasses && d < 64; k = c->classes[k].parent, d++)
    for (int e = 0; e < c->classes[k].nextended_mods; e++)
      if (comp_module_includes(c, c->classes[k].extended_mods[e], mod, 0)) return 1;
  return 0;
}
/* Does class `ci` or a superclass extend any module? */
int comp_class_extends_any(Compiler *c, int ci) {
  for (int k = ci, d = 0; k >= 0 && k < c->nclasses && d < 64; k = c->classes[k].parent, d++)
    if (c->classes[k].nextended_mods > 0) return 1;
  return 0;
}
