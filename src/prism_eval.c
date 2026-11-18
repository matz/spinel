/* prism_eval.c -- the parse-side evaluator (included into spinel_parse.c).
 *
 * The value a Prism expression has when the compiler can know it: nil, true,
 * false, an Integer, a String, a Symbol and an Array of these, with the
 * interpolation, `||`, `&&`, `if`, parentheses, local reads and the methods
 * ct_value.c runs (mv_call, mv_sub) that compile-time code uses. It walks
 * Prism's nodes where ct_eval.c walks the node table's, and computes with the
 * same values (ct_value.h): the parse needs it before the node table exists
 * (the macros it expands, the files a require names).
 *
 * A context holds the locals bound so far. What a caller knows that the
 * evaluator does not -- a macro's own state -- it answers through the
 * context's hooks: a call on no receiver, and the instance variables. A
 * context without hooks knows none of them, and those nodes are not known.
 * Answers 0 where the value is not known for certain. */
#include "ct_value.h"

typedef struct { char *name; Mv v; } PevVar;
typedef struct PevCtx PevCtx;
typedef struct {
  /* a call on no receiver (or self), named `name`: its value, 1 when known */
  int (*self_call)(PevCtx *c, pm_call_node_t *n, const char *name, Mv *out);
  /* an instance variable read, or `defined?` of one */
  int (*node)(PevCtx *c, pm_node_t *n, Mv *out);
} PevHooks;
struct PevCtx {
  PevVar loc[64]; int nloc;
  int full;               /* a table was full: the evaluation cannot be trusted */
  const PevHooks *hooks;
  /* the directory of the file the expression is in, where it is asked what
     path it builds: `__dir__`, `File.dirname(__FILE__)`, `File.join`,
     `File.expand_path` and `freeze` are known only then (NULL: they are not) */
  const char *dir;
};

static int pev_eval(PevCtx *c, pm_node_t *n, Mv *out);

/* `File.join` of Strings: a "/" between them, none doubled */
static int pev_file_join(Mv *v, int n, Mv *out) {
  MvBuf b = {0};
  for (int i = 0; i < n; i++) {
    if (v[i].k != MV_STR) { free(b.p); return 0; }
    const char *s = v[i].s;
    size_t len = strlen(s);
    if (i) { while (b.len && b.p[b.len - 1] == '/') b.len--; if (b.p) b.p[b.len] = 0; mvb_puts(&b, "/"); while (*s == '/') { s++; len--; } }
    mvb_putn(&b, s, len);
  }
  *out = mv_str(b.p ? b.p : "", b.len, MV_STR);
  free(b.p);
  return 1;
}

/* the path forms of a context that has a directory (PevCtx.dir): 1 and the
   value, 0 when not known, -1 when the call is none of them */
