/* Class bodies built by a loop over a literal Hash (included into
 * spinel_parse.c, after sp_macro.c, whose evaluator it uses).
 *
 * A library sometimes defines a family of constants and classes from a table,
 * with a loop the class body runs once at load time:
 *
 *   module HTTPStatus
 *     StatusMessage = { 200 => 'OK', 404 => 'Not Found' }
 *     CodeToError = {}
 *     StatusMessage.each { |code, message|
 *       err_name = message.gsub(/[ \-]/, '')
 *       parent = code < 400 ? Success : ClientError
 *       const_set("RC_#{err_name.upcase}", code)
 *       err_class = Class.new(parent)
 *       err_class.instance_variable_set(:@code, code)
 *       const_set(err_name, err_class)
 *       CodeToError[code] = err_class
 *     }
 *   end
 *
 * Every name and value here is known when the program is compiled. This pass
 * runs the loop over the table and writes what it does back as the class
 * body's own statements, on the loop's lines:
 *
 *   RC_OK = 200; class OK < Success; @code = 200; end; CodeToError[200] = OK; ...
 *
 * The loop is replaced only when everything it does is understood:
 *  - it is a statement of a class or module body, `H.each` / `H.each_pair`
 *    with a block of two parameters, and H is a constant assigned a Hash
 *    literal of literal keys and values earlier in the same body, assigned
 *    nowhere else, and named elsewhere only as the receiver of a call that
 *    reads it;
 *  - its block writes only its own locals, from values the evaluator computes
 *    (prism_eval.c's pev_eval), a constant, an `if` / `case` choosing among
 *    those, or `Class.new(Constant)`;
 *  - and otherwise only calls `const_set(name, value)` with a computed
 *    constant name, `instance_variable_set(:@x, value)` on a class it made
 *    before that class is named, `Const[key] = value`, or `freeze` on a
 *    String it holds.
 * A class it makes must be named by a const_set before anything else uses
 * it, with a name the program gives a value nowhere else. The program must
 * leave the core classes whose methods the loop runs alone, give the table
 * no methods of its own, and have no inherited hook (CRuby calls it before
 * the class has a name). Anything else leaves the loop as written. */
#include "ct_value.h"

typedef enum { CL_MV = 0, CL_CONST, CL_KLASS } ClKind;
/* lit: a String the table spells as a literal, key or value; frozen: the
   loop froze it (a Hash's String key is frozen). Only such a String is
   written out, as that literal (always frozen in Spinel): one the loop did
   not freeze is unfrozen in CRuby, and one it computes is a new object. */
typedef struct { ClKind k; Mv mv; char *cref; int klass; int lit, frozen; } ClVal;
typedef struct { char *parent; char *name; char *ivars; } ClKlass;   /* ivars: "@a = 1; @b = 2; " */
typedef struct { char *name; ClVal v; } ClVar;

typedef struct {
  ClVar loc[64]; int nloc;
  ClKlass *kl; int nkl, ckl;
  MvBuf *out;
  MxCtx mx;          /* the plain values, for pev_eval */
  const char *src;   /* the whole program */
  const pm_node_t *root;          /* its tree, to look for other definitions of a name */
  const uint8_t *lo, *hi;         /* the loop's own text */
  char **names; int nnames;   /* the constants the loop has named so far */
} ClCtx;

/* Is the constant `name` given a value anywhere else in the program (a
   class or module of that name, a write, a target of a multiple assignment,
   a const_set outside the loop with that name or one not known), or twice by
   this loop? A const_set then replaces an existing value (with CRuby's
   warning), which a class definition would reopen instead. Any constant of
   that last name counts, whatever its scope. */
typedef struct { const char *name; const uint8_t *lo, *hi; int hit; } ClTaken;

/* A call's first argument when it is a Symbol or String literal, else NULL */
static const pm_string_t *cl_lit_arg0(const pm_call_node_t *cn) {
  const pm_node_t *a0 = cn->arguments && cn->arguments->arguments.size ? cn->arguments->arguments.nodes[0] : NULL;
  return !a0 ? NULL
       : PM_NODE_TYPE(a0) == PM_SYMBOL_NODE ? &((const pm_symbol_node_t *)a0)->unescaped
       : PM_NODE_TYPE(a0) == PM_STRING_NODE ? &((const pm_string_node_t *)a0)->unescaped : NULL;
}

static int cl_id_is(pm_constant_id_t id, const char *name) {
  char *s = mx_name(id);
  int r = strcmp(s, name) == 0;
  free(s);
  return r;
}

