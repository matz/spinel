/* analyze_program.c -- the program's files and their load order.

   Under --load-model=graph (#9898, CRuby's load model) each file the program
   loads is a root of the node table (nt_root; the entry's is the table's
   root) and each require a LoadNode naming the file it loads
   (`target_file`). A file's top level is a body of its own, run when the
   file is first required, and its locals are its own; `self` there is main,
   one object for the whole program. This module owns the files:

     - one top-level scope per file, shaped as main's (no name, no def, no
       class: the Toplevel), whose walk registers the file's classes, methods
       and locals (prog_file_scopes, prog_walk_files);
     - which scopes are a file's top level (prog_is_top_scope) and which
       nodes a file's root or its statements (prog_is_root*);
     - the program's top-level statements in load order (prog_top_stmts):
       a load at the top level runs its file's top level there, once; and
       with them the files no load there places (prog_visit_files);
     - what the program refuses for now: a require that is not a top-level
       statement, one that resolves to no file at compile time, an autoload
       (prog_refuse_loads), and the statements the readers of the top level
       in order would have to place across a load (prog_refuse_order). */

#include "analyze_internal.h"
#include "builtin_names.h"
#include <limits.h>

static int prog_graph(const Compiler *c) { return c->nt->nfile_roots > 0; }
/* Is the program its files (--load-model=graph), each a root? */
int prog_has_files(const Compiler *c) { return prog_graph(c); }

int prog_root_stmts(const Compiler *c, int i) {
  int r = nt_root(c->nt, i);
  return r >= 0 ? nt_ref(c->nt, r, "statements") : -1;
}

/* Is node id a file's root (its ProgramNode)? */
int prog_is_root(const Compiler *c, int id) {
  if (id < 0 || nt_kind(c->nt, id) != NK_ProgramNode) return 0;
  for (int i = 0; i < nt_nroots(c->nt); i++) if (nt_root(c->nt, i) == id) return 1;
  return 0;
}

/* Is node st a file's top-level statement list? */
int prog_is_root_stmts(const Compiler *c, int st) {
  if (st < 0 || nt_kind(c->nt, st) != NK_StatementsNode) return 0;
  for (int i = 0; i < nt_nroots(c->nt); i++) if (prog_root_stmts(c, i) == st) return 1;
  return 0;
}

/* Is `s` a file's top level: main's (scope 0) or a required file's? The
   required files' scopes are made one after another (prog_file_scopes). */
int prog_is_top_scope(const Compiler *c, const Scope *s) {
  int i = (int)(s - c->scopes), n = nt_nroots(c->nt);
  return i == 0 || (prog_graph(c) && n > 1 && i >= c->file_scope[1] && i <= c->file_scope[n - 1]);
}

/* A required file's top-level scope, after main's (scope 0). */
void prog_file_scopes(Compiler *c) {
  int n = nt_nroots(c->nt);
  c->file_scope = calloc((size_t)n, sizeof(int));
  if (!c->file_scope) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 1; prog_graph(c) && i < n; i++) {
    Scope *s = comp_scope_new(c, NULL, -1);
    { s->body = prog_root_stmts(c, i); comp_touch_scope(s->idx); }
    c->file_scope[i] = c->nscopes - 1;
  }
}

/* The walk of the files' top levels, each in its own scope, as main's is
   walked in scope 0 (walk_scope). Under --load-model=graph it goes in load
   order (prog_visit_files), so what a later statement defines is registered
   after what an earlier one does, whichever file holds each: an alias of a
   method a required file defines, a reopening's methods. A LoadNode names
   its file and holds no node of it, so no walk enters another file. */
typedef struct { Compiler *c; unsigned char *claimed; } ProgWalk;
static void prog_walk_node(void *ud, int node, int f) {
  ProgWalk *w = ud;
  Compiler *c = w->c;
  int r = nt_root(c->nt, f);
  if (node != r && !w->claimed[f]) {   /* the file's root and statement list, as a walk of the root claims them */
    int st = prog_root_stmts(c, f);
    c->nscope[r] = c->file_scope[f]; c->node_cbody[r] = -1;
    if (st >= 0) { c->nscope[st] = c->file_scope[f]; c->node_cbody[st] = -1; }
  }
  w->claimed[f] = 1;
  walk_scope(c, node, c->file_scope[f], -1);
}
void prog_walk_files(Compiler *c) {
  const NodeTable *nt = c->nt;
  if (!prog_graph(c)) { walk_scope(c, nt->root_id, 0, -1); return; }
  ProgWalk w = { c, calloc((size_t)nt_nroots(nt), 1) };
  if (!w.claimed) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  prog_visit_files(c, prog_walk_node, &w);
  free(w.claimed);
}