static int pev_path_call(PevCtx *c, pm_call_node_t *n, const char *name, Mv *out) {
  pm_node_list_t *args = n->arguments ? &n->arguments->arguments : NULL;
  size_t argc = args ? args->size : 0;
  int file = n->receiver && PM_NODE_TYPE(n->receiver) == PM_CONSTANT_READ_NODE;
  if (file) { char *rn = cstr(((pm_constant_read_node_t *)n->receiver)->name); file = strcmp(rn, "File") == 0; free(rn); }
  if (!n->receiver && !argc && strcmp(name, "__dir__") == 0) { *out = mv_str(c->dir, strlen(c->dir), MV_STR); return 1; }
  if (n->receiver && !argc && strcmp(name, "freeze") == 0) return pev_eval(c, n->receiver, out);
  if (file && argc == 1 && strcmp(name, "dirname") == 0 && PM_NODE_TYPE(args->nodes[0]) == PM_SOURCE_FILE_NODE) {
    *out = mv_str(c->dir, strlen(c->dir), MV_STR);
    return 1;
  }
  if (!file || !argc || argc > 16) return -1;
  Mv v[16];
  int expand = strcmp(name, "expand_path") == 0;
  /* `File.expand_path("../x", __FILE__)`: __FILE__ is read as a directory,
     so its first `..` is the file's own directory and x is beside it */
  if (file && expand && argc == 2 && PM_NODE_TYPE(args->nodes[1]) == PM_SOURCE_FILE_NODE) {
    Mv lit;
    if (!pev_eval(c, args->nodes[0], &lit) || lit.k != MV_STR) return 0;
    if (lit.s[0] == '/') { *out = lit; return 1; }
    if (strncmp(lit.s, "../", 3) != 0) return 0;
    Mv pair[2] = { mv_str(c->dir, strlen(c->dir), MV_STR), mv_str(lit.s + 3, strlen(lit.s + 3), MV_STR) };
    return pev_file_join(pair, 2, out);
  }
  if (!expand && strcmp(name, "join") != 0) return -1;
  if (expand && argc > 2) return -1;
  for (size_t i = 0; i < argc; i++) {
    if (PM_NODE_TYPE(args->nodes[i]) == PM_SPLAT_NODE || !pev_eval(c, args->nodes[i], &v[i])) return 0;
    if (v[i].k != MV_STR) return 0;
  }
  if (!expand) return pev_file_join(v, (int)argc, out);
  /* `File.expand_path(path)` of an absolute path (`File.dirname(__FILE__)`):
     the path; a relative one is read against the current directory, which
     only the run time knows */
  if (argc == 1) {
    if (v[0].s[0] != '/') return 0;
    *out = v[0];
    return 1;
  }
  /* `File.expand_path(path, dir)` of a path under the directory it is given */
  if (v[0].s[0] == '/') { *out = v[0]; return 1; }
  Mv pair[2] = { v[1], v[0] };
  return pev_file_join(pair, 2, out);
}

static Mv *pev_lookup(PevVar *vars, int n, const char *name) {
  for (int i = n - 1; i >= 0; i--) if (strcmp(vars[i].name, name) == 0) return &vars[i].v;
  return NULL;
}

/* Set `name` in a table of `cap` variables; 0 when it was full and the name is new */
static int pev_set(PevVar *vars, int *n, int cap, const char *name, Mv v) {
  Mv *p = pev_lookup(vars, *n, name);
  if (p) { *p = v; return 1; }
  if (*n >= cap) return 0;
  vars[*n].name = strdup(name); vars[*n].v = v; (*n)++;
  return 1;
}
/* Bind local `name` in an evaluation: its table full makes the evaluation untrusted (`full`) */
static void pev_bind(PevCtx *c, const char *name, Mv v) {
  if (!pev_set(c->loc, &c->nloc, 64, name, v)) c->full = 1;
}


static int pev_eval_list(PevCtx *c, pm_node_list_t *l, Mv *out) {
  Mv v; memset(&v, 0, sizeof v); v.k = MV_ARR;
  v.a = calloc(l->size + 1, sizeof(Mv)); v.n = (int)l->size;
  for (size_t i = 0; i < l->size; i++) {
    if (PM_NODE_TYPE(l->nodes[i]) == PM_SPLAT_NODE) return 0;
    if (!pev_eval(c, l->nodes[i], &v.a[i])) return 0;
  }
  *out = v;
  return 1;
}

/* the value of a string-ish node's parts */
static int pev_eval_parts(PevCtx *c, pm_node_list_t *parts, MvBuf *b) {
  for (size_t i = 0; i < parts->size; i++) {
    pm_node_t *p = parts->nodes[i];
    if (PM_NODE_TYPE(p) == PM_STRING_NODE) {
      pm_string_node_t *s = (pm_string_node_t *)p;
      mvb_putn(b, (const char *)pm_string_source(&s->unescaped), pm_string_length(&s->unescaped));
    }
    else if (PM_NODE_TYPE(p) == PM_EMBEDDED_STATEMENTS_NODE) {
      pm_embedded_statements_node_t *e = (pm_embedded_statements_node_t *)p;
      if (!e->statements || e->statements->body.size != 1) return 0;
      Mv v;
      if (!pev_eval(c, e->statements->body.nodes[0], &v)) return 0;
      if (v.k == MV_ARR) { if (!mv_inspect_exact(v)) return 0; mv_inspect(v, b); }
      else mv_to_s(v, b);
    }
    else if (PM_NODE_TYPE(p) == PM_INTERPOLATED_STRING_NODE) {
      if (!pev_eval_parts(c, &((pm_interpolated_string_node_t *)p)->parts, b)) return 0;
    }
    else return 0;
  }
  return 1;
}