static bool cl_taken_visit(const pm_node_t *n, void *data) {
  ClTaken *t = (ClTaken *)data;
  if (t->hit) return false;
  switch (PM_NODE_TYPE(n)) {
  case PM_CLASS_NODE: case PM_MODULE_NODE: {
    const pm_node_t *cp = PM_NODE_TYPE(n) == PM_CLASS_NODE ? ((pm_class_node_t *)n)->constant_path
                                                           : ((pm_module_node_t *)n)->constant_path;
    pm_constant_id_t id = PM_NODE_TYPE(cp) == PM_CONSTANT_READ_NODE ? ((pm_constant_read_node_t *)cp)->name
                        : PM_NODE_TYPE(cp) == PM_CONSTANT_PATH_NODE ? ((pm_constant_path_node_t *)cp)->name : 0;
    if (!id || cl_id_is(id, t->name)) t->hit = 1;
    break;
  }
  case PM_CONSTANT_WRITE_NODE: t->hit = cl_id_is(((pm_constant_write_node_t *)n)->name, t->name); break;
  case PM_CONSTANT_OR_WRITE_NODE: t->hit = cl_id_is(((pm_constant_or_write_node_t *)n)->name, t->name); break;
  case PM_CONSTANT_AND_WRITE_NODE: t->hit = cl_id_is(((pm_constant_and_write_node_t *)n)->name, t->name); break;
  case PM_CONSTANT_OPERATOR_WRITE_NODE: t->hit = cl_id_is(((pm_constant_operator_write_node_t *)n)->name, t->name); break;
  case PM_CONSTANT_TARGET_NODE: t->hit = cl_id_is(((pm_constant_target_node_t *)n)->name, t->name); break;
  case PM_CONSTANT_PATH_WRITE_NODE: case PM_CONSTANT_PATH_OR_WRITE_NODE: case PM_CONSTANT_PATH_AND_WRITE_NODE:
  case PM_CONSTANT_PATH_OPERATOR_WRITE_NODE: {
    const pm_constant_path_node_t *tg =
      PM_NODE_TYPE(n) == PM_CONSTANT_PATH_WRITE_NODE ? ((pm_constant_path_write_node_t *)n)->target
      : PM_NODE_TYPE(n) == PM_CONSTANT_PATH_OR_WRITE_NODE ? ((pm_constant_path_or_write_node_t *)n)->target
      : PM_NODE_TYPE(n) == PM_CONSTANT_PATH_AND_WRITE_NODE ? ((pm_constant_path_and_write_node_t *)n)->target
      : ((pm_constant_path_operator_write_node_t *)n)->target;
    if (cl_id_is(tg->name, t->name)) t->hit = 1;
    break;
  }
  case PM_CONSTANT_PATH_TARGET_NODE: t->hit = cl_id_is(((pm_constant_path_target_node_t *)n)->name, t->name); break;
  case PM_CALL_NODE: {
    const pm_call_node_t *cn = (const pm_call_node_t *)n;
    if (n->location.start >= t->lo && n->location.end <= t->hi) return false;   /* the loop itself */
    char *m = mx_name(cn->name);
    int cs = strcmp(m, "const_set") == 0;
    free(m);
    if (cs) {
      const pm_string_t *lit = cl_lit_arg0(cn);
      if (!lit || (pm_string_length(lit) == strlen(t->name) &&
                   memcmp(pm_string_source(lit), t->name, strlen(t->name)) == 0))
        t->hit = 1;
    }
    break;
  }
  default: break;
  }
  return !t->hit;
}

static int cl_name_taken(ClCtx *c, const char *name) {
  for (int i = 0; i < c->nnames; i++) if (strcmp(c->names[i], name) == 0) return 1;
  ClTaken t = { name, c->lo, c->hi, 0 };
  pm_visit_node(c->root, cl_taken_visit, &t);
  return t.hit;
}

static void cl_take_name(ClCtx *c, const char *name) {
  c->names = realloc(c->names, sizeof(char *) * (size_t)(c->nnames + 1));
  c->names[c->nnames++] = strdup(name);
}

static void cl_sync(ClCtx *c) {
  c->mx.ev.nloc = 0;
  for (int i = 0; i < c->nloc && i < 64; i++)
    if (c->loc[i].v.k == CL_MV) pev_bind(&c->mx.ev, c->loc[i].name, c->loc[i].v.mv);
}

