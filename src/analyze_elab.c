/* analyze_elab.c -- the load-phase elaborator's record (#9898).

   CRuby builds a program's classes by running its load phase: the files in
   load order, each file's statements in order, and class bodies as they
   run. The record says where in that phase each statement runs, so that a
   question of order ("was this defined before that call?") is read from it
   rather than from node ids, which are a file's own order only.

   A point is (region, seq). Region 0 is the load phase the load graph
   places (#10305): the entry, and each file at the statement that first
   loads it (prog_ordered_load). Each file it does not place (one loaded
   under a condition, in a method or a block, or by a computed path) has a
   region of its own after them, whose points order among themselves and
   against any other region are UNKNOWN. seq counts the statements in the
   order they run. The walk:
     - enters a file at its load: its root's point, its BEGIN blocks, then
       its statements;
     - enters the bodies that run where they stand: class, module and
       `class << x` bodies, BEGIN blocks, and the blocks the load phase runs
       at once (class_eval and the other eval and exec forms, the class
       builders, and the builtin iterators: each, times, loop, tap, ...);
     - enters the branches of a condition, a loop or a rescue, whose
       statements may not run: their points are `may`; so are those of a
       block given to any other call (a program method's), which may run
       before the call answers;
     - does not enter method bodies, lambdas and the blocks kept to run
       later (proc, define_method, Thread.new, at_exit, ...): elab_point_of
       answers no point there.
   Any other node takes the point of its nearest walked statement. The walk
   follows the load phase's control flow over statements, which is not a
   value-position question, so it is its own recursion over the few node
   kinds that hold statements (value_pos.h walks the nodes a value comes
   from, and never enters a block).

   The map is a function of the tree as it stands, rebuilt when the node
   table changes, as the parent map is: the readers ask the order ahead of
   the scope walk (the builtins/ per-call-site copies) and after it, of
   nodes later passes add. Under --load-model=inline the walk is the same
   over the one program text, so a statement a desugaring placed mid-body
   has its point there, whatever its id.

   prog_order and prog_sort read the map (analyze_program.c), by the rank of
   the top-level statement holding each node and node ids within one, which
   is the order they gave before the map. SPINEL_ELAB_CHECK=1 compares each
   of their answers with the full points' (elab_order) and reports where the
   two differ. */

#include "analyze_internal.h"
#include "builtin_names.h"
#include "builtin_ops.h"
#include "timing.h"
#include <limits.h>

int g_dump_load_record = 0;

/* One walked statement. top is the index of the top-level statement (or
   file root) holding it, and rank, on that one, its place in the order
   prog_order reads: each file's statements in their list order, a placed
   file's right after its load (a BEGIN block keeps its place in the list
   there; the walk runs it first). -1 outside region 0. */
typedef struct { int node, file, region, top, rank, depth, frame; unsigned char may; } LrStmt;

/* The order questions SPINEL_ELAB_CHECK counts, by reader */
typedef struct { const char *reader; long asked, differ; } LrCheck;
enum { LR_CHECK_MAX = 16, LR_CHECK_SHOW = 100 };

enum { LR_LATE = 1 };   /* runs later: in a method body, a lambda or a block kept to run later */

struct LoadRecord {
  LrStmt *st; int nst, cst;  /* the walked statements, in the order they run: seq is the index */
  int *at;                   /* [n] each node's statement; -1 under no file's root */
  unsigned char *bits;       /* [n] LR_* */
  int n, nregion;
  unsigned ver;              /* the node table the map is of */
  int check, shown;
  LrCheck fam[LR_CHECK_MAX];
  double ms; int builds;
  /* the histories (elab_program) */
  int recorded;
  struct LrHist *hist; int nhist, chist, *hhead;
  struct LrClass *cls; int ncls;
};

static LoadRecord *lr_get(Compiler *c) {
  if (!c->lrec) {
    c->lrec = calloc(1, sizeof *c->lrec);
    if (!c->lrec) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    const char *e = getenv("SPINEL_ELAB_CHECK");
    c->lrec->check = e && *e && *e != '0';
  }
  return c->lrec;
}

typedef struct { Compiler *c; LoadRecord *r; unsigned char *entered; int region, rank; } ElabWalk;

/* Where the walk stands: the file, the top-level statement holding it,
   the statement whose body opened the class frame it is in (-1: main's),
   whether it may not run, and its nesting for the dump. */
typedef struct { int file, top, frame, may, depth; } ElabAt;

