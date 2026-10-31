/* analyze_const.c -- which constants a read can reach before their assignment.

   A constant is a C global assigned where the program assigns it, and CRuby
   raises NameError for a read, defined?, const_defined? or const_get that
   runs before that. A constant read after its assignment is the common case
   and needs nothing at run time, so the analysis settles it: its assignment
   is a statement of the top level or of a class or module body (a
   Struct.new, Class.new block's body counts; so does a multiple assignment's
   constant targets), and either

     - nothing that runs before it, in program order and entering the bodies,
       can run code of the program's: only definitions (def, class, module,
       alias, attr_*, include, extend, prepend, a visibility call) and the
       settled assignments of other constants, whose values are built of
       literals, settled constants, class constructors and the builtin
       operators on those; or
     - no read of the constant (a bare name or a path, defined?, const_get,
       const_defined?) and no `constants` call comes earlier in program order
       than the assignment: a method that reads it and is written before the
       assignment is such a read, one written after it is not, since the def
       has not run before the assignment has. An assignment's own value is
       earlier than it.

   Program order is the order the load phase runs, as the load record has
   it: a required file's top level runs at its load. Each read asks the
   record's constant histories whether a write completes before its point.

   An assignment is a statement of a body when it is a plain `K = v`, a
   constant target of a multiple assignment, or `Mod::K = v`. Other
   assignments of the same constant (inside a method, a block or a branch,
   the `||=`, `op=` and const_set forms) matter only if one can come before
   that first one in program order.

   Every other constant keeps a presence flag, LocalVar.const_early: its
   assignment sets it, and a read that finds it clear raises NameError
   (codegen_util.c, const_has_flag). The walk reads no types, so the guard
   folds that decide `defined?(K)` before inference (comp_defined_guard_true)
   can ask it. A program that defines const_missing, which spinel does not
   call, flags only the constants a `constants` listing alone can see before
   their assignment, and a name that is also a class's is not flagged.

   A program that defines a method a definition runs (inherited, included,
   method_added ...) has its definitions counted as code of the program's
   too, and one that reopens a builtin class has the builtin operators
   counted so. The pass also names, for each flagged bare read, the class or
   module it is written in (`const_cref`), as the error shows it. */

#include "analyze_internal.h"
#include "builtin_names.h"

typedef struct {
  Compiler *c;
  const NodeTable *nt;
  char *settled;     /* per node: a ConstantWriteNode no code of the program's precedes */
  char *seen;        /* per constant: a settled assignment has run */
  char *plain;       /* per constant: ... of a value built of literals alone */
  int clean;         /* nothing that has run so far can run the program's code */
  const int *par;    /* the parent map */
  char **crefs;      /* the lexical names of the classes and modules flagged reads are written in, as an error shows them */
  int ncrefs, ccrefs;
  int hooks;         /* a definition can run a hook of the program's */
  int cmissing;      /* the program defines const_missing, which spinel does not call */
  int reopened;      /* a builtin class is reopened: its operators may be the program's */
} ConstWalk;

static int cw_index(const ConstWalk *w, const LocalVar *lv) { return (int)(lv - w->c->consts); }

static void cw_stmt(ConstWalk *w, int id, int depth);

/* Statements of a body, run in order. */
static void cw_stmts(ConstWalk *w, int body, int depth) {
  if (body < 0) return;
  if (nt_kind(w->nt, body) != NK_StatementsNode) { cw_stmt(w, body, depth); return; }
  int n = 0; const int *ids = nt_arr(w->nt, body, "body", &n);
  for (int i = 0; i < n; i++) cw_stmt(w, ids[i], depth);
}

/* Evaluating node id runs no code of the program's: 0 when it can, 1 when it
   cannot, 2 when it also yields a value built of literals alone (whose
   builtin operators run none either). A body a class builder runs is walked
   as the statements it is. */