/* The LoadNode that first runs file f's top level, in the load order the
   parse's load graph decides (#10305): the `load_by` its root carries. -1
   for the entry and for a file the parse does not place (one another
   require may run first: under a condition, in a method, block or class
   body, in a value, or any file once a require's path is computed at run
   time). */
static int prog_load_by(const Compiler *c, int f) {
  const NodeTable *nt = c->nt;
  int r = f > 0 && f < nt->nfile_roots ? nt_root(nt, f) : -1;
  return r >= 0 ? (int)nt_int(nt, r, "load_by", -1) : -1;
}

/* The file a load at the top level runs: the target of the LoadNode that
   first runs it (prog_load_by), else -1. Its file's statements come next in
   load order. */
int prog_ordered_load(const Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (id < 0 || nt_kind(nt, id) != NK_LoadNode) return -1;
  int f = (int)nt_int(nt, id, "target_file", -1);
  return f > 0 && prog_load_by(c, f) == id ? f : -1;
}

/* The program's top-level statements in load order: the entry's, and after
   the statement that first loads a file at the top level (prog_ordered_load)
   that file's. A file loaded only elsewhere -- under a condition, in a
   method, block or class body -- has no place in this order. Each statement
   comes with the file whose top level holds it. */
typedef void (*ProgStmtFn)(void *ud, int stmt, int file);
static void prog_top_walk(const Compiler *c, int f, ProgStmtFn fn, void *ud) {
  int st = prog_root_stmts(c, f), n = 0;
  const int *v = st >= 0 ? nt_arr(c->nt, st, "body", &n) : NULL;
  for (int k = 0; k < n; k++) {
    fn(ud, v[k], f);
    int t = prog_ordered_load(c, v[k]);
    if (t >= 0) prog_top_walk(c, t, fn, ud);
  }
}
static void prog_top_from(const Compiler *c, ProgStmtFn fn, void *ud) {
  prog_top_walk(c, 0, fn, ud);
}
typedef struct { void (*fn)(void *, int); void *ud; } ProgStmtOnly;
static void prog_stmt_only(void *ud, int stmt, int file) {
  (void)file;
  ProgStmtOnly *o = ud;
  o->fn(o->ud, stmt);
}
void prog_top_stmts(const Compiler *c, void (*fn)(void *ud, int stmt), void *ud) {
  ProgStmtOnly o = { fn, ud };
  prog_top_from(c, prog_stmt_only, &o);
}
/* ...as an array: the statements in load order, and in *file (when asked)
   the file each is a statement of; *n their number. The caller frees both. */
typedef struct { int *stmt, *file, n, cap; } ProgSeq;
static void prog_seq_add(void *ud, int stmt, int file) {
  ProgSeq *q = ud;
  if (q->n == q->cap) {
    q->cap = q->cap ? q->cap * 2 : 16;
    q->stmt = realloc(q->stmt, sizeof(int) * (size_t)q->cap);
    q->file = realloc(q->file, sizeof(int) * (size_t)q->cap);
    if (!q->stmt || !q->file) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  q->stmt[q->n] = stmt; q->file[q->n++] = file;
}
int *prog_top_seq(const Compiler *c, int *n, int **file) {
  ProgSeq q = { NULL, NULL, 0, 0 };
  prog_top_from(c, prog_seq_add, &q);
  *n = q.n;
  if (file) *file = q.file;
  else free(q.file);
  return q.stmt;
}

/* The program's files in load order, for a pass that reads what an
   earlier statement defined: fn(ud, stmt, file) for each top-level
   statement as prog_top_seq orders them, then fn(ud, root, file) for each
   file none of them is in (one no load at the top level places, or the
   entry when it has no statement). */
void prog_visit_files(const Compiler *c, void (*fn)(void *ud, int node, int file), void *ud) {
  int n = nt_nroots(c->nt), sn = 0, *file = NULL;
  int *seq = prog_top_seq(c, &sn, &file);
  unsigned char *seen = calloc((size_t)(n > 0 ? n : 1), 1);
  if (!seen) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int k = 0; k < sn; k++) { seen[file[k]] = 1; fn(ud, seq[k], file[k]); }
  for (int f = 0; f < n; f++)
    if (!seen[f] && nt_root(c->nt, f) >= 0) fn(ud, nt_root(c->nt, f), f);
  free(seen); free(seq); free(file);
}