static int lr_add(ElabWalk *w, int node, ElabAt at) {
  LoadRecord *r = w->r;
  if (r->nst == r->cst) {
    r->cst = r->cst ? r->cst * 2 : 256;
    r->st = realloc(r->st, sizeof *r->st * (size_t)r->cst);
    if (!r->st) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  int k = r->nst++;
  r->st[k] = (LrStmt){ node, at.file, w->region, at.top < 0 ? k : at.top, -1, at.depth, at.frame, (unsigned char)at.may };
  r->at[node] = k;
  return k;
}

static void elab_inner(ElabWalk *w, int s, int e, ElabAt at);

/* The statements of a body that runs here: a statement list, or a body
   with rescue clauses (a BeginNode), whose parts elab_inner walks. e is
   the statement whose body it is. */
static void elab_body(ElabWalk *w, int body, int e, ElabAt at) {
  const NodeTable *nt = w->c->nt;
  if (body < 0) return;
  at.depth++;
  if (nt_kind(nt, body) == NK_BeginNode) { at.depth--; elab_inner(w, body, e, at); return; }
  int n = 1;
  const int *v = &body;   /* a body of one expression is its one statement */
  if (nt_kind(nt, body) == NK_StatementsNode) v = nt_arr(nt, body, "body", &n);
  for (int k = 0; k < n; k++) {
    if (v[k] < 0 || w->r->at[v[k]] != -2) continue;   /* a subtree two lists share: once */
    elab_inner(w, v[k], lr_add(w, v[k], at), at);
  }
}

/* When the block of `call` runs: 1 before the call answers (class_eval
   and kin, a class builder's `Class.new(B) do`, a builtin iterator's: the
   iterator rows), -1 later (is_deferred_block_call: a Proc, a method body,
   a thread, ...), 0 not known (any other call's: a program method may
   yield to it, or keep it). The receiver is not typed yet, so a builtin
   iterator's name stands for the iterator. */
static int elab_block_runs(const NodeTable *nt, int call) {
  const char *nm = nt_kind(nt, call) == NK_CallNode ? nt_str(nt, call, "name") : NULL;
  int rv = nm ? nt_ref(nt, call, "receiver") : -1;
  const char *rn = rv >= 0 && nt_kind(nt, rv) == NK_ConstantReadNode ? nt_str(nt, rv, "name") : NULL;
  if (!nm) return 0;
  if (is_deferred_block_call(rn, nm)) return -1;
  return is_eval_exec_family(nm) || is_class_builder(rn, nm) || iter_row_named(nm) ? 1 : 0;
}

/* Does the block of `call` run as a class body (a frame): class_eval and
   the other eval and exec forms, a class builder's block? */
static int elab_opens_frame(const NodeTable *nt, int call) {
  const char *nm = nt_str(nt, call, "name");
  int rv = nt_ref(nt, call, "receiver");
  const char *rn = rv >= 0 && nt_kind(nt, rv) == NK_ConstantReadNode ? nt_str(nt, rv, "name") : NULL;
  return nm && (is_eval_exec_family(nm) || is_class_builder(rn, nm));
}

/* A written value's blocks: a call's, or the parts of `x rescue y` */
static void elab_value(ElabWalk *w, int v, int e, ElabAt at) {
  NodeKind k = v >= 0 ? nt_kind(w->c->nt, v) : NK_NilNode;
  if (k == NK_CallNode || k == NK_RescueModifierNode) elab_inner(w, v, e, at);
}

/* A body under a condition: its statements may not run */
static void elab_maybe(ElabWalk *w, int body, int e, ElabAt at) {
  at.may = 1;
  elab_body(w, body, e, at);
}

/* The bodies inside statement e's node s that run while it does. A class
   body, and the block of a call that runs it as one (class_eval and kin,
   a class builder), is a frame of its own: e opened it. */
static void elab_inner(ElabWalk *w, int s, int e, ElabAt at) {
  const NodeTable *nt = w->c->nt;
  if (s < 0) return;
  ElabAt in = at;
  in.frame = e;
  switch (nt_kind(nt, s)) {
    case NK_ClassNode: case NK_ModuleNode: case NK_SingletonClassNode:
      elab_body(w, nt_ref(nt, s, "body"), e, in);
      break;
    case NK_PreExecutionNode:
      elab_body(w, nt_ref(nt, s, "statements"), e, at);
      break;
    case NK_ParenthesesNode:
      elab_body(w, nt_ref(nt, s, "body"), e, at);
      break;
    case NK_IfNode: case NK_UnlessNode: {   /* an elsif ladder as a loop */
      int b = s;
      while (b >= 0 && (nt_kind(nt, b) == NK_IfNode || nt_kind(nt, b) == NK_UnlessNode)) {
        elab_maybe(w, nt_ref(nt, b, "statements"), e, at);
        b = nt_kind(nt, b) == NK_IfNode ? nt_ref(nt, b, "subsequent") : nt_ref(nt, b, "else_clause");
      }
      if (b >= 0) elab_maybe(w, nt_ref(nt, b, "statements"), e, at);
      break;
    }
    case NK_CaseNode: case NK_CaseMatchNode: {
      int n = 0;
      const int *v = nt_arr(nt, s, "conditions", &n);
      for (int k = 0; k < n; k++) elab_maybe(w, nt_ref(nt, v[k], "statements"), e, at);
      int el = nt_ref(nt, s, "else_clause");
      if (el >= 0) elab_maybe(w, nt_ref(nt, el, "statements"), e, at);
      break;
    }
    case NK_WhileNode: case NK_UntilNode: case NK_ForNode:
      elab_maybe(w, nt_ref(nt, s, "statements"), e, at);
      break;
    case NK_BeginNode: {
      elab_body(w, nt_ref(nt, s, "statements"), e, at);
      for (int r = nt_ref(nt, s, "rescue_clause"); r >= 0; r = nt_ref(nt, r, "subsequent"))
        elab_maybe(w, nt_ref(nt, r, "statements"), e, at);
      int el = nt_ref(nt, s, "else_clause"), en = nt_ref(nt, s, "ensure_clause");
      if (el >= 0) elab_maybe(w, nt_ref(nt, el, "statements"), e, at);
      if (en >= 0) elab_body(w, nt_ref(nt, en, "statements"), e, at);
      break;
    }
    /* `X = Class.new(B) do ... end`; under `||=` / `&&=` the value may not run */
    case NK_ConstantWriteNode: case NK_ConstantPathWriteNode: case NK_LocalVariableWriteNode:
    case NK_InstanceVariableWriteNode: case NK_GlobalVariableWriteNode: case NK_ClassVariableWriteNode:
      elab_value(w, nt_ref(nt, s, "value"), e, at);
      break;
    case NK_ConstantOrWriteNode: case NK_ConstantPathOrWriteNode: case NK_LocalVariableOrWriteNode:
    case NK_InstanceVariableOrWriteNode: case NK_GlobalVariableOrWriteNode: case NK_ClassVariableOrWriteNode:
    case NK_ConstantAndWriteNode: case NK_ConstantPathAndWriteNode: case NK_LocalVariableAndWriteNode:
    case NK_InstanceVariableAndWriteNode: case NK_GlobalVariableAndWriteNode: case NK_ClassVariableAndWriteNode:
      at.may = 1;
      elab_value(w, nt_ref(nt, s, "value"), e, at);
      break;
    case NK_RescueModifierNode: {   /* `x rescue y`: y may not run */
      elab_value(w, nt_ref(nt, s, "expression"), e, at);
      at.may = 1;
      elab_value(w, nt_ref(nt, s, "rescue_expression"), e, at);
      break;
    }
    case NK_CallNode: {
      int b = nt_ref(nt, s, "block"), runs = elab_block_runs(nt, s);
      if (b < 0 || nt_kind(nt, b) != NK_BlockNode || runs < 0) break;
      if (runs == 0) at.may = 1;
      in.may = at.may;
      elab_body(w, nt_ref(nt, b, "body"), e, elab_opens_frame(nt, s) ? in : at);
      break;
    }
    default: break;
  }
}

/* File f's top level, entered at its load: its root, its BEGIN blocks (run
   first, when the file loads), then its statements, each placed file right
   after the statement that loads it. */
static void elab_file(ElabWalk *w, int f) {
  const Compiler *c = w->c;
  const NodeTable *nt = c->nt;
  int root = nt_root(nt, f), st = prog_root_stmts(c, f), n = 0;
  w->entered[f] = 1;
  if (root < 0 || w->r->at[root] != -2) return;
  ElabAt at = { f, -1, -1, 0, 0 };
  int e = lr_add(w, root, at);
  w->r->st[e].rank = w->region == 0 ? w->rank++ : -1;
  if (st >= 0) w->r->at[st] = e;
  const int *v = st >= 0 ? nt_arr(nt, st, "body", &n) : NULL;
  for (int k = 0; k < n; k++)
    if (v[k] >= 0 && nt_kind(nt, v[k]) == NK_PreExecutionNode && w->r->at[v[k]] == -2) {
      int b = lr_add(w, v[k], at);
      ElabAt in = at; in.top = b;
      elab_inner(w, v[k], b, in);
    }
  for (int k = 0; k < n; k++) {
    if (v[k] < 0) continue;
    int s = w->r->at[v[k]];
    if (nt_kind(nt, v[k]) != NK_PreExecutionNode) {
      if (s != -2) continue;
      s = lr_add(w, v[k], at);
      ElabAt in = at; in.top = s;
      elab_inner(w, v[k], s, in);
    }
    if (s >= 0 && w->r->st[s].top == s) w->r->st[s].rank = w->region == 0 ? w->rank++ : -1;
    int t = prog_ordered_load(c, v[k]);
    if (t >= 0 && !w->entered[t]) elab_file(w, t);
  }
}

/* Is a node under parent p one that runs later than p's statement: under
   a method, a lambda, or a block kept to run later? */
static int lr_defers(const NodeTable *nt, const int *par, int p) {
  NodeKind k = nt_kind(nt, p);
  return k == NK_DefNode || k == NK_LambdaNode || (k == NK_BlockNode && par[p] >= 0 && elab_block_runs(nt, par[p]) < 0);
}

static void elab_points_build(Compiler *c) {
  double t0 = sp_timing_now();
  LoadRecord *r = lr_get(c);
  const NodeTable *nt = c->nt;
  const int *par = comp_parent_map(c);
  int n = nt->count, nroots = nt_nroots(nt);
  free(r->at); free(r->bits);
  r->at = malloc(sizeof(int) * (size_t)(n ? n : 1));
  r->bits = calloc((size_t)(n ? n : 1), 1);
  int *stack = malloc(sizeof(int) * (size_t)(n ? n : 1));
  ElabWalk w = { c, r, calloc((size_t)nroots, 1), 0, 0 };
  if (!r->at || !r->bits || !stack || !w.entered) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int id = 0; id < n; id++) r->at[id] = -2;
  r->nst = 0; r->n = n; r->ver = nt->version; r->nregion = 1;
  elab_file(&w, 0);
  for (int f = 1; f < nroots; f++)   /* the files no load places, a region each */
    if (!w.entered[f] && nt_root(nt, f) >= 0) { w.region = r->nregion++; elab_file(&w, f); }
  for (int id = 0; id < n; id++) {   /* the rest from their nearest walked statement */
    int s = 0, a = id;
    while (r->at[a] == -2 && par[a] >= 0) { stack[s++] = a; a = par[a]; }
    if (r->at[a] == -2) r->at[a] = -1;
    while (s > 0) {
      int b = stack[--s], p = par[b];
      r->at[b] = r->at[p];
      if ((r->bits[p] & LR_LATE) || lr_defers(nt, par, p)) r->bits[b] |= LR_LATE;
    }
  }
  free(stack); free(w.entered);
  r->builds++;
  if (sp_timing_on()) r->ms += sp_timing_now() - t0;
}

static LoadRecord *elab_fresh(Compiler *c) {
  LoadRecord *r = lr_get(c);
  if (!r->at || r->n != c->nt->count || r->ver != c->nt->version) elab_points_build(c);
  return r;
}

/* The point at which node's statement runs; region -1 (no point) for a
   node in a method body, a lambda or a block run later, or under no file. */
LoadPoint elab_point_of(Compiler *c, int node) {
  LoadRecord *r = elab_fresh(c);
  int e = node >= 0 && node < r->n ? r->at[node] : -1;
  if (e < 0 || (r->bits[node] & LR_LATE)) return (LoadPoint){ -1, -1 };
  return (LoadPoint){ r->st[e].region, e };
}

/* Does node's statement run only under a condition (a branch, a loop body,
   a rescue clause)? */
int elab_point_may(Compiler *c, int node) {
  LoadRecord *r = elab_fresh(c);
  int e = node >= 0 && node < r->n ? r->at[node] : -1;
  return e >= 0 && r->st[e].may;
}

/* PROG_BEFORE / AFTER / SAME, or UNKNOWN for two regions or no point. */
int elab_cmp(LoadPoint a, LoadPoint b) {
  if (a.region < 0 || b.region < 0 || a.region != b.region) return PROG_UNKNOWN;
  return a.seq == b.seq ? PROG_SAME : a.seq < b.seq ? PROG_BEFORE : PROG_AFTER;
}