static int cw_value_walk(ConstWalk *w, int id, int depth);
static int cw_value(ConstWalk *w, int id, int depth) {
  int r = cw_value_walk(w, id, depth);
  /* from here on the program's code may have run */
  if (!r) w->clean = 0;
  return r;
}
static int cw_value_walk(ConstWalk *w, int id, int depth) {
  const NodeTable *nt = w->nt;
  if (id < 0) return 2;
  if (depth > 100) return 0;
  switch (nt_kind(nt, id)) {
    case NK_IntegerNode: case NK_FloatNode: case NK_StringNode: case NK_SymbolNode:
    case NK_NilNode: case NK_TrueNode: case NK_FalseNode: case NK_RationalNode:
    case NK_ImaginaryNode: case NK_RegularExpressionNode:
      return 2;
    case NK_SelfNode: case NK_LambdaNode: case NK_LocalVariableReadNode:
      return 1;
    case NK_DefNode: case NK_AliasMethodNode: case NK_UndefNode:
      return w->hooks ? 0 : 1;
    case NK_ArrayNode: case NK_HashNode: case NK_KeywordHashNode: case NK_StatementsNode:
    case NK_InterpolatedStringNode: case NK_RangeNode: case NK_AssocNode: case NK_ParenthesesNode:
    case NK_EmbeddedStatementsNode: {
      int r = 2;
      static const char *const arrs[] = { "elements", "body", "parts", NULL };
      static const char *const refs[] = { "left", "right", "key", "value", "statements", "body", NULL };
      for (int k = 0; arrs[k]; k++) {
        int n = 0; const int *ids = nt_arr(nt, id, arrs[k], &n);
        for (int i = 0; ids && i < n; i++) { int v = cw_value(w, ids[i], depth + 1); if (v < r) r = v; }
      }
      for (int k = 0; refs[k]; k++) {
        int ch = nt_ref(nt, id, refs[k]);
        if (ch >= 0) { int v = cw_value(w, ch, depth + 1); if (v < r) r = v; }
      }
      /* an interpolation converts its value: only a literal-built one is the builtin's */
      if (nt_kind(nt, id) == NK_EmbeddedStatementsNode && r == 1) r = 0;
      return r;
    }
    case NK_LocalVariableWriteNode:
      return cw_value(w, nt_ref(nt, id, "value"), depth + 1) ? 1 : 0;
    case NK_ConstantReadNode: case NK_ConstantPathNode: {
      /* a class or a builtin constant is there; a value constant must have been assigned */
      int par = nt_kind(nt, id) == NK_ConstantPathNode ? nt_ref(nt, id, "parent") : -1;
      if (par >= 0 && !cw_value(w, par, depth + 1)) return 0;
      const char *nm = nt_str(nt, id, "name");
      LocalVar *lv = nm ? comp_const(w->c, nm) : NULL;
      if (!lv) return 1;
      int i = cw_index(w, lv);
      return w->seen[i] ? (w->plain[i] ? 2 : 1) : 0;
    }
    case NK_CallNode: {
      const char *nm = nt_str(nt, id, "name");
      int r = nt_ref(nt, id, "receiver"), a = nt_ref(nt, id, "arguments"), blk = nt_ref(nt, id, "block");
      int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
      if (!nm) return 0;
      int ar = 2;
      for (int i = 0; i < an; i++) { int v = cw_value(w, av[i], depth + 1); if (v < ar) ar = v; }
      if (ar == 0) return 0;
      if (r < 0) {
        if (blk >= 0) return 0;
        /* the declarations of a body; a mixin runs the module's hook, if any */
        if (is_attr_reader_family(nm) || is_attr_writer_family(nm) || visibility_of_name(nm))
          return 1;
        return is_mixin_call(nm) && !w->hooks ? 1 : 0;
      }
      int rv = cw_value(w, r, depth + 1);
      if (rv == 0) return 0;
      const char *rn = nt_kind(nt, r) == NK_ConstantReadNode ? nt_str(nt, r, "name") : NULL;
      /* Struct.new, Data.define, Class.new, Module.new run the body they take */
      if (rn && is_class_builder(rn, nm) && !w->hooks && !w->reopened) {
        if (blk < 0) return 1;
        if (nt_kind(nt, blk) != NK_BlockNode) return 0;
        cw_stmts(w, nt_ref(nt, blk, "body"), depth + 1);
        return w->clean ? 1 : 0;
      }
      if (blk >= 0) return 0;
      if (rn && is_value_constructor(rn, nm) && ar == 2 && !w->reopened) return 1;
      if (is_freeze_family(nm) && an == 0) return rv;
      if (rv == 2 && ar == 2 && !w->reopened &&
          (is_arith_op(nm) || is_cmp_op(nm) || is_eq_op(nm) || is_bit_op(nm) || is_shift_op(nm)))
        return 2;
      /* +"lit" */
      if (rv == 2 && an == 0 && !w->reopened && is_unary_sign(nm))
        return 2;
      return 0;
    }
    default:
      return 0;
  }
}