/* ---- the program's order ----
   One order, the parse's (#10305): node ids are a file's own order (flatten
   numbers a file's nodes as it walks its tree), and between files the load
   graph's, which each placed file's root carries (prog_load_by): a file's
   top level runs at the top-level statement that first loads it, after that
   statement and before the file's next one. The load-phase record
   (analyze_elab.c) ranks the top-level statements in the order they run,
   from the entry, each file's root ranked at its entry, ahead of its
   statements; a node takes the rank of its file's top-level statement
   holding it, and node ids order only the nodes of one statement. A file
   the parse does not place (another require may run it first: one under a
   condition, in a method or a block, or any file once a require's path is
   computed at run time) runs at a time the text does not fix: its nodes
   have no rank, and against another file no order. A node under no file's
   root (one a desugaring made and hung nowhere) keeps the id order it had.
   The record's own points also order the statements inside one top-level
   statement (class bodies, the blocks run at once); prog_order does not
   read them yet, and SPINEL_ELAB_CHECK=1 reports where they would answer
   otherwise (elab_check_order). */
static int prog_order_now(Compiler *c, int a, int b) {
  if (a == b) return PROG_SAME;
  if (!prog_graph(c) || a < 0 || b < 0) return a < b ? PROG_BEFORE : PROG_AFTER;
  int fa, fb, ra, rb;
  elab_top_place(c, a, &fa, &ra);
  elab_top_place(c, b, &fb, &rb);
  if (fa < 0 || fb < 0) return a < b ? PROG_BEFORE : PROG_AFTER;
  /* two top-level statements by their ranks, in one file too: a statement
     the parse or a desugaring put in a file's list (a boot library's load
     ahead of the entry's statements) has ids past the file's own */
  if (ra >= 0 && rb >= 0 && ra != rb) return ra < rb ? PROG_BEFORE : PROG_AFTER;
  if (fa == fb) return a < b ? PROG_BEFORE : PROG_AFTER;   /* one statement's nodes, as flatten numbered them */
  return PROG_UNKNOWN;
}
static int prog_order_for(Compiler *c, int a, int b, const char *reader) {
  int o = prog_order_now(c, a, b);
  if (elab_check_on(c)) elab_check_order(c, reader, a, b, o);
  return o;
}
int prog_order(Compiler *c, int a, int b) { return prog_order_for(c, a, b, "prog_order"); }

/* v[0..n) sorted by key[], stably (a merge sort): key[i] is v[i]'s */
static void prog_sort_keyed(int *v, long long *key, int n) {
  long long *tkey = malloc(sizeof(long long) * (size_t)n);
  int *tmp = malloc(sizeof(int) * (size_t)n);
  if (!tkey || !tmp) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int w = 1; w < n; w *= 2) {
    for (int lo = 0; lo < n; lo += 2 * w) {
      int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        if (key[j] < key[i]) { tkey[k] = key[j]; tmp[k++] = v[j++]; }
        else { tkey[k] = key[i]; tmp[k++] = v[i++]; }
      }
      while (i < mid) { tkey[k] = key[i]; tmp[k++] = v[i++]; }
      while (j < hi) { tkey[k] = key[j]; tmp[k++] = v[j++]; }
    }
    memcpy(v, tmp, sizeof(int) * (size_t)n);
    memcpy(key, tkey, sizeof(long long) * (size_t)n);
  }
  free(tkey); free(tmp);
}