/* The place prog_order reads (analyze_program.c): node's file (-1 under no
   file's root) and the rank of the top-level statement holding it (-1 in a
   file the load graph does not place). */
void elab_top_place(Compiler *c, int node, int *file, int *rank) {
  LoadRecord *r = elab_fresh(c);
  int e = node >= 0 && node < r->n ? r->at[node] : -1;
  *file = e >= 0 ? r->st[e].file : -1;
  *rank = e >= 0 && r->st[e].region == 0 ? r->st[r->st[e].top].rank : -1;
}

/* The record's order of two nodes: by the points of their statements
   (a method body's nodes at its def's), node ids within one statement. */
int elab_order(Compiler *c, int a, int b) {
  if (a == b) return PROG_SAME;
  LoadRecord *r = elab_fresh(c);
  int ea = a >= 0 && a < r->n ? r->at[a] : -1, eb = b >= 0 && b < r->n ? r->at[b] : -1;
  if (ea < 0 || eb < 0 || ea == eb) return a < b ? PROG_BEFORE : PROG_AFTER;
  if (r->st[ea].region != r->st[eb].region) return PROG_UNKNOWN;
  return ea < eb ? PROG_BEFORE : PROG_AFTER;
}

/* prog_sort's key in the record's order: the statement's seq, a node
   under no file last. */
long long elab_sort_key(Compiler *c, int node) {
  LoadRecord *r = elab_fresh(c);
  int e = node >= 0 && node < r->n ? r->at[node] : -1;
  return ((long long)(e >= 0 ? e : INT_MAX) << 32) | (unsigned)node;
}

/* ---- the histories ----
   elab_program records each definition at its statement as the load phase
   runs them, in seq order, in the frame its statement's body opens: the
   class (or singleton class) a class body, `class << x` or class_eval
   block runs in, main's at a file's top level. */
typedef struct LrHist { int a, b; char *name; LrDef *v; int n, cap, next; } LrHist;   /* a: class, or -2 - ElabVar; b: singleton */
/* sup: the superclass the first header naming one names, as resolved
   there (has_sup); LR_UNKNOWN_CLASS when it is not a class the record knows */
typedef struct LrClass { LrOpen *open; int nopen, copen; LrMixin *mix; int nmix, cmix, sup; unsigned char has_sup; } LrClass;
enum { LR_BUCKETS = 4096, LR_MAIN = -1, LR_UNKNOWN_CLASS = -2 };
/* the histories that are not a method's or a variable's: a frame's bare
   visibility words, keyed by the frame's node, and the code a class's
   body ran that the record cannot follow, keyed as a constant owner */
enum { LR_KEY_VIS = -5, LR_KEY_OPAQUE = -6 };

static void *lr_grow(void *v, int *cap, int n, size_t sz) {
  if (n < *cap) return v;
  *cap = *cap ? *cap * 2 : 8;
  v = realloc(v, sz * (size_t)*cap);
  if (!v) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  return v;
}

static unsigned lr_hash(int a, int b, const char *name) {
  unsigned h = (unsigned)a * 31u + (unsigned)b * 7u;
  for (const char *q = name; *q; q++) h = h * 33u + (unsigned char)*q;
  return h % LR_BUCKETS;
}

static LrHist *lr_hist(LoadRecord *r, int a, int b, const char *name, int make) {
  if (!name || !r->hhead) return NULL;
  unsigned h = lr_hash(a, b, name);
  for (int i = r->hhead[h]; i >= 0; i = r->hist[i].next)
    if (r->hist[i].a == a && r->hist[i].b == b && sp_streq(r->hist[i].name, name)) return &r->hist[i];
  if (!make) return NULL;
  r->hist = lr_grow(r->hist, &r->chist, r->nhist, sizeof *r->hist);
  LrHist *x = &r->hist[r->nhist];
  *x = (LrHist){ a, b, strdup(name), NULL, 0, 0, r->hhead[h] };
  if (!x->name) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  r->hhead[h] = r->nhist++;
  return x;
}