static int pev_pattern(pm_node_t *n, PevCtx *c, MvPat *pt) {
  memset(pt, 0, sizeof *pt);
  if (PM_NODE_TYPE(n) == PM_REGULAR_EXPRESSION_NODE) {
    pm_regular_expression_node_t *re = (pm_regular_expression_node_t *)n;
    if (n->flags & (PM_REGULAR_EXPRESSION_FLAGS_IGNORE_CASE | PM_REGULAR_EXPRESSION_FLAGS_EXTENDED |
                    PM_REGULAR_EXPRESSION_FLAGS_MULTI_LINE | PM_REGULAR_EXPRESSION_FLAGS_ONCE)) return 0;
    return mv_pat_regexp((const char *)re->content_loc.start, (const char *)re->content_loc.end, pt);
  }
  Mv v;
  if (!pev_eval(c, n, &v) || v.k != MV_STR || !mv_ascii(v.s) || !v.s[0]) return 0;
  pt->lit = 1; pt->text = v.s; pt->tn = strlen(v.s);
  return 1;
}

static int pev_eval_call(PevCtx *c, pm_call_node_t *n, Mv *out) {
  char *name = cstr(n->name);
  int ok = 0;
  pm_node_list_t *args = n->arguments ? &n->arguments->arguments : NULL;
  size_t argc = args ? args->size : 0;
  if (n->block) goto done;
  if (c->dir) {
    int r = pev_path_call(c, n, name, out);
    if (r >= 0) { ok = r; goto done; }
  }
  if (!n->receiver || PM_NODE_TYPE(n->receiver) == PM_SELF_NODE) {
    /* a call on no receiver reaches what the context's owner knows: a macro, the class's own name */
    ok = c->hooks && c->hooks->self_call(c, n, name, out);
    goto done;
  }
  Mv r;
  if (!pev_eval(c, n->receiver, &r)) goto done;
  if (argc == 2 && (strcmp(name, "gsub") == 0 || strcmp(name, "sub") == 0) && r.k == MV_STR && mv_ascii(r.s)) {
    MvPat pt; Mv rep;
    if (pev_pattern(args->nodes[0], c, &pt) && pev_eval(c, args->nodes[1], &rep) && rep.k == MV_STR)
      ok = mv_sub(r, &pt, rep.s, name[0] == 'g', out);
    goto done;
  }
  Mv a0; memset(&a0, 0, sizeof a0);
  if (argc == 1 && !pev_eval(c, args->nodes[0], &a0)) goto done;
  if (argc > 1) goto done;
  ok = mv_call(name, r, (int)argc, a0, out);
done:
  free(name);
  return ok;
}

