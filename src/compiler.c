#include "compiler.h"
#include "share.h"
#include "repr.h"
#include "analyze.h"
#include "builtin_names.h"
#include "codegen_poly.h"
#include "value_pos.h"

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

/* see compiler.h: every change of a table infer_type reads */
unsigned long comp_ty_edits;

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
  c->nil_tested = calloc((size_t)n, 1);
  c->head_held = calloc((size_t)n, 1);
  if (!c->head_held) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
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
  c->lw_joined = calloc((size_t)n, sizeof(TyKind));
  if (!c->lw_joined) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  c->bop_inf = calloc((size_t)n, sizeof *c->bop_inf);
  c->ucall_inf = calloc((size_t)n, sizeof *c->ucall_inf);
  c->node_cap = n;
  /* On only when set to something: empty is off, as SPINEL_DEFER_REFUSALS
     reads it, and so is "0", as SPINEL_GATE_RAISE=0 and SPINEL_INLINE_FORCE=0
     are. An environment that exports the variable as "0" or "" means off. */
  c->share_strings = sp_share_strings_env();
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
    /* by doubling: the rewrites append a few nodes between asks, and a
       resize to the count each time copied the arrays per append */
    if (nt->count > c->node_ord_cap) {
      int cap = nt->count > 2 * c->node_ord_cap ? nt->count : 2 * c->node_ord_cap;
      int *g = realloc(c->node_ord, sizeof(int) * (size_t)cap);
      int *gb = g ? realloc(c->node_base, sizeof(int) * (size_t)cap) : NULL;
      if (!g || !gb) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      c->node_ord = g; c->node_base = gb; c->node_ord_cap = cap;
    }
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
  c->ntype = realloc((TyKind *)c->ntype, sizeof(TyKind) * (size_t)n);
  c->norigin = realloc(c->norigin, sizeof(int) * (size_t)n);
  c->nilnarrow = realloc((TyKind *)c->nilnarrow, sizeof(TyKind) * (size_t)n);
  c->strbuf_box = realloc((unsigned char *)c->strbuf_box, (size_t)n);
  c->strbuf_handle_demand = realloc(c->strbuf_handle_demand, (size_t)n);
  c->strbuf_read_raw = realloc(c->strbuf_read_raw, (size_t)n);
  c->poly_strbuf_lift = realloc(c->poly_strbuf_lift, (size_t)n);
  c->nil_tested = realloc(c->nil_tested, (size_t)n);
  c->head_held = realloc(c->head_held, (size_t)n);
  if (!c->head_held) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  c->nscope = realloc(c->nscope, sizeof(int) * (size_t)n);
  c->node_cbody = realloc(c->node_cbody, sizeof(int) * (size_t)n);
  c->empty_arr_recv = realloc((char *)c->empty_arr_recv, (size_t)n);
  c->empty_hash_recv = realloc((char *)c->empty_hash_recv, (size_t)n);
  c->empty_hash_arg = realloc((char *)c->empty_hash_arg, (size_t)n);
  c->store_misfit_arg = realloc(c->store_misfit_arg, (size_t)n);
  c->ivar_widen_src = realloc(c->ivar_widen_src, (size_t)n);
  c->hash_want = realloc((TyKind *)c->hash_want, sizeof(TyKind) * (size_t)n);
  c->arr_want = realloc((TyKind *)c->arr_want, sizeof(TyKind) * (size_t)n);
  c->poly_builtin_ty = realloc((TyKind *)c->poly_builtin_ty, sizeof(TyKind) * (size_t)n);
  c->lw_joined = realloc(c->lw_joined, sizeof(TyKind) * (size_t)n);
  if (!c->lw_joined) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  c->bop_inf = realloc(c->bop_inf, sizeof *c->bop_inf * (size_t)n);
  for (int i = c->node_cap; i < n; i++) c->bop_inf[i] = NULL;
  c->ucall_inf = realloc(c->ucall_inf, sizeof *c->ucall_inf * (size_t)n);
  memset(c->ucall_inf + c->node_cap, 0, sizeof *c->ucall_inf * (size_t)(n - c->node_cap));
  for (int i = c->node_cap; i < n; i++) { ((TyKind *)c->ntype)[i] = TY_UNKNOWN; c->norigin[i] = -1; ((TyKind *)c->nilnarrow)[i] = TY_UNKNOWN; c->nscope[i] = 0; c->node_cbody[i] = -1; ((char *)c->empty_arr_recv)[i] = 0; ((char *)c->empty_hash_recv)[i] = 0; ((char *)c->empty_hash_arg)[i] = 0; c->store_misfit_arg[i] = 0; c->ivar_widen_src[i] = 0; ((TyKind *)c->hash_want)[i] = TY_UNKNOWN; ((TyKind *)c->arr_want)[i] = TY_UNKNOWN; ((TyKind *)c->poly_builtin_ty)[i] = TY_UNKNOWN; c->lw_joined[i] = TY_UNKNOWN; ((unsigned char *)c->strbuf_box)[i] = 0; c->strbuf_handle_demand[i] = 0; c->strbuf_read_raw[i] = 0; c->poly_strbuf_lift[i] = 0; c->nil_tested[i] = 0; c->head_held[i] = 0; }
  c->node_cap = n;
}