static void lr_version(LoadRecord *r, int a, int b, const char *name, LrDef d) {
  LrHist *x = lr_hist(r, a, b, name, 1);
  if (!x) return;
  if (d.target && !(d.target = strdup(d.target))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  x->v = lr_grow(x->v, &x->cap, x->n, sizeof *x->v);
  x->v[x->n++] = d;
}

/* cls's history (LR_MAIN: main's, Object's), NULL past the record's classes */
static LrClass *lr_class(LoadRecord *r, int cls) {
  return cls >= LR_MAIN && cls + 1 < r->ncls ? &r->cls[cls + 1] : NULL;
}

/* The class a class or module header opens, as the scope walk decided it:
   the class of its header's own class body (node_cbody) */
static int lr_class_opened(Compiler *c, int node) {
  int cp = nt_ref(c->nt, node, "constant_path");
  int ci = cp >= 0 && cp < c->node_cap ? c->node_cbody[cp] : -1;
  return ci >= 0 ? ci : LR_UNKNOWN_CLASS;
}

/* elab_program's state: each statement's frame class (fc) and singleton
   flag (fs), each frame's visibility state (vs) and each file's top
   level's (vmain) */
typedef struct { Compiler *c; LoadRecord *r; int *fc; unsigned char *fs, *vs, *vmain; } ElabRec;

/* The constant name a class or module header (or a read) names, as the
   program wrote it: a header the qualify passes renamed keeps the Ruby path
   it resolved to (qc_ruby_name), whose last name it is; a read they renamed
   keeps the name it had (qc_orig_name). */
static const char *lr_written_name(const NodeTable *nt, int cp) {
  const char *rn = nt_str(nt, cp, "qc_ruby_name"), *q = rn;
  for (const char *p = rn ? strstr(rn, "::") : NULL; p; p = strstr(p + 2, "::")) q = p + 2;
  return rn ? q : nt_str(nt, cp, "qc_orig_name") ? nt_str(nt, cp, "qc_orig_name") : nt_str(nt, cp, "name");
}

static int lr_const_at(Compiler *c, int node, const char *name, const LrDef **out);
static int lr_const_in(Compiler *c, int cls, const char *name, LoadPoint p, int late, int top, const LrDef **out, int depth);

/* The class a constant's write or definition holds: a class or module
   header's (the scope walk's decision), a class builder's; else none known */
static int lr_cref(Compiler *c, int e);
static int lr_search(Compiler *c, int cls, int sg, const char *name, int mod, LoadPoint p, int late, int vis, const LrDef **out, int depth);

static int lr_class_of(Compiler *c, const LrDef *d) {
  NodeKind k = d ? nt_kind(c->nt, d->node) : NK_NilNode;
  if (k == NK_ClassNode || k == NK_ModuleNode) return lr_class_opened(c, d->node);
  if (k == NK_ConstantWriteNode && class_def_body(c, d->node) >= 0)
    for (int ci = 0; ci < c->nclasses; ci++) if (c->classes[ci].def_node == d->node) return ci;
  return LR_UNKNOWN_CLASS;
}

/* The class a constant reference names (a mixin's argument, `class << K`,
   `def K.m`, `K.class_eval`), resolved as CRuby does where it stands: a
   name through its lexical scopes and then the ancestors (lr_const_at), a
   path `A::B` in A and its ancestors, `::B` in Object's. A name the
   program defines nowhere it can reach is a core class's or module's, as
   registered. */
static int lr_class_ref(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const LrDef *d = NULL;
  NodeKind k = node >= 0 ? nt_kind(nt, node) : NK_NilNode;
  if (k == NK_ConstantReadNode) {
    const char *nm = lr_written_name(nt, node);
    int o = lr_const_at(c, node, nm, &d), ci;
    if (o == ELAB_YES) return lr_class_of(c, d);
    if (o == ELAB_NO && (is_builtin_class_name(nm) || is_builtin_module_const_name(nm)) && (ci = comp_class_index(c, nm)) >= 0) return ci;
    return LR_UNKNOWN_CLASS;
  }
  if (k != NK_ConstantPathNode) return LR_UNKNOWN_CLASS;
  int par = nt_ref(nt, node, "parent"), pc = par < 0 ? LR_MAIN : lr_class_ref(c, par);
  LoadPoint p = elab_point_of(c, node);
  if (pc < LR_MAIN || lr_const_in(c, pc, lr_written_name(nt, node), p, p.region < 0, pc == LR_MAIN, &d, 0) != ELAB_YES) return LR_UNKNOWN_CLASS;
  return lr_class_of(c, d);
}

/* The class a call's receiver names: none or self, the frame's; a
   constant, its class (not singleton) */
static int lr_receiver(ElabRec *x, int recv, int fc, int fs, int *sg) {
  NodeKind k = recv >= 0 ? nt_kind(x->c->nt, recv) : NK_SelfNode;
  *sg = k == NK_SelfNode ? fs : 0;
  if (k == NK_SelfNode) return fc;
  return lr_class_ref(x->c, recv);
}


/* The call whose block opens the frame of a statement: the statement's
   own call, a written value's, or either side of `x rescue y`; -1 for
   none, and LR_UNKNOWN_CLASS when both sides of a rescue modifier open
   one, which the walk gives the statement's one frame */
static int lr_frame_call(const NodeTable *nt, int node) {
  NodeKind k = nt_kind(nt, node);
  int v = k == NK_CallNode || k == NK_RescueModifierNode ? node : nt_ref(nt, node, "value");
  int a = v, b = -1;
  if (v >= 0 && nt_kind(nt, v) == NK_RescueModifierNode) { a = nt_ref(nt, v, "expression"); b = nt_ref(nt, v, "rescue_expression"); }
  int fa = a >= 0 && nt_kind(nt, a) == NK_CallNode && nt_ref(nt, a, "block") >= 0 && elab_opens_frame(nt, a);
  int fb = b >= 0 && nt_kind(nt, b) == NK_CallNode && nt_ref(nt, b, "block") >= 0 && elab_opens_frame(nt, b);
  return fa && fb ? LR_UNKNOWN_CLASS : fa ? a : fb ? b : -1;
}

/* The frame entry e's body opens: its class in fc[e], singleton in fs[e];
   for any other statement, the frame it stands in. */
static void lr_frame(ElabRec *x, int e) {
  const NodeTable *nt = x->c->nt;
  const LrStmt *s = &x->r->st[e];
  int f = s->frame, node = s->node, cls = f >= 0 ? x->fc[f] : LR_MAIN, sg = f >= 0 ? x->fs[f] : 0;
  NodeKind k = nt_kind(nt, node);
  int call = lr_frame_call(nt, node);
  x->vs[e] = BVIS_PUBLIC;
  if (k == NK_ClassNode || k == NK_ModuleNode) { cls = lr_class_opened(x->c, node); sg = 0; }
  else if (k == NK_SingletonClassNode) {
    int ex = nt_ref(nt, node, "expression");
    cls = nt_kind(nt, ex) == NK_SelfNode ? cls : lr_class_ref(x->c, ex);
    sg = 1;
  }
  else if ((k == NK_ConstantWriteNode || k == NK_LocalVariableWriteNode) && class_def_body(x->c, node) >= 0) {
    /* `X = Struct.new(:a) do`: the class registration homes there, once it has */
    cls = LR_UNKNOWN_CLASS; sg = 0;
    for (int ci = 0; ci < x->c->nclasses; ci++) if (x->c->classes[ci].def_node == node) cls = ci;
  }
  else if (call == LR_UNKNOWN_CLASS) { cls = LR_UNKNOWN_CLASS; sg = 0; }
  else if (call >= 0) {
    const char *nm = nt_str(nt, call, "name");
    int rsg;
    cls = is_class_eval_family(nm) || is_instance_eval_family(nm) ? lr_receiver(x, nt_ref(nt, call, "receiver"), cls, sg, &rsg) : LR_UNKNOWN_CLASS;
    sg = is_instance_eval_family(nm) ? 1 : 0;
  }
  if (cls < LR_MAIN) cls = LR_UNKNOWN_CLASS;
  x->fc[e] = cls; x->fs[e] = (unsigned char)sg;
}

/* A visibility word's SP_VIS_* (module_function: private) */
static unsigned char lr_vis(int bvis) {
  return bvis == BVIS_PRIVATE || bvis == BVIS_MODULE_FUNCTION ? SP_VIS_PRIVATE : bvis == BVIS_PROTECTED ? SP_VIS_PROTECTED : SP_VIS_PUBLIC;
}

/* A method version of (cls, sg, name) by node, under the frame's
   visibility state bvis; a module_function one also copies it to the
   singleton */
static int lr_vis_at(Compiler *c, int cls, const char *name, int sg, LoadPoint p);

/* ...: a definition of initialize and its kin is private wherever it
   stands, and an alias takes its original's visibility */
static void lr_method(ElabRec *x, int cls, int sg, const char *name, int node, LrKind kind, int bvis, int may, const char *target) {
  if (cls < LR_MAIN || !name) return;
  int vis = sg ? SP_VIS_PUBLIC : lr_vis(bvis), ov = kind == LR_ALIAS && target ? lr_vis_at(x->c, cls, target, sg, elab_point_of(x->c, node)) : -1;
  if (!sg && (kind == LR_DEF || kind == LR_DEFINE_METHOD) && is_implicitly_private_name(name)) vis = SP_VIS_PRIVATE;
  if (ov >= 0) vis = ov;
  lr_version(x->r, cls, sg, name, (LrDef){ node, (unsigned char)kind, (unsigned char)vis, (unsigned char)may, 0, target });
  if (!sg && bvis == BVIS_MODULE_FUNCTION)
    lr_version(x->r, cls, 1, name, (LrDef){ node, LR_COPY, SP_VIS_PUBLIC, (unsigned char)may, 0, name });
}

static void lr_def(ElabRec *x, int def, int fc, int fs, int bvis, int may) {
  int recv = nt_ref(x->c->nt, def, "receiver"), sg = fs, cls = fc;
  if (recv >= 0) { cls = lr_receiver(x, recv, fc, 1, &sg); sg = 1; }
  lr_method(x, cls, sg, nt_str(x->c->nt, def, "name"), def, LR_DEF, bvis, may, NULL);
}

/* A write of a variable: a global's or main's instance variable's by name,
   a constant's by name and the class whose body (lexical scope) writes it,
   as lr_const_owner numbers it (0: not known) */
static int lr_const_owner(int cls) { return cls >= LR_MAIN ? cls + 2 : 0; }
static void lr_write(LoadRecord *r, ElabVar kind, int owner, const char *name, int node, int nilv, int may) {
  lr_version(r, -2 - (int)kind, owner, name, (LrDef){ node, LR_WRITE, 0, (unsigned char)may, (unsigned char)nilv, NULL });
}

/* A call statement's definitions: attrs, aliases, visibility, mixins,
   define_method, undef_method / remove_method, on the class its receiver
   names. vs is the frame's visibility state. */
static void lr_call(ElabRec *x, int node, int fc, int fs, unsigned char *vs, int fnode, int may) {
  const NodeTable *nt = x->c->nt;
  const char *nm = nt_str(nt, node, "name");
  int sg, recv = nt_ref(nt, node, "receiver"), cls = lr_receiver(x, recv, fc, fs, &sg), an = nt_ref(nt, node, "arguments"), ac = 0;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  int bvis = visibility_of_name(nm), opaque = 0;
  if (bvis && !ac && (recv < 0 || nt_kind(nt, recv) == NK_SelfNode)) {
    /* the frame's visibility from here on: kept by the frame's node, so
       also in a body whose class the record does not know */
    *vs = (unsigned char)bvis;
    lr_version(x->r, LR_KEY_VIS, fnode, "", (LrDef){ node, LR_VIS, (unsigned char)bvis, (unsigned char)may, 0, NULL });
    return;
  }
  if (!nm || cls < LR_MAIN) return;
  int naming = bvis || is_method_naming_call(nm), dm = is_define_method_name(nm) || is_define_singleton_method_name(nm);
  /* `include A, B` puts A nearest the class (ancestors [C, A, B]), as
     `include B` then `include A` would, and so do prepend and extend: the
     arguments of a mixin are recorded last first */
  int mix = is_mixin_call(nm);
  for (int j = 0; j < ac; j++) {
    int i = mix ? ac - 1 - j : j;
    const char *an_ = vis_arg_name(nt, av[i]);
    if (!an_ && nt_kind(nt, av[i]) != NK_DefNode && naming && (!dm || i == 0)) opaque = 1;   /* a computed name */
    if (bvis && nt_kind(nt, av[i]) == NK_DefNode) lr_def(x, av[i], cls, sg, bvis, may);
    else if (bvis) lr_method(x, cls, sg, an_, node, LR_VIS, bvis, may, NULL);
    else if (is_attr_reader_family(nm) || is_attr_writer_family(nm)) {
      if (is_attr_reader_family(nm)) lr_method(x, cls, sg, an_, node, LR_READER, *vs, may, NULL);
      char w[256];
      if (an_ && is_attr_writer_family(nm) && snprintf(w, sizeof w, "%s=", an_) < (int)sizeof w)
        lr_method(x, cls, sg, w, node, LR_WRITER, *vs, may, NULL);
    }
    else if (mix) {
      LrClass *k = lr_class(x->r, cls);
      int mod = lr_class_ref(x->c, av[i]);
      opaque |= mod < 0;
      if (!k) continue;
      k->mix = lr_grow(k->mix, &k->cmix, k->nmix, sizeof *k->mix);
      k->mix[k->nmix++] = (LrMixin){ node, is_include_name(nm) ? LR_INCLUDE : is_prepend_name(nm) ? LR_PREPEND : LR_EXTEND, mod };
    }
    else if (is_undef_method_name(nm) || is_remove_method_name(nm))
      lr_method(x, cls, sg, an_, node, is_undef_method_name(nm) ? LR_UNDEF : LR_REMOVE, *vs, may, NULL);
  }
  if (dm && ac > 0)
    lr_method(x, cls, sg || is_define_singleton_method_name(nm), vis_arg_name(nt, av[0]), node, LR_DEFINE_METHOD, *vs, may, NULL);
  if (is_alias_method_name(nm) && ac == 2)
    lr_method(x, cls, sg, vis_arg_name(nt, av[0]), node, LR_ALIAS, *vs, may, vis_arg_name(nt, av[1]));
  /* code the record cannot follow, run on a class: a send, an eval of a
     String, or a method of the program called on the class (its singleton
     method, from its body or through its constant) or from its body (a
     top-level one) */
  if (!naming && !mix) {
    LoadPoint p = elab_point_of(x->c, node);
    int bare = recv < 0 || nt_kind(nt, recv) == NK_SelfNode;
    opaque = is_send_family(nm) || (is_eval_exec_family(nm) && nt_ref(nt, node, "block") < 0) ||
             (!sg && lr_search(x->c, cls, 1, nm, -1, p, 0, 0, NULL, 0) != ELAB_NO) ||
             (bare && lr_search(x->c, LR_MAIN, 0, nm, -1, p, 0, 0, NULL, 0) != ELAB_NO);
  }
  if (opaque && cls >= 0)
    lr_version(x->r, LR_KEY_OPAQUE, lr_const_owner(cls), "", (LrDef){ node, LR_OPAQUE, 0, (unsigned char)may, 0, NULL });
}

/* What statement e defines or writes, in its frame */
static void lr_record_stmt(ElabRec *x, int e) {
  const NodeTable *nt = x->c->nt;
  LoadRecord *r = x->r;
  const LrStmt *s = &r->st[e];
  int node = s->node, f = s->frame, may = s->may;
  int fc = f >= 0 ? x->fc[f] : LR_MAIN, fs = f >= 0 ? x->fs[f] : 0;
  unsigned char *vs = f >= 0 ? &x->vs[f] : &x->vmain[s->file];
  NodeKind k = nt_kind(nt, node);
  lr_frame(x, e);
  if ((k == NK_ClassNode || k == NK_ModuleNode || k == NK_ConstantWriteNode || k == NK_LocalVariableWriteNode) &&
      x->fc[e] >= 0 && x->fc[e] != fc) {   /* a body the class runs */
    LrClass *kc = lr_class(r, x->fc[e]);
    int body = k == NK_ClassNode || k == NK_ModuleNode ? nt_ref(nt, node, "body") : class_def_body(x->c, node);
    int cp = k == NK_ClassNode || k == NK_ModuleNode ? nt_ref(nt, node, "constant_path") : -1;
    /* the first opening defines its constant: in the lexical scope for a
       name, in the class the scope walk nested it in for a path */
    if (kc && !kc->nopen && cp >= 0) {
      int owner = nt_kind(nt, cp) == NK_ConstantReadNode ? lr_cref(x->c, e) : x->c->classes[x->fc[e]].enclosing_class;
      lr_write(r, ELAB_CONST, lr_const_owner(owner), lr_written_name(nt, cp), node, 0, may);
    }
    int sc = k == NK_ClassNode ? nt_ref(nt, node, "superclass") : -1;
    if (kc && sc >= 0 && !kc->has_sup) { kc->sup = lr_class_ref(x->c, sc); kc->has_sup = 1; }
    if (kc) { kc->open = lr_grow(kc->open, &kc->copen, kc->nopen, sizeof *kc->open); kc->open[kc->nopen++] = (LrOpen){ node, body }; }
  }
  switch (k) {
    case NK_DefNode: lr_def(x, node, fc, fs, *vs, may); break;
    case NK_AliasMethodNode: {
      int nn = nt_ref(nt, node, "new_name"), on = nt_ref(nt, node, "old_name");
      lr_method(x, fc, fs, nn >= 0 ? vis_arg_name(nt, nn) : NULL, node, LR_ALIAS, *vs, may, on >= 0 ? vis_arg_name(nt, on) : NULL);
      break;
    }
    case NK_UndefNode: {
      int n = 0;
      const int *v = nt_arr(nt, node, "names", &n);
      for (int i = 0; i < n; i++) lr_method(x, fc, fs, vis_arg_name(nt, v[i]), node, LR_UNDEF, *vs, may, NULL);
      break;
    }
    case NK_CallNode: lr_call(x, node, fc, fs, vs, f >= 0 ? r->st[f].node : nt_root(nt, s->file), may); break;
    case NK_ConstantWriteNode: case NK_ConstantOrWriteNode: {   /* in its lexical scope */
      int v = nt_ref(nt, node, "value");
      lr_write(r, ELAB_CONST, lr_const_owner(lr_cref(x->c, e)), lr_written_name(nt, node), node, v < 0 || nt_kind(nt, v) == NK_NilNode,
               may || k == NK_ConstantOrWriteNode);
      break;
    }
    case NK_GlobalVariableWriteNode: case NK_GlobalVariableOrWriteNode:   /* `$a = $b = v` writes both */
      for (int w = node; w >= 0 && (nt_kind(nt, w) == NK_GlobalVariableWriteNode || w == node); w = nt_ref(nt, w, "value")) {
        const char *gn = nt_str(nt, w, "name");
        int v = nt_ref(nt, w, "value");
        if (gn && *gn) lr_write(r, ELAB_GVAR, 0, gn + 1, node, v < 0 || nt_kind(nt, v) == NK_NilNode, may || k == NK_GlobalVariableOrWriteNode);
      }
      break;
    case NK_InstanceVariableWriteNode: case NK_InstanceVariableOrWriteNode: {
      int v = nt_ref(nt, node, "value");
      if (f < 0) lr_write(r, ELAB_MAIN_IVAR, 0, nt_str(nt, node, "name"), node, v < 0 || nt_kind(nt, v) == NK_NilNode, may || k == NK_InstanceVariableOrWriteNode);
      break;
    }
    default: break;
  }
}

static void elab_check_record(Compiler *c);

/* The scope walk of each file's top level (prog_walk_files), then the
   record of what the load phase defines, in the order it runs. */
void elab_program(Compiler *c) {
  prog_walk_files(c);
  double t0 = sp_timing_now();   /* the record's own cost, the map's build included */
  LoadRecord *r = elab_fresh(c);
  r->ncls = c->nclasses + 1;
  r->cls = calloc((size_t)r->ncls, sizeof *r->cls);
  r->hhead = malloc(sizeof(int) * LR_BUCKETS);
  int nroots = nt_nroots(c->nt);
  ElabRec x = { c, r, malloc(sizeof(int) * (size_t)(r->nst + 1)), calloc((size_t)r->nst + 1, 1), calloc((size_t)r->nst + 1, 1),
                malloc((size_t)nroots) };
  if (!r->cls || !r->hhead || !x.fc || !x.fs || !x.vs || !x.vmain) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int b = 0; b < LR_BUCKETS; b++) r->hhead[b] = -1;
  /* each file's top level starts private: its methods are Object's private ones */
  for (int f = 0; f < nroots; f++) x.vmain[f] = BVIS_PRIVATE;
  for (int e = 0; e < r->nst; e++) lr_record_stmt(&x, e);
  free(x.fc); free(x.fs); free(x.vs); free(x.vmain);
  r->recorded = 1;
  sp_timing_end(t0, "an_elab_record", "");
  if (r->check) elab_check_record(c);
}