/* Sort v[0..n) into the program's order of the nodes they stand for:
   node_of[v[i]] when node_of is given, else v[i]. By the rank of the
   top-level statement holding each node, and the nodes of one statement by
   id, as prog_order answers for two of them; a node that runs at no known
   point (one under no root, which a desugaring hung nowhere, or one of a
   file the load graph does not place) after all the others, by id, so the
   order is a total one even where prog_order answers by ids or UNKNOWN.
   Under the inline model, the node ids. */
void prog_sort(Compiler *c, int *v, int n, const int *node_of) {
  if (n < 2) return;
  long long *key = malloc(sizeof(long long) * (size_t)n);
  int *was = elab_check_on(c) ? malloc(sizeof(int) * (size_t)n) : NULL;
  if (!key || (elab_check_on(c) && !was)) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  if (was) memcpy(was, v, sizeof(int) * (size_t)n);
  for (int i = 0; i < n; i++) {
    int id = node_of ? node_of[v[i]] : v[i], f = -1, r = 0;
    if (prog_graph(c)) { elab_top_place(c, id, &f, &r); if (f < 0) r = -1; }
    key[i] = ((long long)(r >= 0 ? r : INT_MAX) << 32) | (unsigned)id;
  }
  prog_sort_keyed(v, key, n);
  if (was) {   /* SPINEL_ELAB_CHECK: the record's order of the same nodes */
    for (int i = 0; i < n; i++) key[i] = elab_sort_key(c, node_of ? node_of[was[i]] : was[i]);
    prog_sort_keyed(was, key, n);
    elab_check_sort(c, v, was, n, node_of);
    free(was);
  }
  free(key);
}

/* prog_order for a reader with no answer of its own where the order is not
   known: -1 a before b, 1 after, 0 the same node; an UNKNOWN is refused at
   node `at`, naming the reader (a file loaded at no fixed point gets one in
   part 3c). */
int prog_cmp(Compiler *c, int a, int b, int at, const char *reader) {
  int o = prog_order_for(c, a, b, reader);
  if (o == PROG_UNKNOWN) {
    char msg[256];
    snprintf(msg, sizeof msg, "--load-model=graph: %s reads the order of two files, one of them loaded at no fixed point, for now", reader);
    unsupported_feature(c, at, msg);
  }
  return o;
}

/* Does root i only define things -- methods, aliases, and classes or modules
   whose bodies do no more -- so that loading it runs nothing? Asked of the
   program as it stands: codegen asks it of the desugared tree. */
static int prog_defines_only_stmts(const NodeTable *nt, int st) {
  if (st < 0) return 1;
  if (nt_kind(nt, st) != NK_StatementsNode) return 0;
  int n = 0; const int *v = nt_arr(nt, st, "body", &n);
  for (int i = 0; i < n; i++) {
    NodeKind k = nt_kind(nt, v[i]);
    /* (a NilNode is what a desugaring leaves of a statement it took out:
       builtins/ files' modules, desugar_builtins) */
    if (k == NK_DefNode || k == NK_AliasMethodNode || k == NK_NilNode) continue;
    if ((k == NK_ClassNode || k == NK_ModuleNode) && nt_ref(nt, v[i], "superclass") < 0 &&
        prog_defines_only_stmts(nt, nt_ref(nt, v[i], "body"))) continue;
    return 0;
  }
  return 1;
}
int prog_defines_only(const Compiler *c, int i) {
  return prog_defines_only_stmts(c->nt, prog_root_stmts(c, i));
}

/* The root whose tree holds node id (a file's ProgramNode), or -1. */
static int prog_root_of(const Compiler *c, const int *parent, int id) {
  while (id >= 0 && parent[id] >= 0) id = parent[id];
  return prog_is_root(c, id) ? id : -1;
}

/* What --load-model=graph does not load yet, refused by name (#9898), asked
   once the load sites are known, ahead of the desugaring:
     - a require that is not a statement of a file's top level: its file
       loads when that code runs (part 3c);
     - a require of the program's own entry file, which CRuby runs again as
       a new file since `$0` is not a loaded feature;
     - one whose path is computed when it runs: its file is chosen among
       the candidates then (part 4);
     - an autoload: its file loads at the constant's first reference
       (part 4);
     - in a required file, an END block, registered when its statement
       runs; `class Class`, which desugar_class_reopen reads at the entry's
       top level only; and a `return` in a block at its top level, which
       ends the file's load from wherever the block runs. */