/* One statement, run in program order. A constant assignment is settled when
   nothing that ran before it can have run the program's code, and so is one
   of the builtin library's (the statements of builtins/ and the packages
   run the library's own code, which never reads the program's constants). */
static void cw_stmt_walk(ConstWalk *w, int id, int depth) {
  const NodeTable *nt = w->nt;
  if (id < 0 || depth > 100) return;
  switch (nt_kind(nt, id)) {
    case NK_StatementsNode:
      cw_stmts(w, id, depth + 1);
      return;
    case NK_ClassNode: case NK_ModuleNode: case NK_SingletonClassNode: {
      /* creating the class runs `inherited` (and const_added) when defined */
      int sup = nt_kind(nt, id) == NK_ClassNode ? nt_ref(nt, id, "superclass") : -1;
      if (w->hooks || (sup >= 0 && !cw_value(w, sup, depth + 1))) w->clean = 0;
      cw_stmts(w, nt_ref(nt, id, "body"), depth + 1);
      return;
    }
    case NK_ConstantWriteNode: {
      int bi = nt_int(nt, id, "node_bi", 0) != 0;
      int was = w->clean || bi;
      int r = cw_value(w, nt_ref(nt, id, "value"), depth + 1);
      if (!r) w->clean = 0;
      const char *nm = nt_str(nt, id, "name");
      LocalVar *lv = nm ? comp_const(w->c, nm) : NULL;
      if (was && (r || bi) && lv) {
        int i = cw_index(w, lv);
        w->settled[id] = 1;
        w->seen[i] = 1;
        w->plain[i] = r == 2;
      }
      return;
    }
    case NK_MultiWriteNode: {
      /* A, B = v: the constant targets are assigned when the value is done */
      static const char *const sides[] = { "lefts", "rights", NULL };
      int bi = nt_int(nt, id, "node_bi", 0) != 0;
      int was = w->clean || bi;
      int r = cw_value(w, nt_ref(nt, id, "value"), depth + 1);
      if (!r) w->clean = 0;
      for (int k = 0; sides[k]; k++) {
        int n = 0; const int *ts = nt_arr(nt, id, sides[k], &n);
        for (int i = 0; i < n; i++) {
          if (nt_kind(nt, ts[i]) != NK_ConstantTargetNode) continue;
          const char *nm = nt_str(nt, ts[i], "name");
          LocalVar *lv = nm ? comp_const(w->c, nm) : NULL;
          if (was && (r || bi) && lv) { w->settled[ts[i]] = 1; w->seen[cw_index(w, lv)] = 1; }
        }
      }
      return;
    }
    case NK_ConstantPathWriteNode: {
      /* Mod::K = v is the assignment of K as a plain one is */
      int bi = nt_int(nt, id, "node_bi", 0) != 0;
      int was = w->clean || bi;
      int r = cw_value(w, nt_ref(nt, id, "value"), depth + 1);
      int tgt = nt_ref(nt, id, "target");
      const char *nm = tgt >= 0 ? nt_str(nt, tgt, "name") : NULL;
      LocalVar *lv = nm ? comp_const(w->c, nm) : NULL;
      if (was && (r || bi) && lv) { w->settled[id] = 1; w->seen[cw_index(w, lv)] = 1; }
      return;
    }
    case NK_ConstantOrWriteNode: case NK_ConstantAndWriteNode:
    case NK_ConstantOperatorWriteNode: case NK_ConstantPathOrWriteNode:
    case NK_ConstantPathAndWriteNode: case NK_ConstantPathOperatorWriteNode:
      if (!cw_value(w, nt_ref(nt, id, "value"), depth + 1)) w->clean = 0;
      return;
    default:
      if (!cw_value(w, id, depth + 1)) w->clean = 0;
      return;
  }
}

static void cw_stmt(ConstWalk *w, int id, int depth) {
  int bi = id >= 0 && nt_int(w->nt, id, "node_bi", 0) != 0, clean = w->clean;
  cw_stmt_walk(w, id, depth);
  if (bi) w->clean = clean;
}
static void cw_top_stmt(void *w, int id) { cw_stmt(w, id, 0); }

/* The constant a node reads by name: a bare or path constant, or the name
   const_get and its kin are given. */