void comp_free(Compiler *c) {
  if (!c) return;
  for (int i = 0; i < c->n_cext_extensions; i++) { free(c->cext_extensions[i].name); free(c->cext_extensions[i].init); }
  free(c->cext_extensions);
  for (int i = 0; i < c->n_cext_stubs; i++) free(c->cext_stubs[i].key);
  free(c->cext_stubs);
  for (int i = 0; i < c->n_cext_classes; i++) free(c->cext_classes[i].path);
  free(c->cext_classes);
  share_facts_free(c);
  share_check_free(c);
  share_plan_free(c);
  free(c->bsl_gen);
  free(c->bsl_val);
  if (g_repr_check) repr_channel_free(c);
  share_routes_free(c);
  free(c->byref_elig);
  c->byref_elig = NULL;
  c->nbyref_elig = 0;
  pivs_facts_free(c);
  strbuf_arg_index_free(c);
  free(c->vs_head); free(c->vs_site); free(c->vs_var); free(c->vs_next); free(c->vs_kind);
  free(c->vs_rparent); free(c->vs_dropped); free(c->vs_whead); free(c->vs_wnext);
  c->vs_head = c->vs_site = c->vs_var = c->vs_next = c->vs_rparent = NULL;
  c->vs_whead = c->vs_wnext = NULL;
  c->vs_dropped = c->vs_kind = NULL;
  c->vs_count = c->vs_cap = 0;
  c->vs_built = 0;
  free(c->cg_parent);
  c->cg_parent = NULL;
  c->cg_parent_n = 0;
  free(c->strh_no);
  c->strh_no = NULL;
  c->strh_no_cap = 0;
  free(c->lazy_hoist);
  c->lazy_hoist = NULL;
  c->lazy_hoist_n = 0;
  free(c->hash_default_arg_memo);
  c->hash_default_arg_memo = NULL;
  free(c->blk_body_map);
  for (int i = 0; i < c->npres; i++) free(c->pres_names[i]);
  free(c->pres_names); free(c->pres_flags); free(c->pres_obs); free(c->pres_memo); free(c->pres_rank);
  c->pres_obs = NULL; c->pres_obs_n = 0; c->pres_memo = NULL; c->pres_memo_n = 0;
  c->pres_rank = NULL; c->pres_rank_n = 0;
  for (int i = 0; i < c->pres_ord_n; i++) free(c->pres_ord[i]);
  free(c->pres_ord); c->pres_ord = NULL; c->pres_ord_n = 0;
  free(c->redef_calls); c->redef_calls = NULL; c->nredef_calls = 0;
  free(c->splat_views); c->splat_views = NULL; c->nsplat_views = 0;
  c->pres_names = NULL; c->pres_flags = NULL; c->npres = c->cpres = 0;
  free(c->nil_fact);
  free(c->nil_elem_fact);
  free(c->cls_fact); free(c->cls_pool);
  free(c->send_param_memo);
  free(c->node_ord); free(c->node_base);
  for (int k = 0; k < c->bi_base_cap; k++) free(c->bi_base_key[k]);
  free(c->bi_base_key); free(c->bi_base_cnt);
  c->blk_body_map = NULL; c->blk_body_map_n = 0;
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
    free((TyKind *)c->classes[i].ivar_types);
    free(c->classes[i].ivar_obj_may_nil);
    free(c->classes[i].ivar_elem_may_nil);
    for (int j = 0; j < c->classes[i].n_rbs_pin_ivars; j++) free(c->classes[i].rbs_pin_ivars[j]);
    free(c->classes[i].rbs_pin_ivars);
    for (int j = 0; j < c->classes[i].nreaders; j++) free(c->classes[i].readers[j]);
    free(c->classes[i].readers);
    for (int j = 0; j < c->classes[i].nown_readers; j++) free(c->classes[i].own_readers[j]);
    free(c->classes[i].own_readers);
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
  free(c->cls_value);
  for (int i = 0; i < c->ngvars; i++) free(c->gvars[i].name);
  free(c->gvars);
  for (int i = 0; i < c->nconsts; i++) free(c->consts[i].name);
  free(c->consts);
  free(c->toplevel_includes);
  free(c->ary_viewed);
  for (int i = 0; i < c->n_ffi_sources; i++) {
    free(c->ffi_sources[i].mod);
    free(c->ffi_sources[i].val);
  }
  free(c->ffi_sources);
  free(c->nscope);
  if (c->stmt_wr_names)
    for (int i = 0; i < c->stmt_wr_cap; i++) free(c->stmt_wr_names[i]);
  free(c->stmt_wr_names); free(c->stmt_wr_state); free(c->stmt_wr_mark);
  free((TyKind *)c->ntype);
  free(c->norigin);
  free(c->node_cbody);
  free((char *)c->empty_arr_recv);
  free((char *)c->empty_hash_recv);
  free(c->store_misfit_arg);
  free(c->ivar_widen_src);
  free((TyKind *)c->hash_want);
  free((TyKind *)c->arr_want);
  free(c->lw_joined);
  free((TyKind *)c->poly_builtin_ty);
  free(c->bop_inf);
  free(c->ucall_inf);
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
  *(TyKind *)&lv->type = TY_UNKNOWN;
  comp_ty_edits++;    /* a new slot is a change of the table */
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
/* Can `A = v` stand as a value (`m(A = v)`): a typed constant whose slot
   the write leaves holding the value. Not a String constant, whose read
   would hand back a copy instead of the String the constant holds, nor
   one set by a guarded Class.new. */
int comp_const_write_valued(const Compiler *c, const LocalVar *cv) {
  return cv && cv->type != TY_UNKNOWN && cv->type != TY_STRING && !cv->init_guarded &&
         repr_slot_kind(c, cv) != RK_STRBUF;
}
/* A `klass::NM` read on a class value names whichever constant the runtime
   class owns: the one keyed "<c_name>__NM", or "<...>__<c_name>__NM" under an
   owner's qualified spelling. Asking each class meant walking every constant
   per class, classes x constants per read. Only a constant keyed "<...>__NM"
   can match, so those are listed once (ascending, in a malloc'd *out the
   caller frees), and each class looks among them. */
int comp_const_tail_cands(Compiler *c, const char *nm, int **out) {
  *out = NULL;
  if (!nm) return 0;
  size_t nl = strlen(nm);
  int n = 0, cap = 0;
  for (int i = 0; i < c->nconsts; i++) {
    const char *cn = c->consts[i].name;
    size_t l = strlen(cn);
    if (l < nl + 2 || strcmp(cn + l - nl, nm) != 0 || strncmp(cn + l - nl - 2, "__", 2) != 0) continue;
    if (n == cap) {
      cap = cap ? cap * 2 : 8;
      *out = realloc(*out, sizeof(int) * (size_t)cap);
      if (!*out) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    (*out)[n++] = i;
  }
  return n;
}
/* The constant class k owns under NM (comp_const_tail_cands above), or -1:
   the first, in table order, whose key is "<c_name>__NM" or ends in
   "__<c_name>__NM". A key too long for the buffer is matched as it was
   before the list, against every constant. */
int comp_class_owned_const(Compiler *c, int k, const char *nm, const int *cands, int ncands) {
  const char *kn = c->classes[k].c_name;
  if (!kn || !nm) return -1;
  char tail[512];
  snprintf(tail, sizeof tail, "%s__%s", kn, nm);
  size_t tl = strlen(tail);
  int all = strlen(kn) + 2 + strlen(nm) >= sizeof tail;
  int n = all ? c->nconsts : ncands;
  for (int j = 0; j < n; j++) {
    int ci = all ? j : cands[j];
    const char *cn = c->consts[ci].name;
    size_t l = strlen(cn);
    if (l < tl || strcmp(cn + l - tl, tail) != 0) continue;
    /* either the whole key, or a component edge before it */
    if (l > tl && strncmp(cn + l - tl - 2, "__", 2) != 0) continue;
    return ci;
  }
  return -1;
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
  s->ret_param = -1;
  s->name = name ? strdup(name) : NULL;
  s->def_node = def_node;
  s->body = -1;
  s->class_id = -1;
  s->rest_idx = -1;
  s->kwrest_idx = -1;
  *(TyKind *)&s->ret = TY_UNKNOWN;
  comp_ty_edits++;    /* a new scope is a change of the tables */
  scope_set_ret_noblock(s, TY_UNKNOWN);
  scope_set_ret_withblock(s, TY_UNKNOWN);
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
  lv_set_type(lv, t);
}

int slot_set(Compiler *c, LocalVar *lv, TyKind merged, TyKind t, int node) {
  int held = 0;
  if (!lv->range_held && !c->range_unheld && (t == TY_RANGE || value_may_be_range(c, node))) { lv->range_held = 1; held = 1; }
  if (merged == lv->type) {
    if (!ty_degraded(merged) && merged != TY_UNKNOWN && node >= 0) lv->last_src = node;
    return held;
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
  lv_set_type(lv, merged);
  return 1;
}

int slot_take(Compiler *c, LocalVar *lv, TyKind t, int node) {
  return slot_set(c, lv, ty_unify(lv->type, t), t, node);
}

/* Is node a read of a local or parameter that a Range was written into? */
int local_range_held(Compiler *c, int node) {
  if (node < 0 || nt_kind(c->nt, node) != NK_LocalVariableReadNode) return 0;
  Scope *s = comp_scope_of(c, node);
  const char *nm = nt_str(c->nt, node, "name");
  LocalVar *lv = s && nm ? scope_local(s, nm) : NULL;
  return lv && lv->range_held;
}

/* One value position of a value that may be a Range: a Range-typed value, a
   read of a slot one was written into, a Range literal, or a call of a method
   whose value can be one (Scope.ret_range). A walk that gave up, which a tree
   cannot cause, may be one. */
static int range_value_arm(const VpLeaf *leaf, void *ud) {
  Compiler *c = ud;
  const NodeTable *nt = c->nt;
  if (leaf->kind == VPK_GAVE_UP) return 1;
  if (leaf->kind != VPK_EXPR) return 0;
  int node = leaf->node;
  if (comp_ntype(c, node) == TY_RANGE) return 1;
  switch (nt_kind(nt, node)) {
  case NK_LocalVariableReadNode: return local_range_held(c, node);
  case NK_RangeNode: return 1;
  case NK_CallNode: {
    const char *nm = nt_str(nt, node, "name");
    int recv = nt_ref(nt, node, "receiver"), mi = -1;
    if (recv < 0 || nt_kind(nt, recv) == NK_SelfNode) mi = comp_self_call_mi(c, node, nm);
    else if (ty_is_object(comp_ntype(c, recv))) mi = comp_method_in_chain(c, ty_object_class(comp_ntype(c, recv)), nm, NULL);
    return mi >= 0 && mi < c->nscopes && c->scopes[mi].ret_range;
  }
  default: return 0;
  }
}

/* Can node's value be a Range, by its form: a Range-typed value, a read of
   a slot one was written into, an arm of a conditional, or a call of a
   method whose value can be one (Scope.ret_range)? Read through the
   iterator with the if, unless, case, case/in and begin shapes and `||`/`&&` (both
   sides), which has no limit on the length of an `elsif` ladder or of a chain
   of `||`. */
int value_may_be_range(Compiler *c, int node) {
  if (node < 0) return 0;
  if (comp_ntype(c, node) == TY_RANGE) return 1;
  return for_each_value_position(c->nt, node, VP_AND_LEFT | VPS_IF | VPS_UNLESS | VPS_CASE | VPS_CASE_IN | VPS_BEGIN | VPS_LOGIC, range_value_arm, c);
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
    /* Classes the runtime builds in C. A reopening is refused, but a NEW class
       of that name (`App::Mutex`, or OpenStruct without require "ostruct")
       would clash with the runtime's C names. The builtin-name checks read
       the Ruby name, not this stem, so mangling is safe here. */
    "Monitor", "Mutex", "Queue", "SizedQueue", "ConditionVariable", "Encoding",
    "OpenStruct",
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
  comp_ty_edits++;    /* a new class is a change of the tables */
  c->anon_struct_ids_valid = 0;
  memset(ci, 0, sizeof(*ci));
  ci->ctor_reachable = 1;   /* conservatively, until compute_instantiated's early pass has looked */
  ci->name = name ? strdup(name) : NULL;
  ci->c_name = sp_class_c_name(name);
  ci->is_builtin_const = is_builtin_class_name(name) || is_builtin_module_const_name(name);
  ci->def_node = def_node;
  if (nt_int(c->nt, def_node, "class_new_anonymous", 0)) c->has_anonymous_classes = 1;
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
/* can a computed instance_variable_set reach an instance of some class (or
   main)? Then names no class declares live in the object-keyed table, and
   the by-name reads consult it (#8219) */
int comp_any_computed_ivar_set(Compiler *c) {
  for (int k = 0; k < c->nclasses; k++) if (c->classes[k].computed_ivar_set) return 1;
  return 0;
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
    ci->ivar_types = realloc((TyKind *)ci->ivar_types, sizeof(TyKind) * (size_t)ci->civars);
    ci->ivar_str_shared = realloc(ci->ivar_str_shared, (size_t)ci->civars);
    ci->ivar_elems_shared = realloc(ci->ivar_elems_shared, (size_t)ci->civars);
    ci->ivar_int_table = realloc(ci->ivar_int_table, (size_t)ci->civars);
    ci->ivar_oa_type = realloc(ci->ivar_oa_type, sizeof(TyKind) * (size_t)ci->civars);
    ci->ivar_oa_seed = realloc(ci->ivar_oa_seed, sizeof(int) * (size_t)ci->civars);
    ci->ivar_oa_conflict = realloc(ci->ivar_oa_conflict, (size_t)ci->civars);
    ci->ivar_nullable_int = realloc(ci->ivar_nullable_int, (size_t)ci->civars);
    ci->ivar_nullable_int_elem = realloc(ci->ivar_nullable_int_elem, (size_t)ci->civars);
    ci->ivar_arr_elem_arr_or_nil = realloc(ci->ivar_arr_elem_arr_or_nil, (size_t)ci->civars);
  }
  ci->ivars[ci->nivars] = strdup(name);
  ((TyKind *)ci->ivar_types)[ci->nivars] = TY_UNKNOWN;
  comp_ty_edits++;    /* a new ivar is a change of the table */
  ci->ivar_str_shared[ci->nivars] = 0;
  ci->ivar_elems_shared[ci->nivars] = 0;
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
    comp_ty_edits++;  /* the ivars are renumbered */
#define IV_SWAP(arr, T) do { T _t = ci->arr[idx]; ((T *)ci->arr)[idx] = ci->arr[m]; ((T *)ci->arr)[m] = _t; } while (0)
    IV_SWAP(ivars, char *);
    IV_SWAP(ivar_types, TyKind);
    IV_SWAP(ivar_str_shared, unsigned char);
    IV_SWAP(ivar_elems_shared, unsigned char);
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
  if (owner != cid || comp_cvar_index(&c->classes[cid], name) >= 0) return owner;
  /* No class in the chain declares it: a module included along it may. A
     module's method is copied into each class including it, and a class
     variable it reads (a parameter default `x = @@x`) is the module's,
     which the copy's own class does not name (the most recent include
     first, as Ruby's ancestors run) */
  for (int p = cid; p >= 0; p = c->classes[p].parent) {
    const ClassInfo *ci = &c->classes[p];
    for (int i = ci->nincluded_mods - 1; i >= 0; i--) {
      int m = ci->included_mods[i];
      if (m >= 0 && m < c->nclasses && comp_cvar_index(&c->classes[m], name) >= 0) return m;
    }
  }
  /* and the modules it extends, whose methods `extend` copies in as the
     class's class methods (`class Template; extend Handlers`): they read
     the module's variable as well */
  for (int p = cid; p >= 0; p = c->classes[p].parent) {
    const ClassInfo *ci = &c->classes[p];
    for (int i = ci->nextended_mods - 1; i >= 0; i--) {
      int m = ci->extended_mods[i];
      if (m >= 0 && m < c->nclasses && comp_cvar_index(&c->classes[m], name) >= 0) return m;
    }
  }
  return owner;
}

int comp_cvar_intern(ClassInfo *ci, const char *name) {
  int idx = comp_cvar_index(ci, name);
  if (idx >= 0) return idx;
  if (ci->ncvars >= ci->ccvars) {
    ci->ccvars = ci->ccvars ? ci->ccvars * 2 : 8;
    ci->cvars = realloc(ci->cvars, sizeof(char *) * (size_t)ci->ccvars);
    ci->cvar_types = realloc((TyKind *)ci->cvar_types, sizeof(TyKind) * (size_t)ci->ccvars);
    ci->cvar_nullable_int = realloc(ci->cvar_nullable_int, (size_t)ci->ccvars);
    ci->cvar_str_shared = realloc(ci->cvar_str_shared, (size_t)ci->ccvars);
    ci->cvar_elems_shared = realloc(ci->cvar_elems_shared, (size_t)ci->ccvars);
  }
  ci->cvars[ci->ncvars] = strdup(name);
  ((TyKind *)ci->cvar_types)[ci->ncvars] = TY_UNKNOWN;
  comp_ty_edits++;    /* a new cvar is a change of the table */
  ci->cvar_nullable_int[ci->ncvars] = 0;
  ci->cvar_str_shared[ci->ncvars] = 0;
  ci->cvar_elems_shared[ci->ncvars] = 0;
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
static int sm_nscopes = -1, sm_buckets = 0, sm_frozen = 0, sm_cap = 0;
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
/* Scopes from `from` on into the chains, ascending so the highest scope
   index ends at the head: a redefined method is its LAST def, for a class
   and at the top level alike */
static void sm_add(Compiler *c, int from, int ns) {
  for (int s = from; s < ns; s++) {
    if (!c->scopes[s].name) continue;
    int cm, cls = comp_scope_own_class(c, s, &cm);
    if (cls >= 0) {
      unsigned b = sm_hash(cls, c->scopes[s].name, cm) % (unsigned)sm_buckets;
      sm_next[s] = sm_head[b]; sm_head[b] = s;
    }
    else {
      unsigned b = sm_hash(-1, c->scopes[s].name, 0) % (unsigned)sm_buckets;
      tm_next[s] = tm_head[b]; tm_head[b] = s;
    }
  }
  sm_nscopes = ns;
}
static void sm_build(Compiler *c) {
  int ns = c->nscopes;
  /* While the index is frozen a scope is only ever appended -- the builtins'
     rewrites add a method per site, each followed by a lookup -- and the
     scopes already indexed keep their names and classes: the new ones go
     onto the chains. Rebuilt from nothing when the table was reset, shrank,
     or outgrew twice its buckets. */
  if (sm_nscopes >= 0 && sm_buckets > 0 && ns > sm_nscopes && ns <= 2 * sm_buckets) {
    if (ns > sm_cap) {
      int cap = 2 * sm_buckets;
      int *n1 = realloc(sm_next, (size_t)cap * sizeof(int)), *n2 = n1 ? realloc(tm_next, (size_t)cap * sizeof(int)) : NULL;
      if (n1) sm_next = n1;
      if (n2) tm_next = n2;
      if (!n1 || !n2) { sm_buckets = 0; sm_nscopes = ns; return; }
      sm_cap = cap;
    }
    sm_add(c, sm_nscopes, ns);
    return;
  }
  free(sm_next); free(sm_head); free(tm_next); free(tm_head);
  sm_buckets = ns > 0 ? ns : 1;
  sm_cap = sm_buckets;
  sm_next = malloc((size_t)sm_cap * sizeof(int));
  sm_head = malloc((size_t)sm_buckets * sizeof(int));
  tm_next = malloc((size_t)sm_cap * sizeof(int));
  tm_head = malloc((size_t)sm_buckets * sizeof(int));
  sm_nscopes = ns;
  if (!sm_next || !sm_head || !tm_next || !tm_head) { sm_buckets = 0; return; }
  for (int i = 0; i < sm_buckets; i++) { sm_head[i] = -1; tm_head[i] = -1; }
  sm_add(c, 0, ns);
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
/* (name, instance or class side) -> per class, the method a chain lookup
   answers and the class that defines it. A poly dispatch, an arity plan and
   the instance_eval forwarding map ask it for every class of the program in
   turn, and each ask walked the class's whole superclass chain, resolving
   the name's aliases at every level: about a fifth of a large program's
   inference fixpoint. The answer is shape alone (scopes, aliases, parents),
   so it is kept while the scope index is frozen and no scope is moved, and
   dropped when the index's epoch, the scope or class count, or the class
   shape (comp_class_shape_changed) moves. */
static unsigned cmc_shape = 0;
void comp_class_shape_changed(void) { cmc_shape++; }
static char **cmc_name; static unsigned char *cmc_cm; static int **cmc_ans;
static int cmc_cap = 0, cmc_n = 0;
static const Compiler *cmc_c; static unsigned cmc_gen, cmc_shape_at;
static int cmc_nscopes = -1, cmc_nclasses = -1;
static void cmc_clear(void) {
  for (int i = 0; i < cmc_cap; i++) if (cmc_name && cmc_name[i]) { free(cmc_name[i]); free(cmc_ans[i]); }
  free(cmc_name); free(cmc_cm); free(cmc_ans);
  cmc_name = NULL; cmc_cm = NULL; cmc_ans = NULL; cmc_cap = cmc_n = 0;
}
/* The answers for (name, is_cm), two ints per class (-2 while unasked), or
   NULL when the memo cannot be trusted now. */
static int *cmc_slot(Compiler *c, const char *name, int is_cm) {
  if (!sm_frozen || mv_n || !name || c->nclasses <= 0) return NULL;
  if (cmc_c != c || cmc_gen != sm_gen || cmc_shape_at != cmc_shape ||
      cmc_nscopes != c->nscopes || cmc_nclasses != c->nclasses) {
    cmc_clear();
    cmc_c = c; cmc_gen = sm_gen; cmc_shape_at = cmc_shape;
    cmc_nscopes = c->nscopes; cmc_nclasses = c->nclasses;
  }
  if (cmc_n * 2 >= cmc_cap) {
    int ocap = cmc_cap; char **on = cmc_name; unsigned char *ocm = cmc_cm; int **oa = cmc_ans;
    cmc_cap = ocap ? ocap * 2 : 256;
    cmc_name = calloc((size_t)cmc_cap, sizeof(char *));
    cmc_cm = calloc((size_t)cmc_cap, 1);
    cmc_ans = calloc((size_t)cmc_cap, sizeof(int *));
    if (!cmc_name || !cmc_cm || !cmc_ans) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < ocap; i++) {
      if (!on[i]) continue;
      unsigned j = (sp_strhash(on[i]) ^ ocm[i]) & (unsigned)(cmc_cap - 1);
      while (cmc_name[j]) j = (j + 1) & (unsigned)(cmc_cap - 1);
      cmc_name[j] = on[i]; cmc_cm[j] = ocm[i]; cmc_ans[j] = oa[i];
    }
    free(on); free(ocm); free(oa);
  }
  unsigned j = (sp_strhash(name) ^ (unsigned)is_cm) & (unsigned)(cmc_cap - 1);
  while (cmc_name[j]) {
    if (cmc_cm[j] == is_cm && sp_streq(cmc_name[j], name)) return cmc_ans[j];
    j = (j + 1) & (unsigned)(cmc_cap - 1);
  }
  int *a = malloc(sizeof(int) * 2 * (size_t)c->nclasses);
  cmc_name[j] = strdup(name);
  if (!a || !cmc_name[j]) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int k = 0; k < 2 * c->nclasses; k++) a[k] = -2;
  cmc_cm[j] = (unsigned char)is_cm; cmc_ans[j] = a; cmc_n++;
  return a;
}
static const char *resolve_alias_side(Compiler *c, int class_id, const char *name, int *start_cls, int *builtin, int sg);
static int chain_lookup(Compiler *c, int class_id, const char *name, int *def_class, int is_cm) {
  int *a = class_id >= 0 && class_id < c->nclasses ? cmc_slot(c, name, is_cm) : NULL;
  if (a && a[2 * class_id] != -2) {
    if (a[2 * class_id] >= 0 && def_class) *def_class = a[2 * class_id + 1];
    return a[2 * class_id];
  }
  int start = class_id, mi = -1, dc = -1, bi = 0;
  const char *rn = resolve_alias_side(c, class_id, name, &start, &bi, is_cm ? 1 : 2);
  /* an alias that captured a builtin's method names no method of the
     program's, whatever the class defines of that name later */
  if (bi && !sp_streq(rn, name)) start = -1;
  for (int cid = start; cid >= 0; cid = c->classes[cid].parent) {
    int m = is_cm ? comp_cmethod_in_class(c, cid, rn) : comp_method_in_class(c, cid, rn);
    if (m >= 0) { mi = m; dc = cid; break; }
  }
  if (a) { a[2 * class_id] = mi; a[2 * class_id + 1] = dc; }
  if (mi >= 0 && def_class) *def_class = dc;
  return mi;
}
int comp_cmethod_in_chain(Compiler *c, int class_id, const char *name, int *def_class) {
  return chain_lookup(c, class_id, name, def_class, 1);
}

int comp_method_in_chain(Compiler *c, int class_id, const char *name, int *def_class) {
  return chain_lookup(c, class_id, name, def_class, 0);
}

int comp_ary_root(Compiler *c, int cid) {
  return cid >= 0 && cid < c->nclasses ? c->classes[cid].ary_root - 1 : -1;
}
int comp_ty_ary_root(Compiler *c, TyKind t) {
  return ty_is_object(t) ? comp_ary_root(c, ty_object_class(t)) : -1;
}
/* An Array subclass nothing is ever put into holds boxed values, as an empty
   `[]` that never settles does. While the inference is still optimistic an
   unsettled kind stays unknown, so a later push can still narrow it. */
TyKind comp_ary_kind(Compiler *c, int cid) {
  int r = comp_ary_root(c, cid);
  if (r < 0) return TY_UNKNOWN;
  TyKind k = c->classes[r].ary_kind;
  if (k != TY_UNKNOWN || g_infer_optimistic) return k;
  return c->classes[r].ary_hash ? TY_POLY_POLY_HASH : TY_POLY_ARRAY;
}
int comp_ary_is_hash(Compiler *c, int cid) {
  return comp_ary_root(c, cid) >= 0 && c->classes[cid].ary_hash;
}
int comp_ary_is_str(Compiler *c, int cid) {
  return comp_ary_root(c, cid) >= 0 && c->classes[cid].ary_str;
}
/* The builtin cid's chain embeds as the flag the comp_arysub_answer family
   takes: 0 Array, 1 Hash, 2 String */
int an_str_mutator_name(const char *nm);
int comp_arysub_str_boxed(const char *n) {
  return an_str_mutator_name(n) || sp_streq(n, "<<") || sp_streq(n, "concat") ||
         sp_streq(n, "replace") || sp_streq(n, "clear") || sp_streq(n, "prepend") ||
         sp_streq(n, "insert") || sp_streq(n, "[]=") || sp_streq(n, "freeze") ||
         sp_streq(n, "force_encoding") || sp_streq(n, "frozen?");
}
int comp_arysub_emb(Compiler *c, int cid) {
  return comp_ary_is_str(c, cid) ? 2 : comp_ary_is_hash(c, cid) ? 1 : 0;
}
/* The builtin class cid's chain embeds: "Array", "Hash" or "String" */
const char *comp_arysub_builtin(Compiler *c, int cid) {
  return comp_ary_is_str(c, cid) ? "String" : comp_ary_is_hash(c, cid) ? "Hash" : "Array";
}

int builtin_instance_method_known(const char *cls, const char *m);
/* A name an Array answers: its own methods and Enumerable's, and the
   Object methods it answers as the Array (#7449). */
int comp_array_method_name(const char *n) {
  return builtin_instance_method_known("Array", n) || is_arysub_kernel_name(n);
}
/* The same for the builtin class cid's chain embeds, Array or Hash, and a
   method the program's reopen of that builtin adds (`class Hash; def
   symbolize_keys`): the builtin's dispatch takes it with self boxed. */
int comp_arysub_builtin_name(Compiler *c, int cid, const char *n) {
  /* ...and an alias that reopen adds of a name it answers (activesupport's
     `class Hash; alias_method :blank?, :empty?`), which the builtin's
     dispatch resolves as it does on the builtin itself */
  static int depth = 0;
  int ro = comp_class_index(c, comp_arysub_builtin(c, cid));
  if (ro >= 0 && depth < 8) {
    const char *rn = comp_resolve_alias_ex(c, ro, n, NULL, NULL);
    if (rn && n && !sp_streq(rn, n)) {
      depth++;
      int r = comp_arysub_builtin_name(c, cid, rn);
      depth--;
      if (r) return 1;
    }
  }
  int hash = comp_ary_is_hash(c, cid);
  if (comp_ary_is_str(c, cid)) {
    if (comp_builtin_kind_reopen_mi(c, TY_STRING, n) >= 0) return 1;
    return builtin_instance_method_known("String", n) || is_arysub_kernel_name(n);
  }
  if (comp_builtin_kind_reopen_mi(c, hash ? TY_POLY_POLY_HASH : TY_POLY_ARRAY, n) >= 0) return 1;
  if (!hash) return comp_array_method_name(n);
  return builtin_instance_method_known("Hash", n) || is_arysub_kernel_name(n);
}
/* The program's reopen of the builtin cid's chain embeds, the class right
   above the chain's root, or -1 */
int comp_arysub_reopen(Compiler *c, int cid) {
  int r = comp_ary_root(c, cid);
  int p = r >= 0 ? c->classes[r].parent : -1;
  return p >= 0 && p != r && p == comp_class_index(c, comp_arysub_builtin(c, cid)) ? p : -1;
}
/* Whether a call named n on an instance of Array subclass cid is Array's:
   no method, reader or writer of the class chain takes the name, it asks
   nothing about the object itself, and Array (or Enumerable, which Array
   includes) has it. */
int comp_arysub_name_is_array(Compiler *c, int cid, const char *n) {
  if (comp_ary_root(c, cid) < 0 || !n) return 0;
  /* a method of the builtin's reopen is the builtin's */
  int dc = -1, ro = comp_arysub_reopen(c, cid);
  if (((comp_method_in_chain(c, cid, n, &dc) >= 0 && (ro < 0 || dc != ro)) ||
       comp_reader_in_chain(c, cid, n, NULL)) && !comp_enum_answers_first(c, cid, n)) return 0;
  char base[256];
  if (setter_base_name(n, base, sizeof base) && comp_writer_in_chain(c, cid, base, NULL)) return 0;
  return !is_arysub_object_name(n) && comp_arysub_builtin_name(c, cid, n);
}
/* Whether call `id` on a receiver of type rt, an Array subclass instance, is
   Array's (comp_arysub_name_is_array), or a `super` into Array was rewritten
   into it (builtin_only). *kind is the embedded Array's kind to answer it as. */
int comp_arysub_call(Compiler *c, int id, TyKind rt, TyKind *kind) {
  int cid = ty_is_object(rt) ? ty_object_class(rt) : -1;
  if (comp_ary_root(c, cid) < 0 || nt_kind(c->nt, id) != NK_CallNode) return 0;
  const char *n = nt_str(c->nt, id, "name");
  if (!n) return 0;
  if (!nt_int(c->nt, id, "builtin_only", 0) && !comp_arysub_name_is_array(c, cid, n)) return 0;
  *kind = comp_ary_kind(c, cid);
  /* A String subclass instance's String is a handle (TY_STRBUF). A call
     that changes it, or answers it, takes it boxed (the poly String arms
     work on the handle in place); any other reads a copy of its bytes. */
  if (comp_ary_is_str(c, cid))
    *kind = comp_arysub_str_boxed(n) ? TY_POLY : TY_STRING;
  return 1;
}
/* The builtin-op row flags of call `id` on an Array (#7449): what it answers
   (bop_answers_self) or, with args_builtin, whether it reads an Array
   argument as an Array (bop_args_as_builtin). Every Array kind reads the
   same family rows. */
static int arysub_call_flags(Compiler *c, int id, int hash, int args_builtin) {
  /* hash: 0 an Array, 1 a Hash, 2 a String (comp_arysub_emb) */
  const char *n = nt_str(c->nt, id, "name");
  int args = nt_ref(c->nt, id, "arguments"), argc = 0;
  if (!n) return 0;
  if (args >= 0) nt_arr(c->nt, args, "arguments", &argc);
  int blk = nt_ref(c->nt, id, "block") >= 0;
  /* and every Hash kind the Hash family's */
  TyKind fam = hash == 2 ? TY_STRING : hash ? TY_POLY_POLY_HASH : TY_POLY_ARRAY;
  return args_builtin ? bop_args_as_builtin(fam, n, argc, blk)
                      : bop_answers_self(fam, n, argc, blk);
}
int comp_arysub_answer(Compiler *c, int id, int hash) { return arysub_call_flags(c, id, hash, 0); }
/* Array's answer to call `id` is its receiver -- always (BOPF_SELF) or when
   it changed it (BOPF_SELF_OR_NIL) -- so on an Array subclass instance it
   is the instance (#7449). */
int comp_arysub_self_result(Compiler *c, int id, int hash) {
  return (comp_arysub_answer(c, id, hash) & (BOPF_SELF | BOPF_SELF_OR_NIL)) != 0;
}

/* Whether the arguments of call `id`, on a receiver of type rt (-1: none),
   are read as Arrays, so an Array subclass instance among them is its Array
   (#7449): the methods of an Array receiver that compare, combine or copy
   another Array (BOPF_ARGS_BUILTIN) -- not the stores, which keep the
   instance itself as an element -- and Kernel#puts, which prints an Array's
   elements. */
int comp_arysub_args_viewed(Compiler *c, int id, TyKind rt) {
  const NodeTable *nt = c->nt;
  const char *n = nt_str(nt, id, "name");
  if (!n || nt_kind(nt, id) != NK_CallNode) return 0;
  if (nt_ref(nt, id, "receiver") < 0)
    return sp_streq(n, "puts") && comp_method_index(c, n) < 0;
  int emb = ty_is_hash(rt) ? 1 : (rt == TY_STRING || rt == TY_STRBUF) ? 2 : 0;
  return (array_new_copies(rt) || emb) && arysub_call_flags(c, id, emb, 1);
}

/* `Array(x)`: of an Array subclass instance x it is x itself (Kernel#Array
   takes an Array as it is, #7449). x's node, else -1; the caller asks x's
   type. */
int comp_arysub_kernel_array(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *n = nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "name") : NULL;
  if (!n || !sp_streq(n, "Array") || nt_ref(nt, id, "receiver") >= 0 || nt_ref(nt, id, "block") >= 0 ||
      comp_method_index(c, n) >= 0) return -1;
  int args = nt_ref(nt, id, "arguments"), an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != 1 || nt_kind(nt, av[0]) == NK_SplatNode) return -1;
  return av[0];
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

/* Whether any builtin kind's own class is reopened with a method of `name`
   (comp_builtin_kind_reopen_mi for some kind). A yield site typed per site
   needs every site's answer once one of them is a reopen's, which the
   per-site table (ty_recv_builtin_result) does not carry for most names. */
int comp_builtin_name_reopened(Compiler *c, const char *name) {
  static const TyKind kinds[] = { TY_INT, TY_FLOAT, TY_STRING, TY_SYMBOL, TY_INT_ARRAY, TY_STR_INT_HASH };
  if (!name) return 0;
  for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++)
    if (comp_builtin_kind_reopen_mi(c, kinds[k], name) >= 0) return 1;
  return 0;
}

/* Whether a call on the chain from a yield up to `call` (`yield.size + 1`)
   names a method some builtin class reopens, an alias that captured the
   builtin (builtin_only) aside: the chain's sites are then typed one by
   one. */
int comp_yield_chain_reopened(Compiler *c, int call) {
  const NodeTable *nt = c->nt;
  for (int n = call, depth = 0; n >= 0 && depth < 16; depth++) {
    if (nt_kind(nt, n) == NK_YieldNode) return 0;
    if (nt_kind(nt, n) != NK_CallNode) return 0;
    if (!nt_int(nt, n, "builtin_only", 0) && comp_builtin_name_reopened(c, nt_str(nt, n, "name"))) return 1;
    n = nt_ref(nt, n, "receiver");
  }
  return 0;
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

void comp_cmethod_extend_vis_set(ClassInfo *ci, const char *name, int kind) {
  vis_table_set(&ci->xcm_vis_names, &ci->xcm_vis_kinds, &ci->nxcm_vis, &ci->cxcm_vis, name, kind);
}

/* On each class, the class body's entry comes before an extend copy's. The
   walk stops at the nearest class that defines the method: a parent's entry
   does not reach past a subclass's own `def self.name`. */
int comp_cmethod_vis_declared(Compiler *c, int class_id, const char *name, int *at) {
  if (!name) return SP_VIS_PUBLIC;
  for (int cid = class_id; cid >= 0; cid = c->classes[cid].parent) {
    ClassInfo *ci = &c->classes[cid];
    for (int i = 0; i < ci->ncm_vis; i++)
      if (sp_streq(ci->cm_vis_names[i], name)) { if (at) *at = cid; return ci->cm_vis_kinds[i]; }
    for (int i = 0; i < ci->nxcm_vis; i++)
      if (sp_streq(ci->xcm_vis_names[i], name)) { if (at) *at = cid; return ci->xcm_vis_kinds[i]; }
    if (comp_cmethod_in_class(c, cid, name) >= 0) break;
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
  /* an attribute a module declares (`attr_reader :x; private :x` in it) is
     no method scope with an origin module: the visibility is the included
     module's own table's (#8201) */
  if (mi < 0) {
    for (int cid = class_id, g = 0; cid >= 0 && cid < c->nclasses && g <= c->nclasses; cid = c->classes[cid].parent, g++) {
      ClassInfo *ci = &c->classes[cid];
      for (int m = 0; m < ci->nincluded_mods; m++) {
        int mci = ci->included_mods[m];
        if (mci < 0 || mci >= c->nclasses) continue;
        ClassInfo *mc_ = &c->classes[mci];
        for (int i = 0; i < mc_->nvis; i++)
          if (sp_streq(mc_->vis_names[i], name)) { if (at) *at = mci; return mc_->vis_kinds[i]; }
      }
    }
  }
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
        if (!name_is_plain_setter(nm)) continue;
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
  if (name_is_plain_setter(nm)) return -1;
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
  if (name_is_plain_setter(nm)) return 0;
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
/* A constant (path) segment that resolves: by its name, as a builtin
   class, module or builtin class's constant (Encoding::UTF_8), or as a
   program class the path names (A::B, renamed apart from an X::B) */
static int const_seg_resolves(Compiler *c, int seg) {
  return const_name_resolves(c, nt_str(c->nt, seg, "name")) || const_seg_builtin(c, seg) ||
         const_path_class(c, seg) >= 0;
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
    return !const_seg_resolves(c, v);
  if (vt && sp_streq(vt, "ConstantPathNode")) {
    /* a leaf no member of the program class its parent names is missing,
       though a constant of that name lives elsewhere (JSON::Coder) */
    if (const_path_misses_member(c, v)) return 1;
    /* Walk tail-to-root; any unresolved segment kills the whole path. */
    for (int seg = v; ; ) {
      if (!const_seg_resolves(c, seg)) return 1;
      int par = nt_ref(nt, seg, "parent");
      if (par < 0) return !const_name_resolves_top_level(c, nt_str(nt, seg, "name")) && !const_seg_builtin(c, seg);
      const char *pty = nt_type(nt, par);
      if (pty && sp_streq(pty, "ConstantPathNode")) { seg = par; continue; }
      if (pty && sp_streq(pty, "ConstantReadNode"))
        return !const_seg_resolves(c, par);
      return 0;                                  /* dynamic parent: no fold */
    }
  }
  return 0;
}

/* Do the segments of the constant / constant path under defined? node `pred`
   all resolve at compile time? Those with a presence flag (a segment whose
   assignment a read can precede, LocalVar.const_early, is defined only once
   it has run) have their keys put in early (at most cap), in the order the
   path is walked, and are counted in the answer; -1 when a segment does not
   resolve, more have a flag than fit, or the path is no plain one. */
static int defined_path_keys(Compiler *c, int pred, const char **early, int cap) {
  const NodeTable *nt = c->nt;
  int ne = 0;
  if (pred < 0 || nt_kind(nt, pred) != NK_DefinedNode) return -1;
  int v = nt_ref(nt, pred, "value");
  if (v < 0) return -1;
  NodeKind vk = nt_kind(nt, v);
  if (vk != NK_ConstantReadNode && vk != NK_ConstantPathNode) return -1;
  /* each leaf must be a member of what its parent names, not only resolve
     by its own name somewhere in the program */
  if (vk == NK_ConstantPathNode && const_path_misses_member(c, v)) return -1;
  for (int seg = v; ; ) {
    const char *nm = nt_str(nt, seg, "name");
    if (!const_seg_resolves(c, seg)) return -1;
    LocalVar *lv = comp_const(c, nm);
    if (lv && lv->const_early && ne < cap) early[ne++] = lv->name;
    else if (lv && lv->const_early) return -1;
    if (vk != NK_ConstantPathNode) return ne;
    int par = nt_ref(nt, seg, "parent");
    /* `::Root` anchor: the root must be defined at the TOP level, not
       through a same-named nested module (#3320) */
    if (par < 0) return const_name_resolves_top_level(c, nm) || const_seg_builtin(c, seg) ? ne : -1;
    if (nt_kind(nt, par) == NK_ConstantPathNode) { seg = par; continue; }
    if (nt_kind(nt, par) == NK_ConstantReadNode) {
      const char *pn = nt_str(nt, par, "name");
      if (!const_seg_resolves(c, par)) return -1;
      LocalVar *plv = comp_const(c, pn);
      if (plv && plv->const_early && ne < cap) early[ne++] = plv->name;
      else if (plv && plv->const_early) return -1;
      return ne;
    }
    return -1;                                  /* dynamic parent: no fold */
  }
}

/* The dual: a defined? guard that is statically TRUE -- every segment of the
   constant / constant path resolves at compile time. Lets value-position
   `defined?(K) ? K : fallback` drop its dead fallback arm (which may not
   type-unify with the live one). Deliberately narrow like
   comp_defined_guard_false: plain constants / constant paths only, no `&&`
   chains (a truthy left arm doesn't decide the conjunction). A segment a
   read can precede the assignment of is not defined yet, so no guard on one
   is static. */
int comp_defined_guard_true(Compiler *c, int pred) {
  const char *early[1];
  return defined_path_keys(c, pred, early, 0) == 0;
}

/* The constants a statically resolved defined? path holds a presence flag
   for, whose flags say whether it is defined now (the count, their keys in
   early); -1 when the path is not one that resolves at compile time. */
int comp_defined_early(Compiler *c, int pred, const char **early, int cap) {
  return defined_path_keys(c, pred, early, cap);
}

/* A literal ArrayNode that is built as an sp_IntArray: one element at least,
   each an integer literal that fits an sp_int. The folds over a nested
   literal read every row through that pointer type without a test, so a row
   built as anything else is not one: an empty `[]` (a poly array, having no
   element to take a kind from) or a row holding a literal past int64 (a
   Bignum, boxed). Each read as an sp_IntArray answered a length of 8 and
   the poly array's storage as its elements. */
static int is_int_array_literal(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty || !sp_streq(ty, "ArrayNode")) return 0;
  int en = 0; const int *els = nt_arr(nt, node, "elements", &en);
  if (en == 0) return 0;
  for (int i = 0; i < en; i++) {
    const char *et = nt_type(nt, els[i]);
    if (!et || !sp_streq(et, "IntegerNode") || nt_str(nt, els[i], "bigval")) return 0;
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

int name_list_has(char **list, int n, const char *name) {
  if (!name) return 0;
  for (int i = 0; i < n; i++) if (sp_streq(list[i], name)) return 1;
  return 0;
}
static void name_add(char ***list, int *n, int *cap, const char *name) {
  if (name_list_has(*list, *n, name)) return;
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
int comp_is_reader(ClassInfo *ci, const char *name) { return name_list_has(ci->readers, ci->nreaders, name); }
void comp_add_own_reader(ClassInfo *ci, const char *name) {
  comp_add_reader(ci, name);
  name_add(&ci->own_readers, &ci->nown_readers, &ci->cown_readers, name);
}
int comp_is_own_reader(ClassInfo *ci, const char *name) { return name_list_has(ci->own_readers, ci->nown_readers, name); }
int comp_is_writer(ClassInfo *ci, const char *name) { return name_list_has(ci->writers, ci->nwriters, name); }

/* A plain setter name: `x=`, but not the operators that also end in `=`
   (`==`, `!=`, `<=`, `>=`, `===`) and not `[]=`, whose value form is its own. */
int name_is_plain_setter(const char *name) {
  size_t ln = name ? strlen(name) : 0;
  if (ln < 2 || name[ln - 1] != '=') return 0;
  char p = name[ln - 2];
  return p != '=' && p != '!' && p != '<' && p != '>' && p != ']';
}
/* A copy of name, and of "attr=" (the setter name an attribute's writer is
   called by), each made as long as the name needs; the caller frees it. */
char *name_dup(const char *name) {
  size_t n = strlen(name);
  char *s = malloc(n + 1);
  if (!s) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(s, name, n + 1);
  return s;
}
char *setter_name_of(const char *attr) {
  size_t n = strlen(attr);
  char *s = malloc(n + 2);
  if (!s) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(s, attr, n);
  s[n] = '=';
  s[n + 1] = '\0';
  return s;
}
/* Is CallNode `id` a setter written as an assignment (`obj.x = v`), whose
   value is the argument whatever the writer returns? Prism flags it
   (nt_attr_write), and a desugar that writes one sets the flag. A call the
   send desugar retargeted (`obj.send(:x=, v)`) is a plain method call, not
   flagged: its value is what the method returns (#4921). `[]=` has its own
   value form, so it is no plain setter. */
int call_is_setter_assign(const NodeTable *nt, int id) {
  return id >= 0 && nt_attr_write(nt, id) && name_is_plain_setter(nt_str(nt, id, "name"));
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
    if (name_list_has(c->classes[cid].undefs, c->classes[cid].nundefs, name)) return 1;
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
int comp_is_sg_civ(ClassInfo *ci, const char *name) { return name_list_has(ci->sg_civ, ci->nsg_civ, name); }
int comp_is_sg_inh(ClassInfo *ci, const char *name) { return name_list_has(ci->sg_inh, ci->nsg_inh, name); }
void comp_add_sg_inh(ClassInfo *ci, const char *name) {
  if (name_list_has(ci->sg_inh, ci->nsg_inh, name)) return;
  if (ci->nsg_inh >= ci->csg_inh) {
    ci->csg_inh = ci->csg_inh ? ci->csg_inh * 2 : 4;
    ci->sg_inh = realloc(ci->sg_inh, sizeof(char *) * (size_t)ci->csg_inh);
  }
  ci->sg_inh[ci->nsg_inh++] = strdup(name);
}
int comp_is_sg_reader(ClassInfo *ci, const char *name) { return name_list_has(ci->sg_readers, ci->nsg_readers, name); }
int comp_is_sg_writer(ClassInfo *ci, const char *name) { return name_list_has(ci->sg_writers, ci->nsg_writers, name); }

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
    ci->alias_sg = realloc(ci->alias_sg, sizeof(int) * (size_t)ci->caliases);
  }
  ci->alias_new[ci->naliases] = strdup(new_name);
  ci->alias_old[ci->naliases] = strdup(old_name);
  ci->alias_cls[ci->naliases] = -1;
  ci->alias_node[ci->naliases] = alias_node;
  ci->alias_builtin[ci->naliases] = 0;
  ci->alias_sg[ci->naliases] = 0;
  ci->naliases++;
  comp_class_shape_changed();
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

/* An instance method's lookup reads the instance aliases only: a singleton
   alias (`class << self; alias_method :stamp, :now`) names a class method,
   and an instance call of the same name must not take it. */
const char *comp_resolve_alias_ex(Compiler *c, int class_id, const char *name, int *start_cls, int *builtin) {
  return resolve_alias_side(c, class_id, name, start_cls, builtin, 2);
}
/* The class-method side of a class reads its singleton aliases
   (`class << self; alias a b`) and those of the modules it extends: an
   instance one names no class method (`alias name cop_name` made a class
   method's Module#name call the class method cop_name), and a module's side
   reads the instance ones too. */
const char *comp_resolve_cm_alias_at(Compiler *c, int class_id, const char *name, int *start_cls) {
  return resolve_alias_side(c, class_id, name, start_cls, NULL, 1);
}
/* Does class `k` have the `class << self` writer that setter `name` (`x=`,
   or an alias of one) reaches? Its attribute name (`x`) goes to `base`. */
int comp_sg_writer_reached(Compiler *c, int k, const char *name, char *base, size_t cap) {
  const char *rn = comp_resolve_cm_alias_at(c, k, name, NULL);
  return setter_base_name(rn ? rn : name, base, cap) && comp_is_sg_writer(&c->classes[k], base);
}
/* ... and, for the builtin an alias captured (2: a builtin singleton method),
   the name of it, as comp_resolve_alias_ex answers for the instance side */
const char *comp_resolve_cm_alias_ex(Compiler *c, int class_id, const char *name, int *builtin) {
  return resolve_alias_side(c, class_id, name, NULL, builtin, 1);
}
static const char *resolve_alias_side(Compiler *c, int class_id, const char *name, int *start_cls, int *builtin, int sg) {
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
    int from = class_id;
    for (int cid = class_id; cid >= 0 && !next; cid = c->classes[cid].parent) {
      ClassInfo *ci = &c->classes[cid];
      /* The class side (1) reads the singleton aliases, and in a module the
         instance ones too: a module's instance alias can be a module
         function as well (`alias shellwords shellsplit; module_function
         :shellwords`, or `extend self`). An instance method's lookup (2)
         reads the instance aliases only. */
      /* ... but not over a class method the module defines itself */
      int mod = ci->def_node >= 0 && nt_kind(c->nt, ci->def_node) == NK_ModuleNode &&
                !(sg == 1 && comp_cmethod_in_class(c, cid, name) >= 0);
      for (int i = (cid == lim_cls ? lim : ci->naliases) - 1; i >= 0; i--) {
        int asg = ci->alias_sg && ci->alias_sg[i];
        if ((sg == 1 ? asg || mod : sg == 2 ? !asg : 1) && sp_streq(ci->alias_new[i], name)) {
          /* a class below that defines the name itself, as the kind looked
             up, answers it: the ancestor's alias of that name is behind it
             (`attr_reader :loc` in a subclass of one with `alias loc
             location`). Asked only once an alias is found, which most
             lookups never do. */
          for (int d = from; d >= 0 && d != cid; d = c->classes[d].parent)
            if (sg == 1 ? comp_cmethod_in_class(c, d, name) >= 0
                        : comp_method_in_class(c, d, name) >= 0 || comp_is_reader(&c->classes[d], name)) return name;
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
            if (builtin) *builtin = ci->alias_builtin[i];
            return next;
          }
          break;
        }
      }
      /* on the class side, a module the class extends brings its instance
         aliases as the class's own -- behind the class methods the class
         defines itself, which its singleton class answers first */
      if (sg == 1 && !next && comp_cmethod_in_class(c, cid, name) >= 0) break;
      for (int e = 0; sg == 1 && !next && e < ci->nextended_mods; e++) {
        int m = ci->extended_mods[e];
        if (m < 0 || m >= c->nclasses) continue;
        ClassInfo *mi = &c->classes[m];
        for (int i = mi->naliases - 1; i >= 0 && !next; i--)
          if (!(mi->alias_sg && mi->alias_sg[i]) && sp_streq(mi->alias_new[i], name)) next = mi->alias_old[i];
      }
    }
    if (!next) return name;
    name = next;
  }
  return name;
}

/* Is `name` in class_id's chain only a singleton alias (`class << self;
   alias a b`), with no instance method, reader, writer (`x=`) or instance
   alias of its own? A reflection that answers for an instance from the
   alias table asks this first. */
int comp_name_is_sg_alias_only(Compiler *c, int class_id, const char *name) {
  int sg = 0;
  /* a writer is listed by the name it assigns (`x` for `x=`) */
  char wbase[256] = "";
  setter_base_name(name, wbase, sizeof wbase);
  for (int cid = class_id; cid >= 0 && cid < c->nclasses; cid = c->classes[cid].parent) {
    ClassInfo *ci = &c->classes[cid];
    for (int i = 0; i < ci->naliases; i++) {
      if (!sp_streq(ci->alias_new[i], name)) continue;
      if (ci->alias_sg && ci->alias_sg[i]) sg = 1; else return 0;
    }
    if (comp_method_in_class(c, cid, name) >= 0 || comp_is_reader(ci, name) || (wbase[0] && comp_is_writer(ci, wbase)))
      return 0;
  }
  return sg;
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

/* Is scope mi the def a native_func gets so a module value can reach it
   (desugar_native_func_defs)? A call on the module's constant that reaches
   it calls the C symbol instead. */
int comp_scope_native_twin(Compiler *c, int mi) {
  return mi >= 0 && mi < c->nscopes && c->scopes[mi].def_node >= 0 &&
         nt_int(c->nt, c->scopes[mi].def_node, "native_twin", 0);
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

/* The class whose bindings serve class k's arm for `name`: k itself for a
   native class, the native class a subclass of one reaches (#9269) where
   that class binds the name and no method of the program's in k's chain
   answers it first. -1 for an arm of the program's methods. */
int comp_native_binding_class(Compiler *c, int k, const char *name) {
  if (k < 0 || k >= c->nclasses) return -1;
  if (c->classes[k].is_native_class) return k;
  int r = comp_native_answer_class(c, k, name);
  return r >= 0 && comp_native_method_find(c, r, name, 0, 0) >= 0 ? r : -1;
}
/* The native class whose binding answers `name` on an instance of class
   cid: cid itself for a native class, and for a subclass of one (#9269)
   the native class its chain reaches, where no method or reader of the
   program's in the chain answers the name first. -1 otherwise. */
int comp_native_answer_class(Compiler *c, int cid, const char *name) {
  if (cid < 0 || cid >= c->nclasses) return -1;
  if (c->classes[cid].is_native_class) return cid;
  int r = c->classes[cid].nsub_root - 1;
  if (r < 0 || !name) return -1;
  if (comp_method_in_chain(c, cid, name, NULL) >= 0 || comp_reader_in_chain(c, cid, name, NULL)) return -1;
  /* (nor an attribute writer of the program's) */
  char base[256];
  if (setter_base_name(name, base, sizeof base) && comp_writer_in_chain(c, cid, base, NULL)) return -1;
  return r;
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
  int nk = comp_native_binding_class(c, k, name);
  if (nk >= 0)
    return comp_native_method_find(c, nk, name, 0, 0) >= 0;
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
  int nk = comp_native_binding_class(c, k, name);
  if (nk >= 0) {
    int nmi = comp_native_method_find(c, nk, name, argc, 0);
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
   2^63-1 are Bignums, and the signed ones take the same path */
int comp_iob_ty_is_64(int t) {
  extern int sp_target_int_bits;
  /* on a 32-bit target the 32-bit types are in the same position: a U32
     above 2**31-1 is a Bignum there (#4647) */
  if (sp_target_int_bits == 32 && t >= 6 && t <= 9) return 1;   /* u32/s32/U32/S32 */
  return t >= 10 && t <= 13;    /* u64/s64/U64/S64 */
}

/* Type-keyed variant: among same-name same-arity bindings, prefer one whose
   arg specs match the call's inferred arg types (putc(65) -> the [:int]
   binding, putc("A") -> [:string]). argtys may be NULL (arity-only). */
int comp_native_method_find_typed(Compiler *c, int class_id, const char *name, int argc, int kind,
                                  const TyKind *argtys) {
  if (class_id < 0 || !name) return -1;
  /* a subclass of a native class answers with the class's bindings where
     no method of the program's in its chain answers the name (#9269) */
  if (class_id < c->nclasses && !c->classes[class_id].is_native_class && c->classes[class_id].nsub_root > 0) {
    class_id = comp_native_answer_class(c, class_id, name);
    if (class_id < 0) return -1;
  }
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
  /* `name` is the attribute's base, and an alias of it is the reader's:
     `alias to_s name` defines no `to_s=`, so it is not followed here */
  for (int cid = class_id; cid >= 0; cid = c->classes[cid].parent)
    if (comp_is_writer(&c->classes[cid], name)) { if (def_class) *def_class = cid; return 1; }
  return 0;
}

/* Whether the program answers `name` at one level of the lookup, class or
   module k itself: a def of its own (not a copy an include made), a reader
   its own body declared, or an alias. */
static int enum_level_answers(Compiler *c, int k, const char *name) {
  ClassInfo *ci = &c->classes[k];
  int mi = comp_method_in_class(c, k, name);
  if (mi >= 0 && (!c->scopes[mi].is_include_copy || c->scopes[mi].is_prepend_copy)) return 1;
  if (comp_is_own_reader(ci, name)) return 1;
  for (int a = 0; a < ci->naliases; a++)
    if ((!ci->alias_sg || !ci->alias_sg[a]) && sp_streq(ci->alias_new[a], name)) return 1;
  return 0;
}
/* Ruby's lookup from level k: 1 when the program answers first, 2 when
   Enumerable does, 0 when neither does at k or below the modules it
   includes (the last included first, Enumerable at its place among them).
   `own_enum` is whether the Enumerable k brings, its own include or that of
   a module it includes, counts: it does not where a superclass has it among
   its ancestors already (the include is then a no-op). */
/* Whether Enumerable is among the ancestors class or module k brings: it
   includes (or prepends) it, or includes a module that does. */
static int enum_reaches(Compiler *c, int k, int depth) {
  ClassInfo *ci = &c->classes[k];
  if (ci->enum_inc_at || ci->enum_prepended) return 1;
  for (int j = 0; j < ci->nincluded_mods && depth < 16; j++) {
    int m = ci->included_mods[j];
    if (m >= 0 && m < c->nclasses && m != k && enum_reaches(c, m, depth + 1)) return 1;
  }
  return 0;
}
static int enum_level_walk(Compiler *c, int k, const char *name, int own_enum, int depth) {
  ClassInfo *ci = &c->classes[k];
  if (ci->enum_prepended) return 2;   /* in front of the class's own */
  if (enum_level_answers(c, k, name)) return 1;
  int at = own_enum && ci->enum_inc_at ? ci->enum_inc_at - 1 : -1;
  for (int j = ci->nincluded_mods - 1; ; j--) {
    if (at >= 0 && j == at - 1) return 2;
    if (j < 0) return 0;
    int m = ci->included_mods[j];
    if (m < 0 || m >= c->nclasses || m == k || depth >= 16) continue;
    int r = enum_level_walk(c, m, name, own_enum, depth + 1);
    if (r) return r;
  }
}
int comp_enum_answers_first(Compiler *c, int cid, const char *name) {
  /* what master answers: the program's method or reader anywhere in the chain */
  int master = comp_method_in_chain(c, cid, name, NULL) < 0 && !comp_reader_in_chain(c, cid, name, NULL);
  if (cid < 0 || cid >= c->nclasses || !name) return master;
  /* a name Enumerable does not have (each, join, index ...): the program's alone */
  if (core_method_vis("Enumerable", name, 0) <= 0) return master;
  /* the topmost class that has Enumerable among its ancestors, by an include
     of its own or of a module: below it the include is a no-op. A Struct, an Array or a native class has Enumerable of its
     own, at a place the includes do not show: master's answer there. */
  int top = -1, n = 0;
  for (int k = cid; k >= 0 && k < c->nclasses && n < 256; k = c->classes[k].parent, n++) {
    ClassInfo *ci = &c->classes[k];
    if (ci->is_struct || ci->is_native_class || ci->nsub_root > 0 || comp_ary_root(c, k) >= 0) {
      /* only a prepended Enumerable is placed: in front of the class it is
         prepended to, behind the ones below it */
      for (int j = cid, g = 0; g < 256; j = c->classes[j].parent, g++) {
        if (c->classes[j].enum_prepended) return 1;
        if (enum_level_answers(c, j, name)) return 0;
        if (j == k || c->classes[j].parent < 0) break;
      }
      return master;
    }
    if (enum_reaches(c, k, 0)) top = k;
    if (k == ci->parent) break;
  }
  n = 0;
  for (int k = cid; k >= 0 && k < c->nclasses && n < 256; k = c->classes[k].parent, n++) {
    int r = enum_level_walk(c, k, name, k == top, 0);
    if (r) return r == 2;
    if (k == c->classes[k].parent) break;
  }
  return master;
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
struct pc_entry { char *name; PolyCand *cands; unsigned char *overridden; int n; int fi; struct pc_entry *next; };
#define PC_BUCKETS 4096
static struct pc_entry *pc_tab[PC_BUCKETS];
static struct pc_entry *cc_tab[PC_BUCKETS];   /* class methods: comp_cmethod_candidates */
static struct pc_entry *pf_tab[PC_BUCKETS];   /* per (name, receiver set): comp_poly_candidates_at */
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
    for (struct pc_entry *e = pf_tab[b]; e; ) { struct pc_entry *nx = e->next; e->next = pc_retired; pc_retired = e; e = nx; }
    pf_tab[b] = NULL;
  }
}
void comp_poly_candidates_reset(void) {
  pc_clear();
  for (int b = 0; b < PC_BUCKETS; b++) {
    for (struct pc_entry *e = cc_tab[b]; e; ) { struct pc_entry *nx = e->next; e->next = pc_retired; pc_retired = e; e = nx; }
    cc_tab[b] = NULL;
  }
  for (struct pc_entry *e = pc_retired; e; ) { struct pc_entry *nx = e->next; free(e->name); free(e->cands); free(e->overridden); free(e); e = nx; }
  pc_retired = NULL;
}
static void pc_build(Compiler *c, const char *name, PolyCand **out, int *n_out) {
  PolyCand *v = NULL; int n = 0, cap = 0;
  for (int k = 0; k < c->nclasses; k++) {
    PolyCand pc; pc.cls = k; pc.rdcls = -1; pc.native = c->classes[k].is_native_class;
    int mdc = -1, rdc = -1;
    pc.mi = comp_method_in_chain(c, k, name, &mdc);
    /* a subclass of a native class that answers the name with the class's
       binding (#9269) */
    if (!pc.native && c->classes[k].nsub_root > 0 && comp_native_binding_class(c, k, name) >= 0) pc.native = 1;
    /* a reader the chain declares below the method answers in its place:
       a subclass's attr_reader overrides the def it inherits, as the
       dispatch's arm does */
    if (!pc.native && comp_reader_in_chain(c, k, name, &rdc) &&
        (pc.mi < 0 || (rdc >= 0 && mdc >= 0 && rdc != mdc && is_descendant(c, rdc, mdc)))) {
      pc.mi = -1; pc.rdcls = rdc;
    }
    if (!pc.native && pc.mi < 0 && pc.rdcls < 0) continue;
    if (n == cap) { cap = cap ? cap * 2 : 8; v = realloc(v, sizeof *v * (size_t)cap); }
    v[n++] = pc;
  }
  *out = v; *n_out = n;
}
static struct pc_entry *pc_memo(Compiler *c, const char *name) {
  if (!sm_frozen) return NULL;
  if (pc_gen_stamp != sm_gen || pc_nscopes_stamp != c->nscopes || pc_nclasses_stamp != c->nclasses || pc_table_stamp != comp_table_gen) {
    pc_clear(); pc_gen_stamp = sm_gen; pc_nscopes_stamp = c->nscopes; pc_nclasses_stamp = c->nclasses; pc_table_stamp = comp_table_gen;
  }
  unsigned b = sp_strhash(name) % PC_BUCKETS;
  for (struct pc_entry *e = pc_tab[b]; e; e = e->next)
    if (sp_streq(e->name, name)) return e;
  /* While a scope is moved for an instance_exec block (comp_scope_move_begin)
     the lookups leave the moved method out, so a list made then would miss
     it after the move: answer it, but keep it off the memo. */
  if (mv_n) return NULL;
  struct pc_entry *e = calloc(1, sizeof *e);
  if (!e) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  e->name = strdup(name);
  if (!e->name) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  e->n = -1;  /* an override query need not build the candidate list */
  e->next = pc_tab[b]; pc_tab[b] = e;
  return e;
}
const PolyCand *comp_poly_candidates(Compiler *c, const char *name, int *n) {
  if (!name) { *n = 0; return NULL; }
  struct pc_entry *e = pc_memo(c, name);
  if (!e || mv_n) {
    /* scope shape may still change: answer fresh, and keep nothing */
    e = calloc(1, sizeof *e);
    pc_build(c, name, &e->cands, &e->n);
    e->next = pc_retired; pc_retired = e;
  }
  else if (e->n < 0) pc_build(c, name, &e->cands, &e->n);
  *n = e->n; return e->cands;
}

/* The candidates of `name` for the call `id`: comp_poly_candidates less the
   classes the call's receiver never holds (cls_fact_excludes, #8530), in the
   same order. Every place that counts, types, binds or emits a boxed call's
   arms asks through here, so they agree on the arms. Memoized per (name,
   receiver set) under comp_poly_candidates' stamps; a receiver with no fact
   answers the full list itself. */
const PolyCand *comp_poly_candidates_at(Compiler *c, int id, const char *name, int *n) {
  int npc = 0;
  const PolyCand *p = comp_poly_candidates(c, name, &npc);
  int recv = id >= 0 ? nt_ref(c->nt, id, "receiver") : -1;
  int fi = cls_fact_idx(c, recv);
  if (fi <= 0) { *n = npc; return p; }
  struct pc_entry *e = NULL;
  int memo = sm_frozen && !mv_n;
  if (memo) {
    (void)pc_memo(c, name);   /* the stamps: a stale table is cleared with pc_tab's */
    unsigned b = (sp_strhash(name) * 31u + (unsigned)fi) % PC_BUCKETS;
    for (e = pf_tab[b]; e; e = e->next)
      if (e->fi == fi && sp_streq(e->name, name)) { *n = e->n; return e->cands; }
    e = calloc(1, sizeof *e);
    if (!e) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    e->name = strdup(name);
    if (!e->name) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    e->fi = fi;
    e->next = pf_tab[b]; pf_tab[b] = e;
  }
  else {
    e = calloc(1, sizeof *e);
    if (!e) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    e->next = pc_retired; pc_retired = e;
  }
  e->cands = npc ? malloc(sizeof *e->cands * (size_t)npc) : NULL;
  if (npc && !e->cands) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  e->n = 0;
  for (int i = 0; i < npc; i++)
    if (!cls_fact_excludes(c, recv, p[i].cls)) e->cands[e->n++] = p[i];
  *n = e->n; return e->cands;
}

/* ---- Which class objects can be values ----
   A class's or a module's object is a value -- something a variable, a box
   or a container can hold, so that a call on a receiver of no fixed class
   may reach its class methods -- unless every way the program names it
   keeps it from being one. It is one when:
     - its constant is read anywhere but as the receiver of a call that
       answers neither it nor a block of its own (a class method of its own,
       or one of is_class_plain_query), a namespace (`K::X`), a class or module
       header, a superclass, an `include`, `extend` or `prepend`, an
       `is_a?`-like test, a `raise`, a `rescue` list, a `when`, or `defined?`;
     - `self` is so read in its class body or class methods (blocks in them
       included), or in a module's instance methods (as a module_function
       or through `extend`, self is the module or the class that extends it);
       the class's descendants then are too (their class methods run the
       same body);
     - `self` appears in a block an instance_eval, class_eval or
       define_singleton_method family call runs on its constant or its self;
     - the program calls `class` anywhere and it is a class (a module has
       no instances);
     - the program asks a class or module for its relatives (ancestors,
       superclass, subclasses, included_modules) and it has a superclass, a
       subclass, or is included, extended or prepended anywhere;
     - it is no class or module the program defines with a header (a
       Struct.new, a singleton class, a reopened builtin, a native class);
     - the program reflects on constants or methods (const_get, nesting,
       ObjectSpace's each_object, eval, binding, Marshal.load, or a Method's
       owner where it makes Methods): every class is. */
/* every class whose name's last segment is nm (`A::B` names B) */
static void cv_mark_name(Compiler *c, unsigned char *v, const char *nm) {
  for (int k = 0; nm && k < c->nclasses; k++) {
    const char *cn = c->classes[k].name, *seg = cn ? strrchr(cn, ':') : NULL;
    if (cn && sp_streq(seg ? seg + 1 : cn, nm)) v[k] = 1;
  }
}
static void cv_mark_tree(Compiler *c, unsigned char *v, int k) {
  if (k < 0 || k >= c->nclasses) return;
  v[k] = 1;
  int nd = 0;
  const int *ds = comp_descendants(c, k, &nd);
  for (int i = 0; i < nd; i++) v[ds[i]] = 1;
  /* (a module's methods run with the class that extends it as self) */
  for (int j = 0; j < c->nclasses; j++)
    for (int e = 0; e < c->classes[j].nextended_mods; e++)
      if (c->classes[j].extended_mods[e] == k) v[j] = 1;
}
/* Is there a `self` in subtree n not marked as no value (nonval), outside
   instance methods (`def m`, with module 1 inside them too) and nested
   class or module bodies? */
static int cv_self_value(const NodeTable *nt, const unsigned char *nonval, int n, int module) {
  if (n < 0) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_SelfNode) return !nonval[n];
  if (k == NK_ClassNode || k == NK_ModuleNode) return 0;
  if (k == NK_DefNode && nt_ref(nt, n, "receiver") < 0 && !module) return 0;
  /* (a `class << self` body's methods are class methods) */
  if (k == NK_SingletonClassNode) module = 1;
  for (int i = 0; i < nt_num_refs(nt, n); i++)
    if (cv_self_value(nt, nonval, nt_ref_at(nt, n, i), module)) return 1;
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int m = 0; const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) if (cv_self_value(nt, nonval, ids[j], module)) return 1;
  }
  return 0;
}
static void cv_nonval_args(const NodeTable *nt, unsigned char *nonval, int call) {
  int args = nt_ref(nt, call, "arguments"), an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  for (int a = 0; a < an; a++) nonval[av[a]] = 1;
}
/* A call with no receiver in a class or module body, made on the class as
   self, that answers the class (is_class_self_answer: include,
   private_class_method ...) makes it a value where its answer is used:
   `X = private_class_method(:helper)` names the class X. Its answer is
   unused as a statement before the last of a list, and as the last of a
   class body whose class expression is unused itself; any other position
   counts as used. cls is the enclosing class's constant path, discarded
   whether n's own value is unused. */
static void cv_self_answers(Compiler *c, unsigned char *v, int n, int cls, int discarded, int depth) {
  const NodeTable *nt = c->nt;
  if (n < 0 || depth > 400) return;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_CallNode && !discarded && cls >= 0 && nt_ref(nt, n, "receiver") < 0 &&
      is_class_self_answer(nt_str(nt, n, "name")))
    cv_mark_name(c, v, nt_str(nt, cls, "name"));
  if (k == NK_StatementsNode) {
    int m = 0; const int *ids = nt_arr(nt, n, "body", &m);
    for (int j = 0; j < m; j++) cv_self_answers(c, v, ids[j], cls, j < m - 1 || discarded, depth + 1);
    return;
  }
  int body = (k == NK_ClassNode || k == NK_ModuleNode) ? nt_ref(nt, n, "body") : -1;
  int ncls = body >= 0 ? nt_ref(nt, n, "constant_path") : cls;
  for (int i = 0; i < nt_num_refs(nt, n); i++) {
    int r = nt_ref_at(nt, n, i);
    if (r == body) cv_self_answers(c, v, r, ncls, discarded, depth + 1);
    else cv_self_answers(c, v, r, ncls, 0, depth + 1);
  }
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int m = 0; const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) cv_self_answers(c, v, ids[j], ncls, 0, depth + 1);
  }
}
static void cv_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  int nk = c->nclasses, nn = nt->count;
  unsigned char *v = calloc((size_t)nk + 1, 1), *nonval = calloc((size_t)nn + 1, 1);
  if (!v || !nonval) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int all = 0, rel = 0, cls_call = 0, owner = 0, methods = 0;
  for (int k = 0; k < nk; k++) {
    int dn = c->classes[k].def_node;
    NodeKind dk = dn >= 0 && dn < nn ? nt_kind(nt, dn) : NK_NONE;
    if ((dk != NK_ClassNode && dk != NK_ModuleNode) || c->classes[k].is_singleton_of ||
        c->classes[k].is_native_class || class_is_prim_reopen(c, k))
      v[k] = 1;
  }
  /* the positions that are no value */
  NT_FOREACH_KIND(nt, NK_ConstantPathNode, n) { int p = nt_ref(nt, n, "parent"); if (p >= 0) nonval[p] = 1; }
  NT_FOREACH_KIND(nt, NK_ClassNode, n) {
    int p = nt_ref(nt, n, "constant_path"), s = nt_ref(nt, n, "superclass");
    if (p >= 0) nonval[p] = 1;
    if (s >= 0) nonval[s] = 1;
  }
  NT_FOREACH_KIND(nt, NK_ModuleNode, n) { int p = nt_ref(nt, n, "constant_path"); if (p >= 0) nonval[p] = 1; }
  NT_FOREACH_KIND(nt, NK_RescueNode, n) {
    int en = 0; const int *ev = nt_arr(nt, n, "exceptions", &en);
    for (int i = 0; i < en; i++) nonval[ev[i]] = 1;
  }
  NT_FOREACH_KIND(nt, NK_CaseNode, n) {
    int wn = 0; const int *wv = nt_arr(nt, n, "conditions", &wn);
    for (int w = 0; w < wn; w++) {
      int en = 0; const int *ev = nt_arr(nt, wv[w], "conditions", &en);
      for (int i = 0; i < en; i++) nonval[ev[i]] = 1;
    }
  }
  NT_FOREACH_KIND(nt, NK_DefinedNode, n) { int p = nt_ref(nt, n, "value"); if (p >= 0) nonval[p] = 1; }
  NT_FOREACH_KIND(nt, NK_DefNode, n) { int r = nt_ref(nt, n, "receiver"); if (r >= 0) nonval[r] = 1; }
  NT_FOREACH_KIND(nt, NK_SingletonClassNode, n) { int e = nt_ref(nt, n, "expression"); if (e >= 0) nonval[e] = 1; }
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    const char *nm = nt_str(nt, n, "name");
    int recv = nt_ref(nt, n, "receiver"), blk = nt_ref(nt, n, "block");
    if (!nm) continue;
    if (is_constant_or_object_reflection(nm)) all = 1;
    /* Marshal.load can make any class's object; a program's own `load` is no
       such call */
    if (recv >= 0 && nt_kind(nt, recv) == NK_ConstantReadNode && is_marshal_load(nt_str(nt, recv, "name"), nm))
      all = 1;
    if (is_method_owner_query(nm)) owner = 1;
    if (is_method_object_maker(nm)) methods = 1;
    if (is_class_relatives_query(nm)) rel = 1;
    if (is_class_query(nm) && nt_ref(nt, n, "arguments") < 0) cls_call = 1;
    if (is_mixin_call(nm) || is_kind_query(nm) || is_raise_alias(nm)) cv_nonval_args(nt, nonval, n);
    NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_NONE;
    if (rk != NK_ConstantReadNode && rk != NK_ConstantPathNode && rk != NK_SelfNode) continue;
    /* a receiver whose call answers no class object: a class method of its
       own (of any class the name may be), or a plain query; a block of its
       own runs elsewhere */
    int plain = blk < 0 && is_class_plain_query(nm);
    if (!plain && blk < 0 && rk != NK_SelfNode) {
      const char *cn = nt_str(nt, recv, "name");
      for (int k = 0; cn && k < nk && !plain; k++) {
        const char *kn = c->classes[k].name, *seg = kn ? strrchr(kn, ':') : NULL;
        plain = kn && sp_streq(seg ? seg + 1 : kn, cn) && comp_cmethod_in_chain(c, k, nm, NULL) >= 0;
      }
    }
    if (!plain && blk < 0 && rk == NK_SelfNode) {
      /* (self is the class object in a class method or a class body, where
         only a class method of the name answers it; an instance method of
         the name does not) */
      Scope *sc = comp_scope_of(c, n);
      int k = sc ? sc->class_id : -1;
      int inst = sc && sc->def_node >= 0 && nt_kind(nt, sc->def_node) == NK_DefNode && !sc->is_cmethod;
      plain = k >= 0 && (comp_cmethod_in_chain(c, k, nm, NULL) >= 0 || (inst && comp_method_in_chain(c, k, nm, NULL) >= 0));
    }
    if (plain) nonval[recv] = 1;
  }
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    const char *nm = nt_str(nt, n, "name");
    int recv = nt_ref(nt, n, "receiver"), blk = nt_ref(nt, n, "block");
    if (!nm || !is_self_rebinding_call(nm) || blk < 0 || !cv_self_value(nt, nonval, blk, 1)) continue;
    if (recv >= 0 && (nt_kind(nt, recv) == NK_ConstantReadNode || nt_kind(nt, recv) == NK_ConstantPathNode))
      cv_mark_name(c, v, nt_str(nt, recv, "name"));
    else if (recv < 0 || nt_kind(nt, recv) == NK_SelfNode) {
      Scope *sc = comp_scope_of(c, n);
      if (sc) cv_mark_tree(c, v, sc->class_id);
    }
  }
  /* constants read as values, and self in a class's own body */
  NT_FOREACH_KIND(nt, NK_ConstantReadNode, n) if (!nonval[n]) cv_mark_name(c, v, nt_str(nt, n, "name"));
  NT_FOREACH_KIND(nt, NK_ConstantPathNode, n) if (!nonval[n]) cv_mark_name(c, v, nt_str(nt, n, "name"));
  for (int k = 0; k < nk; k++) {
    int dn = c->classes[k].def_node;
    if (dn < 0 || dn >= nn || (nt_kind(nt, dn) != NK_ClassNode && nt_kind(nt, dn) != NK_ModuleNode)) continue;
    if (cv_self_value(nt, nonval, nt_ref(nt, dn, "body"), nt_kind(nt, dn) == NK_ModuleNode)) cv_mark_tree(c, v, k);
  }
  /* (a reopened class's other bodies, and methods defined on a constant) */
  NT_FOREACH_KIND(nt, NK_ClassNode, n) {
    int p = nt_ref(nt, n, "constant_path");
    if (p >= 0 && cv_self_value(nt, nonval, nt_ref(nt, n, "body"), 0)) {
      for (int k = 0; k < nk; k++) {
        const char *kn = c->classes[k].name, *seg = kn ? strrchr(kn, ':') : NULL;
        if (kn && sp_streq(seg ? seg + 1 : kn, nt_str(nt, p, "name"))) cv_mark_tree(c, v, k);
      }
    }
  }
  NT_FOREACH_KIND(nt, NK_ModuleNode, n) {
    int p = nt_ref(nt, n, "constant_path");
    if (p >= 0 && cv_self_value(nt, nonval, nt_ref(nt, n, "body"), 1)) {
      for (int k = 0; k < nk; k++) {
        const char *kn = c->classes[k].name, *seg = kn ? strrchr(kn, ':') : NULL;
        if (kn && sp_streq(seg ? seg + 1 : kn, nt_str(nt, p, "name"))) cv_mark_tree(c, v, k);
      }
    }
  }
  NT_FOREACH_KIND(nt, NK_DefNode, n) {
    int r = nt_ref(nt, n, "receiver");
    if (r >= 0 && nt_kind(nt, r) != NK_SelfNode && cv_self_value(nt, nonval, nt_ref(nt, n, "body"), 0))
      cv_mark_name(c, v, nt_str(nt, r, "name"));
  }
  NT_FOREACH_KIND(nt, NK_SingletonClassNode, n) {
    int e = nt_ref(nt, n, "expression");
    if (e >= 0 && nt_kind(nt, e) != NK_SelfNode && cv_self_value(nt, nonval, nt_ref(nt, n, "body"), 1))
      cv_mark_name(c, v, nt_str(nt, e, "name"));
  }
  cv_self_answers(c, v, nt_ref(nt, nt->root_id, "statements"), -1, 1, 0);
  if (owner && methods) all = 1;
  for (int k = 0; k < nk; k++) {
    int dn = c->classes[k].def_node;
    if (all || (cls_call && !(dn >= 0 && dn < nn && nt_kind(nt, dn) == NK_ModuleNode))) v[k] = 1;
    if (rel && c->classes[k].parent >= 0) v[k] = v[c->classes[k].parent] = 1;
    for (int e = 0; rel && e < c->classes[k].nincluded_mods; e++) v[c->classes[k].included_mods[e]] = 1;
    for (int e = 0; rel && e < c->classes[k].nextended_mods; e++) v[c->classes[k].extended_mods[e]] = 1;
  }
  free(nonval);
  free(c->cls_value);
  c->cls_value = v;
  c->cls_value_n = nk;
  c->cls_value_nodes = nn;
}
int comp_class_may_be_value(Compiler *c, int k) {
  if (k < 0 || k >= c->nclasses || !c->nt) return 1;
  /* A writer's value temporary (emit_setter_assign_value) appends a local
     variable read at each site it emits, and rebuilding over the whole
     table for each of them made this most of a large program's codegen. */
  if (c->cls_value && c->cls_value_n == c->nclasses && c->cls_value_nodes < c->nt->count &&
      nt_appended_only_local_reads(c->nt, c->cls_value_nodes))
    c->cls_value_nodes = c->nt->count;
  if (!c->cls_value || c->cls_value_n != c->nclasses || c->cls_value_nodes != c->nt->count) cv_build(c);
  return c->cls_value[k];
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

/* The same name memo also holds whether each class has a descendant with
   its own definition. Reader-slot queries need this fact across call sites,
   not another descendant walk per representation query. The candidate memo's
   stamps invalidate it too; a temporarily moved scope must answer fresh. */
int comp_method_overridden(Compiler *c, int cid, const char *name, int cmeth) {
  if (cid < 0 || cid >= c->nclasses || !name) return 0;
  struct pc_entry *memo = !cmeth && !mv_n ? pc_memo(c, name) : NULL;
  if (memo) {
    if (!memo->overridden) {
      memo->overridden = calloc((size_t)c->nclasses, sizeof *memo->overridden);
      if (!memo->overridden) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    if (memo->overridden[cid]) return memo->overridden[cid] == 2;
  }
  int nd = 0, overridden = 0;
  const int *ds = comp_descendants(c, cid, &nd);
  for (int i = 0; i < nd; i++) {
    int k = ds[i];
    if (k == cid) continue;
    if (!sp_streq(cmeth ? comp_resolve_cm_alias_at(c, k, name, NULL) : comp_resolve_alias(c, k, name), name) ||
        (cmeth ? comp_cmethod_in_class(c, k, name) : comp_method_in_class(c, k, name)) >= 0) {
      overridden = 1;
      break;
    }
  }
  if (memo) memo->overridden[cid] = overridden ? 2 : 1;
  return overridden;
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

/* The scope node_id is in. A node a desugar appended is past the per-node
   arrays until comp_grow_node_arrays runs, and is in scope 0 there, as the
   grown entry would say: bounded by the table's count, the read went past
   the end of nscope (desugar_value_callable_forwards asks of the receivers
   of calls appended since). */
Scope *comp_scope_of(Compiler *c, int node_id) {
  if (node_id < 0 || node_id >= c->node_cap) return &c->scopes[0];
  int idx = c->nscope[node_id];
  if (idx < 0 || idx >= c->nscopes) idx = 0;
  return &c->scopes[idx];
}

int comp_is_local_write(NodeKind k) {
  return k == NK_LocalVariableWriteNode || k == NK_LocalVariableTargetNode ||
         k == NK_LocalVariableOrWriteNode || k == NK_LocalVariableAndWriteNode ||
         k == NK_LocalVariableOperatorWriteNode;
}
/* Does the subtree under n assign the variable read node `arg` reads: a
   local of the same scope, or an instance variable, by any write kind
   (plain, operator, `||=`, `&&=`, a multiple-assignment target)? */
static int comp_subtree_writes_var(Compiler *c, int n, int arg, const char *name, int depth) {
  const NodeTable *nt = c->nt;
  if (n < 0 || depth > 300) return 0;
  NodeKind k = nt_kind(nt, n);
  int ivar = nt_kind(nt, arg) == NK_InstanceVariableReadNode;
  int w = ivar ? k == NK_InstanceVariableWriteNode || k == NK_InstanceVariableOrWriteNode ||
                 k == NK_InstanceVariableAndWriteNode || k == NK_InstanceVariableOperatorWriteNode ||
                 k == NK_InstanceVariableTargetNode
               : comp_is_local_write(k);
  if (w && nt_str(nt, n, "name") && sp_streq(nt_str(nt, n, "name"), name) &&
      (ivar || comp_scope_of(c, n) == comp_scope_of(c, arg))) return 1;
  int nr = nt_num_refs(nt, n);
  for (int i = 0; i < nr; i++)
    if (comp_subtree_writes_var(c, nt_ref_at(nt, n, i), arg, name, depth + 1)) return 1;
  int na = nt_num_arrs(nt, n);
  for (int i = 0; i < na; i++) {
    int an = 0; const int *av = nt_arr_at(nt, n, i, &an);
    for (int j = 0; j < an; j++)
      if (comp_subtree_writes_var(c, av[j], arg, name, depth + 1)) return 1;
  }
  return 0;
}
int comp_node_writes_var(Compiler *c, int n, int arg) {
  const NodeTable *nt = c->nt;
  NodeKind ak = arg >= 0 ? nt_kind(nt, arg) : NK__COUNT;
  const char *name = arg >= 0 ? nt_str(nt, arg, "name") : NULL;
  if (n < 0 || !name || (ak != NK_LocalVariableReadNode && ak != NK_InstanceVariableReadNode)) return 0;
  return comp_subtree_writes_var(c, n, arg, name, 0);
}
int comp_block_rebinds_arg(Compiler *c, int blk, int arg) {
  const NodeTable *nt = c->nt;
  NodeKind ak = arg >= 0 ? nt_kind(nt, arg) : NK__COUNT;
  const char *name = arg >= 0 ? nt_str(nt, arg, "name") : NULL;
  if (blk < 0 || !name || (ak != NK_LocalVariableReadNode && ak != NK_InstanceVariableReadNode)) return 0;
  if (ak == NK_LocalVariableReadNode) {
    LocalVar *lv = scope_local(comp_scope_of(c, arg), name);
    if (lv && lv->proc_rebinds) return 1;
    return nt_kind(nt, blk) == NK_BlockNode && comp_subtree_writes_var(c, blk, arg, name, 0);
  }
  /* an instance variable: a write in the block, or one a method the block
     calls on self makes, or anything else that runs code the walk cannot
     name (subtree_may_write_ivar); a block passed as a value (`&pr`) is a
     proc whose body is not known here */
  if (nt_kind(nt, blk) != NK_BlockNode) return 1;
  return subtree_may_write_ivar(c, nt_ref(nt, blk, "body"), name, comp_ivar_owner(c, arg), 0);
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

/* Every CallNode with a literal block, chained by the scope its block's
   parameters live in (comp_scope_of of the block). A block parameter's
   source walk looked at every CallNode of the program per parameter; this
   walks them once. */
static void bcall_build(Compiler *c) {
  free(c->bcall_head); free(c->bcall_next);
  const NodeTable *nt = c->nt;
  int n = nt->count;
  int ns = c->nscopes > 0 ? c->nscopes : 1;
  if (!comp_chain_alloc(&c->bcall_head, &c->bcall_next, ns, n, &c->bcall_built)) return;
  c->bcall_nscopes = ns;
  c->bcall_count = n;
  for (int s = 0; s < ns; s++) c->bcall_head[s] = -1;
  for (int u = n - 1; u >= 0; u--) {   /* reverse: chains run in node order */
    c->bcall_next[u] = -1;
    if (nt_kind(nt, u) != NK_CallNode) continue;
    int blk = nt_ref(nt, u, "block");
    if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) continue;
    int si = (int)(comp_scope_of(c, blk) - c->scopes);
    if (si < 0 || si >= ns) continue;
    c->bcall_next[u] = c->bcall_head[si];
    c->bcall_head[si] = u;
  }
  c->bcall_version = nt->version;
  c->bcall_built = 1;
}
int comp_bcall_first(Compiler *c, int scope_idx) {
  if (!c->bcall_built || c->bcall_version != c->nt->version || c->bcall_count != c->nt->count ||
      c->bcall_nscopes < c->nscopes) bcall_build(c);
  if (!c->bcall_built || scope_idx < 0 || scope_idx >= c->bcall_nscopes) return -1;
  return c->bcall_head[scope_idx];
}
int comp_bcall_next(const Compiler *c, int u) {
  return (u >= 0 && u < c->bcall_count) ? c->bcall_next[u] : -1;
}

/* Every ReturnNode, chained by the scope it is in (comp_scope_of), in node
   order. The values a method answers through `return` were found by a
   walk over every ReturnNode of the program per method asked about. */
static void ret_build(Compiler *c) {
  free(c->ret_head); free(c->ret_next);
  const NodeTable *nt = c->nt;
  int n = nt->count;
  int ns = c->nscopes > 0 ? c->nscopes : 1;
  if (!comp_chain_alloc(&c->ret_head, &c->ret_next, ns, n, &c->ret_built)) return;
  c->ret_nscopes = ns;
  c->ret_count = n;
  for (int s = 0; s < ns; s++) c->ret_head[s] = -1;
  for (int u = n - 1; u >= 0; u--) {   /* reverse: chains run in node order */
    c->ret_next[u] = -1;
    if (nt_kind(nt, u) != NK_ReturnNode) continue;
    Scope *sc = comp_scope_of(c, u);
    int si = sc ? (int)(sc - c->scopes) : -1;
    if (si < 0 || si >= ns) continue;
    c->ret_next[u] = c->ret_head[si];
    c->ret_head[si] = u;
  }
  c->ret_version = nt->version;
  c->ret_built = 1;
}
int comp_ret_first(Compiler *c, int scope_idx) {
  if (!c->ret_built || c->ret_version != c->nt->version || c->ret_count != c->nt->count ||
      c->ret_nscopes < c->nscopes) ret_build(c);
  if (!c->ret_built || scope_idx < 0 || scope_idx >= c->ret_nscopes) return -1;
  return c->ret_head[scope_idx];
}
int comp_ret_next(const Compiler *c, int u) {
  return (u >= 0 && u < c->ret_count) ? c->ret_next[u] : -1;
}

/* Every ivar read handed to a call as an argument, chained by the ivar's
   name: one entry per (CallNode, argument) pair. Asking whether an ivar is
   lent to a callee walked every CallNode of the program once per question,
   and a machine-generated program asks it for nearly every method. Chains
   carry hash collisions: callers keep their name/owner filters. An argument
   array rewritten in place (nt_node_set_arr) leaves the table's version
   alone, so a caller that may run after such a rewrite drops the index with
   comp_ivarg_invalidate first. */
static void ivarg_build(Compiler *c) {
  free(c->ivarg_head); free(c->ivarg_next); free(c->ivarg_call); free(c->ivarg_arg);
  c->ivarg_call = c->ivarg_arg = NULL;
  const NodeTable *nt = c->nt;
  int n = nt->count, np = 0;
  for (int u = 0; u < n; u++) {
    if (nt_kind(nt, u) != NK_CallNode) continue;
    int a = nt_ref(nt, u, "arguments"), ac = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
    for (int k = 0; k < ac; k++)
      if (nt_kind(nt, av[k]) == NK_InstanceVariableReadNode && nt_str(nt, av[k], "name")) np++;
  }
  int nb = 16;
  while (nb < np && nb < (1 << 22)) nb <<= 1;
  if (!comp_chain_alloc(&c->ivarg_head, &c->ivarg_next, nb, np, &c->ivarg_built)) return;
  c->ivarg_call = malloc((size_t)(np > 0 ? np : 1) * sizeof(int));
  c->ivarg_arg = malloc((size_t)(np > 0 ? np : 1) * sizeof(int));
  if (!c->ivarg_call || !c->ivarg_arg) {
    free(c->ivarg_head); free(c->ivarg_next); free(c->ivarg_call); free(c->ivarg_arg);
    c->ivarg_head = c->ivarg_next = c->ivarg_call = c->ivarg_arg = NULL;
    c->ivarg_built = 0;
    return;
  }
  c->ivarg_nbuckets = nb;
  c->ivarg_count = np;
  for (int b = 0; b < nb; b++) c->ivarg_head[b] = -1;
  int e = np;
  for (int u = n - 1; u >= 0; u--) {   /* reverse: chains run in node order */
    if (nt_kind(nt, u) != NK_CallNode) continue;
    int a = nt_ref(nt, u, "arguments"), ac = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
    for (int k = ac - 1; k >= 0; k--) {
      if (nt_kind(nt, av[k]) != NK_InstanceVariableReadNode) continue;
      const char *an = nt_str(nt, av[k], "name");
      if (!an) continue;
      unsigned b = sp_strhash(an) & (unsigned)(nb - 1);
      e--;
      c->ivarg_call[e] = u;
      c->ivarg_arg[e] = av[k];
      c->ivarg_next[e] = c->ivarg_head[b];
      c->ivarg_head[b] = e;
    }
  }
  c->ivarg_version = nt->version;
  c->ivarg_built = 1;
}
int comp_ivarg_first(Compiler *c, const char *name) {
  if (!c->ivarg_built || c->ivarg_version != c->nt->version) ivarg_build(c);
  if (!c->ivarg_built || !name) return -1;
  return c->ivarg_head[sp_strhash(name) & (unsigned)(c->ivarg_nbuckets - 1)];
}
void comp_ivarg_invalidate(Compiler *c) { c->ivarg_built = 0; }
int comp_ivarg_next(const Compiler *c, int e) {
  return (e >= 0 && e < c->ivarg_count) ? c->ivarg_next[e] : -1;
}
int comp_ivarg_call(const Compiler *c, int e) {
  return (e >= 0 && e < c->ivarg_count) ? c->ivarg_call[e] : -1;
}
int comp_ivarg_arg(const Compiler *c, int e) {
  return (e >= 0 && e < c->ivarg_count) ? c->ivarg_arg[e] : -1;
}

/* The PRES_* facts of ivar name `ivn` (c->pres_names); comp_pres_note adds
   to them. */
int comp_pres_flags(const Compiler *c, const char *ivn) {
  for (int i = 0; ivn && i < c->npres; i++)
    if (sp_streq(c->pres_names[i], ivn)) return c->pres_flags[i];
  return 0;
}
void comp_pres_note(Compiler *c, const char *ivn, int flag) {
  if (!ivn) return;
  for (int i = 0; i < c->npres; i++)
    if (sp_streq(c->pres_names[i], ivn)) { c->pres_flags[i] |= (unsigned char)flag; return; }
  if (c->npres == c->cpres) {
    c->cpres = c->cpres ? c->cpres * 2 : 8;
    c->pres_names = realloc(c->pres_names, sizeof(char *) * (size_t)c->cpres);
    c->pres_flags = realloc(c->pres_flags, (size_t)c->cpres);
    if (!c->pres_names || !c->pres_flags) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  c->pres_names[c->npres] = strdup(ivn);
  c->pres_flags[c->npres++] = (unsigned char)flag;
}

/* The owning class of an ivar READ/WRITE node under the same storage rules
   the emitters use (instance method -> class, top-level -> Toplevel; class
   methods / instance_eval contexts return -1). */
int comp_ivar_owner(Compiler *c, int node) {
  Scope *cs = comp_scope_of(c, node);
  if (!cs) return -1;
  /* a class method's ivar is its class's civ_ slot, typed with the class's
     ivars (strbuf_ivar_owner): the same rules make it the handle */
  if (cs->is_cmethod) return cs->class_id;
  if (cs->class_id >= 0) return cs->class_id;
  return comp_class_index(c, "Toplevel");
}

/* The variable a site names, by the read kind, name and key the String
   refusals name a variable by (SaName): a local by its scope, an ivar by its
   owning class (comp_ivar_owner), a global by its name with aliases resolved
   (key -1). 0 when node `v` names none of them. */
static int vsite_key(Compiler *c, int v, NodeKind *kind, const char **name, int *key) {
  const NodeTable *nt = c->nt;
  const char *nm = v >= 0 ? nt_str(nt, v, "name") : NULL;
  if (!nm) return 0;
  Scope *s;
  switch (nt_kind(nt, v)) {
    case NK_LocalVariableReadNode: case NK_LocalVariableWriteNode:
    case NK_LocalVariableOrWriteNode: case NK_LocalVariableAndWriteNode:
      s = comp_scope_of(c, v);
      *kind = NK_LocalVariableReadNode; *name = nm; *key = s ? (int)(s - c->scopes) : -1;
      return s != NULL;
    case NK_InstanceVariableReadNode: case NK_InstanceVariableWriteNode:
    case NK_InstanceVariableOrWriteNode: case NK_InstanceVariableAndWriteNode:
      *kind = NK_InstanceVariableReadNode; *name = nm; *key = comp_ivar_owner(c, v);
      return *key >= 0;
    case NK_GlobalVariableReadNode: case NK_GlobalVariableWriteNode:
    case NK_GlobalVariableOrWriteNode: case NK_GlobalVariableAndWriteNode:
      *kind = NK_GlobalVariableReadNode; *name = comp_resolve_gvar(c, nm + 1); *key = -1;
      return *name != NULL;
    /* a constant by its name alone, as comp_const keys it */
    case NK_ConstantReadNode: case NK_ConstantWriteNode: case NK_ConstantOrWriteNode: case NK_ConstantAndWriteNode:
      *kind = NK_ConstantReadNode; *name = nm; *key = -1;
      return 1;
    /* a class variable by its name alone, as the share facts key it */
    case NK_ClassVariableReadNode: case NK_ClassVariableWriteNode:
    case NK_ClassVariableOrWriteNode: case NK_ClassVariableAndWriteNode:
      *kind = NK_ClassVariableReadNode; *name = nm; *key = -1;
      return 1;
    default:
      return 0;
  }
}
static unsigned vsite_hash(VsKind k, NodeKind kind, const char *name, int key) {
  return sp_strhash(name) ^ ((unsigned)kind * 0x9e3779b1u) ^ ((unsigned)(key + 1) * 2654435761u) ^
         ((unsigned)(k + 1) * 0x85ebca6bu);
}
static int vsite_is_store(NodeKind k) {
  return k == NK_LocalVariableWriteNode || k == NK_LocalVariableOrWriteNode || k == NK_LocalVariableAndWriteNode ||
         k == NK_InstanceVariableWriteNode || k == NK_InstanceVariableOrWriteNode ||
         k == NK_InstanceVariableAndWriteNode || k == NK_GlobalVariableWriteNode ||
         k == NK_GlobalVariableOrWriteNode || k == NK_GlobalVariableAndWriteNode ||
         k == NK_ClassVariableWriteNode || k == NK_ClassVariableOrWriteNode || k == NK_ClassVariableAndWriteNode ||
         k == NK_ConstantWriteNode || k == NK_ConstantOrWriteNode || k == NK_ConstantAndWriteNode;
}
static int vsite_is_read(const NodeTable *nt, int n) {
  NodeKind k = n >= 0 ? nt_kind(nt, n) : NK_NONE;
  return k == NK_LocalVariableReadNode || k == NK_InstanceVariableReadNode || k == NK_GlobalVariableReadNode;
}

/* Variable-site chains: for each variable the String refusals name, the
   nodes that read it (VS_READ), write it (VS_WRITE, `x = v`; VS_STORE, any
   `=`, `||=` or `&&=`, a class variable's and a constant's too), call a String
   mutator on it (VS_MUT: the call, also through the calls that answer their
   receiver, `x.to_s << y`) or call any method on it (VS_RECV: the call whose
   receiver it is), one entry per (kind, site), chained by (kind, variable)
   in node order. The refusals asked each of those by walking a
   node kind per candidate. Beside the chains, two per-node lookups the
   same walk fills: the call whose receiver a node is (comp_recv_parent),
   and whether it is a statement the next statement follows
   (comp_value_dropped). Chains
   carry hash collisions: callers keep their kind/name/key filters
   (comp_vsite_var is the read or write naming an entry's variable).
   Revalidated against nt->version, the scope index's generation and the
   Toplevel class an ivar may key on, as the mutation tables are; while the
   scope index is not frozen each question rebuilds. */
static void vsite_add(Compiler *c, VsKind k, int site, int var) {
  NodeKind vk; const char *vn; int key;
  if (!vsite_key(c, var, &vk, &vn, &key)) return;
  if (c->vs_count == c->vs_cap) {
    c->vs_cap = c->vs_cap ? c->vs_cap * 2 : 256;
    c->vs_site = realloc(c->vs_site, sizeof(int) * (size_t)c->vs_cap);
    c->vs_var = realloc(c->vs_var, sizeof(int) * (size_t)c->vs_cap);
    c->vs_next = realloc(c->vs_next, sizeof(int) * (size_t)c->vs_cap);
    c->vs_kind = realloc(c->vs_kind, (size_t)c->vs_cap);
    if (!c->vs_site || !c->vs_var || !c->vs_next || !c->vs_kind) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  int e = c->vs_count++;
  int *head = &c->vs_head[vsite_hash(k, vk, vn, key) & (unsigned)(c->vs_nbuckets - 1)];
  c->vs_kind[e] = (unsigned char)k;
  c->vs_site[e] = site;
  c->vs_var[e] = var;
  c->vs_next[e] = *head;
  *head = e;
}
static void vsite_build(Compiler *c, int toplevel) {
  const NodeTable *nt = c->nt;
  int n = nt->count;
  int nb = 16;
  while (nb < n && nb < (1 << 22)) nb <<= 1;
  size_t sz = (size_t)(n > 0 ? n : 1);
  free(c->vs_head); free(c->vs_rparent); free(c->vs_dropped); free(c->vs_whead); free(c->vs_wnext);
  c->vs_head = malloc(sizeof(int) * (size_t)nb);
  c->vs_rparent = malloc(sz * sizeof(int));
  c->vs_dropped = calloc(sz, 1);
  c->vs_whead = malloc(sz * sizeof(int));
  c->vs_wnext = malloc(sz * sizeof(int));
  if (!c->vs_head || !c->vs_rparent || !c->vs_dropped || !c->vs_whead || !c->vs_wnext) {
    fprintf(stderr, "spinel: out of memory\n");
    exit(1);
  }
  c->vs_nbuckets = nb;
  c->vs_count = 0;
  c->vs_nodes = n;
  for (int b = 0; b < nb; b++) c->vs_head[b] = -1;
  for (int i = 0; i < n; i++) c->vs_rparent[i] = c->vs_whead[i] = c->vs_wnext[i] = -1;
  for (int u = n - 1; u >= 0; u--) {   /* reverse: chains run in node order */
    NodeKind k = nt_kind(nt, u);
    /* every store into a variable, its `||=` and `&&=` and a class
       variable's included (VS_STORE) */
    if (vsite_is_store(k)) vsite_add(c, VS_STORE, u, u);
    if (k == NK_LocalVariableWriteNode) {
      int v = nt_ref(nt, u, "value");
      if (v >= 0 && v < n) { c->vs_wnext[u] = c->vs_whead[v]; c->vs_whead[v] = u; }
    }
    if (vsite_is_read(nt, u)) vsite_add(c, VS_READ, u, u);
    else if (k == NK_LocalVariableWriteNode || k == NK_InstanceVariableWriteNode || k == NK_GlobalVariableWriteNode)
      vsite_add(c, VS_WRITE, u, u);
    else if (k == NK_StatementsNode) {
      int bn = 0; const int *bb = nt_arr(nt, u, "body", &bn);
      for (int i = 0; i + 1 < bn; i++) {
        int v = an_unparen(nt, bb[i]);
        if (v >= 0 && v < n) c->vs_dropped[v] = 1;
      }
    }
    else if (k == NK_CallNode && nt_ref(nt, u, "receiver") >= 0) {
      int r = nt_ref(nt, u, "receiver"), ru = an_unparen(nt, r);
      const char *un = nt_str(nt, u, "name");
      if (ru >= 0 && ru < n) c->vs_rparent[ru] = u;
      /* a constant or a class variable is a receiver here too: the share
         rule's stores into its container (strbuf_static_store_walk) */
      if (vsite_is_read(nt, ru) || nt_kind(nt, ru) == NK_ConstantReadNode ||
          nt_kind(nt, ru) == NK_ClassVariableReadNode)
        vsite_add(c, VS_RECV, u, ru);
      if (un && sp_str_mutator(un, SP_MUT_LOCAL)) {
        while (str_self_call(nt, an_unparen(nt, r))) r = nt_ref(nt, an_unparen(nt, r), "receiver");
        int base = an_unparen(nt, r);
        if (vsite_is_read(nt, base)) vsite_add(c, VS_MUT, u, base);
      }
    }
  }
  c->vs_version = nt->version;
  c->vs_gen = comp_scope_index_gen();
  c->vs_toplevel = toplevel;
  c->vs_built = 1;
}
static void vsite_sync(Compiler *c) {
  int toplevel = comp_class_index(c, "Toplevel");
  if (!c->vs_built || !comp_scope_index_is_frozen() || c->vs_version != c->nt->version ||
      c->vs_nodes != c->nt->count || c->vs_gen != comp_scope_index_gen() || c->vs_toplevel != toplevel)
    vsite_build(c, toplevel);
}
/* the entry e or the first after it of e's own kind */
static int vsite_same_kind(const Compiler *c, int e, int k) {
  while (e >= 0 && c->vs_kind[e] != k) e = c->vs_next[e];
  return e;
}
int comp_vsite_first(Compiler *c, VsKind k, NodeKind kind, const char *name, int key) {
  vsite_sync(c);
  if (!name) return -1;
  return vsite_same_kind(c, c->vs_head[vsite_hash(k, kind, name, key) & (unsigned)(c->vs_nbuckets - 1)], k);
}
int comp_vsite_next(const Compiler *c, int e) {
  return (e >= 0 && e < c->vs_count) ? vsite_same_kind(c, c->vs_next[e], c->vs_kind[e]) : -1;
}
int comp_vsite_node(const Compiler *c, int e) {
  return (e >= 0 && e < c->vs_count) ? c->vs_site[e] : -1;
}
int comp_vsite_var(const Compiler *c, int e) {
  return (e >= 0 && e < c->vs_count) ? c->vs_var[e] : -1;
}
int comp_recv_parent(Compiler *c, int n) {
  vsite_sync(c);
  n = an_unparen(c->nt, n);
  return n >= 0 && n < c->vs_nodes ? c->vs_rparent[n] : -1;
}
int comp_lwrite_of_value(Compiler *c, int n) {
  vsite_sync(c);
  return n >= 0 && n < c->vs_nodes ? c->vs_whead[n] : -1;
}
int comp_lwrite_next(const Compiler *c, int w) {
  return w >= 0 && w < c->vs_nodes ? c->vs_wnext[w] : -1;
}
int comp_value_dropped(Compiler *c, int n) {
  vsite_sync(c);
  n = an_unparen(c->nt, n);
  return n >= 0 && n < c->vs_nodes && c->vs_dropped[n];
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

/* Every ReturnNode in a scope, chained in node order. A method's values are
   its body's and its returns', and finding the returns walked every
   ReturnNode of the program per question; the walks that follow a call into
   its callee ask once per call they follow. The same shape as the CallNode
   chain above, which a scope's own returns could share one day. */
static void sret_build(Compiler *c) {
  free(c->sret_head); free(c->sret_next);
  int n = c->nt->count;
  int ns = c->nscopes > 0 ? c->nscopes : 1;
  if (!comp_chain_alloc(&c->sret_head, &c->sret_next, ns, n, &c->sret_built)) return;
  c->sret_nscopes = ns;
  c->sret_count = n;
  for (int s = 0; s < ns; s++) c->sret_head[s] = -1;
  for (int r = n - 1; r >= 0; r--) {   /* reverse: chains run in node order */
    c->sret_next[r] = -1;
    if (nt_kind(c->nt, r) != NK_ReturnNode) continue;
    int si = c->nscope ? c->nscope[r] : 0;
    if (si < 0 || si >= ns) si = 0;
    c->sret_next[r] = c->sret_head[si];
    c->sret_head[si] = r;
  }
  c->sret_version = c->nt->version;
  c->sret_built = 1;
}
int comp_sret_first(Compiler *c, int scope_idx) {
  if (!c->sret_built || c->sret_version != c->nt->version ||
      c->sret_nscopes < c->nscopes) sret_build(c);
  if (!c->sret_built || scope_idx < 0 || scope_idx >= c->sret_nscopes) return -1;
  return c->sret_head[scope_idx];
}
int comp_sret_next(const Compiler *c, int r) {
  return (r >= 0 && r < c->sret_count) ? c->sret_next[r] : -1;
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
      /* `~re` is Regexp#~, a match against $_. `~5`, `~x` and `~self` are
         Integer#~ (builtins/integer.rb spells the last in bit_length, so any
         program that requires securerandom has one), and a bare gets beside
         them is still ARGF's. Syntax only: a regexp literal, interpolated or
         not, is the match; a Regexp held in a variable is not seen. */
      if (cn && sp_streq(cn, "~") && ac == 0) {
        int rcv = nt_ref(nt, i, "receiver");
        if (rcv < 0 || nt_kind(nt, rcv) == NK_RegularExpressionNode ||
            nt_kind(nt, rcv) == NK_InterpolatedRegularExpressionNode) return 0;
      }
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
      /* ... its instance aliases, and the main object's singleton ones
         (`class << self; alias d c; end` at the top level) */
      const char *res = comp_resolve_alias(c, tl, name);
      if (res && sp_streq(res, name)) res = comp_resolve_cm_alias_at(c, tl, name, NULL);
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
  *(TyKind *)&lv->type = TY_UNKNOWN;
  comp_ty_edits++;    /* a new local is a change of the scope's table */
  why_reset(&lv->why);
  lv->last_src = -1;
  lv->gc_root = 0;
  lv->is_param = 0;
  lv->is_block_param = 0;
  lv_set_proc_ret(lv, TY_UNKNOWN);
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
  lv->push_fill_elem = TY_UNKNOWN;
  lv->boxed_known_elem = TY_UNKNOWN;
  lv->boxed_store_key = TY_UNKNOWN;
  lv->boxed_store_val = TY_UNKNOWN;
  lv->store_key_src = 0;
  lv->store_val_src = 0;
  lv->store_elems_src = 0;
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
  char key[320];
  snprintf(key, sizeof key, "%s%s", s->is_cmethod ? "self." : "", s->name);
  const char *t = comp_prep_chain_target(c, s->class_id, key);
  /* A proc-form clone ("<m>#pf", make_yield_proc_forms) has no chain entry
     of its own: its super reaches the shadow the method's does -- the
     shadow's own clone when it has one, as the clone of a class's
     initialize does that calls super into an included module's on a cycle
     of constructors, or the shadow itself. A yielding shadow without a
     clone is kept so make_yield_proc_forms can give it one. Sent up the
     parent chain instead, it skipped that method or raised "no superclass
     method". */
  size_t n = strlen(key);
  if (t || n <= 3 || n >= sizeof key || strcmp(key + n - 3, "#pf") != 0) return t;
  key[n - 3] = '\0';
  t = comp_prep_chain_target(c, s->class_id, key);
  if (!t) return NULL;
  char pf[320];
  snprintf(pf, sizeof pf, "%s#pf", t);
  int (*in_class)(Compiler *, int, const char *) = s->is_cmethod ? comp_cmethod_in_class : comp_method_in_class;
  int k = in_class(c, s->class_id, pf);
  if (k >= 0) return c->scopes[k].name;
  k = in_class(c, s->class_id, t);
  return k >= 0 ? t : NULL;
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
  /* sample and min / max answer one of the elements too, in their
     zero-argument form (with a count they answer a new Array) */
  if (sp_streq(nm, "first") || sp_streq(nm, "last") || sp_streq(nm, "sample") ||
      sp_streq(nm, "min") || sp_streq(nm, "max")) {
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
    {"reverse_each","SP_PENUM_REVERSE_EACH"}, {"uniq","SP_PENUM_UNIQ"},
    {"to_h","SP_PENUM_TO_H"},
    {"transform_keys","SP_PENUM_TRANSFORM_KEYS"}, {"transform_values","SP_PENUM_TRANSFORM_VALUES"},
    {"transform_keys!","SP_PENUM_TRANSFORM_KEYS_BANG"}, {"transform_values!","SP_PENUM_TRANSFORM_VALUES_BANG"},
    {"select!","SP_PENUM_SELECT_BANG"}, {"filter!","SP_PENUM_FILTER_BANG"},
    {"reject!","SP_PENUM_REJECT_BANG"}, {"keep_if","SP_PENUM_KEEP_IF"}, {"delete_if","SP_PENUM_DELETE_IF"},
    {NULL,NULL}
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
  return bop_name_has_reader(name, BOP_READ_NUMERIC);
}

int poly_container_read_p(const char *name) {
  return bop_name_has_reader(name, BOP_READ_CONTAINER);
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
  return bop_name_has_reader(name, BOP_READ_STRING);
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


/* Is module m module mod, or does it include mod (through its own includes)? */
int comp_module_includes(Compiler *c, int m, int mod, int depth) {
  if (m == mod) return 1;
  if (m < 0 || m >= c->nclasses || depth > 16) return 0;
  ClassInfo *mi = &c->classes[m];
  for (int k = 0; k < mi->nincluded_mods; k++)
    if (comp_module_includes(c, mi->included_mods[k], mod, depth + 1)) return 1;
  return 0;
}
/* Does module `mod` -- or a module including it -- take part in class `ci`'s
   singleton ancestors, through an `extend` of `ci` or of a superclass? Then
   `ci.is_a?(mod)` is true. */
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

/* Class.new identities acquire their public name only at an executed constant write. */
/* Can the program hold a singleton class as a value (`k.singleton_class`,
   self in a `class << X` body)? Asked once; the answer does not change
   after the analysis. */
int comp_has_sclass_values(Compiler *c) {
  static const Compiler *memo_c = NULL;
  static int memo = 0;
  if (memo_c == c) return memo;
  const NodeTable *nt = c->nt;
  int any = 0;
  for (int id = 0; id < nt->count && !any; id++)
    any = (nt_kind(nt, id) == NK_SelfNode && nt_int(nt, id, "sclass_value", 0)) ||
          (nt_kind(nt, id) == NK_CallNode && nt_str(nt, id, "name") &&
           sp_streq(nt_str(nt, id, "name"), "singleton_class"));
  memo_c = c; memo = any;
  return any;
}

int comp_class_anonymous(Compiler *c, int ci) {
  return ci >= 0 && ci < c->nclasses &&
         nt_int(c->nt, c->classes[ci].def_node, "class_new_anonymous", 0);
}

/* The table that prints a class value for to_s and inspect: a Class.new class
   has a display form of its own, and every other program reads the one name table. */
const char *comp_class_display_fn(Compiler *c) {
  return c->has_anonymous_classes ? "sp_class_display" : "sp_class_to_s";
}