static ClVal *cl_lookup(ClCtx *c, const char *name) {
  for (int i = 0; i < c->nloc; i++) if (strcmp(c->loc[i].name, name) == 0) return &c->loc[i].v;
  return NULL;
}

static int cl_set(ClCtx *c, const char *name, ClVal v) {
  ClVal *o = cl_lookup(c, name);
  if (o) *o = v;
  else {
    if (c->nloc >= 64) return 0;
    c->loc[c->nloc].name = strdup(name);
    c->loc[c->nloc].v = v;
    c->nloc++;
  }
  cl_sync(c);
  return 1;
}

static char *cl_text(const pm_node_t *n) {
  return strndup((const char *)n->location.start, (size_t)(n->location.end - n->location.start));
}

static int cl_eval(ClCtx *c, pm_node_t *n, ClVal *out);

/* the value as Ruby source, where it reads back as itself */
static int cl_spell(ClCtx *c, ClVal v, MvBuf *b) {
  if (v.k == CL_CONST) { mvb_puts(b, v.cref); return 1; }
  if (v.k == CL_KLASS) {
    if (!c->kl[v.klass].name) return 0;     /* an anonymous class has no spelling */
    mvb_puts(b, c->kl[v.klass].name);
    return 1;
  }
  if (v.mv.k == MV_UNDEF) return 0;
  if (v.mv.k == MV_STR && !(v.lit && v.frozen)) return 0;
  mv_inspect(v.mv, b);
  return 1;
}

static int cl_eval(ClCtx *c, pm_node_t *n, ClVal *out) {
  memset(out, 0, sizeof *out);
  switch (PM_NODE_TYPE(n)) {
  case PM_CONSTANT_READ_NODE: case PM_CONSTANT_PATH_NODE:
    out->k = CL_CONST; out->cref = cl_text(n);
    return 1;
  case PM_LOCAL_VARIABLE_READ_NODE: {
    char *nm = mx_name(((pm_local_variable_read_node_t *)n)->name);
    ClVal *v = cl_lookup(c, nm);
    free(nm);
    if (!v) return 0;
    *out = *v; return 1;
  }
  case PM_PARENTHESES_NODE: {
    pm_node_t *b = ((pm_parentheses_node_t *)n)->body;
    if (b && PM_NODE_TYPE(b) == PM_STATEMENTS_NODE) {
      if (((pm_statements_node_t *)b)->body.size != 1) return 0;
      b = ((pm_statements_node_t *)b)->body.nodes[0];
    }
    return b && cl_eval(c, b, out);
  }
  case PM_IF_NODE: {
    pm_if_node_t *in = (pm_if_node_t *)n;
    Mv pv;
    if (!pev_eval(&c->mx.ev, in->predicate, &pv)) return 0;
    pm_statements_node_t *arm = in->statements;
    if (!mv_truthy(pv)) {
      pm_node_t *e = in->subsequent;
      if (!e) { out->mv = mv_nil(); return 1; }
      if (PM_NODE_TYPE(e) != PM_ELSE_NODE) return cl_eval(c, e, out);
      arm = ((pm_else_node_t *)e)->statements;
    }
    if (!arm) { out->mv = mv_nil(); return 1; }
    return arm->body.size == 1 && cl_eval(c, arm->body.nodes[0], out);
  }
  case PM_CALL_NODE: {
    pm_call_node_t *cn = (pm_call_node_t *)n;
    char *name = mx_name(cn->name);
    int is_new = strcmp(name, "new") == 0;
    free(name);
    /* Class.new(Constant) */
    if (is_new && cn->receiver && PM_NODE_TYPE(cn->receiver) == PM_CONSTANT_READ_NODE && !cn->block &&
        cn->arguments && cn->arguments->arguments.size == 1) {
      char *rn = mx_name(((pm_constant_read_node_t *)cn->receiver)->name);
      int cls = strcmp(rn, "Class") == 0;
      free(rn);
      if (!cls) return 0;
      ClVal pv;
      if (!cl_eval(c, cn->arguments->arguments.nodes[0], &pv) || pv.k != CL_CONST) return 0;
      if (c->nkl == c->ckl) { c->ckl = c->ckl ? c->ckl * 2 : 16; c->kl = realloc(c->kl, sizeof(ClKlass) * (size_t)c->ckl); }
      c->kl[c->nkl].parent = pv.cref; c->kl[c->nkl].name = NULL; c->kl[c->nkl].ivars = strdup("");
      out->k = CL_KLASS; out->klass = c->nkl++;
      return 1;
    }
    break;
  }
  default: break;
  }
  out->k = CL_MV;
  return pev_eval(&c->mx.ev, n, &out->mv);
}