static const char *cw_read_name(const NodeTable *nt, int id) {
  switch (nt_kind(nt, id)) {
    case NK_ConstantReadNode: case NK_ConstantPathNode:
      return nt_str(nt, id, "name");
    case NK_CallNode: {
      const char *cn = nt_str(nt, id, "name");
      int a = nt_ref(nt, id, "arguments"), an = 0;
      const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
      if (!cn || !is_const_query_name(cn) || an < 1) return NULL;
      NodeKind ak = nt_kind(nt, av[0]);
      return ak == NK_SymbolNode ? nt_str(nt, av[0], "value") : ak == NK_StringNode ? nt_str(nt, av[0], "content") : NULL;
    }
    default:
      return NULL;
  }
}

/* Whether node id belongs to a placed file and is outside the compiler's
   constant-table methods. Reads in those tables enumerate the body's
   constants; they are not reads the program executes. Deferred bodies
   belong to the point of the statement that defines or makes them. */
static int cw_placed(ConstWalk *w, int id) {
  for (int a = id; a >= 0; a = w->par[a])
    if (nt_kind(w->nt, a) == NK_DefNode && is_const_table_def_name(nt_str(w->nt, a, "name"))) return 0;
  return elab_point_from(w->c, id).region >= 0;
}

/* The class or module a flagged read is written in, as an error names it:
   an index + 1 into w->crefs, 0 at the top level. A Class.new block's body
   is none (the code in it is written where the call is), and neither is a
   `class << x` of anything but self or outside any class. */