void prog_refuse_loads(Compiler *c) {
  const NodeTable *nt = c->nt;
  if (!prog_graph(c)) return;
  int *parent = an_parent_map(nt);
  if (!parent) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int id = 0; id < nt->count; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k == NK_CallNode) {
      const char *cn = nt_str(nt, id, "name");
      if (cn && is_autoload_name(cn) && nt_ref(nt, id, "receiver") < 0)
        unsupported_feature(c, id, "--load-model=graph loads an autoload's file at its constant's first reference only later, for now");
    }
    else if (k == NK_LoadNode) {
      /* (one that names no file runs no file's top level: it raises
         LoadError wherever it stands) */
      const char *how = nt_str(nt, id, "how");
      if (!nt_int(nt, id, "boot", 0) && !is_load_how_missing(how) && !prog_is_root_stmts(c, parent[id]))
        unsupported_feature(c, id, "--load-model=graph loads only a require that is a statement of a file's top level, for now");
      if (nt_int(nt, id, "target_file", -1) == 0)
        unsupported_feature(c, id, "--load-model=graph does not load the program's own entry file again, for now");
      if (is_load_how_computed(how))
        unsupported_feature(c, id, "--load-model=graph loads only a require whose path is known at compile time, for now");
    }
    else if (k == NK_PostExecutionNode) {
      int r = prog_root_of(c, parent, id);
      if (r >= 0 && r != nt->root_id)
        unsupported_feature(c, id, "--load-model=graph: an END block in a required file, for now");
    }
    else if (k == NK_ReturnNode) {
      /* the nearest method, lambda or block around it: a block's, at the top level */
      int a = parent[id];
      while (a >= 0 && nt_kind(nt, a) != NK_DefNode && nt_kind(nt, a) != NK_LambdaNode && nt_kind(nt, a) != NK_BlockNode)
        a = parent[a];
      int r = a >= 0 && nt_kind(nt, a) == NK_BlockNode ? prog_root_of(c, parent, id) : -1;
      for (int b = a; r >= 0 && b >= 0; b = parent[b])
        if (nt_kind(nt, b) == NK_DefNode || nt_kind(nt, b) == NK_LambdaNode) r = -1;
      if (r >= 0 && r != nt->root_id)
        unsupported_feature(c, id, "--load-model=graph: a `return` in a block at a required file's top level, for now");
    }
  }
  for (int i = 1; i < nt_nroots(nt); i++) {
    int st = prog_root_stmts(c, i), n = 0;
    const int *v = st >= 0 ? nt_arr(nt, st, "body", &n) : NULL;
    for (int k = 0; k < n; k++)
      if ((nt_kind(nt, v[k]) == NK_ClassNode || nt_kind(nt, v[k]) == NK_ModuleNode) &&
          is_class_const(nt, nt_ref(nt, v[k], "constant_path")))
        unsupported_feature(c, v[k], "--load-model=graph: `class Class` in a required file (desugar_class_reopen reads one file), for now");
  }
  free(parent);
}

/* A definition at a file's top level, by name, for the definitions two files
   both make (prog_refuse_order) */
typedef struct { const char *cls, *name; int root, node; } ProgDef;
static int prog_def_cmp(const void *a, const void *b) {
  const ProgDef *x = a, *y = b;
  int d = strcmp(x->cls, y->cls);
  return d ? d : strcmp(x->name, y->name);
}
static void prog_def_add(ProgDef **v, int *n, int *cap, const char *cls, const char *name, int root, int node) {
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 32;
    *v = realloc(*v, sizeof **v * (size_t)*cap);
    if (!*v) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  (*v)[(*n)++] = (ProgDef){ cls, name, root, node };
}

/* The statements a reader of the top level in order would have to place
   across a load (part 3b), refused by name, asked just ahead of the
   redefinition readers, once the builtins/ files' modules are taken apart
   (desugar_builtins, desugar_builtin_scalar_defs). In a required file's top
   level:
     - a top-level `def self.m` (rename_main_singleton_defs);
     - `include` (qc_object_mixin_write, which runs ahead of the load
       record);
     - a write of main's instance variable (nf_written_before);
   and across files:
     - a top-level method defined in a required file and again anywhere
       (rename_redefined_toplevel_defs);
     - a method a class reopened at the top level of two files defines in
       both (version_redefined_reopen_defs). */