/* `when` values: an Integer, String or Symbol literal, or an Integer range */
static int cl_when_matches(ClCtx *c, pm_node_t *w, Mv v, int *hit) {
  if (PM_NODE_TYPE(w) == PM_RANGE_NODE) {
    pm_range_node_t *rg = (pm_range_node_t *)w;
    Mv lo, hi;
    if (!rg->left || !rg->right || !pev_eval(&c->mx.ev, rg->left, &lo) || !pev_eval(&c->mx.ev, rg->right, &hi) ||
        lo.k != MV_INT || hi.k != MV_INT) return 0;
    int excl = (w->flags & PM_RANGE_FLAGS_EXCLUDE_END) != 0;
    *hit = v.k == MV_INT && v.i >= lo.i && (excl ? v.i < hi.i : v.i <= hi.i);
    return 1;
  }
  Mv wv;
  if (!pev_eval(&c->mx.ev, w, &wv) || (wv.k != MV_INT && wv.k != MV_STR && wv.k != MV_SYM)) return 0;
  *hit = mv_eq(v, wv);
  return 1;
}

static int cl_exec_stmts(ClCtx *c, pm_statements_node_t *s);

static int cl_exec(ClCtx *c, pm_node_t *n) {
  MvBuf *o = c->out;
  switch (PM_NODE_TYPE(n)) {
  case PM_LOCAL_VARIABLE_WRITE_NODE: {
    pm_local_variable_write_node_t *w = (pm_local_variable_write_node_t *)n;
    if (w->depth != 0) return 0;     /* a local of the scope around the block */
    ClVal v;
    if (!cl_eval(c, w->value, &v)) return 0;
    char *nm = mx_name(w->name);
    int ok = cl_set(c, nm, v);
    free(nm);
    return ok;
  }
  case PM_IF_NODE: case PM_UNLESS_NODE: {
    int is_if = PM_NODE_TYPE(n) == PM_IF_NODE;
    pm_node_t *pred = is_if ? ((pm_if_node_t *)n)->predicate : ((pm_unless_node_t *)n)->predicate;
    Mv pv;
    if (!pev_eval(&c->mx.ev, pred, &pv)) return 0;
    pm_statements_node_t *then = is_if ? ((pm_if_node_t *)n)->statements : ((pm_unless_node_t *)n)->statements;
    pm_node_t *els = is_if ? ((pm_if_node_t *)n)->subsequent : (pm_node_t *)((pm_unless_node_t *)n)->else_clause;
    if (mv_truthy(pv) == is_if) return cl_exec_stmts(c, then);
    if (!els) return 1;
    if (PM_NODE_TYPE(els) == PM_ELSE_NODE) return cl_exec_stmts(c, ((pm_else_node_t *)els)->statements);
    return cl_exec(c, els);
  }
  case PM_CASE_NODE: {
    pm_case_node_t *cs = (pm_case_node_t *)n;
    Mv v;
    if (!cs->predicate || !pev_eval(&c->mx.ev, cs->predicate, &v)) return 0;
    for (size_t i = 0; i < cs->conditions.size; i++) {
      pm_when_node_t *wn = (pm_when_node_t *)cs->conditions.nodes[i];
      if (PM_NODE_TYPE(cs->conditions.nodes[i]) != PM_WHEN_NODE) return 0;
      for (size_t j = 0; j < wn->conditions.size; j++) {
        int hit = 0;
        if (!cl_when_matches(c, wn->conditions.nodes[j], v, &hit)) return 0;
        if (hit) return cl_exec_stmts(c, wn->statements);
      }
    }
    return cs->else_clause ? cl_exec_stmts(c, cs->else_clause->statements) : 1;
  }
  case PM_CALL_NODE: {
    pm_call_node_t *cn = (pm_call_node_t *)n;
    if (cn->block) return 0;
    pm_node_list_t *args = cn->arguments ? &cn->arguments->arguments : NULL;
    size_t argc = args ? args->size : 0;
    for (size_t i = 0; i < argc; i++)
      if (PM_NODE_TYPE(args->nodes[i]) == PM_SPLAT_NODE || PM_NODE_TYPE(args->nodes[i]) == PM_KEYWORD_HASH_NODE ||
          PM_NODE_TYPE(args->nodes[i]) == PM_BLOCK_ARGUMENT_NODE) return 0;
    char *name = mx_name(cn->name);
    int self_recv = !cn->receiver || PM_NODE_TYPE(cn->receiver) == PM_SELF_NODE;
    int ok = 0;
    if (self_recv && argc == 2 && strcmp(name, "const_set") == 0) {
      Mv cnm; ClVal v;
      if (pev_eval(&c->mx.ev, args->nodes[0], &cnm) && (cnm.k == MV_STR || cnm.k == MV_SYM) &&
          isupper((unsigned char)cnm.s[0]) && mv_sym_plain(cnm.s) && cnm.s[strlen(cnm.s) - 1] != '?' &&
          cnm.s[strlen(cnm.s) - 1] != '!' && cnm.s[strlen(cnm.s) - 1] != '=' &&
          !cl_name_taken(c, cnm.s) && cl_eval(c, args->nodes[1], &v)) {
        cl_take_name(c, cnm.s);
        if (v.k == CL_KLASS && !c->kl[v.klass].name) {
          /* the class takes its name: its definition, with what was set on it */
          ClKlass *k = &c->kl[v.klass];
          k->name = strdup(cnm.s);
          mvb_puts(o, "class "); mvb_puts(o, k->name); mvb_puts(o, " < "); mvb_puts(o, k->parent);
          mvb_puts(o, "; "); mvb_puts(o, k->ivars); mvb_puts(o, "end; ");
          ok = 1;
        }
        else {
          MvBuf vb = {0};
          if (cl_spell(c, v, &vb)) {
            mvb_puts(o, cnm.s); mvb_puts(o, " = "); mvb_puts(o, vb.p ? vb.p : "nil"); mvb_puts(o, "; ");
            ok = 1;
          }
          free(vb.p);
        }
      }
    }
    else if (!self_recv && argc == 2 && strcmp(name, "instance_variable_set") == 0) {
      ClVal r, v; Mv ivn;
      if (cl_eval(c, cn->receiver, &r) && r.k == CL_KLASS && !c->kl[r.klass].name &&
          pev_eval(&c->mx.ev, args->nodes[0], &ivn) && (ivn.k == MV_SYM || ivn.k == MV_STR) &&
          ivn.s[0] == '@' && ivn.s[1] != '@' && mv_sym_plain(ivn.s + 1) && cl_eval(c, args->nodes[1], &v)) {
        MvBuf vb = {0};
        if (cl_spell(c, v, &vb)) {
          ClKlass *k = &c->kl[r.klass];
          size_t need = strlen(k->ivars) + strlen(ivn.s) + (vb.p ? strlen(vb.p) : 3) + 8;
          char *nv = malloc(need);
          snprintf(nv, need, "%s%s = %s; ", k->ivars, ivn.s, vb.p ? vb.p : "nil");
          free(k->ivars); k->ivars = nv;
          ok = 1;
        }
        free(vb.p);
      }
    }
    else if (!self_recv && argc == 2 && strcmp(name, "[]=") == 0 &&
             (PM_NODE_TYPE(cn->receiver) == PM_CONSTANT_READ_NODE || PM_NODE_TYPE(cn->receiver) == PM_CONSTANT_PATH_NODE)) {
      ClVal kv, v;
      MvBuf kb = {0}, vb = {0};
      if (cl_eval(c, args->nodes[0], &kv) && cl_eval(c, args->nodes[1], &v) &&
          cl_spell(c, kv, &kb) && cl_spell(c, v, &vb)) {
        char *rt = cl_text(cn->receiver);
        mvb_puts(o, rt); mvb_puts(o, "["); mvb_puts(o, kb.p); mvb_puts(o, "] = "); mvb_puts(o, vb.p); mvb_puts(o, "; ");
        free(rt);
        ok = 1;
      }
      free(kb.p); free(vb.p);
    }
    else if (!self_recv && argc == 0 && strcmp(name, "freeze") == 0) {
      /* a value the loop holds: a String it spells out after this is frozen (cl_spell) */
      ClVal r;
      ok = cl_eval(c, cn->receiver, &r) && r.k == CL_MV && (r.mv.k == MV_STR || r.mv.k == MV_INT || r.mv.k == MV_SYM);
      if (ok && PM_NODE_TYPE(cn->receiver) == PM_LOCAL_VARIABLE_READ_NODE) {
        char *nm = mx_name(((pm_local_variable_read_node_t *)cn->receiver)->name);
        ClVal *lv = cl_lookup(c, nm);
        if (lv) lv->frozen = 1;
        free(nm);
      }
    }
    free(name);
    return ok;
  }
  default: return 0;
  }
}