static int cw_cref_at(ConstWalk *w, int id) {
  const NodeTable *nt = w->nt;
  for (int a = w->par[id]; a >= 0; a = w->par[a]) {
    NodeKind k = nt_kind(nt, a);
    if (k != NK_ClassNode && k != NK_ModuleNode && k != NK_SingletonClassNode) continue;
    char buf[512];
    if (k == NK_SingletonClassNode) {
      int ex = nt_ref(nt, a, "expression"), out = ex >= 0 && nt_kind(nt, ex) == NK_SelfNode ? cw_cref_at(w, a) : 0;
      if (!out) continue;
      snprintf(buf, sizeof buf, "#<Class:%s>", w->crefs[out - 1]);
    }
    else {
      if (nt_int(nt, a, "class_new_anonymous", 0)) continue;
      int cp = nt_ref(nt, a, "constant_path");
      const char *nm = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
      int ci = elab_header_class(w->c, a, NULL, NULL);
      const char *q = ci >= 0 ? class_ruby_name(w->c, ci) : nm;
      if (!q) continue;
      snprintf(buf, sizeof buf, "%s", q);
    }
    for (int i = 0; i < w->ncrefs; i++) if (sp_streq(w->crefs[i], buf)) return i + 1;
    if (w->ncrefs == w->ccrefs) {
      w->ccrefs = w->ccrefs ? w->ccrefs * 2 : 16;
      w->crefs = realloc(w->crefs, sizeof(char *) * (size_t)w->ccrefs);
      if (!w->crefs) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    w->crefs[w->ncrefs] = strdup(buf);
    if (!w->crefs[w->ncrefs]) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    return ++w->ncrefs;
  }
  return 0;
}

/* Is node id the target of a path assignment (`Mod::K = v`)? It is no read
   of the constant; its receiver is one. */
static int cw_path_target(const ConstWalk *w, int id) {
  int p = w->par[id];
  NodeKind k = p >= 0 ? nt_kind(w->nt, p) : NK_NilNode;
  return (k == NK_ConstantPathWriteNode || k == NK_ConstantPathOrWriteNode || k == NK_ConstantPathAndWriteNode ||
          k == NK_ConstantPathOperatorWriteNode) && nt_ref(w->nt, p, "target") == id;
}

static int const_class_presence(Compiler *c, int id);

/* Mark the constants some assignment can follow a read of: LocalVar.const_early
   (see the header comment). */
void an_const_presence(Compiler *c) {
  const NodeTable *nt = c->nt;
  ConstWalk w = { .c = c, .nt = nt, .clean = 1, .par = comp_parent_map(c) };
  size_t nn = (size_t)(nt->count > 0 ? nt->count : 1), nc = (size_t)(c->nconsts > 0 ? c->nconsts : 1);
  w.settled = calloc(nn, 1);
  w.seen = calloc(nc, 1);
  w.plain = calloc(nc, 1);
  char *listed_only = calloc(nc, 1);      /* per constant: early only to a `constants` listing */
  if (!w.settled || !w.seen || !w.plain ||
      !listed_only) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  for (int id = 0; id < nt->count; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k == NK_DefNode) {
      const char *dn = nt_str(nt, id, "name");
      if (dn && is_definition_hook_name(dn)) w.hooks = 1;
      if (dn && is_const_missing_name(dn)) w.cmissing = 1;
    }
    else if (k == NK_ClassNode || k == NK_ModuleNode) {
      int cp = nt_ref(nt, id, "constant_path");
      const char *cn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
      if (cn && (is_builtin_class_name(cn) || is_builtin_reopen_name(cn))) w.reopened = 1;
    }
  }
  prog_top_stmts(c, cw_top_stmt, &w);   /* the files' top levels, in load order */
  char *first_settled = calloc(nc, 1), *early_reads = calloc(nc, 1);
  if (!first_settled || !early_reads) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < c->nconsts; i++) {
    int early, first = elab_const_first_write(c, c->consts[i].name, &early);
    first_settled[i] = first >= 0 && w.settled[first];
    if (early) c->consts[i].const_early = 1;
  }
  /* Every read and listing asks at its statement's point, including the
     defining statement of a body that may run later. */
  for (int id = 0; id < nt->count; id++) {
    const char *rn = cw_read_name(nt, id), *cn = nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "name") : NULL;
    LocalVar *lv = rn && !cw_path_target(&w, id) ? comp_const(c, rn) : NULL;
    int listed = cn && is_constants_list_name(cn), computed = cn && !rn && is_const_query_name(cn) && nt_ref(nt, id, "arguments") >= 0;
    if ((!lv && !listed && !computed) || !cw_placed(&w, id)) continue;
    for (int i = lv ? cw_index(&w, lv) : 0, end = lv ? i + 1 : c->nconsts; i < end; i++)
      if (!first_settled[i] && elab_const_slot_present_at(c, c->consts[i].name, elab_point_from(c, id)) != ELAB_YES)
        early_reads[i] |= listed ? 2 : computed ? 4 : 1;
  }
  for (int i = 0; i < c->nconsts; i++) {
    if (c->consts[i].const_early || !early_reads[i]) continue;
    c->consts[i].const_early = 1;
    listed_only[i] = early_reads[i] == 2;
  }
  /* A name that is also a class's (an alias `Key = Impl::Key`) is one slot of
     the flat table for the value and the class, and a read of the class must
     not wait for the value. A program that defines const_missing keeps
     these reads unguarded: spinel does not call it, so a flag would raise
     the wrong NameError. A constant no read can reach before its
     assignment, only a `constants` listing, keeps its flag there: the
     listing leaves it out until it is assigned, as CRuby's does, and no
     read tests it early. */
  for (int i = 0; i < c->nconsts; i++)
    if (c->consts[i].const_early &&
        ((w.cmissing && !listed_only[i]) || comp_class_index(c, c->consts[i].name) >= 0))
      c->consts[i].const_early = 0;
  /* the name a flagged read shows ahead of the constant: "" at the top
     level; all found before any is set, which edits the tree the points
     and the parent map are of */
  int *cref = calloc(nn, sizeof(int));   /* per node: 1 + its cw_cref_at, 0 for no flagged read */
  if (!cref) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int id = 0; id < nt->count; id++) {
    LocalVar *lv = nt_kind(nt, id) == NK_ConstantReadNode ? comp_const(c, nt_str(nt, id, "name")) : NULL;
    if (lv && lv->const_early && cw_placed(&w, id)) cref[id] = 1 + cw_cref_at(&w, id);
  }
  for (int id = 0; id < nt->count; id++)
    if (cref[id]) nt_node_set_str((NodeTable *)nt, id, "const_cref", cref[id] > 1 ? w.crefs[cref[id] - 2] : "");
  /* Compute consumption once, while the constant pass visits reads. A
     syntactic read may become reachable later in inference. */
  for (int ci = 0; ci < c->nclasses; ci++) c->classes[ci].presence_read = 0;
  NodeKind kinds[] = { NK_ConstantReadNode, NK_ConstantPathNode };
  for (int k = 0; k < 2; k++) NT_FOREACH_KIND(nt, kinds[k], id) {
    int parent = w.par[id];
    NodeKind pk = nt_kind(nt, parent);
    if (cw_path_target(&w, id) || ((pk == NK_ClassNode || pk == NK_ModuleNode) && nt_ref(nt, parent, "constant_path") == id)) continue;
    int ci = const_class_presence(c, id);
    if (ci) c->classes[ci - 1].presence_read = 1;
  }
  free(cref);
  for (int i = 0; i < w.ncrefs; i++) free(w.crefs[i]);
  free(w.crefs);
  free(w.settled);
  free(w.seen); free(w.plain); free(listed_only);
  free(first_settled); free(early_reads);
}