/* ---- the queries ---- */
/* The live version of history h at p (late: as of the load's end):
   ELAB_YES, ELAB_NO (none: ask on), LR_STOP (undefined here), ELAB_UNKNOWN. */
enum { LR_STOP = 3 };
/* The last entry of history h before p (late: of the load phase), in *out
   (when asked): ELAB_YES, ELAB_NO for none, ELAB_UNKNOWN when one in
   another region or one that may not have run comes after the last sure
   one -- at the load phase's end too, as elab_written_before answers.
   skip_vis passes over the visibility versions of a method. */
static int lr_last_before(Compiler *c, const LrHist *h, LoadPoint p, int late, int skip_vis, const LrDef **out) {
  int unsure = 0;
  for (int i = h ? h->n - 1 : -1; i >= 0; i--) {
    const LrDef *v = &h->v[i];
    if (skip_vis && v->kind == LR_VIS) continue;
    int o = late ? (elab_point_of(c, v->node).region == 0 ? PROG_BEFORE : PROG_UNKNOWN) : elab_cmp(elab_point_of(c, v->node), p);
    if (o == PROG_UNKNOWN) { unsure = 1; continue; }
    if (o != PROG_BEFORE) continue;
    if (v->may) { unsure = 1; continue; }
    if (out) *out = v;
    return unsure ? ELAB_UNKNOWN : ELAB_YES;
  }
  return unsure ? ELAB_UNKNOWN : ELAB_NO;
}