void prog_refuse_order(Compiler *c) {
  const NodeTable *nt = c->nt;
  if (!prog_graph(c)) return;
  int *parent = an_parent_map(nt);
  if (!parent) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  ProgDef *defs = NULL;
  int nd = 0, cd = 0;
  for (int i = 0; i < nt_nroots(nt); i++) {
    int st = prog_root_stmts(c, i), n = 0;
    const int *v = st >= 0 ? nt_arr(nt, st, "body", &n) : NULL;
    for (int k = 0; k < n; k++) {
      NodeKind sk = nt_kind(nt, v[k]);
      const char *nm = nt_str(nt, v[k], "name");
      if (i > 0 && sk == NK_DefNode && nt_ref(nt, v[k], "receiver") >= 0)
        unsupported_feature(c, v[k], "--load-model=graph: a top-level `def self.m` in a required file (rename_main_singleton_defs reads one file), for now");
      if (i > 0 && sk == NK_CallNode && nt_ref(nt, v[k], "receiver") < 0 && nm && is_include_name(nm))
        unsupported_feature(c, v[k], "--load-model=graph: a top-level include in a required file (qc_object_mixin_write reads one file), for now");
      if (sk == NK_DefNode && nt_ref(nt, v[k], "receiver") < 0 && nm)
        prog_def_add(&defs, &nd, &cd, "", nm, i, v[k]);
      int cp = sk == NK_ClassNode ? nt_ref(nt, v[k], "constant_path") : -1;
      const char *cn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
      int b = cn ? nt_ref(nt, v[k], "body") : -1, bn = 0;
      const int *bv = b >= 0 && nt_kind(nt, b) == NK_StatementsNode ? nt_arr(nt, b, "body", &bn) : NULL;
      for (int j = 0; j < bn; j++)
        if (nt_kind(nt, bv[j]) == NK_DefNode && nt_ref(nt, bv[j], "receiver") < 0 && nt_str(nt, bv[j], "name"))
          prog_def_add(&defs, &nd, &cd, cn, nt_str(nt, bv[j], "name"), i, bv[j]);
    }
  }
  /* the same name defined twice: a top-level method once in a required
     file, or a class's method in two files */
  if (nd > 1) qsort(defs, (size_t)nd, sizeof *defs, prog_def_cmp);
  for (int a = 0, b; a < nd; a = b) {
    int req = 0, files = 0;
    for (b = a; b < nd && !prog_def_cmp(&defs[a], &defs[b]); b++) {
      req |= defs[b].root > 0;
      files |= defs[b].root != defs[a].root;
    }
    int top = !*defs[a].cls;
    if (b - a < 2 || !req || (!top && !files)) continue;
    for (int q = a; q < b; q++)
      if (defs[q].root > 0)
        unsupported_feature(c, defs[q].node, top
          ? "--load-model=graph: a top-level method a required file defines again (rename_redefined_toplevel_defs reads one file), for now"
          : "--load-model=graph: a method a class reopened in another file defines again (version_redefined_reopen_defs reads one file), for now");
  }
  free(defs);
  for (int id = 0; id < nt->count; id++) {
    NodeKind k = nt_kind(nt, id);
    int ivar = k == NK_InstanceVariableWriteNode || k == NK_InstanceVariableOrWriteNode ||
               k == NK_InstanceVariableAndWriteNode || k == NK_InstanceVariableOperatorWriteNode;
    if (!ivar) continue;
    int r = prog_root_of(c, parent, id);
    if (r < 0 || r == nt->root_id) continue;
    /* main's: outside any method, class, module, block or lambda */
    int in_body = 0;
    for (int a = parent[id]; a >= 0 && !in_body; a = parent[a]) {
      NodeKind ak = nt_kind(nt, a);
      in_body = ak == NK_DefNode || ak == NK_ClassNode || ak == NK_ModuleNode || ak == NK_SingletonClassNode ||
                ak == NK_BlockNode || ak == NK_LambdaNode;
    }
    if (!in_body)
      unsupported_feature(c, id, "--load-model=graph: a write of main's instance variable in a required file (nf_written_before reads one file), for now");
  }
  free(parent);
}