int const_has_flag(const LocalVar *lv) {
  return lv && lv->const_early && (lv->type == TY_NIL || is_scalar_ret(lv->type));
}
/* A header in an unplaced file has not made its class present merely by
   being registered in the compiler's class table. */
int an_class_needs_presence(Compiler *c, int ci) {
  if (ci < 0 || ci >= c->nclasses || !prog_has_files(c)) return 0;
  /* A builtin already exists; a later header only reopens it. */
  if (c->classes[ci].is_builtin_const || builtin_class_id(c->classes[ci].name) ||
      is_builtin_exception_name(c->classes[ci].name)) return 0;
  int node = c->classes[ci].def_node;
  return node >= 0 && elab_point_from(c, node).region > 0;
}
/* The lexical class and its enclosing classes already exist when their
   bodies or methods run. The scope walk owns both of these facts. */
static int const_class_presence(Compiler *c, int id) {
  const char *nm = nt_str(c->nt, id, "name");
  int ci = nm ? comp_class_index(c, nm) : -1;
  if (!an_class_needs_presence(c, ci)) return 0;
  Scope *scope = comp_scope_of(c, id);
  int own = scope && scope->class_id >= 0 ? scope->class_id : id < c->node_cap ? c->node_cbody[id] : -1;
  for (; own >= 0; own = c->classes[own].enclosing_class)
    if (own == ci) return 0;
  return ci + 1;
}

/* The creation that sets the flag is a header or a class-valued constant
   assignment. Codegen renders the same store after either definition. */
int an_class_presence_write(Compiler *c, int id) {
  NodeKind k = nt_kind(c->nt, id);
  int ci = -1;
  if (k == NK_ClassNode || k == NK_ModuleNode) ci = elab_header_class(c, id, NULL, NULL);
  else if (k == NK_ConstantWriteNode || k == NK_ConstantPathWriteNode) {
    int target = k == NK_ConstantWriteNode ? id : nt_ref(c->nt, id, "target");
    const char *nm = nt_str(c->nt, target, "name");
    ci = nm ? comp_class_index(c, nm) : -1;
    if (ci >= 0 && c->classes[ci].def_node != id) ci = -1;
  }
  return an_class_presence_is_read(c, ci) ? ci + 1 : 0;
}

/* The slot whose presence evaluating this constant node observes. Path
   parents ask separately, before their child's value is used. */
ConstReadEffect an_const_read_effect(Compiler *c, int id) {
  NodeKind k = nt_kind(c->nt, id);
  if (!nt_kind_is_constant(k)) return (ConstReadEffect){0};
  const char *nm = nt_str(c->nt, id, "name");
  LocalVar *lv = nm ? comp_const(c, nm) : NULL;
  return (ConstReadEffect){ const_has_flag(lv) && (k != NK_ConstantPathNode || (!const_path_in_closed_builtin(c, id) && !const_path_misses_toplevel(c, id))) ? lv : NULL,
                           elab_autoload_read(c, id), const_class_presence(c, id) };
}

/* A definition-only file still runs a header's presence store when a
   constant read in this program consumes it. */
int an_class_presence_is_read(const Compiler *c, int ci) {
  return ci >= 0 && ci < c->nclasses && c->classes[ci].presence_read;
}

/* A constant whose class value a consumer may fold away must retain the
   read. Inference has already resolved builtin values and class aliases. */
int an_const_read_keeps_class(Compiler *c, int id) {
  return nt_kind_is_constant(nt_kind(c->nt, id)) && comp_ntype(c, id) == TY_CLASS;
}

int an_const_read_is_value(Compiler *c, int id) {
  NodeKind k = nt_kind(c->nt, id);
  const char *nm = nt_str(c->nt, id, "name");
  return (nt_kind_is_constant(k)) && nm &&
         comp_class_index(c, nm) < 0 && !is_builtin_class_name(nm) && comp_const(c, nm);
}