/* ...of a method: an undef stops the search (LR_STOP), a removal passes it
   on; a visibility change (`private :m`) counts only when vis asks for it */
static int lr_own(Compiler *c, const LrHist *h, LoadPoint p, int late, int vis, const LrDef **out) {
  const LrDef *v = NULL;
  int o = lr_last_before(c, h, p, late, !vis, &v);
  if (out && v) *out = v;
  return o != ELAB_YES ? o : v->kind == LR_UNDEF ? LR_STOP : v->kind == LR_REMOVE ? ELAB_NO : ELAB_YES;
}

/* The class searched after cls: its superclass, Object (main's methods and
   constants) for a class with none; none (LR_UNKNOWN_CLASS) for a module
   and for Object itself */
static int lr_super(Compiler *c, int cls) {
  if (cls < 0 || cls >= c->nclasses) return LR_UNKNOWN_CLASS;
  const LrClass *k = lr_class(c->lrec, cls);
  int d = c->classes[cls].def_node;
  if (k && k->has_sup && k->sup >= 0) return k->sup;   /* the record's, which it has while it is made */
  if (c->classes[cls].parent >= 0) return c->classes[cls].parent;
  return d >= 0 && nt_kind(c->nt, d) == NK_ModuleNode ? LR_UNKNOWN_CLASS : LR_MAIN;
}

/* Did mixin m of a class happen before p (late: in the load phase)?
   ELAB_YES / NO / UNKNOWN */
static int lr_mixed_before(Compiler *c, const LrMixin *m, LoadPoint p, int late, int may) {
  int o = late ? (elab_point_of(c, m->node).region == 0 ? PROG_BEFORE : PROG_UNKNOWN) : elab_cmp(elab_point_of(c, m->node), p);
  return o == PROG_UNKNOWN ? ELAB_UNKNOWN : o != PROG_BEFORE ? ELAB_NO : may ? ELAB_UNKNOWN : ELAB_YES;
}

/* The search of `name` (or, with name NULL, of module `mod`) along cls's
   ancestors at p: its prepends (the last first), cls, its includes (the
   last first), then its superclass; a singleton's own methods, its extends,
   then the superclass's singleton. With vis, an ancestor's visibility
   change of the name is found as well, for the visibility it gives. */
static int lr_search(Compiler *c, int cls, int sg, const char *name, int mod, LoadPoint p, int late, int vis, const LrDef **out, int depth) {
  LoadRecord *r = c->lrec;
  if (depth > 64 || cls < LR_MAIN) return ELAB_UNKNOWN;
  if (!name && cls == mod && !sg) return ELAB_YES;
  const LrClass *k = lr_class(r, cls);
  int unsure = 0;
  for (int pass = 0; pass < 3; pass++) {   /* prepends, own, includes or extends */
    if (pass == 1) {
      int o = name ? lr_own(c, lr_hist(r, cls, sg, name, 0), p, late, vis, out) : ELAB_NO;
      if (o == ELAB_YES) return unsure ? ELAB_UNKNOWN : ELAB_YES;
      if (o == LR_STOP) return unsure ? ELAB_UNKNOWN : ELAB_NO;
      if (o == ELAB_UNKNOWN) unsure = 1;
      continue;
    }
    for (int i = k ? k->nmix - 1 : -1; i >= 0; i--) {
      const LrMixin *m = &k->mix[i];
      int want = sg ? LR_EXTEND : pass == 0 ? LR_PREPEND : LR_INCLUDE;
      if (m->kind != want || (sg && pass == 0)) continue;
      int b = lr_mixed_before(c, m, p, late, elab_point_may(c, m->node));
      if (b == ELAB_NO) continue;
      int o = m->mod < 0 ? ELAB_UNKNOWN : !name && m->mod == mod ? ELAB_YES : lr_search(c, m->mod, 0, name, mod, p, late, vis, out, depth + 1);
      if (o == ELAB_YES && b == ELAB_YES) return unsure ? ELAB_UNKNOWN : ELAB_YES;
      if (o != ELAB_NO) unsure = 1;
    }
  }
  /* a class's chain ends at Object, whose methods main's top level defines
     (main's own singleton is not Object's) */
  int sup = lr_super(c, cls);
  if (sg && sup == LR_MAIN) sup = LR_UNKNOWN_CLASS;
  int o = sup >= LR_MAIN ? lr_search(c, sup, sg, name, mod, p, late, vis, out, depth + 1) : ELAB_NO;
  return o == ELAB_NO ? (unsure ? ELAB_UNKNOWN : ELAB_NO) : unsure ? ELAB_UNKNOWN : o;
}

int elab_method_at(Compiler *c, int cls, const char *name, int sg, LoadPoint p, const LrDef **out) {
  if (out) *out = NULL;
  if (!c->lrec || !c->lrec->recorded || !name) return ELAB_UNKNOWN;
  int o = lr_search(c, cls, sg, name, -1, p, p.region < 0, 0, out, 0);
  return o == LR_STOP ? ELAB_NO : o;
}

int elab_mixed_at(Compiler *c, int cls, int sg, int mod, LoadPoint p) {
  if (!c->lrec || !c->lrec->recorded || mod < 0) return ELAB_UNKNOWN;
  return lr_search(c, cls, sg, NULL, mod, p, p.region < 0, 0, NULL, 0);
}

int elab_written_before(Compiler *c, ElabVar kind, const char *name, LoadPoint p) {
  if (!c->lrec || !c->lrec->recorded) return ELAB_UNKNOWN;
  const LrHist *h = lr_hist(c->lrec, -2 - (int)kind, kind == ELAB_CONST ? lr_const_owner(LR_MAIN) : 0, name, 0);
  int unsure = 0;
  for (int i = 0; h && i < h->n; i++) {
    const LrDef *v = &h->v[i];
    int o = p.region < 0 ? (v->may || elab_point_of(c, v->node).region != 0 ? PROG_UNKNOWN : PROG_BEFORE)
                         : elab_cmp(elab_point_of(c, v->node), p);
    if (o == PROG_UNKNOWN) unsure = 1;
    else if (o == PROG_BEFORE && !v->nil) { if (!v->may) return ELAB_YES; unsure = 1; }
  }
  return unsure ? ELAB_UNKNOWN : ELAB_NO;
}

/* The constants of cls and its ancestors at p: its own, its mixins' (the
   last first), then its superclass's, and with top Object's after a
   class's. A scoped read (`A::B`) leaves Object's out, as CRuby does. */
static int lr_const_in(Compiler *c, int cls, const char *name, LoadPoint p, int late, int top, const LrDef **out, int depth) {
  LoadRecord *r = c->lrec;
  if (depth > 64 || cls < LR_MAIN) return ELAB_UNKNOWN;
  int o = lr_last_before(c, lr_hist(r, -2 - (int)ELAB_CONST, lr_const_owner(cls), name, 0), p, late, 0, out), unsure = 0;
  if (o != ELAB_NO) return o;
  const LrClass *k = lr_class(r, cls);
  for (int i = k ? k->nmix - 1 : -1; i >= 0; i--) {
    const LrMixin *m = &k->mix[i];
    int b = m->kind == LR_EXTEND ? ELAB_NO : lr_mixed_before(c, m, p, late, elab_point_may(c, m->node));
    if (b == ELAB_NO) continue;
    int mo = m->mod < 0 ? ELAB_UNKNOWN : lr_const_in(c, m->mod, name, p, late, top, out, depth + 1);
    if (mo == ELAB_YES && b == ELAB_YES) return unsure ? ELAB_UNKNOWN : ELAB_YES;
    if (mo != ELAB_NO) unsure = 1;
  }
  int sup = lr_super(c, cls);
  if (sup == LR_MAIN && !top) sup = LR_UNKNOWN_CLASS;
  o = sup >= LR_MAIN ? lr_const_in(c, sup, name, p, late, top, out, depth + 1) : ELAB_NO;
  return unsure ? ELAB_UNKNOWN : o;
}

/* The class whose body is node's lexical scope (cref): the nearest class or
   module body around its statement. A class_eval or class builder's block
   keeps the scope it is written in. LR_UNKNOWN_CLASS in `class << x`. */
static int lr_cref(Compiler *c, int e) {
  const LoadRecord *r = c->lrec;
  for (int f = e >= 0 ? r->st[e].frame : -1; f >= 0; f = r->st[f].frame) {
    NodeKind k = nt_kind(c->nt, r->st[f].node);
    if (k == NK_SingletonClassNode) return LR_UNKNOWN_CLASS;
    if (k == NK_ClassNode || k == NK_ModuleNode) return lr_class_opened(c, r->st[f].node);
  }
  return LR_MAIN;
}

/* What constant `name` read at node resolves to, as CRuby resolves it there:
   each lexical scope's own constants, innermost first, then the ancestors
   of the innermost (Object's last) */