static int cl_exec_stmts(ClCtx *c, pm_statements_node_t *s) {
  if (!s) return 1;
  for (size_t i = 0; i < s->body.size; i++) if (!cl_exec(c, s->body.nodes[i])) return 0;
  return 1;
}

/* ---- where the table's constant is named ---- */
typedef struct {
  pm_constant_id_t name;
  const pm_node_t *loop;     /* the loop's own call */
  int writes, bad;
} ClUse;

static int cl_read_only(const char *m) {
  static const char *const R[] = { "each", "each_pair", "[]", "fetch", "key?", "has_key?", "include?",
    "member?", "key", "keys", "values", "size", "length", "empty?", "dig", "to_a", "frozen?", "freeze",
    "each_key", "each_value", "values_at", "has_value?", "value?", "count", "any?", "all?", "none?",
    "find", "detect", "map", "collect", "select", "filter", "reject", "min_by", "max_by", "sort_by",
    "group_by", "sum", "inject", "reduce", "to_h", "invert", "sort", "first", "inspect", "to_s", "hash", "==", NULL };
  for (int i = 0; R[i]; i++) if (strcmp(R[i], m) == 0) return 1;
  return 0;
}

static bool cl_use_visit(const pm_node_t *n, void *data) {
  ClUse *u = (ClUse *)data;
  switch (PM_NODE_TYPE(n)) {
  case PM_CONSTANT_WRITE_NODE: case PM_CONSTANT_OR_WRITE_NODE: case PM_CONSTANT_AND_WRITE_NODE:
  case PM_CONSTANT_OPERATOR_WRITE_NODE: {
    pm_constant_id_t nm = PM_NODE_TYPE(n) == PM_CONSTANT_WRITE_NODE ? ((pm_constant_write_node_t *)n)->name
                        : PM_NODE_TYPE(n) == PM_CONSTANT_OR_WRITE_NODE ? ((pm_constant_or_write_node_t *)n)->name
                        : PM_NODE_TYPE(n) == PM_CONSTANT_AND_WRITE_NODE ? ((pm_constant_and_write_node_t *)n)->name
                        : ((pm_constant_operator_write_node_t *)n)->name;
    if (nm == u->name) u->writes++;
    break;
  }
  case PM_CONSTANT_PATH_WRITE_NODE: case PM_CONSTANT_PATH_OR_WRITE_NODE: case PM_CONSTANT_PATH_AND_WRITE_NODE:
  case PM_CONSTANT_PATH_OPERATOR_WRITE_NODE: case PM_CONSTANT_TARGET_NODE: case PM_CONSTANT_PATH_TARGET_NODE:
    /* a write of any path, or a multiple assignment: not followed */
    u->bad = 1;
    break;
  case PM_CALL_NODE: {
    const pm_call_node_t *cn = (const pm_call_node_t *)n;
    const pm_node_t *r = cn->receiver;
    int mine = r && ((PM_NODE_TYPE(r) == PM_CONSTANT_READ_NODE && ((pm_constant_read_node_t *)r)->name == u->name) ||
                     (PM_NODE_TYPE(r) == PM_CONSTANT_PATH_NODE && ((pm_constant_path_node_t *)r)->name == u->name));
    if (mine) {
      char *m = mx_name(cn->name);
      int ro = cl_read_only(m);
      /* these answer the table itself, which the caller could then change */
      if (n != u->loop && (strcmp(m, "each") == 0 || strcmp(m, "each_pair") == 0 || strcmp(m, "each_key") == 0 ||
                           strcmp(m, "each_value") == 0 || strcmp(m, "freeze") == 0 || strcmp(m, "to_h") == 0))
        ro = 0;
      free(m);
      /* the receiver read is fine; its arguments and block are visited */
      if (!ro) u->bad = 1;
      if (cn->arguments) pm_visit_node((const pm_node_t *)cn->arguments, cl_use_visit, u);
      if (cn->block) pm_visit_node(cn->block, cl_use_visit, u);
      return false;
    }
    break;
  }
  case PM_CONSTANT_READ_NODE:
    if (((pm_constant_read_node_t *)n)->name == u->name) u->bad = 1;   /* handed on as a value */
    break;
  case PM_CONSTANT_PATH_NODE:
    if (((pm_constant_path_node_t *)n)->name == u->name) u->bad = 1;
    return false;
  default: break;
  }
  return true;
}