static int pev_eval(PevCtx *c, pm_node_t *n, Mv *out) {
  if (!n) { *out = mv_nil(); return 1; }
  switch (PM_NODE_TYPE(n)) {
  case PM_NIL_NODE: *out = mv_nil(); return 1;
  case PM_TRUE_NODE: *out = mv_nil(); out->k = MV_TRUE; return 1;
  case PM_FALSE_NODE: *out = mv_nil(); out->k = MV_FALSE; return 1;
  case PM_INTEGER_NODE: {
    pm_integer_node_t *in = (pm_integer_node_t *)n;
    unsigned long long uv;
    if (in->value.values) {
      if (in->value.length > 2) return 0;
      uv = (unsigned long long)in->value.values[0] |
           (in->value.length > 1 ? (unsigned long long)in->value.values[1] << 32 : 0);
      if (uv > 0x7fffffffffffffffULL) return 0;
    }
    else uv = in->value.value;
    *out = mv_nil(); out->k = MV_INT;
    out->i = in->value.negative ? -(long long)uv : (long long)uv;
    return 1;
  }
  case PM_STRING_NODE: {
    pm_string_node_t *s = (pm_string_node_t *)n;
    *out = mv_str((const char *)pm_string_source(&s->unescaped), pm_string_length(&s->unescaped), MV_STR);
    return 1;
  }
  case PM_SYMBOL_NODE: {
    pm_symbol_node_t *s = (pm_symbol_node_t *)n;
    *out = mv_str((const char *)pm_string_source(&s->unescaped), pm_string_length(&s->unescaped), MV_SYM);
    return 1;
  }
  case PM_INTERPOLATED_STRING_NODE: case PM_INTERPOLATED_SYMBOL_NODE: {
    pm_node_list_t *parts = PM_NODE_TYPE(n) == PM_INTERPOLATED_STRING_NODE
      ? &((pm_interpolated_string_node_t *)n)->parts : &((pm_interpolated_symbol_node_t *)n)->parts;
    MvBuf b = {0};
    if (!pev_eval_parts(c, parts, &b)) { free(b.p); return 0; }
    *out = mv_str(b.p ? b.p : "", b.len, PM_NODE_TYPE(n) == PM_INTERPOLATED_STRING_NODE ? MV_STR : MV_SYM);
    free(b.p);
    return 1;
  }
  case PM_ARRAY_NODE: return pev_eval_list(c, &((pm_array_node_t *)n)->elements, out);
  case PM_LOCAL_VARIABLE_READ_NODE: {
    char *nm = cstr(((pm_local_variable_read_node_t *)n)->name);
    Mv *v = pev_lookup(c->loc, c->nloc, nm);
    free(nm);
    if (!v) return 0;
    *out = *v; return 1;
  }
  /* the instance variables a macro keeps: the context's owner answers */
  case PM_INSTANCE_VARIABLE_READ_NODE: case PM_DEFINED_NODE:
    return c->hooks && c->hooks->node(c, n, out);
  case PM_PARENTHESES_NODE: {
    pm_node_t *b = ((pm_parentheses_node_t *)n)->body;
    if (b && PM_NODE_TYPE(b) == PM_STATEMENTS_NODE) {
      pm_statements_node_t *s = (pm_statements_node_t *)b;
      if (s->body.size != 1) return 0;
      b = s->body.nodes[0];
    }
    return pev_eval(c, b, out);
  }
  case PM_AND_NODE: case PM_OR_NODE: {
    pm_node_t *l = PM_NODE_TYPE(n) == PM_AND_NODE ? ((pm_and_node_t *)n)->left : ((pm_or_node_t *)n)->left;
    pm_node_t *r = PM_NODE_TYPE(n) == PM_AND_NODE ? ((pm_and_node_t *)n)->right : ((pm_or_node_t *)n)->right;
    Mv lv;
    if (!pev_eval(c, l, &lv)) return 0;
    if ((PM_NODE_TYPE(n) == PM_AND_NODE) != mv_truthy(lv)) { *out = lv; return 1; }
    return pev_eval(c, r, out);
  }
  /* __LINE__ / __FILE__ are the macro source's: kept as written */
  case PM_SOURCE_LINE_NODE: case PM_SOURCE_FILE_NODE: return 0;
  case PM_CALL_NODE: return pev_eval_call(c, (pm_call_node_t *)n, out);
  case PM_IF_NODE: {
    /* `c ? a : b`, and an if whose arms are one expression each */
    pm_if_node_t *in = (pm_if_node_t *)n;
    Mv pv;
    if (!pev_eval(c, in->predicate, &pv)) return 0;
    pm_statements_node_t *arm = in->statements;
    if (!mv_truthy(pv)) {
      pm_node_t *e = in->subsequent;
      if (!e) { *out = mv_nil(); return 1; }
      if (PM_NODE_TYPE(e) != PM_ELSE_NODE) return pev_eval(c, e, out);
      arm = ((pm_else_node_t *)e)->statements;
    }
    if (!arm) { *out = mv_nil(); return 1; }
    if (arm->body.size != 1) return 0;
    return pev_eval(c, arm->body.nodes[0], out);
  }
  default: return 0;
  }
}