static int lr_const_at(Compiler *c, int node, const char *name, const LrDef **out) {
  LoadRecord *r = elab_fresh(c);
  LoadPoint p = elab_point_of(c, node);
  int e = node >= 0 && node < r->n ? r->at[node] : -1, late = p.region < 0, first = LR_MAIN;
  if (e < 0 || !name) return ELAB_UNKNOWN;
  for (int g = r->st[e].frame; g >= 0; g = r->st[g].frame) {   /* the class and module bodies around it */
    NodeKind k = nt_kind(c->nt, r->st[g].node);
    if (k == NK_SingletonClassNode) return ELAB_UNKNOWN;
    if (k != NK_ClassNode && k != NK_ModuleNode) continue;
    int ci = lr_class_opened(c, r->st[g].node);
    if (ci < 0) return ELAB_UNKNOWN;
    if (first == LR_MAIN) first = ci;
    int o = lr_last_before(c, lr_hist(r, -2 - (int)ELAB_CONST, lr_const_owner(ci), name, 0), p, late, 0, out);
    if (o != ELAB_NO) return o;
  }
  /* then the innermost scope's ancestors, and Object's for a module too:
     a lexical read falls back to Object */
  int o = lr_const_in(c, first, name, p, late, 1, out, 0);
  if (o == ELAB_NO && first != LR_MAIN && lr_super(c, first) == LR_UNKNOWN_CLASS) o = lr_const_in(c, LR_MAIN, name, p, late, 1, out, 0);
  return o;
}

int elab_const_at(Compiler *c, int node, const char *name, const LrDef **out) {
  if (out) *out = NULL;
  return c->lrec && c->lrec->recorded ? lr_const_at(c, node, name, out) : ELAB_UNKNOWN;
}

int elab_body_visibility_at(Compiler *c, int node) {
  LoadRecord *r = c->lrec;
  if (!r || !r->recorded) return 0;
  elab_fresh(c);
  int e = node >= 0 && node < r->n ? r->at[node] : -1;
  if (e < 0 || (r->bits[node] & LR_LATE)) return 0;
  int f = r->st[e].frame, fnode = f >= 0 ? r->st[f].node : nt_root(c->nt, r->st[e].file);
  const LrDef *d = NULL;
  int o = lr_last_before(c, lr_hist(r, LR_KEY_VIS, fnode, "", 0), elab_point_of(c, node), 0, 0, &d);
  return o == ELAB_YES ? d->vis : o == ELAB_NO ? (f >= 0 ? BVIS_PUBLIC : BVIS_PRIVATE) : 0;
}

/* elab_method_vis_at's answer, also while the record is being made: the
   last version or visibility change of the name along the class's
   ancestors before p */
static int lr_vis_at(Compiler *c, int cls, const char *name, int sg, LoadPoint p) {
  const LrDef *d = NULL;
  return lr_search(c, cls, sg, name, -1, p, p.region < 0, 1, &d, 0) == ELAB_YES && d ? d->vis : -1;
}
int elab_method_vis_at(Compiler *c, int cls, const char *name, int sg, LoadPoint p) {
  return c->lrec && c->lrec->recorded ? lr_vis_at(c, cls, name, sg, p) : -1;
}

int elab_opaque_before(Compiler *c, int cls, LoadPoint p) {
  if (!c->lrec || !c->lrec->recorded) return ELAB_UNKNOWN;
  return lr_last_before(c, lr_hist(c->lrec, LR_KEY_OPAQUE, lr_const_owner(cls), "", 0), p, p.region < 0, 0, NULL);
}

const LrOpen *elab_class_bodies(Compiler *c, int cls, int *n) {
  const LrClass *k = c->lrec ? lr_class(c->lrec, cls) : NULL;
  *n = k ? k->nopen : 0;
  return k ? k->open : NULL;
}

const LrMixin *elab_class_mixins(Compiler *c, int cls, int *n) {
  const LrClass *k = c->lrec ? lr_class(c->lrec, cls) : NULL;
  *n = k ? k->nmix : 0;
  return k ? k->mix : NULL;
}

/* ---- SPINEL_ELAB_CHECK ---- */
int elab_check_on(Compiler *c) { return lr_get(c)->check; }

static LrCheck *lr_family(LoadRecord *r, const char *reader) {
  for (int i = 0; i < LR_CHECK_MAX; i++) {
    if (!r->fam[i].reader) r->fam[i].reader = reader;
    if (r->fam[i].reader == reader || sp_streq(r->fam[i].reader, reader)) return &r->fam[i];
  }
  return &r->fam[LR_CHECK_MAX - 1];
}

static const char *lr_order_name(int o) {
  return o == PROG_BEFORE ? "before" : o == PROG_AFTER ? "after" : o == PROG_SAME ? "same" : "unknown";
}

static void lr_node_desc(Compiler *c, int id, char *buf, size_t sz) {
  const NodeTable *nt = c->nt;
  LoadRecord *r = elab_fresh(c);
  if (id < 0 || id >= r->n) { snprintf(buf, sz, "%d", id); return; }
  snprintf(buf, sz, "%d(%s line %d file %d at #%d)", id, nt_type(nt, id), (int)nt_int(nt, id, "node_line", 0),
           (int)nt_int(nt, id, "node_file", 0), r->at[id]);
}

/* prog_order answered `now` for (a, b), asked by `reader` */
void elab_check_order(Compiler *c, const char *reader, int a, int b, int now) {
  LoadRecord *r = lr_get(c);
  int rec = elab_order(c, a, b);
  LrCheck *f = lr_family(r, reader);
  f->asked++;
  if (rec == now) return;
  f->differ++;
  if (r->shown++ >= LR_CHECK_SHOW) return;
  char da[96], db[96];
  lr_node_desc(c, a, da, sizeof da); lr_node_desc(c, b, db, sizeof db);
  fprintf(stderr, "spinel-elab-check: order reader=\"%s\" a=%s b=%s now=%s record=%s\n",
          reader, da, db, lr_order_name(now), lr_order_name(rec));
}

/* prog_sort put the nodes of v in the order `now`; the record's is `rec` */
void elab_check_sort(Compiler *c, const int *now, const int *rec, int n, const int *node_of) {
  LoadRecord *r = lr_get(c);
  LrCheck *f = lr_family(r, "prog_sort");
  int moved = 0, first = -1;
  for (int i = 0; i < n; i++) if (now[i] != rec[i]) { moved++; if (first < 0) first = i; }
  f->asked++;
  if (!moved) return;
  f->differ++;
  if (r->shown++ >= LR_CHECK_SHOW) return;
  char da[96], db[96];
  lr_node_desc(c, node_of ? node_of[now[first]] : now[first], da, sizeof da);
  lr_node_desc(c, node_of ? node_of[rec[first]] : rec[first], db, sizeof db);
  fprintf(stderr, "spinel-elab-check: sort n=%d moved=%d at=%d now=%s record=%s\n", n, moved, first, da, db);
}

/* The readers the histories will answer for, asked once the record is
   made: class_body_list's bodies of each class against the record's, and
   gvar_seeded_before_read against "a non-nil write before the first read
   the load phase runs". */
static void elab_check_record(Compiler *c) {
  LoadRecord *r = c->lrec;
  int *bci = NULL, *bnode = NULL, nb = class_body_list(c, &bci, &bnode);
  for (int ci = 0; ci < c->nclasses; ci++) {
    int nr = 0, no = 0, same = 1, k = 0;
    const LrOpen *o = elab_class_bodies(c, ci, &no);
    for (int i = 0; i < nb; i++) if (bci[i] == ci && bnode[i] >= 0) nr++;
    if (!nr && !no) continue;
    for (int i = 0; i < nb && same; i++)
      if (bci[i] == ci && bnode[i] >= 0) { while (k < no && o[k].body < 0) k++; same = k < no && o[k++].body == bnode[i]; }
    while (same && k < no && o[k].body < 0) k++;
    same = same && k == no;
    LrCheck *f = lr_family(r, "class_body_list");
    f->asked++;
    if (same) continue;
    f->differ++;
    if (r->shown++ < LR_CHECK_SHOW)
      fprintf(stderr, "spinel-elab-check: bodies class=%s reader=%d record=%d\n", c->classes[ci].name, nr, no);
  }
  free(bci); free(bnode);
  /* the first read of each written global the load phase runs */
  LoadPoint *first = malloc(sizeof *first * (size_t)(r->nhist ? r->nhist : 1));
  if (!first) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < r->nhist; i++) first[i] = (LoadPoint){ -1, -1 };
  for (int id = 0; id < c->nt->count; id++) {
    NodeKind k = nt_kind(c->nt, id);
    if (k != NK_GlobalVariableReadNode && k != NK_GlobalVariableOperatorWriteNode &&
        k != NK_GlobalVariableOrWriteNode && k != NK_GlobalVariableAndWriteNode) continue;
    const char *gn = nt_str(c->nt, id, "name");
    LrHist *h = gn && *gn ? lr_hist(r, -2 - (int)ELAB_GVAR, 0, gn + 1, 0) : NULL;
    LoadPoint q = elab_point_of(c, id);
    if (!h || q.region < 0) continue;
    LoadPoint *m = &first[h - r->hist];
    if (m->region < 0 || elab_cmp(q, *m) == PROG_BEFORE) *m = q;
  }
  for (int i = 0; i < r->nhist; i++) {
    if (r->hist[i].a != -2 - (int)ELAB_GVAR) continue;
    int now = gvar_seeded_before_read(c, r->hist[i].name);
    int rec = elab_written_before(c, ELAB_GVAR, r->hist[i].name, first[i]) == ELAB_YES;
    LrCheck *f = lr_family(r, "gvar_seeded_before_read");
    f->asked++;
    if (now == rec) continue;
    f->differ++;
    if (r->shown++ < LR_CHECK_SHOW)
      fprintf(stderr, "spinel-elab-check: seeded $%s now=%d record=%d\n", r->hist[i].name, now, rec);
  }
  free(first);
}

/* ---- --dump-load-record ---- */
void elab_dump(Compiler *c, FILE *out) {
  LoadRecord *r = elab_fresh(c);
  const NodeTable *nt = c->nt;
  fprintf(out, "load record: %d points, %d regions\n", r->nst, r->nregion);
  for (int e = 0; e < r->nst; e++) {
    const LrStmt *s = &r->st[e];
    int cp = nt_ref(nt, s->node, "constant_path");
    const char *nm = cp >= 0 ? nt_str(nt, cp, "name") : nt_str(nt, s->node, "name");
    fprintf(out, "%*sr%d #%d%s f%d %s%s%s line %d\n", 2 * s->depth, "", s->region, e, s->may ? " may" : "",
            s->file, nt_type(nt, s->node), nm ? " " : "", nm ? nm : "", (int)nt_int(nt, s->node, "node_line", 0));
  }
  if (!r->recorded) return;
  static const char *const kinds[] = { "def", "define_method", "reader", "writer", "alias", "copy", "undef", "remove", "vis", "write", "opaque" };
  static const char *const words[] = { "", "module_function", "public", "private", "protected" };
  static const char *const mixes[] = { "include", "extend", "prepend" };
  static const char *const vars[] = { "$", "main ", "const " };
  for (int ci = LR_MAIN; ci + 1 < r->ncls; ci++) {
    const LrClass *k = lr_class(r, ci);
    if (!k->nopen && !k->nmix) continue;
    fprintf(out, "class %s:", ci == LR_MAIN ? "main" : c->classes[ci].name);
    for (int i = 0; i < k->nopen; i++) fprintf(out, " body#%d", elab_point_of(c, k->open[i].node).seq);
    for (int i = 0; i < k->nmix; i++)
      fprintf(out, " %s %s#%d", mixes[k->mix[i].kind], k->mix[i].mod >= 0 ? c->classes[k->mix[i].mod].name : "?",
              elab_point_of(c, k->mix[i].node).seq);
    fputc('\n', out);
  }
  for (int i = 0; i < r->nhist; i++) {
    const LrHist *h = &r->hist[i];
    if (h->a == LR_KEY_VIS) fprintf(out, "visibility in #%d:", elab_point_of(c, h->b).seq);
    else if (h->a == LR_KEY_OPAQUE) fprintf(out, "opaque %s:", h->b >= 2 ? c->classes[h->b - 2].name : "main");
    else if (h->a < LR_MAIN) fprintf(out, "%s%s%s%s:", vars[-2 - h->a], h->b >= 2 ? c->classes[h->b - 2].name : "",
                                h->b >= 2 ? "::" : h->b == 0 && h->a == -2 - (int)ELAB_CONST ? "?::" : "", h->name);
    else fprintf(out, "%s%s%s:", h->a == LR_MAIN ? "main" : c->classes[h->a].name, h->b ? "." : "#", h->name);
    for (int j = 0; j < h->n; j++) {
      const LrDef *v = &h->v[j];
      fprintf(out, " %s#%d%s%s", kinds[v->kind], elab_point_of(c, v->node).seq, v->may ? " may" : "", v->nil ? " nil" : "");
      if (v->kind == LR_ALIAS && v->target) {   /* the version the alias takes, as the query answers it */
        const LrDef *d = NULL;
        int o = elab_method_at(c, h->a, v->target, h->b, elab_point_of(c, v->node), &d);
        if (o == ELAB_YES && d) fprintf(out, " (%s: %s#%d)", v->target, kinds[d->kind], elab_point_of(c, d->node).seq);
        else fprintf(out, " (%s: %s)", v->target, o == ELAB_NO ? "none" : "unknown");
      }
      if (h->a == LR_KEY_VIS) fprintf(out, " %s", v->vis < 5 ? words[v->vis] : "?");
      else if (v->kind != LR_WRITE && v->kind != LR_OPAQUE && v->vis != SP_VIS_PUBLIC) fprintf(out, " %s", v->vis == SP_VIS_PRIVATE ? "private" : "protected");
    }
    if (h->a >= LR_MAIN)   /* what a query from a method body gets: as of the load phase's end */
      fprintf(out, " end=%s", (const char *[]){ "no", "yes", "unknown" }[elab_method_at(c, h->a, h->name, h->b, (LoadPoint){ -1, -1 }, NULL)]);
    fputc('\n', out);
  }
}

/* The record's visibility of each instance method at the end of the load
   phase against the class's own table (comp_method_vis), which the
   visibility pass fills after the scope walk: asked as the compile ends,
   while the classes are still there (comp_free) */
static void elab_check_visibility(Compiler *c) {
  LoadRecord *r = c->lrec;
  for (int i = 0; i < r->nhist; i++) {
    const LrHist *h = &r->hist[i];
    if (h->a < 0 || h->b || h->a >= c->nclasses) continue;
    int rec = elab_method_vis_at(c, h->a, h->name, 0, (LoadPoint){ -1, -1 }), now = comp_method_vis(&c->classes[h->a], h->name);
    if (rec < 0) continue;
    LrCheck *f = lr_family(r, "method visibility");
    f->asked++;
    if (rec == now) continue;
    f->differ++;
    if (r->shown++ < LR_CHECK_SHOW)
      fprintf(stderr, "spinel-elab-check: visibility %s#%s now=%d record=%d\n", c->classes[h->a].name, h->name, now, rec);
  }
  /* ...and of each method a class has only from a module it includes,
     against the visibility the class table finds up the class's chain */
  for (int ci = 0; ci < c->nclasses && ci + 1 < r->ncls; ci++) {
    const LrClass *k = lr_class(r, ci);
    for (int j = 0; j < k->nmix; j++) {
      if (k->mix[j].mod < 0 || k->mix[j].kind == LR_EXTEND) continue;
      for (int i = 0; i < r->nhist; i++) {
        const LrHist *h = &r->hist[i];
        if (h->a != k->mix[j].mod || h->b || lr_hist(r, ci, 0, h->name, 0)) continue;
        int rec = elab_method_vis_at(c, ci, h->name, 0, (LoadPoint){ -1, -1 }), now = comp_method_vis_in_chain(c, ci, h->name);
        if (rec < 0) continue;
        LrCheck *f = lr_family(r, "method visibility through a mixin");
        f->asked++;
        if (rec == now) continue;
        f->differ++;
        if (r->shown++ < LR_CHECK_SHOW)
          fprintf(stderr, "spinel-elab-check: visibility %s#%s (from %s) now=%d record=%d\n", c->classes[ci].name, h->name,
                  c->classes[h->a].name, now, rec);
      }
    }
  }
}

void elab_free(Compiler *c) {
  LoadRecord *r = c->lrec;
  if (!r) return;
  if (r->check && r->recorded) elab_check_visibility(c);
  if (r->builds && sp_timing_on()) {
    char extra[64];
    snprintf(extra, sizeof extra, " builds=%d", r->builds);
    sp_timing_end(sp_timing_now() - r->ms, "an_elab_points", extra);
  }
  for (int i = 0; r->check && i < LR_CHECK_MAX && r->fam[i].reader; i++)
    fprintf(stderr, "spinel-elab-check: family=\"%s\" asked=%ld differ=%ld\n", r->fam[i].reader, r->fam[i].asked, r->fam[i].differ);
  for (int i = 0; i < r->nhist; i++) {
    for (int j = 0; j < r->hist[i].n; j++) free((char *)r->hist[i].v[j].target);
    free(r->hist[i].name); free(r->hist[i].v);
  }
  for (int i = 0; i < r->ncls; i++) { free(r->cls[i].open); free(r->cls[i].mix); }
  free(r->hist); free(r->hhead); free(r->cls);
  free(r->st); free(r->at); free(r->bits); free(r);
  c->lrec = NULL;
}
