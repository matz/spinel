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
#include "implicit_calls.h"
#include "timing.h"
#include <limits.h>

int g_dump_load_record = 0;

/* One walked statement. top is the index of the top-level statement (or
   file root) holding it, and rank, on that one, its place in the order
   prog_order reads: each file's statements in their list order, a placed
   file's right after its load (a BEGIN block keeps its place in the list
   there; the walk runs it first). -1 outside region 0. last is the last
   statement of the bodies it runs, which run before it completes. partial:
   it runs at once if at all, but maybe not at all or cut short (in an
   iterator's block, which may run no time, or a begin body with rescue
   clauses, where a raise skips the rest): a write there is on no path for
   sure. rep: the nearest statement around it whose body may run more than
   once (a loop, an iterator's block, a begin body a rescue may retry), -1
   for none: a statement after it in that body may also run before it.
   fmay, fpartial: may and partial counted from the frame (ElabPos): a
   class body opened in an iterator's block runs its statements whole.
   list: the statement list it is an element of (a StatementsNode; -1 for
   a lone expression), up: the statement whose body holds it (-1 at a
   file's top level), which lr_may_dominates climbs. kept: in a block a
   call may keep to run later (ElabPos.kept). runs: it may run code of the
   program's (lr_run_points); nrun: how many statements before it
   do. */
typedef struct { int node, file, region, top, rank, depth, frame, last, rep; unsigned char may, partial, fmay, fpartial, kept; int list, up, nrun; unsigned char runs; } LrStmt;

/* The order questions SPINEL_ELAB_CHECK counts, by reader */
typedef struct { const char *reader; long asked, differ; } LrCheck;
enum { LR_CHECK_MAX = 32, LR_CHECK_SHOW = 100 };

enum { LR_LATE = 1, LR_BODY = 2 };   /* runs later: in a method body, a lambda or a block kept to run later; in a method body */

/* A def of the program's, by its name (LoadRecord.defs) */
typedef struct { const char *name; int node; } LrSite;

/* Flat constant slots: first plain write, earliest other write, and the
   earliest sure/possible completions in each load region. */
typedef struct LrSlot { const char *name; int first, next, points, other; LoadPoint done, from; } LrSlot;
typedef struct LrSlotPoint { int region, sure, any, next; } LrSlotPoint;

struct LoadRecord {
  LrStmt *st; int nst, cst;  /* the walked statements, in the order they run: seq is the index */
  int *at;                   /* [n] each node's statement; -1 under no file's root */
  unsigned char *bits;       /* [n] LR_* */
  int n, ncap, nregion;
  struct ElabWalk *walk;     /* the registration's walk, while it runs (elab_walk_enter) */
  unsigned ver;              /* the node table the map is of */
  int check, shown;
  LrCheck fam[LR_CHECK_MAX];
  double ms; int builds;
  /* the call plan's frame (elab_plan_enter): the map one build made, swapped
     in while call_target asks its questions, with the parents it climbed */
  struct LrPlan { LrStmt *st; int nst, cst, *at; unsigned char *bits; int n, ncap; } plan;
  int *up;                   /* [n] the parent each node's point was climbed from */
  int *plan_par, plan_depth;
  unsigned char plan_made;
  /* the histories (elab_program) */
  int recorded;
  int frontier;              /* the statements recorded so far, in seq order: while the walk records, a query at p is complete for p.seq <= frontier (lr_ready) */
  struct ElabRec *rec;       /* the record's state while elab_program makes it */
  struct HostStep *host;     /* ...and the host a walk-time form splices through (elab_walk_host) */
  struct LrHist *hist; int nhist, chist, *hhead;
  struct LrClass *cls; int ncls;
  /* [ncls] the classes the record holds: those the walk made and the class
     builders' (elab_record_builders). One registration makes otherwise
     after the walk (an object's singleton class, which its extends and
     defs fill) has methods the record never saw, and no entry (lr_class). */
  unsigned char *held;
  char **names; int nnames, cnames;     /* the names the program defines methods by, sorted (lr_names_take) */
  LrSite *defs; int ndefs;              /* ...and its defs, sorted by name */
  unsigned char callbacks;              /* lr_callbacks_define's memo: 1 no, 2 yes */
  char **reopens; int nreopens, creopens;   /* the builtin classes and modules a header of the program reopens, sorted (lr_reopens_take) */
  unsigned char names_any;              /* ...and one it defines by a name the record cannot read */
  unsigned char structs;     /* registration has made the class builders' classes (elab_record_builders) */
  int builders;              /* the builder blocks recorded before their class was made (lr_frame) */
  /* the method histories by name (lr_vers_index), as of nentries entries */
  long nentries, indexed;
  unsigned char *clsmod; int clsmod_all; struct { int nhist, ncls, recorded; unsigned ver; } clsmod_of;   /* lr_class_object_modules, and the record state it is of */
  int *mixed, nmixed; struct { int nhist, ncls, recorded; unsigned ver; } mixed_of;   /* lr_mixed */
  const LrDef **cand; int ncand, ccand, cand_bad; unsigned char collect;   /* elab_class_candidates: the writes lr_last_before passes */
  long opened;               /* LrHist.open is of this many entries (lr_open_make), -1 for none made */
  struct LrSettle *set; int nset, cset;   /* the answers recorded ahead of a later region, to recompute (lr_settle_regions) */
  unsigned char unseen;      /* elab_runs_unseen's memo: 1 no, 2 yes */
  unsigned char consts;      /* lr_const_seen's memo: 1 the program makes constants only as it writes them, 2 not */
  struct { int node; char *name; } *srcw; int nsrcw;   /* the constant writes the source has, before any pass (elab_note_const_writes) */
  LrSlot *slot; int nslot, cslot, *slot_head, slot_builds;
  LrSlotPoint *slotp; int nslotp, cslotp; long slot_entries;
  int runs_of, nrun, *rend;  /* lr_run_points: the build its bits are of, how many statements run code, each region's end */
  int *rinst, *urun;         /* ...from which statement on an object of each class may exist, each def's body may run (lr_rta) */
  unsigned char defs_computed;          /* ...and whether the program makes or copies a method by a computed name */
  struct LrVers *vers; int nvers, cvers, *vers_head;
  int follow;                /* the aliases lr_class_of is following, to cap a chain */
  struct LrHeader *hdr; int nhdr, chdr, *hdr_head;   /* the class and module headers, in the order the walk reaches them (elab_header_class), and the class builders' calls and writes noted ahead of the walk (elab_builder_note) */
  ElabSurface *surf; int *surf_at;   /* elab_class_surface's, by class side: [2 * (cls + 1) + sg] .. [+ 1] */
  int *dscope, ndscope;      /* [ndscope] by node: the method scope the scope walk made of a def or define_method (elab_def_scope), 0 for none */
  int core[CORE_N];          /* every header with a core name as it registers (elab_header_opened), its index + 1; 0 before one; only Object, BasicObject, Module and Kernel are read (lr_core) */
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

/* reg: the scope walk registers each statement as the record reaches it
   (elab_program); otherwise the record alone is rebuilt (elab_fresh) */
typedef struct ElabWalk { Compiler *c; LoadRecord *r; unsigned char *entered; int region, rank, reg, share; } ElabWalk;

/* The map covers nodes [0, n): the ones made since join unplaced */
static void lr_fit(LoadRecord *r, int n) {
  if (n > r->ncap) {
    int cap = r->ncap ? r->ncap : 256;
    while (cap < n) cap *= 2;
    r->at = realloc(r->at, sizeof(int) * (size_t)cap);
    r->bits = realloc(r->bits, (size_t)cap);
    if (!r->at || !r->bits) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    r->ncap = cap;
  }
  for (int id = r->n; id < n; id++) { r->at[id] = -2; r->bits[id] = 0; }
  if (n > r->n) r->n = n;
}

static void lr_record_upto(LoadRecord *r, int n);
static void lr_check_map(Compiler *c, const int *was, const unsigned char *wbits);
static void lr_check_class_ref(Compiler *c, int node, int ci);
static int lr_class_ref_at(Compiler *c, int node, int *core);

/* Statement `node` joins the record where the walk stands, *pos; pos then
   stands on its path (EP_SPINE), as statement e. Under the registration's
   walk, the statements before it are recorded first: the walk has passed
   them, and registered what they define. Its last statement is not known
   until the walk leaves it (elab_walk_leave): until then it completes
   after any point. */
static int lr_add(ElabWalk *w, int node, ElabPos *pos) {
  LoadRecord *r = w->r;
  if (w->reg) lr_record_upto(r, r->nst);
  if (r->nst == r->cst) {
    r->cst = r->cst ? r->cst * 2 : 256;
    r->st = realloc(r->st, sizeof *r->st * (size_t)r->cst);
    if (!r->st) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  int k = r->nst++;
  r->st[k] = (LrStmt){ node, pos->file, w->region, pos->top < 0 ? k : pos->top, -1, pos->depth, pos->frame, w->reg ? INT_MAX : k, pos->rep, pos->may, pos->partial, pos->fmay, pos->fpartial, pos->kept, pos->list, pos->e };
  r->at[node] = k;
  pos->e = k; pos->top = r->st[k].top; pos->kind = EP_SPINE;
  return k;
}

static int lr_defines(const LoadRecord *r, const char *name);
static int lr_literal_kind(NodeKind k);
static int lr_lazy_later(Compiler *c, int call);

/* When the block of `call` runs, from the syntax alone (the receiver is
   not typed yet): ELAB_BLK_NOW before the call answers, for certain: a
   builtin iterator's (the iterator rows) on a literal of a builtin class
   the program reopens none of; ELAB_BLK_LATER not now, at a time the
   syntax does not tell (is_deferred_block_call: a Proc, a method body, a
   thread, at_exit, ...; with lazy, a builtin lazy chain's whose value
   leaves the expression unforced, lr_lazy_later); ELAB_BLK_ASSUMED taken
   to run before the call answers, as an iterator's name, class_eval and
   kin and a class builder's `Class.new(B) do` read, though a method of
   the program's the call reaches may keep the block (whether it does is
   the typed call target's fact); ELAB_BLK_UNKNOWN for any other call's (a
   program method may yield to it, or keep it). The record places a block
   by its answer with lazy 0: the forms is_deferred_block_call names run it
   later than their statement whatever the program does, and their calls
   see every def of the record. */
static int elab_block_when(Compiler *c, int call, int lazy) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_kind(nt, call) == NK_CallNode ? nt_str(nt, call, "name") : NULL;
  int rv = nm ? nt_ref(nt, call, "receiver") : -1;
  const char *rn = rv >= 0 && nt_kind(nt, rv) == NK_ConstantReadNode ? nt_str(nt, rv, "name") : NULL;
  if (!nm) return ELAB_BLK_UNKNOWN;
  if (is_deferred_block_call(rn, nm) || (lazy && lr_lazy_later(c, call))) return ELAB_BLK_LATER;
  if (is_eval_exec_family(nm) || is_class_builder(rn, nm)) return ELAB_BLK_ASSUMED;
  if (!iter_row_named(nm)) return ELAB_BLK_UNKNOWN;
  return rv >= 0 && c->lrec && !c->lrec->nreopens && lr_literal_kind(nt_kind(nt, rv)) ? ELAB_BLK_NOW : ELAB_BLK_ASSUMED;
}

/* When the block of `node` (a call's, or an END's body, run at exit)
   runs: elab_block_when with the lazy chain. The record places a lazy
   chain's block by its call's name (lazy 0) and an END body at its
   statement: that a block does not run now does not tell that it runs
   after a later def (the chain may be forced before it, the program may
   exit before it), so the record binds no later def to its calls; only
   a run-time check can. */
int elab_block_runs(Compiler *c, int node) {
  return nt_kind(c->nt, node) == NK_PostExecutionNode ? ELAB_BLK_LATER : elab_block_when(c, node, 1);
}

/* Is call a builtin lazy enumerator's whose block runs only once the chain
   is forced: a lazy-returning call (is_lazy_chain_name) on a chain of them
   from a `lazy` the program does not define, whose value leaves the
   expression with no call forcing it there (`.first`, `.to_a`, `.each { }`:
   any other name taking it as receiver)? Climbed by the value edges
   (vp_climb: parentheses, `begin`, a branch, a write's value), it leaves
   as a statement's dropped value, an argument handed on, a jump's value or
   a method's answer; any other use keeps the reading that it runs now.
   A chain name the program defines somewhere (a reopened
   Enumerator::Lazy#map, say) may run the block as it is called, so the
   chain is not known to be later either. */
static int lr_lazy_chain_call(const LoadRecord *r, const char *name) {
  return is_lazy_chain_name(name) && (!r || !lr_defines(r, name));
}
static int lr_lazy_later(Compiler *c, int call) {
  const NodeTable *nt = c->nt;
  const LoadRecord *r = c->lrec;
  int lz = 0;
  if (!lr_lazy_chain_call(r, nt_str(nt, call, "name"))) return 0;
  for (int v = nt_ref(nt, call, "receiver"); !lz && v >= 0 && nt_kind(nt, v) == NK_CallNode; v = nt_ref(nt, v, "receiver")) {
    const char *nm = nt_str(nt, v, "name");
    if (is_lazy_name(nm)) lz = !r || !lr_defines(r, nm) ? 1 : -1;
    else if (!lr_lazy_chain_call(r, nm)) return 0;
  }
  if (lz != 1) return 0;
  const int *par = comp_parent_map(c);
  for (int v = call;;) {
    VpUp up;
    VpUpKind k = vp_climb(nt, par, v, VPS_ALL | VP_WRITES, &up);
    if (k == VPU_DROPPED) return 1;
    if (k != VPU_ENDED) return 0;   /* (a subtree a pass moved out of the tree too: the parent map names its old place) */
    NodeKind pk = nt_kind(nt, up.parent);
    if (pk == NK_CallNode && nt_ref(nt, up.parent, "receiver") == up.node) {
      if (!lr_lazy_chain_call(r, nt_str(nt, up.parent, "name"))) return 0;
      v = up.parent;
      continue;
    }
    return pk == NK_ArgumentsNode || pk == NK_ProgramNode || pk == NK_ReturnNode || pk == NK_NextNode || pk == NK_BreakNode || vp_up_def(nt, &up) >= 0;
  }
}

/* Does the block of `call` run as a class body (a frame): class_eval and
   the other eval and exec forms, a class builder's block? */
static int elab_opens_frame(const NodeTable *nt, int call) {
  const char *nm = nt_str(nt, call, "name");
  int rv = nt_ref(nt, call, "receiver");
  const char *rn = rv >= 0 && nt_kind(nt, rv) == NK_ConstantReadNode ? nt_str(nt, rv, "name") : NULL;
  return nm && (is_eval_exec_family(nm) || is_class_builder(rn, nm));
}

/* A body that runs where it stands, at q: a statement list, a lone
   expression, which is a statement, or a body with rescue clauses (a
   BeginNode), whose parts are on the statement's path */
static ElabPos elab_body_pos(const NodeTable *nt, int b, ElabPos q) {
  NodeKind k = nt_kind(nt, b);
  if (k == NK_BeginNode) { q.kind = EP_SPINE; return q; }
  q.depth++;
  q.kind = k == NK_StatementsNode ? EP_LIST : EP_NEW;
  q.list = -1;
  return q;
}

/* A written value on the statement's path: a call's (its block), a
   require's (its file), or the parts of `x rescue y` */
static ElabPos elab_value_pos(const NodeTable *nt, int v, ElabPos q) {
  NodeKind k = nt_kind(nt, v);
  if (k == NK_CallNode || k == NK_RescueModifierNode || k == NK_LoadNode) q.kind = EP_SPINE;
  return q;
}

/* Where the load phase stands at child ch of node p, which stands at pp.
   On a statement's path: a class body, and the block of a call that runs
   it as one (class_eval and kin, a class builder), is a frame of its own,
   which the statement opened; a branch, a loop's body and a rescue clause
   may not run, and the next arm of an elsif ladder or a rescue chain is
   on the path; a branch whose condition is a constant (`x if true`,
   nt_pred_const, which the scope walk folds too) runs whenever the
   condition is reached, and the arms after one that always runs never do,
   which the record keeps as may not run; a value written under `||=` /
   `&&=` or after `rescue` may not run, as the right side of `&&` and `||`
   does; a block run at once by a call the record does not know (a program
   method may yield to it, or keep it) may not run, and an iterator's may
   run partly (zero times, or several). */
ElabPos elab_child(Compiler *c, int p, int ch, const ElabPos *pp) {
  const NodeTable *nt = c->nt;
  ElabPos q = *pp;
  NodeKind k = nt_kind(nt, p);
  q.kind = EP_NODE; q.defer = 0; q.sure = 0;
  if (pp->e < 0 || ch < 0) return q;
  if (k == NK_DefNode) q.bits |= LR_LATE | LR_BODY;
  else if (k == NK_LambdaNode || (k == NK_BlockNode && pp->defer)) q.bits |= LR_LATE;
  if (pp->kind == EP_LIST) { q.kind = EP_NEW; q.list = p; return q; }
  int spine = pp->kind == EP_SPINE;
  if (!spine && k != NK_CallNode) return q;
  switch (k) {
    case NK_ClassNode: case NK_ModuleNode: case NK_SingletonClassNode:
      if (spine && ch == nt_ref(nt, p, "body")) { q.frame = pp->e; q.fmay = q.fpartial = 0; return elab_body_pos(nt, ch, q); }
      break;
    case NK_PreExecutionNode: case NK_EnsureNode:
      if (spine && ch == nt_ref(nt, p, "statements")) return elab_body_pos(nt, ch, q);
      break;
    case NK_ParenthesesNode: case NK_BlockNode:
      if (spine && ch == nt_ref(nt, p, "body")) return elab_body_pos(nt, ch, q);
      break;
    case NK_IfNode: case NK_UnlessNode: case NK_ElseNode: case NK_WhenNode: case NK_InNode: case NK_RescueNode:
    case NK_WhileNode: case NK_UntilNode: case NK_ForNode: {
      /* a condition's constant predicate: whether its statements, and its
         else, run whenever the condition is reached */
      int pc = k == NK_IfNode || k == NK_UnlessNode ? nt_pred_const(nt, nt_ref(nt, p, "predicate")) : -1;
      int then_runs = k == NK_IfNode ? pc == 1 : k == NK_UnlessNode ? pc == 0 : k == NK_ElseNode && pp->sure;
      if (spine && ch == nt_ref(nt, p, "statements")) {
        if (!then_runs) q.may = q.fmay = 1;
        if (k == NK_WhileNode || k == NK_UntilNode || k == NK_ForNode) q.rep = pp->e;
        return elab_body_pos(nt, ch, q);
      }
      if (spine && (((k == NK_IfNode || k == NK_RescueNode) && ch == nt_ref(nt, p, "subsequent")) ||
                    (k == NK_UnlessNode && ch == nt_ref(nt, p, "else_clause")))) {
        q.kind = EP_SPINE;
        /* the next arm is reached only when this one's predicate fails: for
           sure where it is a constant false; an else then runs */
        int falls = k == NK_IfNode ? pc == 0 : k == NK_UnlessNode && pc == 1;
        if (!falls && k != NK_RescueNode) q.may = q.fmay = 1;
        q.sure = falls && nt_kind(nt, ch) == NK_ElseNode;
      }
      break;
    }
    case NK_CaseNode: case NK_CaseMatchNode:   /* its branches and its else */
      if (spine && ch != nt_ref(nt, p, "predicate")) q.kind = EP_SPINE;
      break;
    case NK_BeginNode:
      if (spine && ch == nt_ref(nt, p, "statements")) {
        q.partial |= nt_ref(nt, p, "rescue_clause") >= 0;
        q.fpartial |= nt_ref(nt, p, "rescue_clause") >= 0;
        if (nt_ref(nt, p, "rescue_clause") >= 0) q.rep = pp->e;   /* `retry` runs it again */
        return elab_body_pos(nt, ch, q);
      }
      if (spine) q.kind = EP_SPINE;   /* its rescue clauses, else and ensure */
      break;
    case NK_ConstantWriteNode: case NK_ConstantPathWriteNode: case NK_LocalVariableWriteNode:
    case NK_InstanceVariableWriteNode: case NK_GlobalVariableWriteNode: case NK_ClassVariableWriteNode:
      if (spine && ch == nt_ref(nt, p, "value")) return elab_value_pos(nt, ch, q);
      break;
    case NK_AndNode: case NK_OrNode:   /* the right side runs only if the left decides so: a lone expression, which may not run */
      if (spine && ch == nt_ref(nt, p, "right")) { q.may = q.fmay = 1; return elab_body_pos(nt, ch, q); }
      break;
    case NK_ConstantOrWriteNode: case NK_ConstantPathOrWriteNode: case NK_LocalVariableOrWriteNode:
    case NK_InstanceVariableOrWriteNode: case NK_GlobalVariableOrWriteNode: case NK_ClassVariableOrWriteNode:
    case NK_ConstantAndWriteNode: case NK_ConstantPathAndWriteNode: case NK_LocalVariableAndWriteNode:
    case NK_InstanceVariableAndWriteNode: case NK_GlobalVariableAndWriteNode: case NK_ClassVariableAndWriteNode:
      if (spine && ch == nt_ref(nt, p, "value")) { q.may = q.fmay = 1; return elab_value_pos(nt, ch, q); }
      break;
    case NK_RescueModifierNode:
      if (!spine) break;
      if (ch == nt_ref(nt, p, "rescue_expression")) q.may = q.fmay = 1;
      return elab_value_pos(nt, ch, q);
    case NK_CallNode: {
      if (ch != nt_ref(nt, p, "block") || nt_kind(nt, ch) != NK_BlockNode) break;
      int runs = elab_block_when(c, p, 0);
      if (runs == ELAB_BLK_LATER) { q.defer = 1; break; }
      /* An immediate block keeps its call's point in an expression too;
         only a call that may keep the block leaves its body unplaced. */
      if (!spine) { q.defer = runs == ELAB_BLK_UNKNOWN; break; }
      q.kind = EP_SPINE;
      if (runs == ELAB_BLK_UNKNOWN) q.may = q.fmay = q.kept = 1;
      if (elab_opens_frame(nt, p)) { q.frame = pp->e; q.fmay = q.fpartial = 0; }
      else { q.partial = q.fpartial = 1; q.rep = pp->e; }   /* an iterator's: zero times, or several */
      break;
    }
    default: break;
  }
  return q;
}

/* Does ref `ref` of node id run after the node's arrays: a case's else,
   after its branches, and a begin's ensure, after its else? (The bridge
   keeps Prism's field order, which lists them earlier.) */
int elab_ref_last(const NodeTable *nt, int id, int ref) {
  NodeKind k = nt_kind(nt, id);
  return ref >= 0 && (((k == NK_CaseNode || k == NK_CaseMatchNode) && ref == nt_ref(nt, id, "else_clause")) ||
                      (k == NK_BeginNode && ref == nt_ref(nt, id, "ensure_clause")));
}

/* Under the registration's walk, node id of the statement pos stands in
   takes that statement's point at its first visit, for a query while the
   walk runs; the climb places it again after the walk (elab_points_walk).
   0 for a node of a statement's own visited before, whose subtree is. */
static int lr_place(LoadRecord *r, int id, const ElabPos *pos) {
  if (r->at[id] == -2) { r->at[id] = pos->e; r->bits[id] = pos->bits; return 1; }
  return pos->kind != EP_NODE;
}

static void elab_file(ElabWalk *w, int f, const ElabPos *site);

/* The file a require at `id`, standing at pos, places (prog_site_places),
   entered where the walk is: its statements follow the require's and share
   the rank of the top-level statement holding it. */
static void elab_site_file(ElabWalk *w, int id, const ElabPos *pos) {
  int t = prog_site_places(w->c, id);
  if (t < 0 || w->entered[t]) return;
  int share = w->share;
  w->share = 1;
  elab_file(w, t, pos);
  w->share = share;
}
/* ...as the scope walk reaches it, in the registration's walk */
void elab_walk_site(Compiler *c, int id, const ElabPos *pos) {
  LoadRecord *r = c->lrec;
  if (r && r->walk && pos->kind != EP_NODE) elab_site_file(r->walk, id, pos);
}

/* The statements at node id, which stands at pos, in the order they run:
   id itself when pos makes it one (a subtree two lists share joins once),
   then those its parts lead to (elab_child) */
static void elab_walk(ElabWalk *w, int id, ElabPos pos) {
  const NodeTable *nt = w->c->nt;
  int e = -1;
  if (id < 0) return;
  if (pos.kind == EP_NEW) {
    if (w->r->at[id] != -2) return;
    e = lr_add(w, id, &pos);
  }
  else if (w->reg && !lr_place(w->r, id, &pos)) return;
  int nr = nt_num_refs(nt, id), na = nt_num_arrs(nt, id);
  for (int pass = 0; pass < 3; pass++) {   /* the refs, the arrays, the refs that run last */
    for (int i = 0; pass != 1 && i < nr; i++) {
      int ch = nt_ref_at(nt, id, i);
      if (elab_ref_last(nt, id, ch) != (pass == 2)) continue;
      ElabPos q = elab_child(w->c, id, ch, &pos);
      if (q.kind != EP_NODE || w->reg) elab_walk(w, ch, q);
    }
    for (int i = 0; pass == 1 && i < na; i++) {
      int m = 0;
      const int *v = nt_arr_at(nt, id, i, &m);
      for (int j = 0; j < m; j++) {
        ElabPos q = elab_child(w->c, id, v[j], &pos);
        if (q.kind != EP_NODE || w->reg) elab_walk(w, v[j], q);
      }
    }
  }
  if (!w->reg && nt_kind(nt, id) == NK_LoadNode && pos.kind != EP_NODE) elab_site_file(w, id, &pos);
  if (e >= 0) w->r->st[e].last = w->r->nst - 1;
}

/* Under the registration's walk (elab_program), the scope walk enters
   node ch at *pos: a statement joins the record (lr_add) and its index is
   returned, for elab_walk_leave; any other node takes its statement's
   point (lr_place); a subtree two lists share, entered again, is entered
   with the record off (e -1, as a walk outside it). */
int elab_walk_enter(Compiler *c, int ch, ElabPos *pos) {
  LoadRecord *r = c->lrec;
  if (!r || !r->walk || ch < 0 || (pos->e < 0 && pos->kind != EP_NEW)) return -1;
  lr_fit(r, c->nt->count);
  if (pos->kind != EP_NEW) { lr_place(r, ch, pos); return -1; }
  if (r->at[ch] == -2) return lr_add(r->walk, ch, pos);
  pos->kind = EP_NODE; pos->e = -1;
  return -1;
}

/* ...and leaves it: statement e completes with the last one its bodies ran */
void elab_walk_leave(Compiler *c, int e) {
  if (e >= 0) c->lrec->st[e].last = c->lrec->nst - 1;
}

/* The host a form the registration's walk runs splices through, while
   the walk runs; NULL otherwise */
struct HostStep *elab_walk_host(Compiler *c) {
  LoadRecord *r = c->lrec;
  return r && r->walk ? r->host : NULL;
}

/* A form at statement node old, which the walk has entered and has not
   passed (nothing recorded of it yet), spliced node with in its place:
   with takes old's statement, at old's point; the statements spliced
   after it join as the walk reaches them, at its position. 0 when old is
   no such statement. */
int elab_walk_replace(Compiler *c, int old, int with) {
  LoadRecord *r = c->lrec;
  if (!r || !r->walk || old < 0 || old >= r->n || with < 0) return 0;
  int k = r->at[old];
  if (k < r->frontier || k >= r->nst || r->st[k].node != old) return 0;
  lr_fit(r, c->nt->count);
  r->st[k].node = with;
  r->at[with] = k;
  r->at[old] = -2;
  return 1;
}

/* The statement list statement node is an element of (LrStmt.list), and
   in *frame the header of the class or module body around it (-1: main's
   top level); -1 when node is no statement of the record */
int elab_stmt_list(Compiler *c, int node, int *frame) {
  LoadRecord *r = c->lrec;
  int k = r && node >= 0 && node < r->n ? r->at[node] : -1;
  if (k < 0 || k >= r->nst || r->st[k].node != node) return -1;
  if (frame) *frame = r->st[k].frame >= 0 ? r->st[r->st[k].frame].node : -1;
  return r->st[k].list;
}

/* The statements under node id, which stands at pos, that the scope walk
   does not enter (a part it handles wholesale), joined as elab_walk joins
   them */
void elab_walk_rest(Compiler *c, int id, ElabPos pos) {
  LoadRecord *r = c->lrec;
  if (!r || !r->walk || id < 0 || (pos.e < 0 && pos.kind != EP_NEW)) return;
  lr_fit(r, c->nt->count);
  elab_walk(r->walk, id, pos);
}

/* The parts of node p, which stands at *pp, each where elab_child puts
   it, its refs and then its arrays: fn(ud, ch, q) answers whether to go
   into ch, from q, which it may change. A part that is the statement's own
   (EP_NODE) may not run where it would not on the statement's path (a
   branch of `c ? a : b`, the right side of `&&`): it takes the may, fmay,
   partial and fpartial the path would give it. */
typedef int (*ElabPartFn)(void *ud, int ch, ElabPos *q);
static void elab_parts(Compiler *c, int p, const ElabPos *pp, ElabPartFn fn, void *ud) {
  const NodeTable *nt = c->nt;
  int nr = nt_num_refs(nt, p), na = nt_num_arrs(nt, p);
  for (int i = 0; i < nr + na; i++) {
    int m = 1, one = i < nr ? nt_ref_at(nt, p, i) : -1;
    const int *v = i < nr ? &one : nt_arr_at(nt, p, i - nr, &m);
    for (int j = 0; j < m; j++) {
      if (v[j] < 0) continue;
      ElabPos q = elab_child(c, p, v[j], pp);
      if (q.kind == EP_NODE) {
        ElabPos on = *pp;
        on.kind = EP_SPINE;
        on = elab_child(c, p, v[j], &on);
        q.may = on.may; q.fmay = on.fmay; q.partial = on.partial; q.fpartial = on.fpartial;
      }
      if (fn(ud, v[j], &q)) elab_parts(c, v[j], &q, fn, ud);
    }
  }
}

/* The requires under `stmt` (a top-level statement, or the tree of one),
   and whether the load phase's walk reaches each where it stands -- not in
   a method, a lambda or a block kept to run later, and not in a part it
   does not enter (an argument, an operand): fn(ud, load, in_place). The
   same elab_child the walk asks, from a position made up for the
   statement; a lone statement's parts are on its path. */
typedef struct { const NodeTable *nt; void (*fn)(void *, int, int); void *ud; } ElabSites;
static int elab_site_part(void *ud, int id, ElabPos *q) {
  const ElabSites *s = ud;
  if (q->kind == EP_NEW) q->kind = EP_SPINE;
  if (nt_kind(s->nt, id) == NK_LoadNode) s->fn(s->ud, id, q->kind != EP_NODE && !(q->bits & LR_LATE));
  return 1;
}
void elab_load_sites(Compiler *c, int stmt, void (*fn)(void *ud, int load, int in_place), void *ud) {
  ElabPos at = { .e = 0, .top = -1, .frame = -1, .rep = -1, .list = -1, .kind = EP_NEW };
  ElabSites s = { c->nt, fn, ud };
  if (stmt >= 0 && elab_site_part(&s, stmt, &at)) elab_parts(c, stmt, &at, elab_site_part, &s);
}

/* File f's top level, entered at its load: its root, its BEGIN blocks (run
   first, when the file loads), then its statements, each placed file right
   after the statement that loads it. Under the registration's walk, the
   scope walk enters each statement in the file's scope, as it runs. A file
   a nested require places (prog_site_places) is entered where the walk
   reaches that require, in the region it stands in: its statements share
   the rank of the top-level statement holding the require (w->share), and
   run as the require does (`site`: it may not run, or run partly). */
static void elab_file(ElabWalk *w, int f, const ElabPos *site) {
  Compiler *c = w->c;
  const NodeTable *nt = c->nt;
  int root = nt_root(nt, f), st = prog_root_stmts(c, f), n = 0, sc = c->file_scope[f];
  w->entered[f] = 1;
  if (root < 0 || w->r->at[root] != -2) return;
  if (w->reg) {   /* the file's root and statement list, as a walk of the root claims them */
    c->nscope[root] = sc; c->node_cbody[root] = -1;
    if (st >= 0) { c->nscope[st] = sc; c->node_cbody[st] = -1; }
  }
  ElabPos at = { .e = -1, .file = f, .top = -1, .frame = -1, .rep = -1, .list = -1, .kind = EP_NEW }, rp = at;
  at.list = st;   /* the top-level statements are the elements of the root's list */
  if (site) { at.may = rp.may = at.fmay = rp.fmay = site->may; at.partial = rp.partial = at.fpartial = rp.fpartial = site->partial; }
  int e = lr_add(w, root, &rp);
  w->r->st[e].last = e;
  w->r->st[e].rank = w->region == 0 ? (w->share ? w->rank : w->rank++) : -1;
  if (st >= 0) w->r->at[st] = e;
  /* the list is read again at each step: a form the walk runs splices
     into it, and what it puts after a statement is reached next */
  for (int k = 0;; k++) {
    const int *vs = st >= 0 ? nt_arr(nt, st, "body", &n) : NULL;
    if (!vs || k >= n) break;
    int v = vs[k];
    if (v < 0 || nt_kind(nt, v) != NK_PreExecutionNode) continue;
    if (w->reg) walk_scope_stmt(c, v, k, sc, at);
    else elab_walk(w, v, at);
  }
  for (int k = 0;; k++) {
    const int *vs = st >= 0 ? nt_arr(nt, st, "body", &n) : NULL;
    if (!vs || k >= n) break;
    int v = vs[k];
    if (v < 0) continue;
    if (nt_kind(nt, v) != NK_PreExecutionNode) {
      int seen = w->r->at[v] != -2;   /* a statement two lists share joins once */
      if (w->reg) walk_scope_stmt(c, v, k, sc, at);
      if (seen) continue;
      if (!w->reg) elab_walk(w, v, at);
    }
    int s = w->r->at[v];
    if (s >= 0 && w->r->st[s].top == s) w->r->st[s].rank = w->region == 0 ? (w->share ? w->rank : w->rank++) : -1;
    int t = prog_ordered_load(c, v);
    if (t >= 0 && !w->entered[t]) elab_file(w, t, site);
  }
}

/* Is a node under parent p one that runs later than p's statement: under
   a method, a lambda, or a block kept to run later (elab_block_when)? */
static int lr_defers(Compiler *c, const int *par, int p) {
  NodeKind k = nt_kind(c->nt, p);
  return k == NK_DefNode || k == NK_LambdaNode || (k == NK_BlockNode && par[p] >= 0 && elab_block_when(c, par[p], 0) == ELAB_BLK_LATER);
}

/* The record's statements, walked from the roots (reg: by the scope walk,
   registering as it goes), then every other node's point */
static void elab_points_walk(Compiler *c, int reg) {
  double t0 = sp_timing_now();
  LoadRecord *r = lr_get(c);
  const NodeTable *nt = c->nt;
  int nroots = nt_nroots(nt);
  ElabWalk w = { c, r, calloc((size_t)nroots, 1), 0, 0, reg, 0 };
  if (!w.entered) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  r->n = 0; r->nst = 0; r->nregion = 1;
  lr_fit(r, nt->count);
  if (reg) r->walk = &w;
  elab_file(&w, 0, NULL);
  for (int f = 1; f < nroots; f++)   /* the files no load places, a region each */
    if (!w.entered[f] && nt_root(nt, f) >= 0) { w.region = r->nregion++; elab_file(&w, f, NULL); }
  r->walk = NULL;
  if (reg) t0 = sp_timing_now();   /* the walk is the registration's; the map's own cost from here */
  lr_fit(r, nt->count);
  r->ver = nt->version;
  const int *par = comp_parent_map(c);
  int n = r->n;
  /* a node the walk placed at its first visit is placed again by the
     climb, as a rebuild places it; check mode compares the two */
  int *was = reg && r->check ? malloc(sizeof(int) * (size_t)(n ? n : 1)) : NULL;
  unsigned char *wbits = was ? malloc((size_t)(n ? n : 1)) : NULL;
  if (was && !wbits) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  if (was) { memcpy(was, r->at, sizeof(int) * (size_t)n); memcpy(wbits, r->bits, (size_t)n); }
  for (int id = 0; reg && id < n; id++)
    if (r->at[id] < 0 || r->st[r->at[id]].node != id) { r->at[id] = -2; r->bits[id] = 0; }
  int *stack = malloc(sizeof(int) * (size_t)(n ? n : 1));
  if (!stack) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  /* The rest from their nearest walked statement, up each node's parent:
     the parent map's, unless no file's root reaches that one (a node a
     desugaring left hung nowhere still holds the child it shares with its
     replacement), then the one the walk down from the roots first reaches
     the node through. */
  int *up = malloc(sizeof(int) * (size_t)(n ? n : 1));
  unsigned char *live = calloc((size_t)(n ? n : 1), 1);
  if (!up || !live) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int sp = 0;
  for (int f = 0; f < nroots; f++) {
    int rt = nt_root(nt, f);
    if (rt >= 0 && rt < n && !live[rt]) { live[rt] = 1; up[rt] = par[rt]; stack[sp++] = rt; }
  }
  while (sp > 0) {
    int p = stack[--sp], nr = nt_num_refs(nt, p), na = nt_num_arrs(nt, p);
    for (int i = 0; i < nr + na; i++) {
      int m = 1, one = i < nr ? nt_ref_at(nt, p, i) : -1;
      const int *v = i < nr ? &one : nt_arr_at(nt, p, i - nr, &m);
      for (int j = 0; j < m; j++)
        if (v[j] >= 0 && v[j] < n && !live[v[j]]) { live[v[j]] = 1; up[v[j]] = p; stack[sp++] = v[j]; }
    }
  }
  for (int id = 0; id < n; id++)
    if (!live[id] || par[id] < 0 || live[par[id]]) up[id] = par[id];
  /* Reuse the reachability scratch for the part's kind and defer bit,
     so the climb asks the same placement rule as the walk. */
  for (int id = 0; id < n; id++) live[id] = (unsigned char)((r->at[id] >= 0 ? EP_SPINE : EP_NODE) << 1);
  for (int id = 0; id < n; id++) {
    int s = 0, a = id;
    while (r->at[a] == -2 && up[a] >= 0) { stack[s++] = a; a = up[a]; }
    if (r->at[a] == -2) r->at[a] = -1;
    while (s > 0) {
      int b = stack[--s], p = up[b];
      r->at[b] = r->at[p];
      ElabPos pos = { .e = r->at[p], .kind = live[p] >> 1, .defer = live[p] & 1, .bits = r->bits[p] };
      pos = elab_child(c, p, b, &pos);
      live[b] = (unsigned char)((pos.kind << 1) | pos.defer);
      r->bits[b] = (unsigned char)pos.bits;
    }
  }
  if (was) lr_check_map(c, was, wbits);
  free(was); free(wbits);
  free(r->up); r->up = up;
  free(live); free(stack); free(w.entered);
  r->builds++;
  if (sp_timing_on()) r->ms += sp_timing_now() - t0;
}

/* The record as of the table's current nodes: rebuilt when they changed,
   except while the registration's walk makes it */
static LoadRecord *elab_fresh(Compiler *c) {
  LoadRecord *r = lr_get(c);
  if (!r->walk && !r->plan_depth && (!r->at || r->n != c->nt->count || r->ver != c->nt->version)) elab_points_walk(c, 0);
  return r;
}

/* ---- the call plan's frame ----
   Which version a call binds to is the call plan's decision (call_target),
   made from the record's facts about the call: its point, the def whose
   body holds it, whether it runs whenever that body runs, and the points
   of the histories' entries. The passes ask it again and again, and append
   nodes between the asks, so a map rebuilt per ask made each such pass
   quadratic in the program (#10422). The facts are decided once instead:
   the first ask copies the map and its parent map, and every later ask
   reads that copy (elab_plan_enter). A node appended since takes the facts
   of the node it was first linked under (nt_origin), as the climb would
   place it there, and a copy (a clone) the facts of the node it copies,
   whose place it takes. A clone that asked the map as it is rebuilt the
   map whenever a node had been appended since its last ask: a Rails
   program's runtime-name sends make a clone of each call per name, and
   desugar_builtin_method_obj, which appends a wrapper between its asks,
   rebuilt the map hundreds of times a round. SPINEL_ELAB_CHECK asks each
   call's version of the map as it is too (elab_check_version). */
static void lr_plan_swap(LoadRecord *r) {
  struct LrPlan live = { r->st, r->nst, r->cst, r->at, r->bits, r->n, r->ncap };
  r->st = r->plan.st; r->nst = r->plan.nst; r->cst = r->plan.cst; r->at = r->plan.at;
  r->bits = r->plan.bits; r->n = r->plan.n; r->ncap = r->plan.ncap;
  r->plan = live;
}
static void *lr_dup(const void *v, size_t sz) {
  void *d = malloc(sz ? sz : 1);
  if (!d) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  if (sz) memcpy(d, v, sz);
  return d;
}
/* Inside the frame: node id, appended since the copy, placed where it was
   first linked, or where the node it copies is; past a chain of 64 such
   links the map as it is answers */
static int lr_plan_place(Compiler *c, int id, int depth) {
  LoadRecord *r = c->lrec;
  if (id < r->n && r->at[id] != -2) return 1;
  char how = 0; int a = 0, k = 0; unsigned pass = 0;
  int o = depth < 64 ? nt_origin(c->nt, id, &how, &a, &pass, &k) : -1;
  if (o < 0 || !lr_plan_place(c, o, depth + 1)) return 0;
  int cap = r->ncap, n0 = r->n;
  lr_fit(r, id + 1);
  if (r->ncap != cap && !(r->plan_par = realloc(r->plan_par, sizeof(int) * (size_t)r->ncap))) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  for (int i = n0; i < r->n; i++) r->plan_par[i] = -1;
  if (how == 'c') {   /* a clone stands where the node it copies stands */
    r->at[id] = r->at[o]; r->bits[id] = r->bits[o]; r->plan_par[id] = r->plan_par[o];
    return 1;
  }
  r->at[id] = r->at[o]; r->plan_par[id] = o;
  r->bits[id] = (unsigned char)(((r->bits[o] & LR_LATE) || lr_defers(c, r->plan_par, o) ? LR_LATE : 0) |
                                ((r->bits[o] & LR_BODY) || nt_kind(c->nt, o) == NK_DefNode ? LR_BODY : 0));
  return 1;
}
/* Swaps the frame in for call_target's questions about call id: 1 when it
   did (elab_plan_leave swaps it out), 0 for a node the copy cannot place,
   which is asked of the map as it is */
int elab_plan_enter(Compiler *c, int id) {
  LoadRecord *r = lr_get(c);
  if (!r->plan_made) {
    elab_fresh(c);
    r->plan.st = lr_dup(r->st, sizeof *r->st * (size_t)r->nst); r->plan.nst = r->plan.cst = r->nst;
    r->plan.at = lr_dup(r->at, sizeof(int) * (size_t)r->ncap);
    r->plan.bits = lr_dup(r->bits, (size_t)r->ncap);
    r->plan.n = r->n; r->plan.ncap = r->ncap;
    r->plan_par = lr_dup(r->up, sizeof(int) * (size_t)r->n);
    r->plan_par = realloc(r->plan_par, sizeof(int) * (size_t)r->ncap);
    r->plan_made = 1;
  }
  if (!r->plan_depth) lr_plan_swap(r);
  r->plan_depth++;
  if (lr_plan_place(c, id, 0)) return 1;
  elab_plan_leave(c);
  return 0;
}
void elab_plan_leave(Compiler *c) {
  LoadRecord *r = c->lrec;
  if (!--r->plan_depth) lr_plan_swap(r);
}

/* The statement entry of node's point in record r as it stands: NULL for
   a node in a method body, a lambda or a block run later; region -1 under
   no file */
static const LrStmt *lr_stmt_of(const LoadRecord *r, int node) {
  int e = node >= 0 && node < r->n ? r->at[node] : -1;
  return e < 0 || (r->bits[node] & LR_LATE) ? NULL : &r->st[e];
}

/* The point at which node's statement runs; region -1 (no point) for a
   node in a method body, a lambda or a block run later, or under no file. */
LoadPoint elab_point_of(Compiler *c, int node) {
  LoadRecord *r = elab_fresh(c);
  const LrStmt *s = lr_stmt_of(r, node);
  return s ? (LoadPoint){ s->region, (int)(s - r->st), node } : (LoadPoint){ -1, -1, -1 };
}

/* Does the registration apply an entry (LrDef, LrMixin) recorded under
   condition may at statement s as the load phase ends: 0 for one with no
   point (s NULL, or of region -1) unless its body is one the walk does
   not place (unplaced), or that may run partly in its class body (in an
   iterator's block or a begin body with rescue clauses there: a body
   opened in one runs whole, LrStmt.fpartial); 1 for one it applies; 2 for
   one under a condition in its class body (one around the body is the
   body's), whose visibility alone it applies (lr_applied) */
static int lr_stmt_applied(const LrStmt *s, int unplaced, int may) {
  if (s && s->region < 0) s = NULL;
  if ((!s && !unplaced) || (s && s->fpartial)) return 0;
  return may && s && (s->fmay || !s->may) ? 2 : 1;
}

/* The point from which node can run: its statement's, and for a node in a
   method body, a lambda or a block run later, that of the statement that
   defines or makes it, which runs first. Region -1 under no file. */
LoadPoint elab_point_from(Compiler *c, int node) {
  LoadRecord *r = elab_fresh(c);
  int e = node >= 0 && node < r->n ? r->at[node] : -1;
  return e < 0 ? (LoadPoint){ -1, -1, -1 } : (LoadPoint){ r->st[e].region, e, -1 };
}

/* The point at which node's statement completes: the last statement of
   the bodies it runs (a class body, a block run at once in its value), or
   its own. Region -1 where elab_point_of has no point. */
LoadPoint elab_point_done(Compiler *c, int node) {
  LoadPoint p = elab_point_of(c, node);
  return p.region < 0 ? p : (LoadPoint){ p.region, c->lrec->st[p.seq].last, -1 };
}

/* Is node in a method body, which runs only when the method is called? */
int elab_in_method_body(Compiler *c, int node) {
  LoadRecord *r = elab_fresh(c);
  return node >= 0 && node < r->n && (r->bits[node] & LR_BODY);
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
/* An answer the record stores as it records a statement, from the
   histories as they stand there, which a file in a later region may change
   (lr_settle_regions): a form's "runs program code" (SET_RUNS), whether a
   call runs code the record cannot follow (SET_OPAQUE), an alias's
   visibility taken from its target's (SET_ALIAS: name, the visibility
   without one in vis0). cls and sg: the frame the statement runs in. */
enum { SET_RUNS, SET_OPAQUE, SET_ALIAS };
typedef struct LrSettle { int fam, node, cls, sg, may, vis0; const char *name; } LrSettle;
static void lr_settle_add(LoadRecord *r, int fam, int node, int cls, int sg, int may, int vis0, const char *name);
static void lr_settle_regions(Compiler *c);
static int lr_form_answer(Compiler *c, int node, int cls, int sg);
/* a: class, or -2 - ElabVar; b: singleton; nlink: the next method history
   of the same name (lr_vers_index); open: the history's end-of-load version
   may be a later one than the last the load phase surely applies
   (elab_history_open) */
typedef struct LrHist { int a, b; char *name; LrDef *v; int n, cap, next, nlink; unsigned char open; } LrHist;
/* sup: the superclass the first header naming one names, as resolved
   there (has_sup); LR_UNKNOWN_CLASS when it is not a class the record knows */
typedef struct LrClass { LrOpen *open; int nopen, copen; LrMixin *mix; int nmix, cmix, sup, sup_ref; unsigned char has_sup; } LrClass;
/* LR_NEW: a header's entity names a new class; LR_LATER_ALIAS: one an
   alias written after it reopens until N2b; LR_BUILTIN: it reopens a
   builtin, the class registration names (LrHeader.want) */
enum { LR_BUCKETS = 4096, LR_MAIN = -1, LR_UNKNOWN_CLASS = -2, LR_LATER = -3, LR_NEW = -4, LR_LATER_ALIAS = -5, LR_BUILTIN = -6 };

/* The program's reopening of core class k (CORE_*) registered so far:
   LR_UNKNOWN_CLASS before its header */
static int lr_core(const LoadRecord *r, int k) {
  return r && r->core[k] ? r->core[k] - 1 : LR_UNKNOWN_CLASS;
}
/* the histories that are not a method's or a variable's: a frame's bare
   visibility words, keyed by the frame's node; the code a class's body ran
   that the record cannot follow, keyed as a constant owner; and whether a
   definition form runs code of the program's, keyed by its node (vis: the
   answer, lr_form_runs); whether a def's body defines or writes anything
   when it runs, keyed by the def (vis: the answer, lr_body_defines) */
enum { LR_KEY_VIS = -5, LR_KEY_OPAQUE = -6, LR_KEY_RUNS = -7, LR_KEY_DEFINES = -8, LR_KEY_AUTO = -9,
       LR_KEY_CONST = -2 - ELAB_CONST,   /* a constant's history, keyed as the variables' (ElabVar) are */
       LR_KEY_EITHER = -10 };   /* a lookup's only: an owner's constant, else its registration, as CRuby's table holds both */

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
  *x = (LrHist){ a, b, strdup(name), NULL, 0, 0, r->hhead[h], -1, 0 };
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
  r->nentries++;
  free(r->surf); free(r->surf_at); r->surf = NULL; r->surf_at = NULL;   /* elab_class_surface's are of fewer entries */
}

/* cls's history (LR_MAIN: main's, Object's), NULL for a class the record
   does not hold (LoadRecord.held) */
static LrClass *lr_class(LoadRecord *r, int cls) {
  return cls >= LR_MAIN && cls + 1 < r->ncls && r->held[cls + 1] ? &r->cls[cls + 1] : NULL;
}

/* A class or module header the scope walk reached: the class registration
   opened there (cls, LR_UNKNOWN_CLASS for none); and, for a header of the
   load phase, its entity as CRuby's `class X` finds it: the class whose
   own constants hold its name (owner) and the name (leaf), what was there
   before the header (found, the write or header the record holds, -1 for
   none) as elab_const_own_at answered (verdict), and the class that names
   (want): the found header's class, LR_UNKNOWN_CLASS for an alias or one
   not known, LR_NEW for a new class. The map also keys a class builder's
   call, and the constant write holding it, by what the builder makes
   (builder, EB_*; EB_NONE on a header): such an entry opens no class
   (cls and want LR_UNKNOWN_CLASS), so the header readers answer it as a
   node they hold nothing for. */
typedef struct LrHeader { int node, cls, owner, verdict, found, want, next; char *leaf; unsigned char builder; } LrHeader;

static LrCheck *lr_family(LoadRecord *r, const char *reader);
static void lr_node_desc(Compiler *c, int id, char *buf, size_t sz);

static LrHeader *lr_header(LoadRecord *r, int node, int make) {
  if (!r->hdr_head) {
    if (!make) return NULL;
    if (!(r->hdr_head = malloc(sizeof(int) * LR_BUCKETS))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int b = 0; b < LR_BUCKETS; b++) r->hdr_head[b] = -1;
  }
  unsigned h = (unsigned)node % LR_BUCKETS;
  for (int i = r->hdr_head[h]; i >= 0; i = r->hdr[i].next) if (r->hdr[i].node == node) return &r->hdr[i];
  if (!make) return NULL;
  r->hdr = lr_grow(r->hdr, &r->chdr, r->nhdr, sizeof *r->hdr);
  r->hdr[r->nhdr] = (LrHeader){ node, LR_UNKNOWN_CLASS, LR_UNKNOWN_CLASS, ELAB_UNKNOWN, -1, LR_UNKNOWN_CLASS, r->hdr_head[h], NULL, EB_NONE };
  r->hdr_head[h] = r->nhdr;
  return &r->hdr[r->nhdr++];
}

/* Does the header at node reopen a builtin (LR_BUILTIN)? */
static int lr_header_builtin(LoadRecord *r, int node) {
  const LrHeader *h = lr_header(r, node, 0);
  return h && h->want == LR_BUILTIN;
}

/* The node -> class map: the class header `node` opens, as registration
   decided it at the header; its entity's owner and leaf in *owner and
   *leaf when asked (LR_UNKNOWN_CLASS and NULL for a header that has none:
   one outside the load phase's walk) */
int elab_header_class(const Compiler *c, int node, int *owner, const char **leaf) {
  const LrHeader *h = c->lrec ? lr_header(c->lrec, node, 0) : NULL;
  if (owner) *owner = h ? h->owner : LR_UNKNOWN_CLASS;
  if (leaf) *leaf = h ? h->leaf : NULL;
  return h ? h->cls : LR_UNKNOWN_CLASS;
}

/* A frame's singleton flag: where its defs go and what its self is. In a
   `class << x` body (LR_FS_SCLASS) both are the singleton class. In an
   instance_eval or instance_exec block on a class (LR_FS_IEVAL) a def, an
   alias or an undef goes on the singleton class, but self is the class
   itself: a call on self there (include, prepend, attr_accessor,
   define_method, alias_method) is the class's own, as in CRuby. In such a
   block on a singleton class (LR_FS_IEVAL_SCLASS, `class << C;
   instance_eval { ... }`) self is that singleton class, and a def goes on
   the singleton class's own singleton, which the record does not hold. */
enum { LR_FS_NONE, LR_FS_SCLASS, LR_FS_IEVAL, LR_FS_IEVAL_SCLASS };

/* elab_program's state: each statement's frame class (fc) and singleton
   flag (fs), each frame's visibility state (vs) and each file's top
   level's (vmain) */
typedef struct ElabRec { Compiler *c; LoadRecord *r; int *fc; unsigned char *fs, *vs, *vmain; int cap; unsigned char unplaced; } ElabRec;

/* The constant name a class or module header (or a read) names, as the
   program wrote it: a header the qualify passes renamed keeps the Ruby path
   it resolved to (qc_ruby_name), whose last name it is; a read they renamed
   keeps the name it had (qc_orig_name). */
static const char *lr_written_name(const NodeTable *nt, int cp) {
  const char *rn = nt_str(nt, cp, "qc_ruby_name"), *q = rn;
  for (const char *p = rn ? strstr(rn, "::") : NULL; p; p = strstr(p + 2, "::")) q = p + 2;
  return rn ? q : nt_str(nt, cp, "qc_orig_name") ? nt_str(nt, cp, "qc_orig_name") : nt_str(nt, cp, "name");
}

static int lr_const_at(Compiler *c, int node, const char *name, LoadPoint p, const LrDef **out, int key);
static int lr_const_in(Compiler *c, int cls, const char *name, LoadPoint p, int late, int top, const LrDef **out, int depth, int key);

static int lr_cref(Compiler *c, int e);
static int lr_search(Compiler *c, int cls, int sg, const char *name, int mod, LoadPoint p, int late, int vis, const LrDef **out, int depth);
static int lr_named(Compiler *c, const char *name);
static const char *lr_const_written(const NodeTable *nt, int id);
static const NodeKind *lr_const_write_kinds(int *n);

/* The class a builder write's block opens (`X = Struct.new(:a) do`,
   `k = Data.define(:a) do`): the one registration homes it in
   (register_structs: a constant's class by its name, a local's the class
   keyed to the write), once registration has run (elab_record_builders);
   LR_UNKNOWN_CLASS before, and for a write with no such block */
static int lr_builder_class(Compiler *c, int node) {
  if (class_def_builder(c->nt, node) >= 0) return elab_header_class(c, node, NULL, NULL);   /* made at the write (elab_rec_open) */
  if (!c->lrec->structs || class_def_body(c->nt, node) < 0) return LR_UNKNOWN_CLASS;
  if (nt_kind(c->nt, node) == NK_ConstantWriteNode) {
    int ci = const_node_class_index(c, node);
    return ci >= 0 ? ci : LR_UNKNOWN_CLASS;
  }
  int ci = comp_class_of_def(c, node);
  return ci >= 0 ? ci : LR_UNKNOWN_CLASS;
}

/* ...asked by codegen, which emits the builder's block at its write */
int elab_builder_class(Compiler *c, int node) {
  int ci = c->lrec ? lr_builder_class(c, node) : LR_UNKNOWN_CLASS;
  return ci >= 0 ? ci : -1;
}

/* The class a constant's write or definition holds: a class or module
   header's (the node -> class map), a class builder's; an alias's, a
   write whose value is a constant reference (`A = Foo`, `A = M::Foo`),
   the class that reference names at the write's point, along a chain of
   aliases up to a cap; else none known */
static int lr_class_of(Compiler *c, const LrDef *d) {
  NodeKind k = d ? nt_kind(c->nt, d->node) : NK_NilNode;
  if (k == NK_ClassNode || k == NK_ModuleNode || (d && class_def_builder(c->nt, d->node) >= 0))
    return elab_header_class(c, d->node, NULL, NULL);
  if (k != NK_ConstantWriteNode) return LR_UNKNOWN_CLASS;
  if (class_def_body(c->nt, d->node) >= 0) return lr_builder_class(c, d->node);
  int v = nt_ref(c->nt, d->node, "value");
  NodeKind vk = v >= 0 ? nt_kind(c->nt, v) : NK_NilNode;
  LoadRecord *r = c->lrec;
  if ((vk != NK_ConstantReadNode && vk != NK_ConstantPathNode) || r->follow >= 16) return LR_UNKNOWN_CLASS;
  r->follow++;
  int ci = lr_class_ref_at(c, v, NULL);
  r->follow--;
  return ci == LR_LATER ? LR_UNKNOWN_CLASS : ci;   /* a later reference keeps the name it is written with, not the alias's */
}

static int lr_name_cmp(const void *a, const void *b);

/* Is name a builtin class or module that a header of the program reopens
   (lr_reopens_take)? Boot knows it before any statement runs; the program's
   class of it is made where the header stands. */
static int lr_reopened(const LoadRecord *r, const char *name) {
  return bsearch(&name, r->reopens, (size_t)r->nreopens, sizeof *r->reopens, lr_name_cmp) != NULL;
}

/* The class of a reference recorded before the header that makes it
   (LR_LATER, ref the reference): once made, kept in *cls; until then,
   LR_LATER, which the queries take as not known. LR_LATER comes only from
   elab_class_ref_at's builtin branch; ref keeps that written name, including
   through qualification (lr_written_name). */
static int lr_later(Compiler *c, int *cls, int ref) {
  if (*cls == LR_LATER) {
    int ci = comp_core_class(c, builtin_core_class_slot(lr_written_name(c->nt, ref)));
    if (ci >= 0) *cls = ci;
  }
  return *cls;
}

/* The class a constant reference names (a mixin's argument, `class << K`,
   `def K.m`, `K.class_eval`), resolved as CRuby does where it stands: a
   name through its lexical scopes and then the ancestors (lr_const_at), a
   path `A::B` in A and its ancestors, `::B` in Object's. A name the
   program defines nowhere it can reach is a core class's or module's, as
   registered: LR_LATER for one a later header of the program's makes a
   class (lr_reopened), which a mixin or a superclass keeps as a reference
   (lr_later). */
/* A read the tree spells bare that the program wrote `::X` (or
   `Object::X`): a desugar or the qualify pass made it bare where only
   Object's X can answer it, and left it root_anchored. It reads Object's
   constants, not the lexical scopes'. */
static int lr_root_read(const NodeTable *nt, int node) {
  return nt_kind(nt, node) == NK_ConstantReadNode && nt_int(nt, node, "root_anchored", 0);
}

static int lr_class_ref_at(Compiler *c, int node, int *core) {
  const NodeTable *nt = c->nt;
  const LrDef *d = NULL;
  NodeKind k = node >= 0 ? nt_kind(nt, node) : NK_NilNode;
  if (core) *core = -1;
  if (k != NK_ConstantPathNode && k != NK_ConstantReadNode) return LR_UNKNOWN_CLASS;
  if (c->lrec->plan_depth) lr_plan_place(c, node, 0);
  const char *nm = lr_written_name(nt, node);
  int o, ci, pc = LR_MAIN;
  if (k == NK_ConstantReadNode && !lr_root_read(nt, node)) o = lr_const_at(c, node, nm, elab_point_of(c, node), &d, LR_KEY_CONST);
  else {
    int par = k == NK_ConstantPathNode ? nt_ref(nt, node, "parent") : -1;
    LoadPoint p = elab_point_of(c, node);
    if (par >= 0) pc = lr_class_ref_at(c, par, NULL);
    if (pc < LR_MAIN) return LR_UNKNOWN_CLASS;
    o = lr_const_in(c, pc, nm, p, p.region < 0, pc == LR_MAIN, &d, 0, LR_KEY_CONST);
  }
  if (o == ELAB_YES) return lr_class_of(c, d);
  /* a name Object does not hold from the program: a core class's or module's */
  if (o == ELAB_NO && pc == LR_MAIN && (is_builtin_class_name(nm) || is_builtin_module_const_name(nm))) {
    int slot = builtin_core_class_slot(nm);
    if (core) *core = slot;
    if ((ci = comp_core_class(c, slot)) >= 0) return ci;
    if (lr_reopened(c->lrec, nm)) return LR_LATER;
  }
  return LR_UNKNOWN_CLASS;
}
/* Asked once the record is made, outside the call plan's frame: answered
   in a frame of its own (elab_plan_enter), so that a pass that moved or
   appended the node since gets the answer at its source position, and the
   map is not rebuilt per ask. A node the frame cannot place answers not
   known. SPINEL_ELAB_CHECK asks the map as it is too. */
int elab_class_ref_at(Compiler *c, int node, int *core) {
  LoadRecord *r = lr_get(c);
  if (!r->recorded || r->walk || r->plan_depth) return lr_class_ref_at(c, node, core);
  int framed = elab_plan_enter(c, node), ci = LR_UNKNOWN_CLASS;
  if (core) *core = -1;
  if (framed) {
    ci = lr_class_ref_at(c, node, core);
    elab_plan_leave(c);
  }
  if (r->check) lr_check_class_ref(c, node, ci);
  return ci;
}

/* The classes the constant a read names may hold where the record cannot
   settle one, from the writes of the history its lookup ends in: a class
   header, or a write of a class or of a conditional of classes. Only for a
   read outside every class body, whose lookup is Object's alone; -1 for
   anything else (a write that is no class, one that cannot be placed, a
   history with none). */
typedef struct { Compiler *c; int *v; int n, cap, bad; } LrClassSet;
static void lr_cls_add(LrClassSet *s, int ci) {
  if (ci < LR_MAIN) { s->bad = 1; return; }
  for (int i = 0; i < s->n; i++) if (s->v[i] == ci) return;
  if (s->n == s->cap && !(s->v = realloc(s->v, sizeof(int) * (size_t)(s->cap = s->cap ? 2 * s->cap : 4)))) {
    fprintf(stderr, "spinel: out of memory\n");
    exit(1);
  }
  s->v[s->n++] = ci;
}
/* a value position of a written value: a constant read naming a class is a
   candidate, anything else (a nil by absence, a call, a gave-up leaf) is not */
static int lr_value_leaf(const VpLeaf *leaf, void *ud) {
  LrClassSet *s = ud;
  NodeKind k = leaf->kind == VPK_EXPR ? nt_kind(s->c->nt, leaf->node) : NK_NONE;
  if (k == NK_ConstantReadNode || k == NK_ConstantPathNode) lr_cls_add(s, lr_class_ref_at(s->c, leaf->node, NULL));
  else s->bad = 1;
  return s->bad;
}
int elab_class_candidates(Compiler *c, int node, int **out) {
  LoadRecord *r = elab_fresh(c);
  const NodeTable *nt = c->nt;
  int e = node >= 0 && node < r->n ? r->at[node] : -1;
  LrClassSet s = { c, NULL, 0, 0, 0 };
  *out = NULL;
  if (e < 0 || r->st[e].frame >= 0 || nt_kind(nt, node) != NK_ConstantReadNode || lr_root_read(nt, node)) return -1;
  r->ncand = r->cand_bad = 0;
  r->collect = 1;
  int o = lr_const_at(c, node, lr_written_name(nt, node), elab_point_of(c, node), NULL, LR_KEY_CONST);
  r->collect = 0;
  if (o == ELAB_NO || r->cand_bad || !r->ncand) return -1;
  for (int i = 0; i < r->ncand && !s.bad; i++) {
    const LrDef *d = r->cand[i];
    int ci = lr_class_of(c, d);
    if (ci >= LR_MAIN) lr_cls_add(&s, ci);
    else if (nt_kind(nt, d->node) == NK_ConstantWriteNode) for_each_value_position(nt, nt_ref(nt, d->node, "value"), 0, lr_value_leaf, &s);
    else s.bad = 1;
  }
  if (s.bad) { free(s.v); return -1; }
  *out = s.v;
  return s.n;
}

/* The class a call's receiver names: none or self, the frame's (its
   singleton where self is a singleton class); a constant, its class (not
   singleton) */
static int lr_receiver(ElabRec *x, int recv, int fc, int fs, int *sg) {
  NodeKind k = recv >= 0 ? nt_kind(x->c->nt, recv) : NK_SelfNode;
  *sg = k == NK_SelfNode && (fs == LR_FS_SCLASS || fs == LR_FS_IEVAL_SCLASS);
  if (k == NK_SelfNode) return fc;
  return lr_class_ref_at(x->c, recv, NULL);
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
  if (k == NK_ClassNode || k == NK_ModuleNode) { cls = elab_header_class(x->c, node, NULL, NULL); sg = 0; }
  else if (k == NK_SingletonClassNode) {
    int ex = nt_ref(nt, node, "expression");
    cls = nt_kind(nt, ex) == NK_SelfNode ? cls : lr_class_ref_at(x->c, ex, NULL);
    sg = LR_FS_SCLASS;
  }
  else if ((k == NK_ConstantWriteNode || k == NK_ConstantPathWriteNode || k == NK_LocalVariableWriteNode) &&
           class_def_body(x->c->nt, node) >= 0) {
    /* `X = Struct.new(:a) do`, `A::B = Class.new do`: the class registration
       homes there, once it has; a block recorded before is recorded again
       then (r->builders) */
    cls = lr_builder_class(x->c, node); sg = 0;
    if (cls == LR_UNKNOWN_CLASS) x->r->builders++;
  }
  else if (call == LR_UNKNOWN_CLASS) { cls = LR_UNKNOWN_CLASS; sg = 0; }
  else if (call >= 0) {
    const char *nm = nt_str(nt, call, "name");
    int rsg;
    cls = is_class_eval_family(nm) || is_instance_eval_family(nm) ? lr_receiver(x, nt_ref(nt, call, "receiver"), cls, sg, &rsg) : LR_UNKNOWN_CLASS;
    /* the receiver's singleton side is self's, and a class_eval's definee */
    sg = is_instance_eval_family(nm) ? (rsg ? LR_FS_IEVAL_SCLASS : LR_FS_IEVAL) : rsg ? LR_FS_SCLASS : LR_FS_NONE;
  }
  if (cls < LR_MAIN) cls = LR_UNKNOWN_CLASS;
  x->fc[e] = cls; x->fs[e] = (unsigned char)sg;
}

/* A visibility word's SP_VIS_* (module_function: private) */
static unsigned char lr_vis(int bvis) {
  return bvis == BVIS_PRIVATE || bvis == BVIS_MODULE_FUNCTION ? SP_VIS_PRIVATE : bvis == BVIS_PROTECTED ? SP_VIS_PROTECTED : SP_VIS_PUBLIC;
}

/* The method scope the scope walk made of def or define_method node
   `node`, which it tells as it makes one (elab_def_scope), ahead of the
   record's version of it; 0 for none */
void elab_def_scope(Compiler *c, int node, int scope) {
  LoadRecord *r = lr_get(c);
  if (node < 0) return;
  if (node >= r->ndscope) {
    int n = node + 1 > 2 * r->ndscope ? node + 1 : 2 * r->ndscope;
    r->dscope = realloc(r->dscope, sizeof *r->dscope * (size_t)n);
    if (!r->dscope) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memset(r->dscope + r->ndscope, 0, sizeof *r->dscope * (size_t)(n - r->ndscope));
    r->ndscope = n;
  }
  r->dscope[node] = scope;
}
static int lr_def_scope(const LoadRecord *r, int node) {
  return node >= 0 && node < r->ndscope ? r->dscope[node] : 0;
}

/* A method version of (cls, sg, name) by node, under the visibility state
   bvis: the frame's where the version is the frame's own definee's (a
   `class << self` body's def is the singleton's), public for one on
   another receiver (`def self.m` in a class body, `K.attr_reader`); a
   module_function one also copies it to the singleton */
static int lr_vis_at(Compiler *c, int cls, const char *name, int sg, LoadPoint p);

/* ...: a definition of initialize and its kin is private wherever it
   stands, and an alias, or an alias's forwarder (lr_def), takes its
   original's visibility */
static void lr_method(ElabRec *x, int cls, int sg, const char *name, int node, LrKind kind, int bvis, int may, const char *target) {
  if (cls < LR_MAIN || !name) return;
  int vis = lr_vis(bvis), ov = target ? lr_vis_at(x->c, cls, target, sg, elab_point_of(x->c, node)) : -1;
  const LrHist *th = x->unplaced && target ? lr_hist(x->r, cls, sg, target, 0) : NULL;
  for (int i = th ? th->n - 1 : -1; i >= 0; i--)   /* an unplaced body's own original, which no query at a point sees */
    if (th->v[i].unplaced && th->v[i].kind <= LR_VIS) { ov = th->v[i].vis; break; }
  if (!sg && (kind == LR_DEF || kind == LR_DEFINE_METHOD) && is_implicitly_private_name(name)) vis = SP_VIS_PRIVATE;
  if (target && !x->unplaced) lr_settle_add(x->r, SET_ALIAS, node, cls, sg, may, vis, name);
  if (ov >= 0) vis = ov;
  int sc = kind == LR_DEF || kind == LR_DEFINE_METHOD ? lr_def_scope(x->r, node) : 0;
  lr_version(x->r, cls, sg, name, (LrDef){ node, (unsigned char)kind, (unsigned char)vis, (unsigned char)may, 0, target, .unplaced = x->unplaced, .scope = sc });
  if (!sg && bvis == BVIS_MODULE_FUNCTION)
    lr_version(x->r, cls, 1, name, (LrDef){ node, LR_COPY, SP_VIS_PUBLIC, (unsigned char)may, 0, name, .unplaced = x->unplaced });
}

/* The class a frame's def, alias or undef defines on (in *sg, on its
   singleton) */
static int lr_definee(int fc, int fs, int *sg) {
  *sg = fs != LR_FS_NONE;
  return fs == LR_FS_IEVAL_SCLASS ? LR_UNKNOWN_CLASS : fc;
}

/* A def's version. The forwarder desugar_inherited_aliases makes of an
   alias of an inherited method (`def nw(*a, &b) = nw#inherited(*a, &b)`)
   is that alias: it takes the visibility of the method it names, not its
   section's, so it names it as an alias does. */
static void lr_def(ElabRec *x, int def, int fc, int fs, int bvis, int may) {
  const NodeTable *nt = x->c->nt;
  int recv = nt_ref(nt, def, "receiver"), sg, cls = lr_definee(fc, fs, &sg);
  if (recv >= 0) { cls = lr_receiver(x, nt_def_receiver(nt, recv), fc, LR_FS_SCLASS, &sg); sg = 1; bvis = BVIS_PUBLIC; }
  int body = nt_int(nt, def, "alias_forwarder", 0) ? nt_ref(nt, def, "body") : -1, n = 0;
  const int *st = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
  const char *fwd = n == 1 && nt_kind(nt, st[0]) == NK_CallNode ? nt_str(nt, st[0], "name") : NULL;
  lr_method(x, cls, sg, nt_str(nt, def, "name"), def, LR_DEF, bvis, may, fwd);
}

/* A write of a variable: a global's or main's instance variable's by name,
   a constant's by name and the class whose body (lexical scope) writes it,
   as lr_const_owner numbers it (0: not known) */
static int lr_const_owner(int cls) { return cls >= LR_MAIN ? cls + 2 : 0; }
static int lr_sg_const_owner(int cls) { return cls >= LR_MAIN ? -cls - 2 : 0; }

/* A known class's singleton has its own constant history, disjoint from
   the class's. A singleton of an arbitrary object remains unknown. */
static int lr_singleton_owner(Compiler *c, int f) {
  int ex = nt_ref(c->nt, c->lrec->st[f].node, "expression");
  int cls = nt_kind(c->nt, ex) == NK_SelfNode ? lr_cref(c, f) : lr_class_ref_at(c, ex, NULL);
  return lr_sg_const_owner(cls);
}

static int lr_const_scope_owner(Compiler *c, int e) {
  const LoadRecord *r = c->lrec;
  for (int f = e >= 0 ? r->st[e].frame : -1; f >= 0; f = r->st[f].frame) {
    NodeKind k = nt_kind(c->nt, r->st[f].node);
    if (k == NK_SingletonClassNode) return lr_singleton_owner(c, f);
    if (k == NK_ClassNode || k == NK_ModuleNode) break;
  }
  return lr_const_owner(lr_cref(c, e));
}
/* Is a written value nil: none, a nil, or the nil a chain of writes ends
   in (`$a = @b = nil`)? */
static int lr_nil_value(const NodeTable *nt, int v) {
  return v < 0 || nt_kind(nt, v) == NK_NilNode || comp_nil_chain_bottom(nt, v) >= 0;
}
/* Record a write under owner, numbered as lr_const_owner numbers it; the
   version keeps the class it names (LrDef.owner) */
static void lr_write(LoadRecord *r, ElabVar kind, int owner, const char *name, int node, int nilv, int may) {
  if (node >= 0 && node < r->n && (r->bits[node] & LR_LATE)) {
    const LrHist *h = lr_hist(r, -2 - (int)kind, owner, name, 0);
    for (int i = 0; h && i < h->n; i++) if (h->v[i].node == node) return;
  }
  lr_version(r, -2 - (int)kind, owner, name, (LrDef){ node, LR_WRITE, 0, (unsigned char)may, (unsigned char)nilv, NULL, owner < 0 ? -owner - 2 : owner - 2 });
}

/* Every constant assignment joins its owner's history at its point. A
   multiple assignment's targets share its completion point; a conditional
   assignment may leave the previous value in place. */
static void lr_const_write(Compiler *c, int node, int e, int fc, int fs, int may) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, node);
  int owner = lr_const_scope_owner(c, e), target = -1, v = nt_ref(nt, node, "value");
  const char *name;
  if (node < c->lrec->n && (c->lrec->bits[node] & LR_LATE)) {
    const int *par = comp_parent_map(c);
    owner = lr_const_owner(LR_MAIN);
    for (int a = par[node]; a >= 0; a = par[a]) {
      NodeKind ak = nt_kind(nt, a);
      if (ak == NK_SingletonClassNode) {
        int f = a < c->lrec->n ? c->lrec->at[a] : -1;
        owner = f >= 0 && c->lrec->st[f].node == a ? lr_singleton_owner(c, f) : 0;
        break;
      }
      if (ak == NK_ClassNode || ak == NK_ModuleNode) { owner = lr_const_owner(elab_header_class(c, a, NULL, NULL)); break; }
    }
    fc = owner < 0 ? -owner - 2 : owner - 2;
    fs = owner < 0 ? LR_FS_SCLASS : LR_FS_NONE;
  }
  switch (k) {
    case NK_ConstantWriteNode: case NK_ConstantTargetNode:
    case NK_ConstantOrWriteNode: case NK_ConstantAndWriteNode: case NK_ConstantOperatorWriteNode:
      break;
    case NK_ConstantPathTargetNode: target = node; break;
    case NK_ConstantPathWriteNode: case NK_ConstantPathOrWriteNode:
    case NK_ConstantPathAndWriteNode: case NK_ConstantPathOperatorWriteNode:
      target = nt_ref(nt, node, "target");
      break;
    case NK_CallNode: {
      const char *nm = nt_str(nt, node, "name");
      int a = nt_ref(nt, node, "arguments"), an = 0;
      const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
      if (!nm || !is_const_set_name(nm) || an < 2 || !(name = vis_arg_name(nt, av[0]))) return;
      int rv = nt_ref(nt, node, "receiver");
      owner = rv < 0 || nt_kind(nt, rv) == NK_SelfNode ?
        (fs == LR_FS_SCLASS ? lr_sg_const_owner(fc) : lr_const_owner(fc)) : lr_const_owner(lr_class_ref_at(c, rv, NULL));
      lr_write(c->lrec, ELAB_CONST, owner, name, node, lr_nil_value(nt, av[1]), may);
      return;
    }
    default: return;
  }
  if (!(name = lr_const_written(nt, node))) return;
  if (target >= 0) {
    int par = nt_ref(nt, target, "parent");
    owner = par < 0 ? lr_const_owner(LR_MAIN) : nt_kind(nt, par) == NK_SelfNode ?
      (fs == LR_FS_SCLASS ? lr_sg_const_owner(fc) : lr_const_owner(fc)) : lr_const_owner(lr_class_ref_at(c, par, NULL));
  }
  may |= k == NK_ConstantOrWriteNode || k == NK_ConstantAndWriteNode ||
         k == NK_ConstantPathOrWriteNode || k == NK_ConstantPathAndWriteNode;
  lr_write(c->lrec, ELAB_CONST, owner, name, node, v >= 0 && lr_nil_value(nt, v), may);
}

static void lr_form_runs(ElabRec *x, int node, int cls, int sg, int may);

static int lr_name_cmp(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static void lr_name(LoadRecord *r, const char *name, const char *suffix) {
  if (!name) { r->names_any = 1; return; }
  r->names = lr_grow(r->names, &r->cnames, r->nnames, sizeof *r->names);
  size_t len = strlen(name) + strlen(suffix) + 1;
  char *d = malloc(len);
  if (!d) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  snprintf(d, len, "%s%s", name, suffix);
  r->names[r->nnames++] = d;
}

/* The names the program defines methods by, wherever it does (a statement
   of the load phase, a class the record does not know, a method body or a
   block run later): a def's, an alias's, an attr form's, a define_method's
   or alias_method's. A definition whose name the record cannot read (a
   computed one, a send of a definition form, an eval of a String) makes it
   answer for any name. Taken once, as the record is made: the passes after
   it rename methods (alias_capture_earlier_def) and add none of the
   program's own, only the helpers they synthesize (elab_names_add). */
static void lr_names_take(Compiler *c) {
  const NodeTable *nt = c->nt;
  LoadRecord *r = c->lrec;
  NT_FOREACH_KIND(nt, NK_DefNode, n) {
    const char *nm = nt_str(nt, n, "name");
    lr_name(r, nm, "");
    if (!nm) continue;
    LrSite *d = realloc(r->defs, sizeof *r->defs * (size_t)(r->ndefs + 1));
    if (!d) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    (r->defs = d)[r->ndefs++] = (LrSite){ nm, n };
  }
  NT_FOREACH_KIND(nt, NK_AliasMethodNode, n) {
    int nn = nt_ref(nt, n, "new_name");
    lr_name(r, nn >= 0 ? vis_arg_name(nt, nn) : NULL, "");
  }
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    const char *nm = nt_str(nt, n, "name");
    int an = nt_ref(nt, n, "arguments"), ac = 0;
    const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
    if (!nm || !ac) continue;
    if (is_send_family(nm) || (is_code_eval_name(nm) && nt_ref(nt, n, "block") < 0)) {
      const char *fn = vis_arg_name(nt, av[0]);
      if (is_code_eval_name(nm) || !fn || is_method_naming_call(fn)) r->names_any = 1;
    }
    else if (is_attr_decl_reader(nm) || is_attr_decl_writer(nm))
      for (int i = 0; i < ac; i++) {
        if (is_attr_decl_reader(nm)) lr_name(r, vis_arg_name(nt, av[i]), "");
        if (is_attr_decl_writer(nm)) lr_name(r, vis_arg_name(nt, av[i]), "=");
      }
    else if (is_define_method_name(nm) || is_define_singleton_method_name(nm) || is_alias_method_name(nm))
      lr_name(r, vis_arg_name(nt, av[0]), "");
  }
  if (r->nnames > 1) qsort(r->names, (size_t)r->nnames, sizeof *r->names, lr_name_cmp);
  if (r->ndefs > 1) qsort(r->defs, (size_t)r->ndefs, sizeof *r->defs, lr_name_cmp);   /* the name leads an LrSite */
}

/* The entries of `name` in v, n of size sz sorted by the name that leads
   each (lr_name_cmp): the first, and their number in *k */
static const void *lr_name_range(const void *v, int n, size_t sz, const char *name, int *k) {
  const char *m = n ? bsearch(&name, v, (size_t)n, sz, lr_name_cmp) : NULL, *lo = m, *hi = m, *end = (const char *)v + (size_t)n * sz;
  *k = 0;
  if (!m) return NULL;
  while (lo > (const char *)v && lr_name_cmp(lo - sz, &name) == 0) lo -= sz;
  while (hi + sz < end && lr_name_cmp(hi + sz, &name) == 0) hi += sz;
  *k = (int)((size_t)(hi - lo) / sz) + 1;
  return lo;
}

int elab_mix_on_singleton(int kind) { return kind == LR_EXTEND || kind == LR_SG_PREPEND; }

/* The kind of mixin call node (named nm) in a frame that is a singleton class's
   (sg) or not. A singleton's include is an extend, and its prepend goes in
   front of its own methods; so does the `extend` that
   desugar_singleton_class_mixin makes of one (singleton_class.prepend M,
   class << K; prepend M), whichever frame it now stands in. */
static LrMixKind lr_mix_kind(const NodeTable *nt, int node, const char *nm, int sg) {
  int marked = (int)nt_int(nt, node, "sclass_prepend", 0), pre = is_prepend_name(nm) || marked;
  if (sg || marked) return pre ? LR_SG_PREPEND : LR_EXTEND;
  return is_include_name(nm) ? LR_INCLUDE : pre ? LR_PREPEND : LR_EXTEND;
}

/* The builtin classes and modules a header of the program reopens, by the
   name the header makes its class under (lr_reopened), taken before the
   walk: a reference to one before its header waits for the class
   (LR_LATER). So does a Thread or a Fiber an attr declaration names: the
   walk makes its reopening there (handle_attr_form). */
static void lr_reopens_take(Compiler *c) {
  const NodeTable *nt = c->nt;
  LoadRecord *r = c->lrec;
  for (int pass = 0; pass < 3; pass++)
    NT_FOREACH_KIND(nt, pass == 2 ? NK_CallNode : pass ? NK_ModuleNode : NK_ClassNode, n) {
      const char *nm = pass < 2 ? class_def_name(nt, n) : handle_attr_decl(nt, n);
      if (!nm || !(is_builtin_class_name(nm) || is_builtin_module_const_name(nm))) continue;
      r->reopens = lr_grow(r->reopens, &r->creopens, r->nreopens, sizeof *r->reopens);
      if (!(r->reopens[r->nreopens++] = strdup(nm))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
  if (r->nreopens > 1) qsort(r->reopens, (size_t)r->nreopens, sizeof *r->reopens, lr_name_cmp);
}

static int lr_body_defines(Compiler *c, int def, int direct, int depth);
static int lr_callbacks_define(Compiler *c, int depth);

/* May a call of `name` in a method body run a method of the program's
   that defines or writes something? Each place the program defines the
   name must be a def whose body does neither (lr_body_defines), and one
   the record has: a def not recorded yet (past the frontier, or in a
   method body, while the record is made), an attr, an alias or a
   define_method may. A name the program defines nowhere is a builtin's,
   which lr_node_defines answers for by its name. */
static int lr_callee_defines(Compiler *c, const char *name, int direct, int depth) {
  LoadRecord *r = c->lrec;
  int nn = 0, nd = 0;
  if (r->names_any) return 1;
  lr_name_range(r->names, r->nnames, sizeof *r->names, name, &nn);
  const LrSite *d = lr_name_range(r->defs, r->ndefs, sizeof *r->defs, name, &nd);
  if (nd < nn) return 1;
  for (int i = 0; i < nd; i++) {
    LoadPoint p = elab_point_of(c, d[i].node);
    if ((!r->recorded && (p.region < 0 || p.seq >= r->frontier)) || lr_body_defines(c, d[i].node, direct, depth + 1)) return 1;
  }
  return 0;
}

/* May node n call into the runtime, which may call back a method of the
   program's by itself (lr_callbacks_define): any call, and any node but
   those that only hold, read or write values. EL2a-2b's lr_node_runs
   draws the same line (its LR_DISPATCH); whichever lands second keeps
   one. */
static int lr_node_dispatches(const NodeTable *nt, int n) {
  switch (nt_kind(nt, n)) {
    case NK_ProgramNode: case NK_StatementsNode: case NK_BlockNode: case NK_ConstantReadNode: case NK_ConstantPathNode:
    case NK_SelfNode: case NK_NilNode: case NK_TrueNode: case NK_FalseNode: case NK_IntegerNode: case NK_FloatNode:
    case NK_StringNode: case NK_SymbolNode: case NK_ArrayNode: case NK_HashNode: case NK_KeywordHashNode:
    case NK_ParenthesesNode: case NK_BeginNode: case NK_IfNode: case NK_UnlessNode: case NK_ElseNode: case NK_WhileNode:
    case NK_UntilNode: case NK_AndNode: case NK_OrNode: case NK_ReturnNode: case NK_BreakNode: case NK_NextNode:
    case NK_ArgumentsNode: case NK_BlockParametersNode: case NK_ParametersNode: case NK_RequiredParameterNode:
    case NK_OptionalParameterNode: case NK_LocalVariableReadNode: case NK_LocalVariableWriteNode:
    case NK_InstanceVariableReadNode: case NK_GlobalVariableReadNode: case NK_ClassVariableReadNode:
      return 0;
    default: return 1;
  }
}

/* Does node n, run, define or write anything (lr_body_defines)? With
   direct, the runtime's own calls back into the program are left out */
static int lr_node_defines(Compiler *c, int n, int direct, int depth) {
  static const NodeKind kinds[] = {
    NK_DefNode, NK_SingletonClassNode, NK_AliasMethodNode, NK_AliasGlobalVariableNode, NK_UndefNode, NK_YieldNode,
    NK_SuperNode, NK_ForwardingSuperNode, NK_ForwardingArgumentsNode, NK_GlobalVariableWriteNode,
    NK_GlobalVariableOrWriteNode, NK_GlobalVariableAndWriteNode, NK_GlobalVariableOperatorWriteNode,
    NK_GlobalVariableTargetNode, NK_InstanceVariableWriteNode, NK_InstanceVariableOrWriteNode,
    NK_InstanceVariableAndWriteNode, NK_InstanceVariableOperatorWriteNode, NK_InstanceVariableTargetNode,
    NK_ClassVariableWriteNode, NK_ClassVariableOrWriteNode, NK_ClassVariableAndWriteNode,
    NK_ClassVariableOperatorWriteNode, NK_ClassVariableTargetNode };
  const NodeTable *nt = c->nt;
  if (n < 0) return 0;
  NodeKind k = nt_kind(nt, n);
  int nc = 0;
  const NodeKind *ck = lr_const_write_kinds(&nc);
  for (int i = 0; i < (int)(sizeof kinds / sizeof *kinds); i++) if (k == kinds[i]) return 1;
  for (int i = 0; i < nc; i++) if (k == ck[i]) return 1;
  if (k == NK_BlockArgumentNode && nt_kind(nt, nt_ref(nt, n, "expression")) != NK_SymbolNode) return 1;   /* a block passed on */
  if (k == NK_CallNode) {
    const char *nm = nt_str(nt, n, "name");
    if (!nm || is_load_effect_call(nm) || (lr_named(c, nm) && lr_callee_defines(c, nm, direct, depth)) ||
        (is_new_name(nm) && lr_callee_defines(c, "initialize", direct, depth))) return 1;
  }
  if (!direct && lr_node_dispatches(nt, n) && lr_callbacks_define(c, depth)) return 1;
  for (int i = 0, nr = nt_num_refs(nt, n); i < nr; i++)
    if (lr_node_defines(c, nt_ref_at(nt, n, i), direct, depth)) return 1;
  for (int i = 0, na = nt_num_arrs(nt, n); i < na; i++) {
    int m = 0;
    const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) if (lr_node_defines(c, ids[j], direct, depth)) return 1;
  }
  return 0;
}

/* Does the body of def node `def`, run, define or write anything: a def,
   a class, module or alias, a call that changes what the record holds or
   runs code it cannot follow (is_load_effect_call), a write of a
   constant, a global, a class variable or an instance variable (its
   class's, run on a class), a yield, a super or a block passed on, a call
   of a method of the program's that may (lr_callee_defines; `new` runs
   initialize), or, but with direct, a node by which the runtime may call
   back one that may (lr_callbacks_define)? A def asked again while it is
   asked (a cycle) may. Memoized by the def (LR_KEY_DEFINES; "direct" for
   the direct answer). */
static int lr_body_defines(Compiler *c, int def, int direct, int depth) {
  LoadRecord *r = c->lrec;
  const char *key = direct ? "direct" : "";
  const LrHist *h = lr_hist(r, LR_KEY_DEFINES, def, key, 0);
  if (h && h->n) return h->v[h->n - 1].vis;
  if (depth > 64) return 1;
  lr_version(r, LR_KEY_DEFINES, def, key, (LrDef){ def, LR_OPAQUE, 1, 0, 0, NULL });   /* while it is asked */
  int o = lr_node_defines(c, nt_ref(c->nt, def, "parameters"), direct, depth) ||
          lr_node_defines(c, nt_ref(c->nt, def, "body"), direct, depth);
  LrHist *m = lr_hist(r, LR_KEY_DEFINES, def, key, 0);
  m->v[m->n - 1].vis = (unsigned char)o;
  return o;
}

/* May a method the runtime calls back by itself (a protocol's,
   method_name_implicitly_invoked; one a builtin or a node's syntax
   calls on an object, ic_core_callback) define or write something?
   Its direct answer is enough: what it calls back in turn is one of these
   too. Kept once no such method may, or once the record is made. */
static int lr_callbacks_define(Compiler *c, int depth) {
  LoadRecord *r = c->lrec;
  if (r->callbacks) return r->callbacks == 2;
  int o = 0;
  for (int i = 0; i < r->nnames && !o; i++)
    if ((i == 0 || strcmp(r->names[i - 1], r->names[i]) != 0) &&
        (method_name_implicitly_invoked(r->names[i]) || ic_core_callback(r->names[i], 0)))
      o = lr_callee_defines(c, r->names[i], 1, depth);
  if (!o || r->recorded) r->callbacks = (unsigned char)(o ? 2 : 1);
  return o;
}

/* Does a call of `name` on class cls (its singleton when sg) at p run a
   method of the program's that may define or write something: any one
   it may find but a def whose body does neither (lr_body_defines)? */
static int lr_runs_defining(Compiler *c, int cls, int sg, const char *name, LoadPoint p) {
  const LrDef *d = NULL;
  int o = lr_search(c, cls, sg, name, -1, p, 0, 0, &d, 0);
  return o != ELAB_NO && !(o == ELAB_YES && d && d->kind == LR_DEF && !lr_body_defines(c, d->node, 0, 0));
}

/* Does call `node` on class cls (in its singleton when sg) run code the
   record cannot follow: a send, an eval of a String, or a method of the
   program that may define or write something (lr_runs_defining), called
   on the class (its singleton method, from its body or through its
   constant) or from its body (a top-level one)? */
static int lr_opaque_call(Compiler *c, int node, int cls, int sg) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, node, "name");
  int recv = nt_ref(nt, node, "receiver");
  LoadPoint p = elab_point_of(c, node);
  int bare = recv < 0 || nt_kind(nt, recv) == NK_SelfNode;
  return is_send_family(nm) || (is_eval_exec_family(nm) && nt_ref(nt, node, "block") < 0) ||
         (!sg && lr_runs_defining(c, cls, 1, nm, p)) || (bare && lr_runs_defining(c, LR_MAIN, 0, nm, p));
}
/* Is call `node` (named nm) a definition form a statement may hold in its
   parts (`r = attr_reader :a`, `p(private :b)`, `memoize private def c`):
   one that gives, takes or sets methods its arguments name? */
static int lr_nested_form(const char *nm) {
  return nm && (visibility_of_name(nm) || class_method_vis_of_name(nm) || is_method_naming_call(nm) ||
                is_attr_decl_reader(nm) || is_attr_decl_writer(nm));
}

/* A definition form a visibility word takes as its argument, whose value
   names the methods it defines (`private attr_accessor :a`: [:a, :a=],
   `private define_method(:m) { }`, `private alias_method :a, :b`): 1 when
   it is one, its methods then set to bvis on (cls, sg) at the word's node */
static int lr_wrapped_vis(ElabRec *x, int call, int cls, int sg, int node, int bvis, int may) {
  const NodeTable *nt = x->c->nt;
  const char *nm = nt_kind(nt, call) == NK_CallNode ? nt_str(nt, call, "name") : NULL;
  int an = nm ? nt_ref(nt, call, "arguments") : -1, ac = 0;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  if (is_attr_decl_reader(nm) || is_attr_decl_writer(nm)) {
    for (int i = 0; i < ac; i++) {
      const char *a = vis_arg_name(nt, av[i]);
      char w[256];
      if (is_attr_decl_reader(nm)) lr_method(x, cls, sg, a, node, LR_VIS, bvis, may, NULL);
      if (a && is_attr_decl_writer(nm) && snprintf(w, sizeof w, "%s=", a) < (int)sizeof w)
        lr_method(x, cls, sg, w, node, LR_VIS, bvis, may, NULL);
    }
    return 1;
  }
  if ((is_define_method_name(nm) || is_alias_method_name(nm)) && ac > 0) {
    lr_method(x, cls, sg, vis_arg_name(nt, av[0]), node, LR_VIS, bvis, may, NULL);
    return 1;
  }
  return 0;
}

/* A call statement's definitions: attrs, aliases, visibility, mixins,
   define_method, undef_method / remove_method, on the class its receiver
   names. vs is the frame's visibility state, which an attr form or a
   define_method takes on its frame's own definee (self). A visibility word
   given a definition form records that form and sets the methods it
   defines; private_class_method and public_class_method set the class's
   singleton methods. nested: a form the statement holds in its parts
   (lr_nested_part), whose own call the form's answers (lr_form_runs, an opaque
   call) are not asked of. */
static void lr_call(ElabRec *x, int node, int fc, int fs, unsigned char *vs, int fnode, int may, int nested) {
  const NodeTable *nt = x->c->nt;
  const char *nm = nt_str(nt, node, "name");
  int sg, recv = nt_ref(nt, node, "receiver"), cls = lr_receiver(x, recv, fc, fs, &sg), an = nt_ref(nt, node, "arguments"), ac = 0;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  int bvis = visibility_of_name(nm), cvis = class_method_vis_of_name(nm), opaque = 0;
  int self = recv < 0 || nt_kind(nt, recv) == NK_SelfNode, st = self ? *vs : BVIS_PUBLIC;
  if (!nested && nm && (bvis || is_method_naming_call(nm) || is_mixin_call(nm))) lr_form_runs(x, node, cls, sg, may);
  if (bvis && !ac && self) {
    /* the frame's visibility from here on: kept by the frame's node, so
       also in a body whose class the record does not know */
    *vs = (unsigned char)bvis;
    if (!x->unplaced) lr_version(x->r, LR_KEY_VIS, fnode, "", (LrDef){ node, LR_VIS, (unsigned char)bvis, (unsigned char)may, 0, NULL });
    return;
  }
  if (!nm || cls < LR_MAIN) return;
  int attr_r = is_attr_decl_reader(nm), attr_w = is_attr_decl_writer(nm);
  int naming = bvis || cvis || attr_r || attr_w || is_method_naming_call(nm), dm = is_define_method_name(nm) || is_define_singleton_method_name(nm);
  /* `include A, B` puts A nearest the class (ancestors [C, A, B]), as
     `include B` then `include A` would, and so do prepend and extend: the
     arguments of a mixin are recorded last first */
  int mix = is_mixin_call(nm);
  for (int j = 0; j < ac; j++) {
    int i = mix ? ac - 1 - j : j;
    const char *an_ = vis_arg_name(nt, av[i]);
    NodeKind ak = nt_kind(nt, av[i]);
    int en = 1;
    const int *ev = &av[i];
    if ((bvis || cvis) && ak == NK_ArrayNode) ev = nt_arr(nt, av[i], "elements", &en);   /* private [:a, :b] */
    if (bvis && ak == NK_CallNode && lr_nested_form(nt_str(nt, av[i], "name"))) {
      lr_call(x, av[i], fc, fs, vs, fnode, may, 1);
      if (lr_wrapped_vis(x, av[i], cls, sg, node, bvis, may)) continue;
    }
    for (int k = 0; (bvis || cvis) && ak == NK_ArrayNode && k < en; k++)
      if (!vis_arg_name(nt, ev[k])) opaque = 1;
    if (!an_ && ak != NK_DefNode && (ak != NK_ArrayNode || !(bvis || cvis)) && naming && (!dm || i == 0)) opaque = 1;   /* a computed name */
    if ((bvis || cvis) && ak == NK_DefNode) {
      /* private def m, private_class_method def self.m */
      lr_def(x, av[i], cls, sg, bvis ? bvis : *vs, may);
      if (cvis && !sg && nt_ref(nt, av[i], "receiver") >= 0) lr_method(x, cls, 1, nt_str(nt, av[i], "name"), node, LR_VIS, cvis, may, NULL);
    }
    else if (bvis || cvis)
      for (int k = 0; k < en && (!cvis || !sg); k++)
        lr_method(x, cls, bvis ? sg : 1, vis_arg_name(nt, ev[k]), node, LR_VIS, bvis ? bvis : cvis, may, NULL);
    else if (attr_r || attr_w) {
      if (attr_r) lr_method(x, cls, sg, an_, node, LR_READER, st, may, NULL);
      char w[256];
      if (an_ && attr_w && snprintf(w, sizeof w, "%s=", an_) < (int)sizeof w)
        lr_method(x, cls, sg, w, node, LR_WRITER, st, may, NULL);
    }
    else if (mix) {
      LrClass *k = lr_class(x->r, cls);
      int mod = lr_class_ref_at(x->c, av[i], NULL);
      opaque |= mod < 0 && mod != LR_LATER;
      if (!k) continue;
      k->mix = lr_grow(k->mix, &k->cmix, k->nmix, sizeof *k->mix);
      k->mix[k->nmix++] = (LrMixin){ node, lr_mix_kind(nt, node, nm, sg), mod, av[i], .cond = !x->unplaced && x->r->at[node] >= 0 && x->r->st[x->r->at[node]].fmay,
                                     .applied = lr_stmt_applied(x->unplaced ? NULL : lr_stmt_of(x->r, node), x->unplaced, may) == 1, .open = fnode };
    }
    else if (is_undef_method_name(nm) || is_remove_method_name(nm))
      lr_method(x, cls, sg, an_, node, is_undef_method_name(nm) ? LR_UNDEF : LR_REMOVE, *vs, may, NULL);
  }
  if (dm && ac > 0)
    lr_method(x, cls, sg || is_define_singleton_method_name(nm), vis_arg_name(nt, av[0]), node, LR_DEFINE_METHOD,
              is_define_singleton_method_name(nm) ? BVIS_PUBLIC : st, may, NULL);
  if (is_alias_method_name(nm) && ac == 2)
    lr_method(x, cls, sg, vis_arg_name(nt, av[0]), node, LR_ALIAS, *vs, may, vis_arg_name(nt, av[1]));
  /* code the record cannot follow, run on a class (lr_opaque_call) */
  if (!naming && !mix && !nested) {
    opaque = lr_opaque_call(x->c, node, cls, sg);
    lr_settle_add(x->r, SET_OPAQUE, node, cls, sg, may, 0, NULL);
  }
  if (opaque && cls >= 0 && !x->unplaced)
    lr_version(x->r, LR_KEY_OPAQUE, lr_const_owner(cls), "", (LrDef){ node, LR_OPAQUE, 0, (unsigned char)may, 0, NULL });
}

/* The definition forms statement node p (standing at pp, in the frame fc,
   fs) holds in its parts, not as itself nor in a statement of its own: a
   def in an argument or a written value (`memoize def m`, `x = def y`), a
   definition form whose value is used (`r = attr_writer :c`, `p(attr_reader
   :e)`, `x = private def m`, a bare `p(private)` too). Each part is where
   elab_child puts it: a statement list, a lone statement and a body run
   later are the walk's, as are the bodies with a self of their own (a
   class's, a block's, a lambda's); `defined?(private)` runs nothing. Off
   the statement's path a part may not run where it would not on the path
   (elab_parts: a branch of `c ? attr_reader(:a) : nil`, the right side of
   `&&`). A form records its own arguments (lr_call). */
typedef struct { ElabRec *x; int fc, fs, fnode; unsigned char *vs; } LrNested;
static void lr_autoload(ElabRec *x, int node, int fc, int fs, int may);
static int lr_nested_part(void *ud, int ch, ElabPos *q) {
  LrNested *n = ud;
  const NodeTable *nt = n->x->c->nt;
  NodeKind k = nt_kind(nt, ch);
  if (q->kind == EP_LIST || q->kind == EP_NEW || (q->bits & LR_LATE) || q->defer) return 0;
  if (k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode || k == NK_BlockNode ||
      k == NK_LambdaNode || k == NK_DefinedNode) return 0;
  /* q->may is a condition inside the statement (lr_record_stmt starts the
     walk with none): a part that may not run where its statement runs (the
     right side of `&&`) is a may version of LR_MAY_IN, which a later
     statement of the list has not run for that (lr_may_dominates) */
  int smay = n->x->r->st[q->e].may, may = q->may ? LR_MAY_IN : smay;
  lr_autoload(n->x, ch, n->fc, n->fs, q->may || smay || q->partial);
  lr_const_write(n->x->c, ch, q->e, n->fc, n->fs, q->may || smay || q->partial);
  if (k == NK_DefNode) { lr_def(n->x, ch, n->fc, n->fs, *n->vs, may); return 0; }
  if (k == NK_CallNode && lr_nested_form(nt_str(nt, ch, "name"))) { lr_call(n->x, ch, n->fc, n->fs, n->vs, n->fnode, may, 1); return 0; }
  return 1;
}

/* A write of a global or of main's instance variable by statement e (in
   main's frame, a file's top level), on every path unless it may not run
   or may run partly (LrStmt.partial) */
static void lr_value_write(Compiler *c, int e) {
  const NodeTable *nt = c->nt;
  LoadRecord *r = c->lrec;
  const LrStmt *s = &r->st[e];
  int node = s->node, may = s->may || s->partial;
  NodeKind k = nt_kind(nt, node);
  switch (k) {
    case NK_GlobalVariableWriteNode: case NK_GlobalVariableOrWriteNode:   /* `$a = $b = v` writes both */
      for (int w = node; w >= 0 && (nt_kind(nt, w) == NK_GlobalVariableWriteNode || w == node); w = nt_ref(nt, w, "value")) {
        const char *gn = nt_str(nt, w, "name");
        int v = nt_ref(nt, w, "value");
        if (gn && *gn) lr_write(r, ELAB_GVAR, 0, gn + 1, node, lr_nil_value(nt, v), may || k == NK_GlobalVariableOrWriteNode);
      }
      break;
    case NK_InstanceVariableWriteNode: case NK_InstanceVariableOrWriteNode: {
      int v = nt_ref(nt, node, "value");
      if (s->frame < 0) lr_write(r, ELAB_MAIN_IVAR, 0, nt_str(nt, node, "name"), node, lr_nil_value(nt, v), may || k == NK_InstanceVariableOrWriteNode);
      break;
    }
    default: break;
  }
}

/* An alias or undef statement's entries, on class side (cls, sg) */
static void lr_alias_undef(ElabRec *x, int node, int cls, int sg, int bvis, int may) {
  const NodeTable *nt = x->c->nt;
  int n = 0, nn = nt_ref(nt, node, "new_name"), on = nt_ref(nt, node, "old_name");
  if (nt_kind(nt, node) == NK_AliasMethodNode)
    lr_method(x, cls, sg, nn >= 0 ? vis_arg_name(nt, nn) : NULL, node, LR_ALIAS, bvis, may, on >= 0 ? vis_arg_name(nt, on) : NULL);
  const int *v = nt_kind(nt, node) == NK_UndefNode ? nt_arr(nt, node, "names", &n) : NULL;
  for (int i = 0; i < n; i++) lr_method(x, cls, sg, vis_arg_name(nt, v[i]), node, LR_UNDEF, bvis, may, NULL);
}

/* A registration on a constant holding a value no module can be (`INST =
   Object.new; INST.autoload(...)`) is none: the call stays the run-time
   call, which raises. Asked once the record is made. */
static void lr_autoload_values(Compiler *c) {
  const NodeTable *nt = c->nt;
  int m = 0;
  for (int k = 0; k < c->nautoloads; k++) {
    const AutoloadRegistration *a = &c->autoloads[k];
    int recv = nt_ref(nt, a->node, "receiver");
    c->auto_at[a->node] = 0;
    if (a->slot || recv < 0 || nt_kind(nt, recv) == NK_SelfNode || !elab_value_not_module(c, recv)) c->autoloads[m++] = *a;
  }
  c->nautoloads = m;
  for (int i = m - 1; i >= 0; i--) c->auto_at[c->autoloads[i].node] = i + 1;
}

static void lr_autoload(ElabRec *x, int node, int fc, int fs, int may) {
  const NodeTable *nt = x->c->nt;
  int n; AutoloadRegistration *a = comp_autoloads(x->c, node, &n);
  if (!n) return;
  int recv = nt_ref(nt, node, "receiver");
  int owner = recv < 0 || nt_kind(nt, recv) == NK_SelfNode ?
    (fs == LR_FS_SCLASS ? lr_sg_const_owner(fc) : lr_const_owner(fc)) : lr_const_owner(lr_class_ref_at(x->c, recv, NULL));
  for (int k = 0; k < n; k++) {
    if (!owner) continue;
    LrHist *h = lr_hist(x->r, LR_KEY_AUTO, owner, a[k].name, 1);
    if (!h->n) a[k].first = 1;
    a[k].slot = (int)(h - x->r->hist) + 1;
    lr_version(x->r, LR_KEY_AUTO, owner, a[k].name, (LrDef){ .node = node, .kind = LR_WRITE, .may = (unsigned char)may, .scope = a[k].slot });
  }
}

/* What statement e defines or writes, in its frame */
static void lr_record_stmt(ElabRec *x, int e) {
  const NodeTable *nt = x->c->nt;
  LoadRecord *r = x->r;
  const LrStmt *s = &r->st[e];
  int node = s->node, f = s->frame, may = s->may, wmay = s->may || s->partial;
  int fc = f >= 0 ? x->fc[f] : LR_MAIN, fs = f >= 0 ? x->fs[f] : 0;
  unsigned char *vs = f >= 0 ? &x->vs[f] : &x->vmain[s->file];
  NodeKind k = nt_kind(nt, node);
  lr_frame(x, e);
  if ((k == NK_ClassNode || k == NK_ModuleNode || k == NK_ConstantWriteNode || k == NK_ConstantPathWriteNode ||
       k == NK_LocalVariableWriteNode) && x->fc[e] >= 0 && x->fc[e] != fc) {   /* a body the class runs */
    LrClass *kc = lr_class(r, x->fc[e]);
    int body = class_def_body(x->c->nt, node);
    int cp = k == NK_ClassNode || k == NK_ModuleNode ? nt_ref(nt, node, "constant_path") : -1;
    /* the first opening defines its constant: in its entity's owner;
       where that is not known, in the lexical scope for a name, in the
       class the scope walk nested it in for a path. A builtin's reopening
       finds Object's constant and writes none, as in CRuby. */
    if (kc && !kc->nopen && cp >= 0 && !lr_header_builtin(r, node)) {
      int owner;
      elab_header_class(x->c, node, &owner, NULL);
      owner = owner >= LR_MAIN ? lr_const_owner(owner) : nt_kind(nt, cp) == NK_ConstantReadNode ?
        lr_const_scope_owner(x->c, e) : lr_const_owner(x->c->classes[x->fc[e]].enclosing_class);
      lr_write(r, ELAB_CONST, owner, lr_written_name(nt, cp), node, 0, may);
    }
    int sc = class_def_super(nt, node);
    if (kc && sc >= 0 && !kc->has_sup) { kc->sup = lr_class_ref_at(x->c, sc, NULL); kc->sup_ref = sc; kc->has_sup = 1; }
    if (kc) { kc->open = lr_grow(kc->open, &kc->copen, kc->nopen, sizeof *kc->open); kc->open[kc->nopen++] = (LrOpen){ node, body }; }
  }
  switch (k) {
    case NK_DefNode: lr_def(x, node, fc, fs, *vs, may); break;
    case NK_AliasMethodNode: case NK_UndefNode: {
      int sg, cls = lr_definee(fc, fs, &sg);
      lr_alias_undef(x, node, cls, sg, *vs, may);
      break;
    }
    case NK_CallNode: lr_call(x, node, fc, fs, vs, f >= 0 ? r->st[f].node : nt_root(nt, s->file), may, 0); break;
    default: lr_value_write(x->c, e); break;
  }
  lr_autoload(x, node, fc, fs, wmay);
  lr_const_write(x->c, node, e, fc, fs, wmay);   /* in its lexical scope, or its explicit owner's */
  /* ...and the definition forms in its parts (a form records its own; a
     file's root holds its statements, each one of its own) */
  int own = k == NK_ProgramNode || k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode ||
            k == NK_AliasMethodNode || k == NK_UndefNode || (k == NK_CallNode && lr_nested_form(nt_str(nt, node, "name")));
  if (!own) {
    ElabPos at = { .e = e, .file = s->file, .top = s->top, .frame = f, .depth = s->depth, .rep = s->rep, .list = -1,
                   .may = 0, .partial = s->partial, .fmay = s->fmay, .fpartial = s->fpartial, .kind = EP_SPINE };
    LrNested n = { x, fc, fs, f >= 0 ? r->st[f].node : nt_root(nt, s->file), vs };
    elab_parts(x->c, node, &at, lr_nested_part, &n);
  }
}

/* A write a pass adds to the load phase after the record (a mixin guard's
   flag): stmt, a statement the walk reaches, written to a global or to
   main's instance variable, recorded as the record records one */
void elab_record_write(Compiler *c, int stmt) {
  LoadRecord *r = c->lrec;
  if (!r || !r->recorded) return;
  elab_fresh(c);
  int e = stmt >= 0 && stmt < r->n ? r->at[stmt] : -1;
  if (e >= 0 && !(r->bits[stmt] & LR_LATE) && r->st[e].node == stmt) lr_value_write(c, e);
}

/* A history for each class registration has made so far, held when the
   walk made it (before the class builders' step) */
static void lr_fit_classes(LoadRecord *r, Compiler *c) {
  if (c->nclasses + 1 <= r->ncls) return;
  r->cls = realloc(r->cls, sizeof *r->cls * (size_t)(c->nclasses + 1));
  r->held = realloc(r->held, (size_t)(c->nclasses + 1));
  if (!r->cls || !r->held) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memset(r->cls + r->ncls, 0, sizeof *r->cls * (size_t)(c->nclasses + 1 - r->ncls));
  memset(r->held + r->ncls, !r->structs, (size_t)(c->nclasses + 1 - r->ncls));
  r->ncls = c->nclasses + 1;
}

/* The statements before n, in seq order, that are not recorded yet */
static void lr_record_upto(LoadRecord *r, int n) {
  ElabRec *x = r->rec;
  if (!x) return;
  Compiler *c = x->c;
  lr_fit_classes(r, c);   /* the classes the walk has made since */
  if (r->frontier >= n) return;
  if (n > x->cap) {
    x->cap = n * 2;
    x->fc = realloc(x->fc, sizeof(int) * (size_t)x->cap);
    x->fs = realloc(x->fs, (size_t)x->cap);
    x->vs = realloc(x->vs, (size_t)x->cap);
    if (!x->fc || !x->fs || !x->vs) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  while (r->frontier < n) lr_record_stmt(x, r->frontier++);
}

/* The histories dropped, for the record to be made again */
static void lr_record_drop(LoadRecord *r) {
  for (int i = 0; i < r->nhist; i++) {
    for (int j = 0; j < r->hist[i].n; j++) free((char *)r->hist[i].v[j].target);
    free(r->hist[i].name); free(r->hist[i].v);
  }
  for (int i = 0; i < r->ncls; i++) { free(r->cls[i].open); free(r->cls[i].mix); }
  if (r->cls) memset(r->cls, 0, sizeof *r->cls * (size_t)r->ncls);
  for (int b = 0; r->hhead && b < LR_BUCKETS; b++) r->hhead[b] = -1;
  r->nhist = 0; r->frontier = 0; r->nset = 0; r->builders = 0;
  r->slot_builds = -1;
  r->nentries = 0; r->indexed = r->opened = -1;   /* the by-name index and the open bits are of the dropped histories */
  free(r->surf); free(r->surf_at); r->surf = NULL; r->surf_at = NULL;
}

static char *lr_dump_text(Compiler *c);
static void lr_check_walked(Compiler *c, const char *walked);
enum { LR_OWNERS_WALK = 1, LR_OWNERS_HELD = 2 };   /* lr_def_owners' checks */
static void lr_def_owners(Compiler *c, int check);
static void lr_later_settle(Compiler *c);

/* The definitions of a body of class ci that the record holds with no
   point (a builder's block, a class body in a lambda or a block kept to
   run later), which no walk of the load phase records (lr_record_stmt
   records a placed body's): its own statements', as a placed body's are
   but at no point a query sees (LrDef.unplaced, L3), after the placed
   ones: a def, an alias or an undef, and a definition form called on self
   (attrs, visibility, define_method, mixins, the arguments of one call
   last first as lr_call has them). fnode: the body's node. No condition stands
   between them and the body (LrMixin.cond); one inside a branch of the
   body is no statement of it, and is not recorded. */
static void lr_body_defs(Compiler *c, int ci, int body, int fnode) {
  const NodeTable *nt = c->nt;
  ElabRec x = { c, c->lrec, .unplaced = 1 };
  unsigned char vs = BVIS_PUBLIC;
  int n = 0;
  const int *st = nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &n) : &body;
  if (nt_kind(nt, body) != NK_StatementsNode) n = 1;
  for (int i = 0; i < n; i++) {
    NodeKind k = nt_kind(nt, st[i]);
    int recv = k == NK_CallNode ? nt_ref(nt, st[i], "receiver") : -1;
    if (k == NK_DefNode) lr_def(&x, st[i], ci, LR_FS_NONE, vs, 0);
    else if (k == NK_AliasMethodNode || k == NK_UndefNode) lr_alias_undef(&x, st[i], ci, 0, vs, 0);
    else if (k == NK_CallNode && (recv < 0 || nt_kind(nt, recv) == NK_SelfNode)) lr_call(&x, st[i], ci, LR_FS_NONE, &vs, fnode, 0, 1);
  }
}

/* A class or module body that is no statement of the load phase: one in
   a lambda or in a block kept to run later (`l = -> { class G; private
   :foo; end }`), or in an expression. It runs when the program runs that
   code, at no point the record orders, but it is its class's body all the
   same, as early as it can run: from the statement that makes the lambda
   or block (elab_point_from), after the bodies whose statements run
   before that one. A body the record cannot order against those is
   placed after them. */
static void lr_record_unplaced(Compiler *c) {
  const NodeTable *nt = c->nt;
  LoadRecord *r = c->lrec;
  lr_fit_classes(r, c);
  /* Deferred writes have no load point, but may complete before any read.
     Reuse the write forms and lexical owners of the placed histories. */
  int nk = 0;
  const NodeKind *kinds = lr_const_write_kinds(&nk);
  for (int k = 0; k <= nk; k++) {
    NodeKind kind = k < nk ? kinds[k] : NK_CallNode;
    NT_FOREACH_KIND(nt, kind, id) {
      if (id >= r->n || !(r->bits[id] & LR_LATE)) continue;
      if (kind == NK_CallNode && !is_const_set_name(nt_str(nt, id, "name"))) continue;
      lr_const_write(c, id, r->at[id], LR_UNKNOWN_CLASS, LR_FS_NONE, 1);
    }
  }
  int ndk = 0; const NodeKind *dk = class_def_kinds(&ndk);
  for (int dh = 0; dh < ndk; dh++)
    NT_FOREACH_KIND(nt, dk[dh], id) {
      if (!class_def_opens_frame(nt, id)) continue;
      int e = id < r->n ? r->at[id] : -1, ci = elab_header_class(c, id, NULL, NULL);
      if ((e >= 0 && r->st[e].node == id) || ci < 0) continue;
      LrClass *kc = lr_class(r, ci);
      int have = !kc;
      for (int i = 0; !have && i < kc->nopen; i++) have = kc->open[i].node == id;
      if (have) continue;
      kc->open = lr_grow(kc->open, &kc->copen, kc->nopen, sizeof *kc->open);
      int at = kc->nopen;
      while (at > 0) {
        int o = kc->open[at - 1].node, eo = o < r->n ? r->at[o] : -1;
        if (e < 0 || eo < 0 || r->st[eo].region != r->st[e].region || eo <= e) break;
        at--;
      }
      memmove(kc->open + at + 1, kc->open + at, sizeof *kc->open * (size_t)(kc->nopen - at));
      kc->open[at] = (LrOpen){ id, class_def_body(nt, id) };
      kc->nopen++;
      if (class_def_body(nt, id) >= 0) lr_body_defs(c, ci, class_def_body(nt, id), id);
    }
}

/* The record dropped and made again over the walked statements */
static void lr_record_again(ElabRec *x) {
  LoadRecord *r = x->r;
  r->recorded = 0;
  lr_record_drop(r);
  for (int f = 0; f < nt_nroots(x->c->nt); f++) x->vmain[f] = BVIS_PRIVATE;
  lr_record_upto(r, r->nst);
  lr_settle_regions(x->c);
  lr_later_settle(x->c);
  lr_record_unplaced(x->c);
  r->recorded = 1;
}

/* The references recorded before the header that makes their class
   (LR_LATER), as the walk leaves them: the class, or not known */
static void lr_later_settle(Compiler *c) {
  LoadRecord *r = c->lrec;
  for (int i = 0; i < r->ncls; i++) {
    LrClass *k = &r->cls[i];
    for (int j = 0; j < k->nmix; j++)
      if (lr_later(c, &k->mix[j].mod, k->mix[j].ref) == LR_LATER) k->mix[j].mod = LR_UNKNOWN_CLASS;
    if (k->has_sup && lr_later(c, &k->sup, k->sup_ref) == LR_LATER) k->sup = LR_UNKNOWN_CLASS;
  }
}

/* The scope walk of each file's top level, in the order the load phase
   runs them (elab_points_walk), which records what each statement defines
   once the walk has passed it, in that order. Besides the record, a
   statement's record reads what the program names before the walk (the
   method names, the builtins it reopens) and the classes registration has
   made so far; a reference to a builtin before the header that makes its
   class waits for it (lr_later). Check mode makes the record again after
   the walk and reports where the two differ. A form the walk runs splices
   through host h (elab_walk_host), which lowers each splice at once: the
   walk reaches its statements next. */
void elab_program(Compiler *c, struct HostStep *h) {
  LoadRecord *r = lr_get(c);
  int nroots = nt_nroots(c->nt);
  ElabRec x = { c, r, NULL, NULL, NULL, malloc((size_t)nroots), 0 };
  r->hhead = malloc(sizeof(int) * LR_BUCKETS);
  if (!r->hhead || !x.vmain) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int b = 0; b < LR_BUCKETS; b++) r->hhead[b] = -1;
  /* each file's top level starts private: its methods are Object's private ones */
  for (int f = 0; f < nroots; f++) x.vmain[f] = BVIS_PRIVATE;
  lr_names_take(c);
  /* Name membership is program-wide, not receiver ownership. Any possible
     definition keeps base's policy: refuse receiverless calls and handle
     explicit receivers as ordinary calls, with no registration for either. */
  if (lr_named(c, "autoload")) c->nautoloads = 0;
  lr_reopens_take(c);
  r->rec = &x;
  r->host = h;
  h->now = 1;
  elab_points_walk(c, 1);
  h->now = 0;
  r->host = NULL;
  double t0 = sp_timing_now();   /* the record's own cost after the walk (the map's is an_elab_points) */
  lr_record_upto(r, r->nst);
  lr_settle_regions(c);
  lr_later_settle(c);
  lr_record_unplaced(c);
  r->recorded = 1;
  if (r->check) {   /* the same record made after the walk, as a check */
    char *walked = lr_dump_text(c);
    lr_record_again(&x);
    lr_check_walked(c, walked);
    free(walked);
  }
  lr_autoload_values(c);
  r->rec = NULL;
  free(x.fc); free(x.fs); free(x.vs); free(x.vmain);
  /* a mixin's duplicate answer asked while the record was made saw only
     what was recorded by then (lr_mixin_dup) */
  for (int k = 0; k < r->ncls; k++)
    for (int i = 0; i < r->cls[k].nmix; i++) r->cls[k].mix[i].dup = 0;
  lr_def_owners(c, r->check ? LR_OWNERS_WALK : 0);
  elab_mark_versions(c);
  sp_timing_end(t0, "an_elab_record", "");
}

static int lr_applied(Compiler *c, const LrDef *v);

/* A def's or define_method's owner, the class side it defines on, is a
   fact of the frame it is written in: each method scope the scope walk made
   of one the record holds takes its history's, whether or not a load point
   applies it (a def under a condition or in an iterator's block, one in a
   body the walk does not place), and a define_method unrolled over computed
   names, which no history holds, takes the class of the record's body it
   runs in (lr_unrolled_owners). The walk's own class stays for what the
   record holds no entry of: a def in a method body, in a lambda or a block
   kept to run later, on an object, or in a frame whose class the record
   does not know (a class_eval on a value). check, under SPINEL_ELAB_CHECK:
   LR_OWNERS_WALK, where the walk's class differs (as the walk leaves the
   scopes, before the class builders' classes are made); LR_OWNERS_HELD,
   each def or define_method the walk made a scope of that the record has
   no owner for, and why. */
static int lr_def_on_object(const NodeTable *nt, int def);
static void lr_unrolled_owners(Compiler *c);
static void lr_def_owners(Compiler *c, int check) {
  LoadRecord *r = c->lrec;
  unsigned char *held = check & LR_OWNERS_HELD ? calloc((size_t)c->nscopes + 1, 1) : NULL;
  if ((check & LR_OWNERS_HELD) && !held) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < r->nhist; i++) {
    const LrHist *h = &r->hist[i];
    int cls = h->a == LR_MAIN ? -1 : h->a;
    for (int j = 0; j < h->n; j++) {
      const LrDef *v = &h->v[j];
      if ((v->kind != LR_DEF && v->kind != LR_DEFINE_METHOD) || !v->scope || v->scope >= c->nscopes) continue;
      if (held) held[v->scope] = h->a < LR_MAIN ? 2 : 1;
      if (h->a < LR_MAIN) continue;
      Scope *sc = &c->scopes[v->scope];
      if (check & LR_OWNERS_WALK) {
        LrCheck *f = lr_family(r, "the record's def owners");
        f->asked++;
        if ((sc->class_id != cls || sc->is_cmethod != h->b) && f->differ++ < LR_CHECK_SHOW)
          fprintf(stderr, "spinel-elab-check: owner %s walk=%d/%d record=%d/%d\n", sc->name, sc->class_id, sc->is_cmethod, cls, h->b);
      }
      if (sc->class_id != cls || sc->is_cmethod != h->b) { sc->class_id = cls; sc->is_cmethod = h->b; comp_touch_scope(sc->idx); }
    }
  }
  lr_unrolled_owners(c);
  for (int n = 0; held && n < r->ndscope; n++) {
    int s = r->dscope[n];
    if (!s || s >= c->nscopes || held[s] == 1) continue;
    LrCheck *f = lr_family(r, "the record's def owners: a def left to the walk's class");
    f->asked++;
    if (f->differ++ < LR_CHECK_SHOW) {
      char d[96];
      lr_node_desc(c, n, d, sizeof d);
      const char *why = held[s] ? "in a frame of no known class" : lr_def_on_object(c->nt, n) ? "on an object"
                        : elab_body_def(c, n) >= 0 ? "in a method body" : "with no entry in the record";
      fprintf(stderr, "spinel-elab-check: owner %s left to the walk (%s), %s, walk=%d/%d\n", c->scopes[s].name, d, why,
              c->scopes[s].class_id, c->scopes[s].is_cmethod);
    }
  }
  free(held);
}

/* A define_method statement that unrolls, `[lits].each { |v|
   define_method("m_#{v}") { } }`, one method scope per element on the
   inner call (collect_dm_each_unroll): its names are computed, so no
   history holds it, but it defines on the class of the body it is a
   statement of, as the record has its statements: in a placed body, a
   record statement whose frame is the body's (a class's LrOpen) that the
   load phase applies (lr_stmt_applied): in the arm a constant condition
   takes, which the record folds, but not under a run-time condition, which
   only the run time decides, nor in an iterator's block, where the walk's
   class stays (fix_struct_block_scopes refuses one in a class builder's
   block); in a body the walk does not place, one of its own statements, as
   lr_body_defs has them. */
static void lr_unrolled_owner(Compiler *c, int call, int ci) {
  int dn = nt_kind(c->nt, call) == NK_CallNode ? dm_each_inner(c->nt, call) : -1;
  for (int k = 0; dn >= 0 && k < c->nscopes; k++)
    if (c->scopes[k].def_node == dn && c->scopes[k].class_id != ci) { c->scopes[k].class_id = ci; comp_touch_scope(c->scopes[k].idx); }
}
static void lr_unrolled_owners(Compiler *c) {
  LoadRecord *r = elab_fresh(c);
  int *fc = malloc(sizeof(int) * (size_t)(r->nst + 1));
  if (!fc) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int e = 0; e < r->nst; e++) fc[e] = -1;
  for (int ci = 0; ci < c->nclasses; ci++) {
    int nb = 0;
    const LrOpen *o = elab_class_bodies(c, ci, &nb);
    for (int i = 0; i < nb; i++) {
      const LrStmt *f = lr_stmt_of(r, o[i].node);
      int n = 0;
      const int *st = !f && o[i].body >= 0 && nt_kind(c->nt, o[i].body) == NK_StatementsNode ? nt_arr(c->nt, o[i].body, "body", &n) : NULL;
      if (f) fc[f - r->st] = ci;
      for (int k = 0; k < n; k++) lr_unrolled_owner(c, st[k], ci);
    }
  }
  for (int e = 0; e < r->nst; e++) {
    const LrStmt *s = &r->st[e];
    if (s->frame >= 0 && fc[s->frame] >= 0 && lr_stmt_applied(s, 0, s->may) == 1) lr_unrolled_owner(c, s->node, fc[s->frame]);
  }
  free(fc);
}

/* See analyze.h */
int elab_def_applied(Compiler *c, int def) {
  LoadRecord *r = elab_fresh(c);
  const LrStmt *s = lr_stmt_of(r, def);
  if (!s || s->region < 0) return 1;
  if (s->fpartial && s->rep >= 0 && dm_each_inner(c->nt, r->st[s->rep].node) == def) s = &r->st[s->rep];
  return lr_stmt_applied(s, 0, s->may) == 1;
}

/* Is def node `def` on an object: on a receiver neither self nor a
   constant (`def obj.m`), which value flow homes (register_singleton_defs)? */
static int lr_def_on_object(const NodeTable *nt, int def) {
  int recv = nt_kind(nt, def) == NK_DefNode && nt_ref(nt, def, "receiver") >= 0 ? nt_def_receiver(nt, nt_ref(nt, def, "receiver")) : -1;
  NodeKind k = recv >= 0 ? nt_kind(nt, recv) : NK_SelfNode;
  return k != NK_SelfNode && k != NK_ConstantReadNode && k != NK_ConstantPathNode;
}

/* The block of the class builder class ci is made by (`X = Struct.new(:a)
   do`), -1 for a class no builder makes */
static int lr_builder_body(Compiler *c, int ci) {
  int d = c->classes[ci].def_node, body = d >= 0 ? class_def_body(c->nt, d) : -1;
  NodeKind k = d >= 0 ? nt_kind(c->nt, d) : NK_NilNode;
  return k == NK_ConstantWriteNode || k == NK_LocalVariableWriteNode ? body : -1;
}

/* The class builders' blocks (`X = Struct.new(:a) do ... end`) are bodies
   of classes that registration makes after the walk (register_structs):
   once it has, a record that holds such a block with no class is made
   again over the same statements, its frame then the builder's class. A
   builder that is no statement of the load phase (one in an expression,
   which desugar_inline_struct_blocks writes to a local, or in a method
   body) runs its block where no point is: the block is its class's body
   all the same, after the ones that have a point. */
void elab_record_builders(Compiler *c) {
  LoadRecord *r = c->lrec;
  if (!r || !r->recorded) return;
  r->structs = 1;
  r->runs_of = -1;   /* lr_rta's facts are of the builders' classes from here: computed afresh */
  lr_fit_classes(r, c);
  for (int ci = 0; ci < c->nclasses; ci++) if (lr_builder_body(c, ci) >= 0) r->held[ci + 1] = 1;
  if (r->builders) {
    double t0 = sp_timing_now();
    elab_fresh(c);
    ElabRec x = { c, r, NULL, NULL, NULL, malloc((size_t)nt_nroots(c->nt)), 0 };
    if (!x.vmain) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    r->rec = &x;
    lr_record_again(&x);
    r->rec = NULL;
    free(x.fc); free(x.fs); free(x.vs); free(x.vmain);
    sp_timing_end(t0, "an_elab_record", "builders");
  }
  for (int ci = 0; ci < c->nclasses; ci++) {
    int d = c->classes[ci].def_node, body = lr_builder_body(c, ci);
    if (body < 0) continue;
    LrClass *kc = lr_class(r, ci);
    int have = 0;
    for (int i = 0; i < kc->nopen; i++) if (kc->open[i].body == body) have = 1;
    if (have) continue;
    kc->open = lr_grow(kc->open, &kc->copen, kc->nopen, sizeof *kc->open);
    kc->open[kc->nopen++] = (LrOpen){ d, body };
    lr_body_defs(c, ci, body, d);
  }
  lr_def_owners(c, r->check ? LR_OWNERS_HELD : 0);
}

static void lr_settle_add(LoadRecord *r, int fam, int node, int cls, int sg, int may, int vis0, const char *name) {
  r->set = lr_grow(r->set, &r->cset, r->nset, sizeof *r->set);
  r->set[r->nset++] = (LrSettle){ fam, node, cls, sg, may, vis0, name };
}

/* The answers stored as the record was made (LrSettle), made again once all
   of it is. They were asked of the histories as the walk had made them, and
   a file in a region after region 0 is walked after everything the entry
   does, so they are blind to it: a method it defines would have changed a
   "runs no program code", a module it defines a mixin's, a method it makes
   private an alias's visibility. Asked again of the whole histories the
   same questions answer UNKNOWN across regions, and are the same where no
   other region matters. In the order the answers depend on one another:
   the classes a mixin and a superclass name, then the forms, the calls and
   the aliases. An answer that can only be taken back by a later region is
   all that changes: a mixin whose module the record no longer knows makes
   its call opaque, as an unknown module does at the time. */
static void lr_settle_regions(Compiler *c) {
  LoadRecord *r = c->lrec;
  if (r->nregion < 2) return;
  for (int i = 0; i < r->ncls; i++) {
    LrClass *k = &r->cls[i];
    for (int j = 0; j < k->nmix; j++) {
      int mod = lr_class_ref_at(c, k->mix[j].ref, NULL), was = k->mix[j].mod;
      k->mix[j].mod = mod;
      if (mod < 0 && mod != LR_LATER && !(was < 0 && was != LR_LATER) && i >= 1)
        lr_version(r, LR_KEY_OPAQUE, lr_const_owner(i - 1), "", (LrDef){ k->mix[j].node, LR_OPAQUE, 0, (unsigned char)elab_point_may(c, k->mix[j].node), 0, NULL });
    }
    if (k->has_sup) k->sup = lr_class_ref_at(c, k->sup_ref, NULL);
  }
  for (int i = 0; i < r->nset; i++) {
    const LrSettle *t = &r->set[i];
    LrHist *h;
    if (t->fam == SET_RUNS) {
      h = lr_hist(r, LR_KEY_RUNS, t->node, "", 0);
      if (h && h->n) h->v[h->n - 1].vis = (unsigned char)lr_form_answer(c, t->node, t->cls, t->sg);
    }
    else if (t->fam == SET_OPAQUE) {
      h = lr_hist(r, LR_KEY_OPAQUE, lr_const_owner(t->cls), "", 0);
      int seen = 0;
      for (int j = 0; h && j < h->n; j++) seen |= h->v[j].node == t->node;
      if (!seen && t->cls >= 0 && lr_opaque_call(c, t->node, t->cls, t->sg))
        lr_version(r, LR_KEY_OPAQUE, lr_const_owner(t->cls), "", (LrDef){ t->node, LR_OPAQUE, 0, (unsigned char)t->may, 0, NULL });
    }
    else {
      h = lr_hist(r, t->cls, t->sg, t->name, 0);
      const char *tg = NULL;
      for (int j = 0; h && j < h->n; j++)
        if (h->v[j].node == t->node && (h->v[j].kind == LR_ALIAS || h->v[j].kind == LR_DEF) && h->v[j].target) {
          tg = h->v[j].target;
          int ov = tg ? lr_vis_at(c, t->cls, tg, t->sg, elab_point_of(c, t->node)) : -1;
          h->v[j].vis = (unsigned char)(ov >= 0 ? ov : t->vis0);
        }
    }
  }
}

/* ---- by name ----
   The method histories of one name across the classes, chained through
   LrHist.nlink from the name's entry; nver counts the versions they hold.
   A version is an entry that gives or takes the method (a def, a
   define_method, an attr, an alias, a module_function copy, an undef or
   remove), not a visibility change. lr_vers_index indexes the histories as
   they stand, again once more entries have been recorded. */
typedef struct LrVers { const char *name; int first, nver, next; unsigned char captured, replaced; } LrVers;   /* captured: elab_version_captured's memo, 1 no, 2 yes; replaced: one history holds two versions, or an alias */

static int lr_is_version(const LrDef *v) { return v->kind <= LR_REMOVE && !v->unplaced; }

static void lr_vers_index(LoadRecord *r) {
  if (r->vers_head && r->indexed == r->nentries) return;
  if (!r->vers_head && !(r->vers_head = malloc(sizeof(int) * LR_BUCKETS))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int b = 0; b < LR_BUCKETS; b++) r->vers_head[b] = -1;
  r->nvers = 0;
  for (int i = 0; i < r->nhist; i++) {
    LrHist *h = &r->hist[i];
    h->nlink = -1;
    if (h->a < LR_MAIN) continue;
    unsigned b = lr_hash(0, 0, h->name);
    int k = r->vers_head[b];
    while (k >= 0 && !sp_streq(r->vers[k].name, h->name)) k = r->vers[k].next;
    if (k < 0) {
      r->vers = lr_grow(r->vers, &r->cvers, r->nvers, sizeof *r->vers);
      k = r->nvers++;
      r->vers[k] = (LrVers){ h->name, -1, 0, r->vers_head[b], 0 };
      r->vers_head[b] = k;
    }
    h->nlink = r->vers[k].first;
    r->vers[k].first = i;
    int nh = 0;
    for (int j = 0; j < h->n; j++) {
      nh += lr_is_version(&h->v[j]);
      if (h->v[j].kind == LR_ALIAS && !h->v[j].unplaced) r->vers[k].replaced = 1;
    }
    r->vers[k].nver += nh;
    if (nh > 1) r->vers[k].replaced = 1;
  }
  r->indexed = r->nentries;
}

static LrVers *lr_vers(LoadRecord *r, const char *name) {
  if (!name) return NULL;
  lr_vers_index(r);
  for (int k = r->vers_head[lr_hash(0, 0, name)]; k >= 0; k = r->vers[k].next)
    if (sp_streq(r->vers[k].name, name)) return &r->vers[k];
  return NULL;
}

int elab_method_versions(Compiler *c, const char *name) {
  LoadRecord *r = c->lrec;
  const LrVers *x = r && r->recorded ? lr_vers(r, name) : NULL;
  return x ? x->nver : 0;
}

int elab_method_replaced(Compiler *c, const char *name) {
  LoadRecord *r = c->lrec;
  const LrVers *x = r && r->recorded ? lr_vers(r, name) : NULL;
  return x && x->replaced;
}

int elab_method_holder(Compiler *c, const char *name, int i, int *cls, int *sg) {
  LoadRecord *r = c->lrec;
  if (!r || !r->recorded) return -1;
  if (i < 0) {
    const LrVers *x = lr_vers(r, name);
    i = x ? x->first : -1;
  }
  else i = r->hist[i].nlink;
  if (i >= 0) { *cls = r->hist[i].a; *sg = r->hist[i].b; }
  return i;
}

/* Each method scope's version facts (analyze.h):
   Scope.versioned when the record holds more than one version of its name;
   Scope.replaced when it is a def's scope that a later scope of the same
   class, side and name takes the method table's place of (a top-level
   method: a later def's), so that the tables answer the later one. */
typedef struct { const char *name; int cls, cm, s; } LrScopeKey;   /* s: -1 - s for a scope no def made */
static int lr_scope_key_cmp(const void *a, const void *b) {
  const LrScopeKey *x = a, *y = b;
  int d = strcmp(x->name, y->name);
  if (d) return d;
  if (x->cls != y->cls) return x->cls < y->cls ? -1 : 1;
  if (x->cm != y->cm) return x->cm - y->cm;
  int xs = x->s < 0 ? -1 - x->s : x->s, ys = y->s < 0 ? -1 - y->s : y->s;
  return xs - ys;
}
void elab_mark_versions(Compiler *c) {
  LoadRecord *r = c->lrec;
  LrScopeKey *keys = NULL;
  int nk = 0;
  if (!r || !r->recorded) return;
  for (int s = 1; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (sc->versioned || sc->replaced) { sc->versioned = sc->replaced = 0; comp_touch_scope(sc->idx); }
    const char *name = sc->mixin_guarded ? comp_scope_user_name(sc) : sc->name;
    const LrVers *x = name && sc->def_node >= 0 ? lr_vers(r, name) : NULL;
    if (!x || x->nver < 2) continue;
    { sc->versioned = 1; comp_touch_scope(sc->idx); }
    int def = nt_kind(c->nt, sc->def_node) == NK_DefNode;
    if (!x->replaced || (sc->class_id < 0 && !def)) continue;
    if (!(nk & (nk - 1)) && !(keys = realloc(keys, sizeof *keys * (size_t)(nk ? 2 * nk : 1)))) {
      fprintf(stderr, "spinel: out of memory\n"); exit(1);
    }
    keys[nk++] = (LrScopeKey){ name, sc->class_id < 0 ? -1 : sc->class_id, sc->class_id < 0 ? 0 : sc->is_cmethod, def ? s : -1 - s };
  }
  if (nk > 1) qsort(keys, (size_t)nk, sizeof *keys, lr_scope_key_cmp);
  for (int i = 0; i + 1 < nk; i++)
    if (keys[i].s >= 0 && keys[i].cls == keys[i + 1].cls && keys[i].cm == keys[i + 1].cm && sp_streq(keys[i].name, keys[i + 1].name)) {
      Scope *sc = &c->scopes[keys[i].s];
      sc->replaced = 1;
      comp_touch_scope(sc->idx);
    }
  free(keys);
}

/* The parents elab_body_def and elab_runs_with climb: those the map
   placed each node through, the frame's inside it (elab_plan_enter). The
   parent map can answer a node a desugaring left hung nowhere, which still
   holds a child it shares with the node that replaced it. */
const int *elab_node_parents(Compiler *c, int *n) {
  LoadRecord *r = elab_fresh(c);
  *n = r->n;
  return r->plan_depth ? r->plan_par : r->up;
}

/* The def node whose body holds node, with no class body or other def
   between; -1 for none. */
static int lr_body_def(const NodeTable *nt, const int *par, int n, int node) {
  for (int p = node >= 0 && node < n ? par[node] : -1; p >= 0 && p < n; p = par[p]) {
    NodeKind k = nt_kind(nt, p);
    if (k == NK_DefNode) return p;
    if (k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode) return -1;
  }
  return -1;
}
int elab_body_def(Compiler *c, int node) {
  int n;
  const int *par = elab_node_parents(c, &n);
  return lr_body_def(c->nt, par, n, node);
}

/* Does node run whenever the body of def node `def` runs: is it in that
   body with no lambda, nested def or block kept to run later between? */
int elab_runs_with(Compiler *c, int node, int def) {
  int n;
  const int *par = elab_node_parents(c, &n);
  for (int p = node >= 0 && node < n ? par[node] : -1; p >= 0 && p < n; p = par[p]) {
    if (p == def) return 1;
    if (lr_defers(c, par, p)) return 0;
  }
  return 0;
}

/* ---- the queries ---- */
/* The live version of history h at p (late: as of the load's end; LR_ONCE:
   at p on the first run of the bodies around it, which may run again):
   ELAB_YES, ELAB_NO (none: ask on), LR_STOP (undefined here), ELAB_UNKNOWN. */
enum { LR_STOP = 3, LR_ONCE = 2 };
/* Has may version v run when p does: is p after v in the statement list
   holding v's statement, or in a body nested in a later statement of that
   list (the same branch has run v)? Climbs the statements whose bodies
   hold p's, as deep as p is nested. */
static int lr_may_dominates(const LoadRecord *r, const LrDef *v, LoadPoint p) {
  const LrStmt *sv = lr_stmt_of(r, v->node);
  if (v->may == LR_MAY_IN || !sv || sv->region < 0 || sv->list < 0 || p.region != sv->region || p.seq < 0 || p.seq >= r->nst) return 0;
  for (int x = p.seq; x >= 0; x = r->st[x].up)
    if (r->st[x].list == sv->list) return x > (int)(sv - r->st);
  return 0;
}

/* Does child x of node p run before its child y? Two elements of one
   list run in its order; other children in the walk's (node ids, as
   elab_order orders a statement's nodes), a ref that runs after the
   arrays (elab_ref_last) last. */
static int lr_child_first(const NodeTable *nt, int p, int x, int y) {
  for (int i = 0, na = nt_num_arrs(nt, p); i < na; i++) {
    int m = 0, ix = -1, iy = -1;
    const int *v = nt_arr_at(nt, p, i, &m);
    for (int j = 0; j < m; j++) { if (v[j] == x) ix = j; if (v[j] == y) iy = j; }
    if (ix >= 0 && iy >= 0) return ix < iy;
  }
  int lx = elab_ref_last(nt, p, x), ly = elab_ref_last(nt, p, y);
  return lx != ly ? ly : x < y;
}

/* How node d of n's statement runs against n, as CRuby runs them: a
   call runs after its operands, so d in n's receiver, arguments or block
   argument runs before it, and d in n's block in it, after it; other two
   part at a node, whose children run in their order (lr_child_first). A d
   ahead of n on a path that may not run whenever the node where they part
   does (elab_child's spine: a branch, the right side of `&&` or `||`, a
   block, a lambda or a method body) may not have run. PROG_BEFORE,
   PROG_AFTER, or PROG_UNKNOWN, as during the registration's walk, which
   has no parents yet. */
static int lr_run_order(Compiler *c, int d, int n) {
  const NodeTable *nt = c->nt;
  int np = 0;
  const int *par = elab_node_parents(c, &np);
  int may = 0;
  for (int x = d; par && x >= 0 && x < np && par[x] >= 0; x = par[x]) {
    int p = par[x], y = n;
    if (p == n) {
      if (x == nt_ref(nt, n, "block") && nt_kind(nt, x) == NK_BlockNode) return PROG_AFTER;
      return may ? PROG_UNKNOWN : PROG_BEFORE;
    }
    while (y >= 0 && y < np && par[y] != p) y = par[y];
    if (y >= 0 && y < np) return !lr_child_first(nt, p, x, y) ? PROG_AFTER : may ? PROG_UNKNOWN : PROG_BEFORE;
    ElabPos pp = { .e = 0, .kind = EP_SPINE };
    ElabPos q = elab_child(c, p, x, &pp);
    may |= q.may || q.partial || q.defer || q.kept || (q.bits & LR_LATE);
  }
  return PROG_UNKNOWN;
}

/* The last entry of history h before p (late: of the load phase), in *out
   (when asked): ELAB_YES, ELAB_NO for none, ELAB_UNKNOWN when one in
   another region or one that may not have run comes after the last sure
   one -- at the load phase's end too, as elab_written_before answers.
   A may entry counts as sure at a p its branch has run it for
   (lr_may_dominates). skip_vis passes over the visibility versions of a
   method. A constant history with only uncertain writes supplies a
   candidate in *out. An entry in the statement of p's node, a method
   body's or a block's in it too, runs as lr_run_order orders it against
   that node, under the condition the node runs under; where that order
   is not sure, the statement's answers. */
static int lr_last_before(Compiler *c, const LrHist *h, LoadPoint p, int late, int skip_vis, const LrDef **out) {
  int unsure = 0;
  LoadRecord *r = c->lrec;
  int take = r->collect && h && h->a == -2 - (int)ELAB_CONST;   /* a constant's writes, for elab_class_candidates */
  int at = !late && p.region >= 0 && p.node >= 0 && p.node < r->n && r->at[p.node] == p.seq;
  for (int i = h ? h->n - 1 : -1; i >= 0; i--) {
    const LrDef *v = &h->v[i];
    if (take && v->unplaced) r->cand_bad = 1;
    if ((skip_vis && v->kind == LR_VIS) || v->unplaced) continue;
    int in = at && v->node >= 0 && v->node < r->n && r->at[v->node] == p.seq;
    int o = in ? lr_run_order(c, v->node, p.node) : PROG_UNKNOWN;
    if (o == PROG_UNKNOWN)
      o = late == 1 ? (elab_point_of(c, v->node).region == 0 ? PROG_BEFORE : PROG_UNKNOWN) : elab_cmp(elab_point_of(c, v->node), p);
    if (o == PROG_UNKNOWN || (o == PROG_BEFORE && !in && v->may && (late == 1 || !lr_may_dominates(c->lrec, v, p)))) {
      unsure = 1;
      if (out && (h->a == LR_KEY_CONST || h->a == LR_KEY_AUTO) && !*out) *out = v;
      if (take) r->cand = lr_grow(r->cand, &r->ccand, r->ncand, sizeof *r->cand), r->cand[r->ncand++] = v;
      continue;
    }
    if (o != PROG_BEFORE) continue;
    if (out) *out = v;
    if (take) r->cand = lr_grow(r->cand, &r->ccand, r->ncand, sizeof *r->cand), r->cand[r->ncand++] = v;
    return unsure ? ELAB_UNKNOWN : ELAB_YES;
  }
  return unsure ? ELAB_UNKNOWN : ELAB_NO;
}

/* May node's statement, which comes after p, also run before p: is it in
   a body around p that runs again (LrStmt.rep), or in the body of a loop
   whose condition p is? */
static int lr_runs_again(Compiler *c, int node, LoadPoint p) {
  LoadRecord *r = c->lrec;
  LoadPoint q = elab_point_of(c, node);
  if (p.region < 0 || elab_cmp(p, q) != PROG_BEFORE) return 0;
  NodeKind k = nt_kind(c->nt, r->st[p.seq].node);
  for (int e = k == NK_WhileNode || k == NK_UntilNode ? p.seq : r->st[p.seq].rep; e >= 0; e = r->st[e].rep)
    if (q.seq <= r->st[e].last) return 1;
  return 0;
}

/* ...of a method: an undef stops the search (LR_STOP), a removal passes it
   on; a visibility change (`private :m`) counts only when vis asks for it.
   An entry after p that may run before it on the body's next run leaves
   the answer unknown. */
static int lr_own(Compiler *c, const LrHist *h, LoadPoint p, int late, int vis, const LrDef **out) {
  const LrDef *v = NULL;
  int o = lr_last_before(c, h, p, late, !vis, &v);
  if (out && v) *out = v;
  for (int i = late || p.region < 0 || !h ? 0 : h->n; i > 0; i--)
    if ((vis || h->v[i - 1].kind != LR_VIS) && lr_runs_again(c, h->v[i - 1].node, p)) return ELAB_UNKNOWN;
  return o != ELAB_YES ? o : v->kind == LR_UNDEF ? LR_STOP : v->kind == LR_REMOVE ? ELAB_NO : ELAB_YES;
}

/* The class searched after cls: its superclass, Object (main's methods and
   constants) for a class with none; none (LR_UNKNOWN_CLASS) for a module
   and for Object itself */
static int lr_super(Compiler *c, int cls) {
  if (cls < 0 || cls >= c->nclasses) return LR_UNKNOWN_CLASS;
  LrClass *k = lr_class(c->lrec, cls);
  if (k && k->has_sup && lr_later(c, &k->sup, k->sup_ref) >= 0) return k->sup;   /* the record's, which it has while it is made */
  if (c->classes[cls].parent >= 0) return c->classes[cls].parent;
  return class_def_is_module(c->nt, c->classes[cls].def_node) ? LR_UNKNOWN_CLASS : LR_MAIN;
}

/* Did mixin m of a class happen before p (late: in the load phase)?
   ELAB_YES / NO / UNKNOWN; unknown too for one after p that may run
   before it on a body's next run */
static int lr_mixed_before(Compiler *c, const LrMixin *m, LoadPoint p, int late, int may) {
  int o = late == 1 ? (elab_point_of(c, m->node).region == 0 ? PROG_BEFORE : PROG_UNKNOWN) : elab_cmp(elab_point_of(c, m->node), p);
  if (!late && lr_runs_again(c, m->node, p)) return ELAB_UNKNOWN;
  return o == PROG_UNKNOWN ? ELAB_UNKNOWN : o != PROG_BEFORE ? ELAB_NO : may ? ELAB_UNKNOWN : ELAB_YES;
}

/* A builtin mixin has no program class unless a reopening supplies
   a recorded history. An unresolved program module stays unknown. */
static int lr_mixin_class(Compiler *c, LrMixin *m) {
  int cls = lr_later(c, &m->mod, m->ref);
  if (cls >= 0) return cls;
  int mod = elab_mixin_module(c, m);
  if (mod >= 0 && builtin_mixin_module_id(c->classes[mod].name)) return m->mod = mod;
  return mod < LR_MAIN ? mod : cls;
}

/* Is mixin m's module already among cls's ancestors (its singleton's when
   sg) where m runs: ELAB_YES / NO / UNKNOWN. A fact of the mixin, asked
   once and kept: the search it makes reads the earlier mixins' answers in
   turn, so asking again at every search would double the work with each
   mixin. Answers given while the record is made are dropped at its end
   (elab_program). The search starts at depth 0, so the kept answer does
   not depend on where it was first asked; an ask while it runs (modules
   that include each other) answers unknown. */
static int lr_mixin_dup(Compiler *c, int cls, int sg, LrMixin *m) {
  if (lr_mixin_class(c, m) < 0) return ELAB_NO;
  if (!m->dup) {
    m->dup = 1 + ELAB_UNKNOWN;
    m->dup = (unsigned char)(1 + lr_search(c, cls, sg, NULL, m->mod, elab_point_of(c, m->node), 0, 0, NULL, 0));
  }
  return m->dup - 1;
}

/* One ancestor's part of the search of `name` (or of module `mod`): cls's
   prepends (the last first), its own entries, its includes (the last
   first); a singleton's prepends, own methods and extends. ELAB_YES found,
   LR_STOP an undef stops the search there, ELAB_NO go on to the next,
   ELAB_UNKNOWN for a class the record does not hold (LoadRecord.held);
   *unsure is set where an entry or a mixin may have happened. */
static int lr_search_one(Compiler *c, int cls, int sg, const char *name, int mod, LoadPoint p, int late, int vis, const LrDef **out, int depth, int *unsure, int shadow) {
  LrClass *k = lr_class(c->lrec, cls);
  if (!k) return ELAB_UNKNOWN;
  for (int pass = 0; pass < (shadow ? 2 : 3); pass++) {   /* prepends, own, includes or extends (with shadow, not these) */
    if (pass == 1) {
      int o = name ? lr_own(c, lr_hist(c->lrec, cls, sg, name, 0), p, late, vis, out) : ELAB_NO;
      if (o == ELAB_YES || o == LR_STOP) return o;
      if (o == ELAB_UNKNOWN) *unsure = 1;
      continue;
    }
    for (int i = k ? k->nmix - 1 : -1; i >= 0; i--) {
      LrMixin *m = &k->mix[i];
      int want = sg ? (pass == 0 ? LR_SG_PREPEND : LR_EXTEND) : pass == 0 ? LR_PREPEND : LR_INCLUDE;
      if (m->kind != want) continue;
      int b = lr_mixed_before(c, m, p, late, elab_point_may(c, m->node));
      if (b == ELAB_NO) continue;
      /* a module already among the ancestors where the mixin runs (an
         earlier mixin's, the superclass's) stays where it is */
      int dup = lr_mixin_dup(c, cls, sg, m);
      if (dup == ELAB_YES) continue;
      if (dup == ELAB_UNKNOWN) *unsure = 1;
      int o = lr_mixin_class(c, m) < 0 ? ELAB_UNKNOWN : !name && m->mod == mod ? ELAB_YES : lr_search(c, m->mod, 0, name, mod, p, late, vis, out, depth + 1);
      if (o == ELAB_YES && b == ELAB_YES) return ELAB_YES;
      if (o != ELAB_NO) *unsure = 1;
    }
  }
  return ELAB_NO;
}

/* Where a singleton's chain ends, the search goes on into instance
   methods: a class's singleton into Class's, then Module's, Object's
   (main's top level's among them) and Kernel's; a module's from Module's;
   main's own singleton from Object's. Each is the program's reopening of
   that class as registered so far (Class's is
   comp_core_class(c, CORE_CLASS_REOPEN)): the histories it searches have no
   entry before its header. */
static int lr_sg_tail_of(Compiler *c, int cls, int tail[5]) {   /* where in tail cls's starts */
  const LoadRecord *r = c->lrec;
  tail[0] = comp_core_class(c, CORE_CLASS_REOPEN), tail[1] = lr_core(r, CORE_MODULE), tail[2] = lr_core(r, CORE_OBJECT);
  tail[3] = LR_MAIN, tail[4] = lr_core(r, CORE_KERNEL);
  return cls == LR_MAIN ? 2 : cls >= 0 && class_def_is_module(c->nt, c->classes[cls].def_node) ? 1 : 0;
}
static int lr_sg_tail(Compiler *c, int cls, const char *name, int mod, LoadPoint p, int late, int vis, const LrDef **out, int depth, int *unsure) {
  int tail[5];
  for (int i = lr_sg_tail_of(c, cls, tail); i < 5; i++) {
    int t = tail[i];
    if (t < LR_MAIN) continue;
    if (!name && t == mod) return ELAB_YES;
    int o = lr_search_one(c, t, 0, name, mod, p, late, vis, out, depth + 1, unsure, 0);
    if (o != ELAB_NO) return o;
  }
  return ELAB_NO;
}

/* The search of `name` (or, with name NULL, of module `mod`) along cls's
   ancestors at p (lr_search_one for each), then its superclass; for a
   singleton, the superclass's singleton, and where that chain ends the
   instance methods every singleton's ancestors end in (lr_sg_tail). With
   vis, an ancestor's visibility change of the name is found as well, for
   the visibility it gives. */
static int lr_search(Compiler *c, int cls, int sg, const char *name, int mod, LoadPoint p, int late, int vis, const LrDef **out, int depth) {
  if (depth > 64 || cls < LR_MAIN) return ELAB_UNKNOWN;
  if (!name && cls == mod && !sg) return ELAB_YES;
  int unsure = 0, o = lr_search_one(c, cls, sg, name, mod, p, late, vis, out, depth, &unsure, 0);
  if (o == ELAB_NO) {
    /* a class's chain ends at Object, whose methods main's top level
       defines (main's own singleton is not Object's) */
    int sup = lr_super(c, cls);
    if (sg && sup == LR_MAIN) sup = LR_UNKNOWN_CLASS;
    o = sup >= LR_MAIN ? lr_search(c, sup, sg, name, mod, p, late, vis, out, depth + 1)
      : sg ? lr_sg_tail(c, cls, name, mod, p, late, vis, out, depth, &unsure) : ELAB_NO;
  }
  if (o == LR_STOP) o = ELAB_NO;
  return unsure ? ELAB_UNKNOWN : o;
}

/* The versions of `name` along cls's chain (its singleton's when sg), as
   lr_search walks it: on each class, its prepends', its own, its includes'
   or extends'; then its superclass's, and where a singleton's chain ends
   the instance methods of lr_sg_tail's classes, each one's own (main's
   own singleton: Object's, main's top level's, Kernel's) */
static int lr_chain_one(Compiler *c, int cls, int sg, const char *name, ElabVersion *out, int n, int cap, int depth);
static int lr_chain_versions(Compiler *c, int cls, int sg, const char *name, ElabVersion *out, int n, int cap, int depth) {
  if (depth > 64 || (n = lr_chain_one(c, cls, sg, name, out, n, cap, depth)) < 0) return -1;
  int sup = lr_super(c, cls), tail[5];
  if (sg && sup == LR_MAIN) sup = LR_UNKNOWN_CLASS;
  if (sup >= LR_MAIN) return lr_chain_versions(c, sup, sg, name, out, n, cap, depth + 1);
  if (!sg) return n;
  for (int i = lr_sg_tail_of(c, cls, tail), j0 = i; i < 5 && n >= 0; i++) {
    int seen = 0;   /* (no reopened Object: comp_class_index's -1 is main's) */
    for (int j = j0; j < i; j++) seen |= tail[j] == tail[i];
    if (tail[i] >= LR_MAIN && !seen) n = lr_chain_one(c, tail[i], 0, name, out, n, cap, depth + 1);
  }
  return n;
}
/* the versions of history h, numbered from 1, onto out[n..cap): the new
   count, -1 past cap */
static int lr_hist_versions(const LoadRecord *r, const LrHist *h, ElabVersion *out, int n, int cap) {
  for (int j = 0, m = 0; h && j < h->n; j++) {
    if (!lr_is_version(&h->v[j])) continue;
    if (n == cap) return -1;
    out[n++] = (ElabVersion){ &h->v[j], (int)(h - r->hist), ++m };
  }
  return n;
}
int elab_history_versions(Compiler *c, int cls, int sg, const char *name, ElabVersion *out, int cap) {
  LoadRecord *r = c->lrec;
  return r && r->recorded ? lr_hist_versions(r, lr_hist(r, cls, sg != 0, name, 0), out, 0, cap) : -1;
}
static int lr_chain_one(Compiler *c, int cls, int sg, const char *name, ElabVersion *out, int n, int cap, int depth) {
  LoadRecord *r = c->lrec;
  const LrClass *k = lr_class(r, cls);
  if (depth > 64 || !k) return -1;
  for (int pass = 0; pass < 3 && n >= 0; pass++) {   /* prepends, own, includes or extends, as lr_search */
    if (pass == 1) {
      n = lr_hist_versions(r, lr_hist(r, cls, sg, name, 0), out, n, cap);
      continue;
    }
    for (int i = k->nmix - 1; i >= 0 && n >= 0; i--) {
      LrMixin *m = &k->mix[i];
      int want = sg ? (pass == 0 ? LR_SG_PREPEND : LR_EXTEND) : pass == 0 ? LR_PREPEND : LR_INCLUDE, mod;
      if (m->kind != want) continue;
      n = (mod = lr_later(c, &m->mod, m->ref)) < 0 ? -1 : lr_chain_versions(c, mod, 0, name, out, n, cap, depth + 1);
    }
  }
  return n;
}
ElabStamp elab_stamp(Compiler *c) {
  const LoadRecord *r = c->lrec;
  ElabStamp s = { c->nt->version, 0, 0, 0, 0 };
  if (r) { s.rec = r->ver; s.nhist = r->nhist; s.ncls = r->ncls; s.recorded = r->recorded; }
  return s;
}
int elab_stamp_same(ElabStamp a, ElabStamp b) {
  return a.nt == b.nt && a.rec == b.rec && a.nhist == b.nhist && a.ncls == b.ncls && a.recorded == b.recorded;
}
int elab_chain_versions(Compiler *c, int cls, int sg, const char *name, ElabVersion *out, int cap) {
  LoadRecord *r = c->lrec;
  return r && r->recorded && name && cls >= LR_MAIN ? lr_chain_versions(c, cls, sg, name, out, 0, cap, 0) : -1;
}

/* Can the record answer at p: once it is made, and while the walk records
   it at a point before which every statement is recorded (the frontier)? */
static int lr_ready(Compiler *c, LoadPoint p) {
  const LoadRecord *r = c->lrec;
  return r && (r->recorded || (r->walk && p.region >= 0 && p.seq <= r->frontier));
}

int elab_method_at(Compiler *c, int cls, const char *name, int sg, LoadPoint p, const LrDef **out) {
  if (out) *out = NULL;
  if (!lr_ready(c, p) || !name) return ELAB_UNKNOWN;
  int o = lr_search(c, cls, sg, name, -1, p, p.region < 0, 0, out, 0);
  return o == LR_STOP ? ELAB_NO : o;
}

int elab_own_method_at(Compiler *c, int cls, const char *name, LoadPoint p) {
  if (!lr_ready(c, p) || !name) return ELAB_UNKNOWN;
  const LrDef *v = NULL;
  int unsure = 0, o = lr_search_one(c, cls, 0, name, -1, p, p.region < 0, 0, &v, 0, &unsure, 1);
  /* a definition under a condition (a branch, a rescue, a modifier; or the
     dispatcher a lone one is lowered to) may not have run */
  const LrStmt *st = o == ELAB_YES && v ? lr_stmt_of(elab_fresh(c), v->node) : NULL;
  if (v && (v->may || nt_int(c->nt, v->node, "cond_lone_disp", 0) || (st && (st->may || st->fmay || st->fpartial)))) return ELAB_UNKNOWN;
  return unsure ? ELAB_UNKNOWN : o == LR_STOP ? ELAB_YES : o;
}

/* Where the version recorded at node in cls's history of `name` (its
   singleton's when sg) stops being that history's live entry: the next
   version's point. */
int elab_version_end(Compiler *c, int cls, const char *name, int sg, int node, LoadPoint *end) {
  *end = (LoadPoint){ -1, -1, -1 };
  LoadRecord *r = c->lrec;
  const LrHist *h = r && r->recorded ? lr_hist(r, cls, sg, name, 0) : NULL;
  int i = 0;
  while (h && i < h->n && !(h->v[i].node == node && lr_is_version(&h->v[i]))) i++;
  LoadPoint p = elab_point_of(c, node);
  if (!h || i == h->n || p.region < 0) return ELAB_UNKNOWN;
  /* a version in another region runs at a time the record does not know: it
     may fall inside the interval, and the history lists it after region 0 */
  for (int j = 0; j < h->n; j++)
    if (j != i && lr_is_version(&h->v[j]) && elab_point_of(c, h->v[j].node).region != p.region) return ELAB_UNKNOWN;
  for (int j = i + 1; j < h->n; j++) {
    const LrDef *v = &h->v[j];
    if (!lr_is_version(v)) continue;
    LoadPoint q = elab_point_of(c, v->node);
    if (v->may || elab_cmp(p, q) != PROG_BEFORE) return ELAB_UNKNOWN;
    *end = q;
    return ELAB_YES;
  }
  return ELAB_NO;
}

/* Is k cls or a superclass of it? */
static int lr_under(Compiler *c, int cls, int k) {
  for (int a = cls, d = 0; a >= LR_MAIN && d < 64; a = lr_super(c, a), d++)
    if (a == k) return 1;
  return 0;
}

/* Is class or module k one of cls's ancestors (its singleton's when sg)
   at p, or may it be: cls, a superclass, a module mixed into one of them? */
static int lr_in_ancestry(Compiler *c, int cls, int sg, int k, LoadPoint p) {
  return lr_under(c, cls, k) || (k >= 0 && lr_search(c, cls, sg, NULL, k, p, 0, 0, NULL, 0) != ELAB_NO);
}

/* Does node's statement run strictly between p0 and p1, or is it not
   known not to (one after p1 that a body around p0 runs again)? */
static int lr_inside(Compiler *c, int node, LoadPoint p0, LoadPoint p1) {
  LoadPoint q = elab_point_of(c, node);
  int a = elab_cmp(p0, q), b = elab_cmp(q, p1);
  return a == PROG_UNKNOWN || b == PROG_UNKNOWN || (a == PROG_BEFORE && b == PROG_BEFORE) ||
         (b == PROG_AFTER && lr_runs_again(c, node, p0));
}

/* The classes that carry a mixin or code the record cannot follow, in order:
   of the others a chain takes nothing (lr_chain_moves, elab_chain_settled).
   Kept for the histories as they stand. */
static const int *lr_mixed(Compiler *c, int *n) {
  LoadRecord *r = c->lrec;
  if (!r->mixed || r->mixed_of.nhist != r->nhist || r->mixed_of.ncls != r->ncls || r->mixed_of.recorded != r->recorded || r->mixed_of.ver != r->ver) {
    free(r->mixed);
    if (!(r->mixed = malloc(sizeof(int) * ((size_t)r->ncls + 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    r->nmixed = 0;
    for (int k = LR_MAIN; k + 1 < r->ncls; k++) {
      const LrClass *kc = lr_class(r, k);
      if ((kc && kc->nmix) || lr_hist(r, LR_KEY_OPAQUE, lr_const_owner(k), "", 0)) r->mixed[r->nmixed++] = k;
    }
    r->mixed_of.nhist = r->nhist; r->mixed_of.ncls = r->ncls; r->mixed_of.recorded = r->recorded; r->mixed_of.ver = r->ver;
  }
  *n = r->nmixed;
  return r->mixed;
}

/* Does cls's chain (its singleton's when sg) change for `name` strictly
   between p0 and p1: a module that has or may have the name mixed into a
   class on it, code the record cannot follow run on one of them? */
static int lr_chain_moves(Compiler *c, int cls, int sg, const char *name, LoadPoint p0, LoadPoint p1) {
  LoadRecord *r = c->lrec;
  int nk = 0;
  const int *mk = lr_mixed(c, &nk);
  for (int i = 0; i < nk; i++) {
    int k = mk[i];
    const LrClass *kc = lr_class(r, k);
    const LrHist *op = lr_hist(r, LR_KEY_OPAQUE, lr_const_owner(k), "", 0);
    if (!lr_in_ancestry(c, cls, sg, k, p1)) continue;
    for (int j = 0; kc && j < kc->nmix; j++) {
      LrMixin *m = &kc->mix[j];
      if (lr_inside(c, m->node, p0, p1) &&
          (lr_mixin_class(c, m) < 0 || lr_search(c, m->mod, 0, name, -1, p1, 1, 0, NULL, 0) != ELAB_NO)) return 1;
    }
    for (int j = 0; op && j < op->n; j++)
      if (lr_inside(c, op->v[j].node, p0, p1)) return 1;
  }
  return 0;
}

static void lr_run_points(Compiler *c);
int elab_chain_settled(Compiler *c, int cls, int sg, const char *name, LoadPoint p) {
  LoadRecord *r = c->lrec;
  if (!r || !r->recorded || !name || cls < LR_MAIN || p.region < 0) return 0;
  lr_run_points(c);
  LoadPoint end = { p.region, r->rend[p.region] };
  int nk = 0;
  const int *mk = lr_mixed(c, &nk);
  for (int i = 0; i < nk; i++) {
    int k = mk[i];
    const LrClass *kc = lr_class(r, k);
    const LrHist *op = lr_hist(r, LR_KEY_OPAQUE, lr_const_owner(k), "", 0);
    if (!lr_in_ancestry(c, cls, sg, k, end)) continue;
    if (op && op->n) return 0;
    for (int j = 0; kc && j < kc->nmix; j++) {
      LrMixin *m = &kc->mix[j];
      if (lr_later(c, &m->mod, m->ref) < 0) return 0;
      if (lr_search(c, m->mod, 0, name, -1, end, 1, 0, NULL, 0) == ELAB_NO) continue;
      if (elab_point_may(c, m->node) || elab_cmp(elab_point_of(c, m->node), p) != PROG_BEFORE) return 0;
    }
  }
  return 1;
}

static int lr_defines(const LoadRecord *r, const char *name);

static int lr_literal_kind(NodeKind k) {
  switch (k) {
    case NK_IntegerNode: case NK_FloatNode: case NK_StringNode: case NK_InterpolatedStringNode: case NK_SymbolNode:
    case NK_ArrayNode: case NK_HashNode: case NK_RangeNode: case NK_NilNode: case NK_TrueNode: case NK_FalseNode:
      return 1;
    default: return 0;
  }
}

/* Is call id a definition form given literals or the modules it names,
   or a builtin literal's method given literals when the program reopens
   no builtin, with no block argument: one with nothing of the program's
   to call (a hook the program defines aside)? */
static int lr_literal_args(const NodeTable *nt, const int *av, int ac, int form);
static int lr_call_on_literals(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name"), *hook[2];
  int recv = nt_ref(nt, id, "receiver"), an = nt_ref(nt, id, "arguments"), blk = nt_ref(nt, id, "block"), ac = 0;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  int form = recv < 0 && nm && definition_form_hooks(nm, 0, hook) > 0;
  return nm && (blk < 0 || nt_kind(nt, blk) == NK_BlockNode) && lr_literal_args(nt, av, ac, form) &&
         (form || (recv >= 0 && !c->lrec->nreopens && lr_literal_kind(nt_kind(nt, recv))));
}

/* Are the arguments av[0..ac) literals (or, to a form, defs as in
   `private def m`, and constants)? */
static int lr_literal_args(const NodeTable *nt, const int *av, int ac, int form) {
  for (int i = 0; i < ac; i++) {
    NodeKind k = nt_kind(nt, av[i]);
    if (!lr_literal_kind(k) && !(form && (k == NK_DefNode || k == NK_ConstantReadNode || k == NK_ConstantPathNode))) return 0;
  }
  return 1;
}


/* Does node run code of the program's where its statement runs (LR_RUN):
   a call, super or yield of a name the program defines a method by (sent
   by a literal name too); a `new`, `allocate` or raise, which makes an
   object (and runs its initialize); a definition form, def, class header
   or constant write whose hook the program defines; a mixin into an object
   the program holds; code, or a method made or called, by a name the
   record cannot read; a callable made to run later (a lambda, a kept
   block, a method object, a block argument). Or may it (LR_DISPATCH): any
   other call, and any node but those that only hold, read or write
   values, may run a method of the program's the runtime calls by itself
   (method_name_implicitly_invoked) on an object it meets, a callable, or
   one made by a name the record cannot read. */
enum { LR_RUN = 1, LR_DISPATCH = 2 };
static int lr_list_has(const char *list, const char *w) {
  size_t n = w ? strlen(w) : 0;
  for (const char *p = list; p && n && *p;) {
    const char *e = strchr(p, ' ');
    size_t l = e ? (size_t)(e - p) : strlen(p);
    if (l == n && !strncmp(p, w, n)) return 1;
    p += l + (e != NULL);
  }
  return 0;
}

/* The modules whose instance methods an object that is a class (bit 1) or a
   module (bit 2) can have: one extended onto a class or a module, or included
   into Object, Module, Class or Kernel (bit 1), or into a module that has the
   bits. Per class and module, once for the histories as they stand; *all when
   a mixin names a module the record cannot resolve. */
static const unsigned char *lr_class_object_modules(Compiler *c, int *all) {
  LoadRecord *r = c->lrec;
  if (!r->clsmod || r->clsmod_of.nhist != r->nhist || r->clsmod_of.ncls != r->ncls || r->clsmod_of.recorded != r->recorded || r->clsmod_of.ver != r->ver) {
    free(r->clsmod);
    if (!(r->clsmod = calloc((size_t)r->ncls + 2, 1))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    r->clsmod_all = 0;
    const char *chain = core_class_ancestors("Class");
    for (int again = 1; again;) {
      again = 0;
      for (int k = LR_MAIN; k + 1 < r->ncls; k++) {
        const LrClass *kc = lr_class(r, k);
        int in_chain = k == LR_MAIN || (k >= 0 && lr_list_has(chain, c->classes[k].name));
        unsigned char have = r->clsmod[k + 1] | (in_chain ? 1 : 0);
        for (int j = 0; kc && j < kc->nmix; j++) {
          int m = lr_mixin_class(c, &kc->mix[j]);
          if (m < 0) {   /* a builtin module has no method of the program's unless the program reopens it, which its history says */
            int builtin = 0;
            for (int i = 0; !builtin && builtin_class_at(i); i++) builtin = builtin_class_at(i)->id == m;
            r->clsmod_all |= !builtin;
            continue;
          }
          /* an extend gives the object its target is: a module's or a class's (main's is no class) */
          unsigned char bits = elab_mix_on_singleton(kc->mix[j].kind) ? (k < 0 ? 0 : comp_class_is_module(c, &c->classes[k]) ? 2 : 1) : have;
          if (bits & ~r->clsmod[m + 1]) { r->clsmod[m + 1] |= bits; again = 1; }
        }
      }
    }
    r->clsmod_of.nhist = r->nhist; r->clsmod_of.ncls = r->ncls; r->clsmod_of.recorded = r->recorded; r->clsmod_of.ver = r->ver;
  }
  *all = r->clsmod_all;
  return r->clsmod;
}
/* Can a method of this name be one a class object answers: some history of
   it is a singleton method, a method of Object, Module, Class or Kernel, or a
   method of a module a class object (class_only: not a module's) has
   (lr_class_object_modules)? A name the program defines by no history the
   record holds can. */
static int lr_class_hook(Compiler *c, const char *name, int class_only) {
  LoadRecord *r = c->lrec;
  if (!lr_defines(r, name)) return 0;
  const LrVers *x = lr_vers(r, name);
  if (!x || x->first < 0) return 1;
  int all = 0;
  const unsigned char *mods = lr_class_object_modules(c, &all);
  const char *chain = core_class_ancestors("Class");
  for (int i = x->first; i >= 0; i = r->hist[i].nlink) {
    const LrHist *h = &r->hist[i];
    if (h->a < LR_MAIN || h->b || h->a == LR_MAIN || h->a >= c->nclasses) return 1;
    if (lr_list_has(chain, c->classes[h->a].name) || (comp_class_is_module(c, &c->classes[h->a]) && (all || (mods[h->a + 1] & (class_only ? 1 : 3))))) return 1;
  }
  return 0;
}
/* Does the class header node id fire a hook the program defines: a name the
   syntax table says a header calls on a class (nt_implied_calls), when some
   method of that name is a class object's? */
static int lr_header_hooks(Compiler *c, int id) {
  NtImplied rows[16];
  int n = nt_implied_calls(c->nt, id, rows, 16);
  for (int i = 0; i < n && i < 16; i++)
    if ((rows[i].side & NTS_CLASS) && lr_class_hook(c, rows[i].name, core_method_vis("Class", rows[i].name, 0) >= 0)) return 1;   /* (a hook Class itself has is a class's) */
  return n > 16;
}

static int lr_node_runs(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const LoadRecord *r = c->lrec;
  const int *par = comp_parent_map(c);
  const char *nm = NULL, *hook[2];
  int recv, an, ac = 0, hooks = r->defs_computed ? LR_DISPATCH : 0;
  const int *av;
  switch (nt_kind(nt, id)) {
    case NK_CallNode:
      nm = nt_str(nt, id, "name");
      recv = nt_ref(nt, id, "receiver");
      an = nt_ref(nt, id, "arguments");
      av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
      if (!nm || is_new_name(nm) || is_allocate_name(nm) || is_raise_alias(nm) || lr_defines(r, nm) || is_method_object_maker(nm) ||
          (is_mixin_call(nm) && recv >= 0 && nt_kind(nt, recv) != NK_SelfNode && nt_kind(nt, recv) != NK_ConstantReadNode) ||
          (ac && (is_code_eval_name(nm) || ((is_send_family(nm) || is_method_naming_call(nm)) && !vis_arg_name(nt, av[0])) ||
                  (is_send_family(nm) && lr_defines(r, vis_arg_name(nt, av[0])))))) return LR_RUN;
      for (int sg = 0; sg < 2; sg++)
        for (int i = definition_form_hooks(nm, sg, hook) - 1; i >= 0; i--)
          if (lr_defines(r, hook[i])) return LR_RUN;
      return lr_call_on_literals(c, id) ? 0 : LR_DISPATCH;
    case NK_AssocNode: recv = nt_ref(nt, id, "key"); return recv >= 0 && lr_literal_kind(nt_kind(nt, recv)) ? 0 : LR_DISPATCH;
    case NK_SuperNode: case NK_ForwardingSuperNode: case NK_YieldNode: case NK_LambdaNode: case NK_BlockArgumentNode: return LR_RUN;
    case NK_BlockNode:
      nm = par[id] >= 0 && nt_kind(nt, par[id]) == NK_CallNode ? nt_str(nt, par[id], "name") : NULL;
      return par[id] >= 0 && elab_block_when(c, par[id], 0) == ELAB_BLK_LATER && !(nm && is_method_naming_call(nm)) ? LR_RUN : 0;
    case NK_DefNode: return lr_defines(r, "method_added") || lr_defines(r, "singleton_method_added") ? LR_RUN : hooks;
    case NK_ClassNode: return lr_header_hooks(c, id) ? LR_RUN : hooks;
    case NK_ConstantWriteNode: case NK_ConstantPathWriteNode: return lr_defines(r, "const_added") ? LR_RUN : hooks;
    case NK_ProgramNode: case NK_StatementsNode: case NK_ModuleNode: case NK_SingletonClassNode:
    case NK_ConstantReadNode: case NK_ConstantPathNode: case NK_SelfNode: case NK_NilNode: case NK_TrueNode:
    case NK_FalseNode: case NK_IntegerNode: case NK_FloatNode: case NK_StringNode: case NK_SymbolNode:
    case NK_ArrayNode: case NK_HashNode: case NK_KeywordHashNode: case NK_ParenthesesNode: case NK_BeginNode: case NK_IfNode: case NK_UnlessNode:
    case NK_ElseNode: case NK_WhileNode: case NK_UntilNode: case NK_AndNode: case NK_OrNode:
    case NK_ArgumentsNode: case NK_BlockParametersNode: case NK_ParametersNode: case NK_RequiredParameterNode:
    case NK_LocalVariableReadNode: case NK_LocalVariableWriteNode: case NK_InstanceVariableReadNode:
    case NK_InstanceVariableWriteNode: case NK_GlobalVariableReadNode: case NK_GlobalVariableWriteNode:
    case NK_ClassVariableReadNode: case NK_ClassVariableWriteNode:
      return 0;
    default: return LR_DISPATCH;
  }
}

/* The load-order rapid type analysis (lr_rta): from which statement on an
   instance of each class may exist, and each method body may run.
   Statement by statement it scans the code that statement runs: its own
   nodes, the blocks and lambdas it makes (live where made), and the body
   of each method it may reach, once, when first reached. A call reaches
   the methods of its name: on a constant the record resolves, those of its
   singleton chain; on any receiver, the instance methods of each class an
   instance of which may exist so far (E), and on one not resolved, every
   singleton method too. A node that may call into the runtime
   (lr_node_runs' LR_DISPATCH) calls the names it may call back
   (method_name_implicitly_invoked, method_missing) and those CRuby's core
   calls on the objects and class objects it is given
   (ic_core_callback); a definition form its hooks; super its
   method's name; send, a method object, an Enumerator or `&:sym` its name
   (`&:new` making any class); refine the refining module's methods. A computed
   name, eval, bind or an object loader calls every name and makes any
   class; a method defined by a computed name only calls every name. new,
   allocate, a builder's [], exception and raise make the class
   of a constant they are given (of a class method's self, that class and
   its subclasses; of `x.class`, none new; else any). Main and the core
   classes have instances from the start. rinst[k + 1]: the first statement
   an object with k among its ancestors may exist (LR_NEVER: none);
   urun[def]: the first statement a recorded def's body may run, 0 for one
   the record did not record. */
enum { LR_NEVER = INT_MAX, LR_CORE = -4, RQ_INST = 1, RQ_CLASS = 2 };
/* The class constant write w holds when a builder made it (`P =
   Struct.new(:x)`): the program's class of it, or LR_CORE when the program
   has none (no block, no header reopens it: only core ancestors);
   LR_UNKNOWN_CLASS for any other write. Unlike lr_builder_class, which
   homes a builder's block once registration has made its class, this
   checks that the write is a builder call, and answers before
   elab_record_builders too (the dump's walk) */
static int lr_rta_builder(Compiler *c, int w) {
  const NodeTable *nt = c->nt;
  int v = nt_kind(nt, w) == NK_ConstantWriteNode ? nt_ref(nt, w, "value") : -1;
  int k;
  if (!class_def_is_record_call(nt, v)) return LR_UNKNOWN_CLASS;
  return (k = comp_class_index(c, lr_written_name(nt, w))) >= 0 ? k : LR_CORE;
}
typedef struct { const char *name; int cls; } LrRtaName;
typedef struct {
  Compiler *c; LoadRecord *r;
  int s, n, *key, *ustart, *ulist, *stack, nstack, *hnext, *hfirst;
  unsigned char *hreach, *rq, *sub, *ext, *seen;
  int allinst, cb, allcls, any, builders, ncname;
  LrRtaName *cname;
} LrRta;

/* name's index in the program's method names (r->names), -1 for none */
static int lr_name_ix(const LoadRecord *r, const char *nm) {
  char **p = nm ? bsearch(&nm, r->names, (size_t)r->nnames, sizeof *r->names, lr_name_cmp) : NULL;
  return p ? (int)(p - r->names) : -1;
}
static void lr_rta_reach(LrRta *t, int hi);
static void lr_rta_recv(LrRta *t, int a) {   /* an object with a among its ancestors may exist */
  LoadRecord *r = t->r;
  LrClass *kc = lr_class(r, a);
  if (a < LR_MAIN || a >= t->c->nclasses || r->rinst[a + 1] != LR_NEVER) return;
  r->rinst[a + 1] = t->s;
  for (int h = t->hfirst[a + 1]; h >= 0; h = t->hnext[h])
    if (t->allinst || t->rq[lr_name_ix(r, r->hist[h].name)] & RQ_INST) lr_rta_reach(t, h);
  for (int j = 0; kc && j < kc->nmix; j++)
    if (!elab_mix_on_singleton(kc->mix[j].kind)) lr_rta_recv(t, lr_later(t->c, &kc->mix[j].mod, kc->mix[j].ref));
}
static void lr_rta_add(LrRta *t, int k) {   /* k into E */
  for (int a = k, d = 0; a >= LR_MAIN && a < t->c->nclasses && d < 64; a = lr_super(t->c, a), d++) lr_rta_recv(t, a);
}
static void lr_rta_ext(LrRta *t, int m, int d) {   /* module m extended onto a class, or included into one that is */
  LrClass *mc = lr_class(t->r, m);
  if (m < 0 || m >= t->c->nclasses || t->ext[m + 1] || d > 64) return;
  t->ext[m + 1] = 1;
  for (int j = 0; mc && j < mc->nmix; j++)
    if (!elab_mix_on_singleton(mc->mix[j].kind)) lr_rta_ext(t, lr_later(t->c, &mc->mix[j].mod, mc->mix[j].ref), d + 1);
}
/* name called on an object of E (RQ_INST) or on any class object (RQ_CLASS) */
static void lr_rta_call(LrRta *t, const char *name, int how) {
  LoadRecord *r = t->r;
  int x = lr_name_ix(r, name);
  if (x < 0 || (t->rq[x] & how) == how) return;
  how &= ~t->rq[x];
  t->rq[x] |= (unsigned char)how;
  const LrVers *v = lr_vers(r, name);
  for (int i = v ? v->first : -1; i >= 0; i = r->hist[i].nlink) {
    const LrHist *h = &r->hist[i];
    if (h->a >= LR_MAIN && h->a < t->c->nclasses && (((how & RQ_INST) && !h->b && r->rinst[h->a + 1] != LR_NEVER) ||
                                                     ((how & RQ_CLASS) && (h->b || t->ext[h->a + 1])))) lr_rta_reach(t, i);
  }
}
/* name called on class k: its singleton chain's, its extended modules' */
static void lr_rta_call_on(LrRta *t, const char *name, int k) {
  LoadRecord *r = t->r;
  lr_rta_call(t, name, RQ_INST);
  for (int a = k, d = 0; a >= LR_MAIN && a < t->c->nclasses && d < 64; a = lr_super(t->c, a), d++) {
    LrClass *kc = lr_class(r, a);
    LrHist *h = lr_hist(r, a, 1, name, 0);
    if (h) lr_rta_reach(t, (int)(h - r->hist));
    for (int j = 0; kc && j < kc->nmix; j++)
      if (elab_mix_on_singleton(kc->mix[j].kind)) lr_rta_call(t, name, RQ_CLASS);   /* the modules extended (lr_rta_ext) */
  }
}
static void lr_rta_unit(LrRta *t, int d) {
  if (d < 0 || d >= t->n || t->seen[d]) return;
  t->seen[d] = 1;
  if (t->r->urun[d] == LR_NEVER) t->r->urun[d] = t->s;
  t->stack[t->nstack++] = d;
}
static void lr_rta_all(LrRta *t, int inst);
static void lr_rta_reach(LrRta *t, int hi) {
  const LrHist *h = &t->r->hist[hi];
  if (t->hreach[hi]) return;
  t->hreach[hi] = 1;
  for (int j = 0; j < h->n; j++) {
    const LrDef *v = &h->v[j];
    if (v->kind == LR_DEF) lr_rta_unit(t, v->node);
    else if ((v->kind == LR_ALIAS || v->kind == LR_COPY) && !v->target) lr_rta_all(t, 0);
    else if (v->kind == LR_ALIAS || v->kind == LR_COPY) h->b ? lr_rta_call_on(t, v->target, h->a) : lr_rta_call(t, v->target, RQ_INST);
  }
}
/* every name (disp 0: every instance method of E, every singleton
   method), or what the runtime may call back (disp 1: the implicitly
   invoked names, and on class objects its callbacks) */
static void lr_rta_all(LrRta *t, int disp) {
  LoadRecord *r = t->r;
  if (!disp && !t->allinst) {
    t->allinst = 1;
    for (int i = 0; i < r->nhist; i++)
      if (r->hist[i].a >= LR_MAIN && r->hist[i].a < t->c->nclasses && r->hist[i].name && !r->hist[i].b && r->rinst[r->hist[i].a + 1] != LR_NEVER) lr_rta_reach(t, i);
  }
  if (t->allcls || (disp && t->cb)) return;
  *(disp ? &t->cb : &t->allcls) = 1;
  for (int x = 0; x < r->nnames; x++) {
    const char *n = r->names[x];
    int imp = disp && (method_name_implicitly_invoked(n) || is_method_missing_name(n));
    int how = (!disp || imp || ic_core_callback(n, 1) ? RQ_CLASS : 0) | (imp || (disp && ic_core_callback(n, 0)) ? RQ_INST : 0);
    if (how) lr_rta_call(t, n, how);
  }
}
static void lr_rta_any(LrRta *t) {
  for (int k = LR_MAIN; !t->any && k < t->c->nclasses; k++) lr_rta_add(t, k);
  t->any = 1;
}

/* The class a constant names: the record's answer; else, where the record
   cannot tell (a method body), the one class the program binds that name
   to by headers and builder writes alone (lr_rta_names); LR_CORE for a
   core class the program binds nothing to, or one a builder made that the
   program has no class of (lr_rta_builder) */
static int lr_rta_const(LrRta *t, int node) {
  const NodeTable *nt = t->c->nt;
  const LrDef *d = NULL;
  int k = lr_class_ref_at(t->c, node, NULL), lo = 0, hi = t->ncname;
  const char *nm = lr_written_name(nt, node);
  if (k >= LR_MAIN) return k;
  if (nt_kind(nt, node) == NK_ConstantReadNode && lr_const_at(t->c, node, nm, elab_point_of(t->c, node), &d, LR_KEY_CONST) == ELAB_YES && d && (k = lr_rta_builder(t->c, d->node)) != LR_UNKNOWN_CLASS) return k;
  while (lo < hi) { int mid = (lo + hi) / 2; if (strcmp(t->cname[mid].name, nm) < 0) lo = mid + 1; else hi = mid; }
  if (lo < t->ncname && !strcmp(t->cname[lo].name, nm)) return t->cname[lo].cls;
  return t->ncname >= 0 && (is_builtin_class_name(nm) || is_builtin_module_const_name(nm)) ? LR_CORE : LR_UNKNOWN_CLASS;
}

/* Does a call of name make an object of its receiver's class: new,
   allocate, exception, a builder's [] */
static int lr_rta_ctor(const LrRta *t, const char *name) {
  return name && (is_new_name(name) || is_allocate_name(name) || is_exception_ctor_name(name) || (t->builders && is_struct_constructor(name)));
}

/* What new, allocate, a builder's [], exception or raise on recv makes */
static void lr_rta_made(LrRta *t, int id, int recv) {
  const NodeTable *nt = t->c->nt;
  NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_SelfNode;
  const Scope *v = rk == NK_SelfNode ? comp_scope_of(t->c, id) : NULL;
  int k = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode ? lr_rta_const(t, recv) : v && v->is_cmethod ? v->class_id : LR_UNKNOWN_CLASS;
  if (k == LR_CORE || (rk == NK_CallNode && is_class_query(nt_str(nt, recv, "name")) && !lr_defines(t->r, "class"))) return;
  if (k < LR_MAIN || k >= t->c->nclasses) { lr_rta_any(t); return; }
  lr_rta_add(t, k);
  for (int j = LR_MAIN; v && !t->sub[k + 1] && j < t->c->nclasses; j++)
    if (lr_under(t->c, j, k)) lr_rta_add(t, j);
  if (v) t->sub[k + 1] = 1;
}

static void lr_rta_node(LrRta *t, int id) {
  const NodeTable *nt = t->c->nt;
  const char *nm, *hook[2], *by;
  NodeKind k = nt_kind(nt, id);
  int recv, an, ac = 0, d;
  const int *av;
  switch (k) {
    case NK_SuperNode: case NK_ForwardingSuperNode:
      d = elab_body_def(t->c, id);
      if (d < 0) lr_rta_all(t, 0);
      else lr_rta_call(t, nt_str(nt, d, "name"), RQ_INST | RQ_CLASS);
      return;
    case NK_DefNode:
      if (t->r->urun[id] == 0) lr_rta_unit(t, id);   /* a def the record did not record: from here */
      /* fallthrough */
    case NK_AliasMethodNode:
      lr_rta_call(t, "method_added", RQ_INST | RQ_CLASS);
      lr_rta_call(t, "singleton_method_added", RQ_INST | RQ_CLASS);
      return;
    case NK_UndefNode:
      lr_rta_call(t, "method_undefined", RQ_INST | RQ_CLASS);
      lr_rta_call(t, "singleton_method_undefined", RQ_INST | RQ_CLASS);
      return;
    case NK_ClassNode: lr_rta_call(t, "inherited", RQ_CLASS); return;
    case NK_ConstantWriteNode: case NK_ConstantPathWriteNode: lr_rta_call(t, "const_added", RQ_CLASS); return;
    case NK_ConstantReadNode: case NK_ConstantPathNode: lr_rta_call(t, "const_missing", RQ_CLASS); return;
    case NK_BlockArgumentNode:
      an = nt_ref(nt, id, "expression");
      nm = an >= 0 && nt_kind(nt, an) == NK_SymbolNode ? nt_str(nt, an, "value") : NULL;
      if (nm) lr_rta_call(t, nm, RQ_INST | RQ_CLASS);
      else lr_rta_all(t, 1);
      if (lr_rta_ctor(t, nm)) {   /* `&:new`: objects of whatever classes are yielded */
        lr_rta_any(t);
        lr_rta_call(t, "initialize", RQ_INST);
      }
      return;
    case NK_CallNode: break;
    default:
      if (lr_node_runs(t->c, id) == LR_DISPATCH) lr_rta_all(t, 1);
      return;
  }
  nm = nt_str(nt, id, "name");
  recv = nt_ref(nt, id, "receiver");
  an = nt_ref(nt, id, "arguments");
  av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_SelfNode, ak;
  int rc = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode ? lr_rta_const(t, recv) : LR_UNKNOWN_CLASS;
  by = nm && (is_send_family(nm) || is_method_object_maker(nm) || is_enumerator_maker(nm)) ? (ac ? vis_arg_name(nt, av[0]) : "") : nm;
  if (!by || (ac && is_code_eval_name(nm)) || is_method_bind_name(by) || is_object_loader(rk == NK_ConstantReadNode ? nt_str(nt, recv, "name") : NULL, by, ac)) {
    lr_rta_all(t, 0);
    lr_rta_any(t);
    return;
  }
  if (ac && is_method_naming_call(nm) && !vis_arg_name(nt, av[0])) lr_rta_all(t, 0);   /* a method of a name not known: any name's, though it makes no object */
  if (!lr_call_on_literals(t->c, id)) lr_rta_all(t, 1);
  if (by != nm) lr_rta_call(t, nm, RQ_INST | RQ_CLASS);   /* send itself, which the program may define */
  if (rk == NK_SymbolNode && is_proc_conversion_name(nm)) lr_rta_call(t, nt_str(nt, recv, "value"), RQ_INST | RQ_CLASS);
  /* inject(:op) and inject(init, :op) call the method the Symbol names on the elements, as `&:op` does */
  if (is_reduce_alias(nm) && ac >= 1 && ac <= 2 && nt_kind(nt, av[ac - 1]) == NK_SymbolNode && nt_str(nt, av[ac - 1], "value"))
    lr_rta_call(t, nt_str(nt, av[ac - 1], "value"), RQ_INST | RQ_CLASS);
  if (is_refine_name(nm)) {   /* a refinement's methods: the refining module's, live from here */
    int m = id;
    while (m >= 0 && nt_kind(nt, m) != NK_ModuleNode) m = comp_parent_map(t->c)[m];
    if ((m = m >= 0 ? elab_header_class(t->c, m, NULL, NULL) : LR_UNKNOWN_CLASS) >= 0) lr_rta_recv(t, m);
    else lr_rta_any(t);
  }
  if (rc >= LR_MAIN) lr_rta_call_on(t, by, rc);   /* a method called, sent or made a callable by its name */
  else lr_rta_call(t, by, RQ_INST | RQ_CLASS);
  if (lr_rta_ctor(t, by)) {
    lr_rta_made(t, id, recv);
    lr_rta_call(t, "initialize", RQ_INST);
  }
  /* raise makes an object of a class it is given, or a core one of a
     message; Timeout.timeout one of its klass */
  int ka = is_raise_alias(by) ? 0 : rk == NK_ConstantReadNode && is_timeout_call(nt_str(nt, recv, "name"), by) ? 1 : -1;
  ak = ka >= 0 && ka < ac ? nt_kind(nt, av[ka]) : NK_StringNode;
  if (ak == NK_ConstantReadNode || ak == NK_ConstantPathNode) {
    if ((d = lr_rta_const(t, av[ka])) >= LR_MAIN) lr_rta_call_on(t, "exception", d);
    lr_rta_made(t, id, av[ka]);
    lr_rta_call(t, "initialize", RQ_INST);
  }
  else if (ak != NK_StringNode && ak != NK_InterpolatedStringNode && !(ak == NK_CallNode && lr_rta_ctor(t, nt_str(nt, av[ka], "name")))) {
    lr_rta_call(t, "exception", RQ_CLASS | RQ_INST);
    lr_rta_any(t);
  }
  /* a module mixed in by a call the record does not hold as a class's
     (on an object, in a method body): its methods have instances */
  for (int i = 0; is_mixin_call(by) && (rk != NK_SelfNode || elab_body_def(t->c, id) >= 0) && i < ac; i++) {
    int m = nt_kind(nt, av[i]) == NK_ConstantReadNode || nt_kind(nt, av[i]) == NK_ConstantPathNode ? lr_rta_const(t, av[i]) : LR_UNKNOWN_CLASS;
    if (m >= 0) lr_rta_recv(t, m);
    else lr_rta_any(t);
  }
  for (int sg = 0; sg < 2; sg++)
    for (int i = definition_form_hooks(by, sg, hook) - 1; i >= 0; i--) lr_rta_call(t, hook[i], RQ_INST | RQ_CLASS);
}

/* The constants the program binds by class and module headers and builder
   writes alone, sorted, each with its one class (LR_CORE: a builder's no
   header reopens; LR_UNKNOWN_CLASS: bound otherwise too, or to two);
   none (ncname -1) when the program sets or removes a constant by a call */
static int lr_rta_name_cmp(const void *a, const void *b) {
  return strcmp(((const LrRtaName *)a)->name, ((const LrRtaName *)b)->name);
}
static void lr_rta_names(LrRta *t) {
  const NodeTable *nt = t->c->nt;
  int n = 0, m = 0;
  if (!(t->cname = malloc(sizeof *t->cname * (size_t)t->n + 1))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int id = 0; id < t->n; id++) {
    NodeKind k = nt_kind(nt, id);
    const char *cn = k == NK_CallNode ? nt_str(nt, id, "name") : NULL;
    int path = k == NK_ConstantPathWriteNode || k == NK_ConstantPathOrWriteNode || k == NK_ConstantPathOperatorWriteNode;
    if (k == NK_ClassNode || k == NK_ModuleNode)
      t->cname[n++] = (LrRtaName){ lr_written_name(nt, nt_ref(nt, id, "constant_path")), elab_header_class(t->c, id, NULL, NULL) };
    else if (path || k == NK_ConstantWriteNode || k == NK_ConstantOrWriteNode || k == NK_ConstantOperatorWriteNode) {
      int b = lr_rta_builder(t->c, id);
      t->cname[n++] = (LrRtaName){ lr_written_name(nt, path ? nt_ref(nt, id, "target") : id), b != LR_UNKNOWN_CLASS ? b : lr_class_of(t->c, &(LrDef){ .node = id }) };
    }
    if ((cn && (is_const_set_name(cn) || is_remove_const_name(cn))) || (n && !t->cname[n - 1].name)) { t->ncname = -1; return; }
  }
  qsort(t->cname, (size_t)n, sizeof *t->cname, lr_rta_name_cmp);
  for (int i = 0; i < n; i++)   /* a name bound to two classes, or otherwise too: unknown */
    if (m && !strcmp(t->cname[m - 1].name, t->cname[i].name)) { if (t->cname[m - 1].cls != t->cname[i].cls) t->cname[m - 1].cls = LR_UNKNOWN_CLASS; }
    else t->cname[m++] = t->cname[i];
  t->ncname = m;
}

static void lr_rta(Compiler *c) {
  LoadRecord *r = c->lrec;
  const int *par = comp_parent_map(c);
  LrRta t = { .c = c, .r = r, .n = c->nt->count < r->n ? c->nt->count : r->n };
  int n = t.n, ncl = c->nclasses + 1, nk = r->nst + n;
  t.key = malloc(sizeof(int) * (size_t)(4 * n + r->nst + 2 + r->nhist + ncl));
  t.hreach = calloc((size_t)(r->nhist + r->nnames + 3 * ncl + n) + 1, 1);
  if (!t.key || !t.hreach) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  t.ustart = t.key + n; t.ulist = t.ustart + nk + 2; t.stack = t.ulist + n; t.hnext = t.stack + n; t.hfirst = t.hnext + r->nhist;
  t.rq = t.hreach + r->nhist; t.sub = t.rq + r->nnames; t.ext = t.sub + ncl; t.seen = t.ext + ncl;
  for (int i = 0; i < n; i++) {   /* each node's innermost def (the def itself is its outer one's) */
    int p = par[i];
    while (p >= 0 && p < n && p > i && nt_kind(c->nt, p) != NK_DefNode) p = par[p];
    t.key[i] = p < 0 || p >= n ? -1 : nt_kind(c->nt, p) == NK_DefNode ? p : t.key[p];
  }
  for (int i = 0; i < n; i++)   /* grouped: a def's nodes; the others by their statement of region 0 */
    t.key[i] = t.key[i] >= 0 ? r->nst + t.key[i] : r->at[i] >= 0 && r->st[r->at[i]].region == 0 ? r->at[i] : -1;
  memset(t.ustart, 0, sizeof(int) * (size_t)(nk + 2));
  for (int i = 0; i < n; i++) if (t.key[i] >= 0) t.ustart[t.key[i] + 2]++;
  for (int i = 1; i < nk + 2; i++) t.ustart[i] += t.ustart[i - 1];
  for (int i = 0; i < n; i++) if (t.key[i] >= 0) t.ulist[t.ustart[t.key[i] + 1]++] = i;
  for (int k = 0; k < ncl; k++) { t.hfirst[k] = -1; r->rinst[k] = LR_NEVER; }
  for (int i = r->nhist - 1; i >= 0; i--) {
    const LrHist *h = &r->hist[i];
    if (h->a < LR_MAIN || !h->name || h->a >= c->nclasses) continue;
    for (int j = 0; j < h->n; j++) if (h->v[j].kind == LR_DEF && h->v[j].node < n) r->urun[h->v[j].node] = LR_NEVER;
    if (!h->b) { t.hnext[i] = t.hfirst[h->a + 1]; t.hfirst[h->a + 1] = i; }
  }
  for (int k = LR_MAIN; k < c->nclasses; k++) {
    LrClass *kc = lr_class(r, k);
    for (int j = 0; kc && j < kc->nmix; j++)
      if (elab_mix_on_singleton(kc->mix[j].kind)) lr_rta_ext(&t, lr_later(c, &kc->mix[j].mod, kc->mix[j].ref), 0);
  }
  NT_FOREACH_KIND(c->nt, NK_CallNode, b)
    if (class_def_is_record_call(c->nt, b)) t.builders = 1;
  lr_rta_names(&t);
  lr_rta_add(&t, LR_MAIN);
  for (int k = 0; k < c->nclasses; k++) if (c->classes[k].is_builtin_const) lr_rta_add(&t, k);
  for (t.s = 0; t.s < r->nst; t.s++)
    for (int i = t.ustart[t.s]; i < t.ustart[t.s + 1]; i++)
      for (lr_rta_node(&t, t.ulist[i]); t.nstack;) {
        int d = r->nst + t.stack[--t.nstack];
        for (int j = t.ustart[d]; j < t.ustart[d + 1]; j++) lr_rta_node(&t, t.ulist[j]);
      }
  free(t.key); free(t.hreach); free(t.cname);
}

/* Each statement's run bit and the count before it (LrStmt.runs, nrun),
   for the map's build, and each region's end. A statement runs code of
   the program's when a node of its own does (LR_RUN), or may when it
   dispatches once the program's code has run (made an object or a
   callable, defined a method by a computed name): from the first
   statement of region 0 that runs it on, and in any other region, which
   runs at a time not known. Before it only a method the runtime calls by
   itself that is there for any object can run: main's, Object's, a
   singleton's or a builtin class's, or one with a module mixed into one
   of those (global). */
static void lr_run_points(Compiler *c) {
  LoadRecord *r = elab_fresh(c);
  if (r->rend && r->runs_of == r->builds) return;
  int implicit = 0, global = 0, seen = 0, n = 0;
  for (int i = 0; i < r->nhist && !global; i++) {
    const LrHist *h = &r->hist[i];
    if (h->a < LR_MAIN || !(method_name_implicitly_invoked(h->name) || is_method_missing_name(h->name))) continue;
    implicit = 1;
    global = h->a == LR_MAIN || h->b || c->classes[h->a].is_builtin_const;
  }
  for (int k = LR_MAIN; implicit && !global && k + 1 < r->ncls; k++) {
    const LrClass *kc = lr_class(r, k);
    for (int j = 0; kc && j < kc->nmix && !global; j++)
      global = k == LR_MAIN || c->classes[k].is_builtin_const || elab_mix_on_singleton(kc->mix[j].kind);
  }
  if (!(r->rend = realloc(r->rend, sizeof(int) * (size_t)r->nregion))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  r->defs_computed = 0;
  NT_FOREACH_KIND(c->nt, NK_CallNode, b) {
    const char *bn = nt_str(c->nt, b, "name");
    int an = nt_ref(c->nt, b, "arguments"), ac = 0;
    const int *av = an >= 0 ? nt_arr(c->nt, an, "arguments", &ac) : NULL;
    if (bn && is_method_naming_call(bn) && ac && !vis_arg_name(c->nt, av[0])) r->defs_computed = 1;
  }
  for (int s = 0; s < r->nst; s++) r->st[s].runs = 0;
  for (int id = 0; id < r->n; id++)
    if (r->at[id] >= 0 && !(r->bits[id] & LR_LATE)) {
      r->st[r->at[id]].runs |= (unsigned char)lr_node_runs(c, id);
    }
  for (int s = 0; s < r->nst; s++) {
    LrStmt *e = &r->st[s];
    seen |= e->region != 0;
    e->runs = (e->runs & LR_RUN) || ((e->runs & LR_DISPATCH) && (global || seen));
    e->nrun = n;
    seen |= e->runs;
    n += e->runs;
    r->rend[e->region] = s + 1;
  }
  r->nrun = n;
  free(r->rinst); free(r->urun);
  r->rinst = calloc((size_t)(r->ncls > c->nclasses ? r->ncls : c->nclasses + 1) + 1, sizeof(int));
  r->urun = calloc((size_t)r->n + 1, sizeof(int));
  if (!r->rinst || !r->urun) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  if (r->nregion == 1) lr_rta(c);
  r->runs_of = r->builds;
}

/* How many statements in seq [a, b) may run code of the program's */
static int lr_runs_in(const LoadRecord *r, int a, int b) {
  return (b < r->nst ? r->st[b].nrun : r->nrun) - (a < r->nst ? r->st[a].nrun : r->nrun);
}

int elab_call_window(Compiler *c, int id, ElabWindow *w) {
  LoadRecord *r = elab_fresh(c);
  const int *par = comp_parent_map(c);
  int a = id;
  *w = (ElabWindow){ { -1, -1 }, { -1, -1 }, 0, 0 };
  while (a >= 0 && a < r->n && elab_point_of(c, a).region < 0) a = par[a];
  LoadPoint p = elab_point_of(c, a), end;
  if (!r->recorded || p.region < 0) return ELAB_UNKNOWN;
  lr_run_points(c);
  w->from = p;
  w->to = (LoadPoint){ p.region, r->rend[p.region] };
  if (a == id) {   /* its own statement, or the outermost body around it that runs again */
    NodeKind k = nt_kind(c->nt, r->st[p.seq].node);
    int e = k == NK_WhileNode || k == NK_UntilNode ? p.seq : r->st[p.seq].rep;
    while (e >= 0 && r->st[e].rep >= 0) e = r->st[e].rep;
    w->from.seq = e >= 0 ? e : p.seq;
    w->to.seq = (e >= 0 ? r->st[e].last : p.seq) + 1;
    w->closed = 1;
    w->wraps = e >= 0;
    return ELAB_YES;
  }
  const Scope *v = nt_kind(c->nt, a) == NK_DefNode ? comp_scope_of(c, id) : NULL;
  if (v && v->def_node == a && v->replaced && elab_body_def(c, id) == a &&
      elab_version_end(c, v->class_id, v->name, v->is_cmethod, a, &end) == ELAB_YES &&
      !elab_version_captured(c, v->name) && elab_runs_with(c, id, a)) {
    w->to = end;
    w->closed = 1;
  }
  if (nt_kind(c->nt, a) == NK_DefNode && r->urun[a] > w->from.seq)   /* nor before the body may first run (lr_rta) */
    w->from.seq = r->urun[a] < w->to.seq ? r->urun[a] : w->to.seq - 1;
  return ELAB_YES;
}

/* The owners whose histories can be in cls's ancestry: its superclass chain
   and the modules mixed into any of those, a module's own mixins too (at any
   time, so a superset of lr_in_ancestry's). -1 when that is not known: a
   superclass or a mixin the record cannot resolve, code it cannot follow run
   on one of them, or more owners than fit. */
static int lr_ancestry_owners(Compiler *c, int cls, int *out, int cap) {
  LoadRecord *r = c->lrec;
  int n = 0, a = cls;
  for (int d = 0; a >= LR_MAIN; a = lr_super(c, a), d++) {
    if (d >= 64 || n >= cap) return -1;
    out[n++] = a;
  }
  if (a != LR_UNKNOWN_CLASS || out[n - 1] != LR_MAIN) return -1;
  for (int i = 0; i < n; i++) {
    if (lr_hist(r, LR_KEY_OPAQUE, lr_const_owner(out[i]), "", 0)) return -1;
    const LrClass *kc = lr_class(r, out[i]);
    for (int j = 0; kc && j < kc->nmix; j++) {
      int mod = lr_mixin_class(c, &kc->mix[j]), have = 0;
      if (mod < LR_MAIN) {   /* a builtin module has a history only if the program reopens it, which no name chain holds */
        int builtin = 0;
        for (int k = 0; !builtin && builtin_class_at(k); k++) builtin = builtin_class_at(k)->id == mod;
        if (!builtin) return -1;
        continue;
      }
      for (int k = 0; k < n && !have; k++) have = out[k] == mod;
      if (have) continue;
      if (n >= cap) return -1;
      out[n++] = mod;
    }
  }
  return n;
}

typedef struct { int m, nh, any, *cp; } LrSpanAcc;
/* history h's versions inside the window (p0, p1), when it is one of cls's ancestors' */
static void lr_span_take(Compiler *c, const LrHist *h, int cls, int sg, LoadPoint p0, LoadPoint p1, LrSpanAcc *s) {
  if ((h->b != sg && !sg) || !lr_in_ancestry(c, cls, sg, h->a, p1)) return;
  int had = s->m;
  for (int j = 0; j < h->n; j++) {
    const LrDef *v = &h->v[j];
    if (!lr_is_version(v) || !lr_inside(c, v->node, p0, p1)) continue;
    LoadPoint q = elab_point_of(c, v->node);
    if (v->may || elab_cmp(p0, q) != PROG_BEFORE || elab_cmp(q, p1) != PROG_BEFORE) { s->any = 1; continue; }
    if (!(s->m & (s->m - 1)) && !(s->cp = realloc(s->cp, sizeof(int) * (size_t)(s->m ? 2 * s->m : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    int k = s->m++;
    while (k > 0 && s->cp[k - 1] > q.seq) { s->cp[k] = s->cp[k - 1]; k--; }
    s->cp[k] = q.seq;
  }
  s->nh += s->m > had;
}

/* elab_method_span; *idle when no statement of w that may run code of the
   program's sees a version (the one version counts all the same) */
static int lr_span(Compiler *c, int cls, const char *name, int sg, const ElabWindow *w, const LrDef **out, int cap, int *idle) {
  LoadRecord *r = c->lrec;
  LoadPoint p0 = w->from, p1 = w->to;
  if (!r || !r->recorded || !name || cls < LR_MAIN || p0.region < 0 || elab_cmp(p0, p1) != PROG_BEFORE) return -1;
  lr_run_points(c);
  /* no call on a class before it opens, nor on an instance of it before
     the first statement one may exist (lr_rta); none in w: w's last
     statement. In a program of more regions, from w's start: more
     versions, never fewer */
  const LrClass *kc = cls >= 0 && !c->classes[cls].is_builtin_const && r->nregion == 1 ? lr_class(r, cls) : NULL;
  LoadPoint b = kc && kc->nopen ? elab_point_of(c, kc->open[0].node) : (LoadPoint){ -1, -1 };
  int bl = b.region != p0.region ? -1 : sg || r->rinst[cls + 1] < b.seq ? b.seq : r->rinst[cls + 1];
  if (bl >= p1.seq) bl = p1.seq - 1;
  if (p0.seq < bl && bl < p1.seq) p0.seq = bl;
  int n = 0, obs0 = 0, owners[64], no = lr_ancestry_owners(c, cls, owners, 64);
  LrSpanAcc acc = { 0, 0, 0, NULL };
  const LrVers *x = lr_vers(r, name);
  if (no >= 0) {   /* the histories of the ancestors alone, through the hash */
    for (int i = 0; i < no; i++)
      for (int side = 0; side <= (sg != 0); side++) {
        const LrHist *h = lr_hist(r, owners[i], side, name, 0);
        if (h) lr_span_take(c, h, cls, sg, p0, p1, &acc);
      }
  }
  else
    for (int i = x ? x->first : -1; i >= 0; i = r->hist[i].nlink) lr_span_take(c, &r->hist[i], cls, sg, p0, p1, &acc);
  int m = acc.m, nh = acc.nh, any = acc.any, *cp = acc.cp;
  /* in a body that runs again, the version at its end is live at its start
     on the next run: so with the changes of one history only; with more, or
     a version w may not run or the record cannot order, any version of the
     chain, while the chain itself holds still */
  if (lr_chain_moves(c, cls, sg, name, p0, p1)) { free(cp); return -1; }
  if (any || (w->wraps && nh > 1)) { free(cp); return ELAB_SPAN_ANY; }
  const LrDef **got = malloc(sizeof *got * (size_t)(m + 1));
  if (!got) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  if (idle) *idle = 1;
  for (int i = 0; i <= m; i++) {
    if (i && i < m && cp[i] == cp[i - 1]) continue;
    int lo = i ? cp[i - 1] : p0.seq, hi = i < m ? cp[i] : p1.seq;
    /* a change point's statement on both sides: a hook runs after it, its
       arguments before */
    int obs = lr_runs_in(r, lo, i < m ? hi + 1 : hi) > 0;
    if (idle && obs) *idle = 0;
    if (!i) obs0 = obs;
    if (!(obs || !m || (i == m && (!w->closed || (w->wraps && obs0))))) continue;
    const LrDef *d = NULL;
    int o = lr_search(c, cls, sg, name, -1, (LoadPoint){ p0.region, hi }, w->wraps ? LR_ONCE : 0, 0, &d, 0);
    if (o == ELAB_UNKNOWN) { free(cp); free(got); return ELAB_SPAN_ANY; }
    if (o != ELAB_YES) d = NULL;
    int k = 0;
    while (k < n && got[k] != d) k++;
    if (k == n) got[n++] = d;
  }
  for (int k = 0; k < n && k < cap; k++) out[k] = got[k];
  free(cp); free(got);
  return n;
}

int elab_method_span(Compiler *c, int cls, const char *name, int sg, const ElabWindow *w, const LrDef **out, int cap) {
  return lr_span(c, cls, name, sg, w, out, cap, NULL);
}

/* Can the program run code the record has not seen? Only through a load
   whose target it could not resolve: under --load-model=graph a LoadNode
   whose path is computed when it runs, inline a require, require_relative,
   load or autoload the parse left in the tree (one it resolved it spliced
   away). A require or a load is Kernel's on no receiver or on Object or
   Kernel (`YAML.load` reads no file); an autoload on any receiver is one
   (`Mod.autoload(:X, path)`, which the parse leaves). The files of a closed
   set a computed path may name are seen: each is a region of its own.
   Read from the tree, once. */
int elab_runs_unseen(Compiler *c) {
  LoadRecord *r = lr_get(c);
  if (!r->unseen) {
    const NodeTable *nt = c->nt;
    int hit = 0, n = 0;
    const int *v = nt_nodes_of_kind(nt, NK_CallNode, &n);
    for (int i = 0; !hit && i < n; i++) {
      int recv = nt_ref(nt, v[i], "receiver");
      const char *nm = nt_str(nt, v[i], "name");
      int nauto; comp_autoloads(c, v[i], &nauto);
      hit = !nauto && is_file_load_name(nm) &&
            (recv < 0 || is_autoload_name(nm) || (nt_kind(nt, recv) == NK_ConstantReadNode && is_object_root(nt_str(nt, recv, "name"))));
    }
    r->unseen = hit ? 2 : 1;
  }
  return r->unseen == 2;
}

/* May a version of `name` run outside its interval? Only the calls bound
   to it reach it while nothing else names the method: no Symbol or String
   in the program spells the name (an alias, a method object, a send, a
   respond_to?, an enum_for), the program names or makes no method by a
   name it computes (Compiler.names_by_value), and it runs no code the
   record has not seen (elab_runs_unseen). */
int elab_version_captured(Compiler *c, const char *name) {
  LrVers *x = c->lrec && c->lrec->recorded ? lr_vers(c->lrec, name) : NULL;
  if (!x) return 1;
  if (!x->captured) {
    const NodeTable *nt = c->nt;
    int hit = c->names_by_value || elab_runs_unseen(c), n = 0;
    const int *v = nt_nodes_of_kind(nt, NK_SymbolNode, &n);
    for (int i = 0; !hit && i < n; i++) { const char *sv = nt_str(nt, v[i], "value"); hit = sv && sp_streq(sv, name); }
    v = nt_nodes_of_kind(nt, NK_StringNode, &n);
    for (int i = 0; !hit && i < n; i++) {
      const char *s = nt_str(nt, v[i], "content");   /* a StringNode's text */
      hit = s && sp_streq(s, name);
    }
    x->captured = hit ? 2 : 1;
  }
  return x->captured == 2;
}

/* Did a pass make node, a copy included (nt_origin), or does it come from
   the builtin prelude (node_bi) or a file the resolver found under
   builtins/ or a pre-installed package (nt_file_tier)? Such a node stands
   where the pass linked it, which for a def or a mixin a lowering appends,
   or a method body it copies into a helper, is after every file, or where
   the program's require of the library is, which CRuby may have loaded
   before the program (`set`, `pathname` at boot): no point in CRuby's
   load order the record holds. Not a copy a rewrite put in its source's
   place (node_in_place: the source became the parentheses that run it), which
   runs where the source stood, in its frame, once, and stands where it is
   linked; nor a node of an eval's text the graft put in place of the eval
   (node_eval), which runs where the eval runs, as the program's own code
   does. A clone of either (how 'c') stands where the clone is. */
static int lr_unplaced(Compiler *c, int node) {
  char how = 0; int a = 0, k = 0; unsigned pass = 0;
  int tier = nt_file_tier(c->nt, (int)nt_int(c->nt, node, "node_file", -1));
  int made = nt_origin(c->nt, node, &how, &a, &pass, &k) >= 0;
  return nt_int(c->nt, node, "node_bi", 0) ||
         (made && (how == 'c' || (!nt_int(c->nt, node, "node_in_place", 0) && !nt_int(c->nt, node, "node_eval", 0)))) ||
         tier == SP_LIB_BUILTIN || tier == SP_LIB_PACKAGE;
}

/* May what the record cannot order supply `name` on cls's chain (its
   singleton's when sg) at p: a version of the name a pass made, a mixin a
   pass made into a class on the chain, or code the record cannot follow
   (a send, an eval of a String) run on one before p? */
static int lr_unordered_supply(Compiler *c, int cls, int sg, const char *name, LoadPoint p) {
  LoadRecord *r = c->lrec;
  const LrVers *x = lr_vers(r, name);
  for (int i = x ? x->first : -1; i >= 0; i = r->hist[i].nlink)
    for (int j = 0; j < r->hist[i].n; j++)
      if (lr_is_version(&r->hist[i].v[j]) && lr_unplaced(c, r->hist[i].v[j].node)) return 1;
  for (int k = LR_MAIN; k + 1 < r->ncls; k++) {
    const LrClass *kc = lr_class(r, k);
    const LrHist *op = lr_hist(r, LR_KEY_OPAQUE, lr_const_owner(k), "", 0);
    if ((!kc || !kc->nmix) && !op) continue;
    if (!lr_in_ancestry(c, cls, sg, k, p)) continue;
    for (int j = 0; kc && j < kc->nmix; j++)
      if (lr_unplaced(c, kc->mix[j].node)) return 1;
    for (int j = 0; op && j < op->n; j++)
      if (elab_cmp(elab_point_of(c, op->v[j].node), p) != PROG_AFTER) return 1;
  }
  return 0;
}

/* Does cls's chain pass a class whose superclass the record does not
   hold: one a call makes (`Struct.new`, `Data.define`), one whose header
   names a superclass the record does not know (`< Struct.new(:a)`, a core
   class), a class with no header? Its ancestors past it are unknown. */
static int lr_chain_open(Compiler *c, int cls) {
  for (int a = cls, d = 0; a >= 0 && a < c->nclasses; a = lr_super(c, a)) {
    if (++d > 64) return 1;
    LrClass *k = lr_class(c->lrec, a);
    int dn = c->classes[a].def_node;
    if (k && k->has_sup) { if (lr_later(c, &k->sup, k->sup_ref) < 0) return 1; continue; }
    if (c->classes[a].parent >= 0 || class_def_is_module(c->nt, dn)) continue;
    if (!class_def_is_class(c->nt, dn) || class_def_super(c->nt, dn) >= 0) return 1;
  }
  return 0;
}
/* Is every instance cls's new makes a plain object: cls a class of the
   program whose chain, as the record knows it, reaches Object through
   classes of the program alone (not Module, Class or Struct), in a program
   that defines no method of new's name? */
static int lr_new_makes_object(Compiler *c, int cls, const char *new_name) {
  if (cls < 0 || lr_named(c, new_name) || lr_chain_open(c, cls)) return 0;
  for (int a = cls, g = 0; g < 64; g++, a = lr_super(c, a)) {
    if (a == LR_MAIN || (a >= 0 && a == comp_core_class(c, CORE_OBJECT))) return 1;
    if (!class_def_is_class(c->nt, a >= 0 && a < c->nclasses ? c->classes[a].def_node : -1)) return 0;
  }
  return 0;
}

/* Is node a value no module can be where it stands: a builtin's own
   constructor on the core class (`Object.new`, `Hash.new`), a program
   class's new whose instances are plain objects (lr_new_makes_object), or
   a constant whose write the record finds there is one? */
int elab_value_not_module(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const LrDef *d = NULL;
  NodeKind k = node >= 0 ? nt_kind(nt, node) : NK_NilNode;
  if (k == NK_ConstantReadNode && !lr_root_read(nt, node)) {
    if (lr_const_at(c, node, lr_written_name(nt, node), elab_point_of(c, node), &d, LR_KEY_CONST) != ELAB_YES ||
        nt_kind(nt, d->node) != NK_ConstantWriteNode) return 0;
    node = nt_ref(nt, d->node, "value");
    k = node >= 0 ? nt_kind(nt, node) : NK_NilNode;
  }
  int r = k == NK_CallNode && nt_ref(nt, node, "block") < 0 ? nt_ref(nt, node, "receiver") : -1;
  if (r < 0 || nt_kind(nt, r) != NK_ConstantReadNode || lr_root_read(nt, r)) return 0;
  const char *cn = nt_str(nt, r, "name"), *mn = nt_str(nt, node, "name");
  if (is_value_constructor(cn, mn))
    return lr_const_at(c, r, cn, elab_point_of(c, r), &d, LR_KEY_CONST) == ELAB_NO;
  return is_new_name(mn) && lr_new_makes_object(c, lr_class_ref_at(c, r, NULL), mn);
}

int elab_call_method(Compiler *c, int id, int cls, const char *name, int sg) {
  LoadRecord *r = elab_fresh(c);
  LoadPoint p = elab_point_of(c, id);
  if (p.region < 0 || !r->recorded || r->st[p.seq].kept || lr_unplaced(c, id) || lr_chain_open(c, cls)) return ELAB_UNKNOWN;
  const LrDef *d = NULL;
  int o = elab_method_at(c, cls, name, sg, p, &d);
  /* an undef or a removal of an earlier definition: the call's answer is
     that undef's, not one of a method not defined yet */
  if (o == ELAB_NO && d) return ELAB_UNKNOWN;
  /* the tables' side holds two of the record's: main's top-level functions
     are its own methods and its singleton's, and the Class reopening's
     class methods are its instance methods, which every class answers */
  if (o == ELAB_NO && cls < 0 && !sg) o = elab_method_at(c, LR_MAIN, name, 1, p, NULL);
  if (o == ELAB_NO && sg && cls >= 0 && cls == comp_core_class(c, CORE_CLASS_REOPEN)) o = elab_method_at(c, cls, name, 0, p, NULL);
  /* a class's singleton chain passes Object's and BasicObject's singletons
     before Class's instance methods */
  int d0 = cls >= 0 ? c->classes[cls].def_node : -1;
  for (int i = 0; o == ELAB_NO && sg && d0 >= 0 && !class_def_is_module(c->nt, d0) && i < 2; i++) {
    int k = lr_core(r, i ? CORE_BASIC_OBJECT : CORE_OBJECT);
    if (k >= 0 && k != cls) o = elab_method_at(c, k, name, 1, p, NULL);
  }
  /* a method_missing may answer the call */
  if (o == ELAB_NO && (elab_method_versions(c, "method_missing") || elab_method_versions(c, "respond_to_missing?"))) return ELAB_UNKNOWN;
  if (o == ELAB_NO && (elab_runs_unseen(c) || lr_unordered_supply(c, cls < 0 ? LR_MAIN : cls, sg, name, p))) return ELAB_UNKNOWN;
  return o;
}

int elab_self_at(Compiler *c, int node) {
  LoadRecord *r = elab_fresh(c);
  LoadPoint p = elab_point_of(c, node);
  if (p.region < 0) return LR_UNKNOWN_CLASS;
  int f = r->st[p.seq].frame;
  if (f < 0) return LR_MAIN;
  /* a class or module body, or a class builder's block (made at its write) */
  int cls = class_def_opens_frame(c->nt, r->st[f].node) ? elab_header_class(c, r->st[f].node, NULL, NULL) : LR_UNKNOWN_CLASS;
  return cls >= 0 ? cls : LR_UNKNOWN_CLASS;
}

int elab_cref_at(Compiler *c, int node) {
  (void)elab_fresh(c);
  LoadPoint p = elab_point_of(c, node);
  return p.region < 0 ? LR_UNKNOWN_CLASS : lr_cref(c, p.seq);
}

int elab_mixed_at(Compiler *c, int cls, int sg, int mod, LoadPoint p) {
  if (!lr_ready(c, p) || mod < 0) return ELAB_UNKNOWN;
  return lr_search(c, cls, sg, NULL, mod, p, p.region < 0, 0, NULL, 0);
}

/* ELAB_YES when either is, else ELAB_UNKNOWN when either is, else ELAB_NO
   (an undef's LR_STOP: none) */
static int lr_either(int a, int b) {
  return a == ELAB_YES || b == ELAB_YES ? ELAB_YES : a == ELAB_UNKNOWN || b == ELAB_UNKNOWN ? ELAB_UNKNOWN : ELAB_NO;
}

/* Does the program define a method `name` anywhere (lr_names_take)? */
static int lr_named(Compiler *c, const char *name) {
  return c->lrec->names_any || lr_defines(c->lrec, name);
}

/* ...by that name itself */
static int lr_defines(const LoadRecord *r, const char *name) {
  return bsearch(&name, r->names, (size_t)r->nnames, sizeof *r->names, lr_name_cmp) != NULL;
}
int elab_names_method(Compiler *c, const char *name) {
  const LoadRecord *r = c->lrec;
  return !r || (!r->recorded && !r->walk) || lr_named(c, name);
}
/* A method the compiler makes for the program after the record took its
   names (a synthesized helper a rewritten call names, such as
   __enum_to_a, which calls the program's #each) joins the set, kept
   sorted. */
void elab_names_add(Compiler *c, const char *name) {
  LoadRecord *r = c->lrec;
  if (!r || !name || lr_named(c, name)) return;
  lr_name(r, name, "");
  char *d = r->names[r->nnames - 1];
  int i = r->nnames - 1;
  while (i > 0 && strcmp(r->names[i - 1], d) > 0) { r->names[i] = r->names[i - 1]; i--; }
  r->names[i] = d;
}

/* Is a method of the program's what a call of `name` on class cls (main
   for LR_MAIN) finds at p: along its singleton's ancestors, which end in
   Class's, Module's, Object's and Kernel's instance methods (main's top
   level's among them)? A class the record does not know: by name. */
static int lr_runs(Compiler *c, int cls, const char *name, LoadPoint p) {
  if (cls < LR_MAIN) return lr_named(c, name) ? ELAB_UNKNOWN : ELAB_NO;
  return lr_search(c, cls, 1, name, -1, p, 0, 0, NULL, 0);
}

/* Does definition form `nm` at node, on class cls (sg: in its singleton),
   run code of the program's: a method of the form's own name the call
   finds, or a hook the form fires (definition_form_hooks) on the class it
   receives, or for a mixin on the module? */
static int lr_form_answer(Compiler *c, int node, int cls, int sg) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, node, "name");
  int an = nt_ref(nt, node, "arguments"), ac = 0;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  LoadPoint p = elab_point_of(c, node);
  const char *hook[2];
  int nh = ac ? definition_form_hooks(nm, sg, hook) : 0, mix = is_mixin_call(nm);
  int o = lr_runs(c, cls, nm, p);
  for (int i = 0; i < nh; i++)
    for (int j = 0; j < (mix ? ac : 1); j++)
      o = lr_either(o, lr_runs(c, mix ? lr_class_ref_at(c, av[j], NULL) : cls, hook[i], p));
  return o;
}
/* ...kept for elab_form_runs */
static void lr_form_runs(ElabRec *x, int node, int cls, int sg, int may) {
  lr_version(x->r, LR_KEY_RUNS, node, "", (LrDef){ node, LR_OPAQUE, (unsigned char)lr_form_answer(x->c, node, cls, sg), (unsigned char)may, 0, NULL });
  lr_settle_add(x->r, SET_RUNS, node, cls, sg, may, 0, NULL);
}

int elab_form_runs(Compiler *c, int node) {
  if (!c->lrec || (!c->lrec->recorded && !c->lrec->walk)) return ELAB_UNKNOWN;
  const LrHist *h = lr_hist(c->lrec, LR_KEY_RUNS, node, "", 0);
  if (h && h->n) return h->v[h->n - 1].vis == ELAB_NO ? ELAB_NO : h->v[h->n - 1].may ? ELAB_UNKNOWN : h->v[h->n - 1].vis;
  if (!c->lrec->recorded) return ELAB_UNKNOWN;   /* not recorded yet */
  /* a form the record did not see as a statement (`private attr_reader
     :x`): by name, kept for the next question */
  const char *nm = nt_str(c->nt, node, "name"), *hook[2];
  int o = nm && lr_named(c, nm);
  for (int sg = 0; sg < 2 && !o; sg++)
    for (int i = definition_form_hooks(nm, sg, hook) - 1; i >= 0 && !o; i--) o = lr_named(c, hook[i]);
  o = o ? ELAB_UNKNOWN : ELAB_NO;
  lr_version(c->lrec, LR_KEY_RUNS, node, "", (LrDef){ node, LR_OPAQUE, (unsigned char)o, 0, 0, NULL });
  return o;
}

int elab_written_before(Compiler *c, ElabVar kind, const char *name, LoadPoint p) {
  if (!lr_ready(c, p)) return ELAB_UNKNOWN;
  const LrHist *h = lr_hist(c->lrec, -2 - (int)kind, kind == ELAB_CONST ? lr_const_owner(LR_MAIN) : 0, name, 0);
  int unsure = 0;
  for (int i = 0; h && i < h->n; i++) {
    const LrDef *v = &h->v[i];
    int o = p.region < 0 ? (v->may || elab_point_of(c, v->node).region != 0 ? PROG_UNKNOWN : PROG_BEFORE)
                         : elab_cmp(elab_point_done(c, v->node), p);
    if (o == PROG_UNKNOWN) unsure = 1;
    else if (o == PROG_BEFORE && !v->nil) { if (!v->may) return ELAB_YES; unsure = 1; }
  }
  return unsure ? ELAB_UNKNOWN : ELAB_NO;
}

/* The flat slot written by a history entry, after qualified names have
   been lowered. Class headers do not assign a value slot. */
static const char *lr_const_slot_name(const NodeTable *nt, int node) {
  NodeKind k = nt_kind(nt, node);
  if (k == NK_ClassNode || k == NK_ModuleNode) return NULL;
  if (k == NK_CallNode) {
    /* a const_set the load stage left a call (desugar_const_set_write):
       Object's sets the top-level slot; any other's slot is not known */
    int a = nt_ref(nt, node, "arguments"), n = 0, rv = nt_ref(nt, node, "receiver");
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &n) : NULL;
    return n && rv >= 0 && nt_kind(nt, rv) == NK_ConstantReadNode && is_object_base_name(nt_str(nt, rv, "name")) ?
      vis_arg_name(nt, av[0]) : NULL;
  }
  int target = nt_ref(nt, node, "target");
  return nt_str(nt, target >= 0 ? target : node, "name");
}

/* A plain assignment is a statement directly in a file or class body,
   including a class builder's body and a multiple assignment's targets. */
static int lr_const_plain(Compiler *c, int node) {
  LoadRecord *r = c->lrec;
  LoadPoint p = elab_point_of(c, node);
  NodeKind k = nt_kind(c->nt, node);
  if (p.region < 0 || (k != NK_ConstantWriteNode && k != NK_ConstantPathWriteNode && k != NK_ConstantTargetNode)) return 0;
  const LrStmt *s = &r->st[p.seq];
  if (s->may || s->partial || (node != s->node && nt_kind(c->nt, s->node) != NK_MultiWriteNode)) return 0;
  for (;;) {
    if (s->depth != (s->frame >= 0 ? r->st[s->frame].depth + 1 : 0)) return 0;
    if (s->frame < 0) return 1;
    s = &r->st[s->frame];
  }
}

static LrSlot *lr_slot(LoadRecord *r, const char *name, int make) {
  unsigned b = lr_hash(0, 0, name);
  for (int k = r->slot_head[b]; k >= 0; k = r->slot[k].next)
    if (sp_streq(r->slot[k].name, name)) return &r->slot[k];
  if (!make) return NULL;
  r->slot = lr_grow(r->slot, &r->cslot, r->nslot, sizeof *r->slot);
  int k = r->nslot++;
  r->slot[k] = (LrSlot){ .name = name, .first = -1, .next = r->slot_head[b], .points = -1 };
  r->slot_head[b] = k;
  return &r->slot[k];
}

/* Presence belongs to the flat slot, regardless of the lexical owner.
   Index the histories once per build, by their current lowered names.
   Regions are incomparable, so each keeps its own earliest completions. */
static void lr_slot_index(Compiler *c) {
  LoadRecord *r = elab_fresh(c);
  if (r->slot_head && r->slot_builds == r->builds && r->slot_entries == r->nentries) return;
  if (!r->slot_head && !(r->slot_head = malloc(sizeof(int) * LR_BUCKETS))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int b = 0; b < LR_BUCKETS; b++) r->slot_head[b] = -1;
  r->nslot = r->nslotp = 0;
  for (int i = 0; i < r->nhist; i++) {
    const LrHist *h = &r->hist[i];
    if (h->a != -2 - (int)ELAB_CONST) continue;
    for (int j = 0; j < h->n; j++) {
      const LrDef *v = &h->v[j];
      const char *name = lr_const_slot_name(c->nt, v->node);
      if (!name) continue;
      LrSlot *s = lr_slot(r, name, 1);
      LoadPoint p = elab_point_done(c, v->node), q = elab_point_from(c, v->node);
      int k = s->points;
      while (k >= 0 && r->slotp[k].region != p.region) k = r->slotp[k].next;
      if (k < 0) {
        r->slotp = lr_grow(r->slotp, &r->cslotp, r->nslotp, sizeof *r->slotp);
        k = r->nslotp++;
        r->slotp[k] = (LrSlotPoint){ p.region, INT_MAX, INT_MAX, s->points };
        s->points = k;
      }
      LrSlotPoint *t = &r->slotp[k];
      if (p.seq < t->any) t->any = p.seq;
      if (!v->may && p.seq < t->sure) t->sure = p.seq;
      if (lr_const_plain(c, v->node)) {
        if (s->first < 0 || elab_cmp(p, s->done) == PROG_BEFORE) { s->first = v->node; s->done = p; }
      }
      else {
        if (!s->other || elab_cmp(q, s->from) == PROG_BEFORE) s->from = q;
        else if (elab_cmp(q, s->from) == PROG_UNKNOWN) s->from = (LoadPoint){ -1, -1, -1 };
        s->other = 1;
      }
    }
  }
  r->slot_builds = r->builds; r->slot_entries = r->nentries;
}

/* Any sure completed write suffices, including nil and before a later
   conditional update; no possible earlier write answers NO. */
int elab_const_slot_present_at(Compiler *c, const char *name, LoadPoint p) {
  if (!lr_ready(c, p)) return ELAB_UNKNOWN;
  lr_slot_index(c);
  LoadRecord *r = c->lrec;
  const LrSlot *s = lr_slot(r, name, 0);
  int unsure = 0;
  for (int k = s ? s->points : -1; k >= 0; k = r->slotp[k].next) {
    const LrSlotPoint *t = &r->slotp[k];
    if (p.region < 0 || t->region != p.region) unsure = 1;
    else if (t->sure < p.seq) return ELAB_YES;
    else if (t->any < p.seq) unsure = 1;
  }
  return unsure ? ELAB_UNKNOWN : ELAB_NO;
}

/* The first plain write, and whether another form may precede it.
   Settled values are the caller's fact, order is the record's. */
int elab_const_first_write(Compiler *c, const char *name, int *early) {
  lr_slot_index(c);
  const LrSlot *s = lr_slot(c->lrec, name, 0);
  *early = s && s->other && (s->first < 0 || elab_cmp(s->from, s->done) != PROG_AFTER);
  return s ? s->first : -1;
}

/* What owner holds of name at p under key: for LR_KEY_EITHER its constant,
   else its autoload registration */
static int lr_owner_last(Compiler *c, int key, int owner, const char *name, LoadPoint p, int late, const LrDef **out) {
  LoadRecord *r = c->lrec;
  int o = lr_last_before(c, lr_hist(r, key == LR_KEY_EITHER ? LR_KEY_CONST : key, owner, name, 0), p, late, 0, out);
  return o != ELAB_NO || key != LR_KEY_EITHER ? o : lr_last_before(c, lr_hist(r, LR_KEY_AUTO, owner, name, 0), p, late, 0, out);
}

/* The constants of cls and its ancestors at p: its own, its mixins' (the
   last first), then its superclass's, and with top Object's after a
   class's. A scoped read (`A::B`) leaves Object's out, as CRuby does. */
static int lr_const_in(Compiler *c, int cls, const char *name, LoadPoint p, int late, int top, const LrDef **out, int depth, int key) {
  LoadRecord *r = c->lrec;
  if (depth > 64) return ELAB_UNKNOWN;
  if (cls < LR_MAIN) {
    for (int i = 0; builtin_class_at(i); i++) {
      const BuiltinClass *b = builtin_class_at(i);
      if (b->id == cls && builtin_mixin_module_id(b->name))
        return is_constless_builtin_name(b->name) ? ELAB_NO : ELAB_UNKNOWN;
    }
    return ELAB_UNKNOWN;
  }
  int o = lr_owner_last(c, key, lr_const_owner(cls), name, p, late, out), unsure = 0;
  if (o != ELAB_NO) return o;
  if (cls >= 0 && builtin_mixin_module_id(c->classes[cls].name) &&
      !is_constless_builtin_name(c->classes[cls].name)) return ELAB_UNKNOWN;
  const LrClass *k = lr_class(r, cls);
  for (int i = k ? k->nmix - 1 : -1; i >= 0; i--) {
    LrMixin *m = &k->mix[i];
    int b = elab_mix_on_singleton(m->kind) ? ELAB_NO : lr_mixed_before(c, m, p, late, elab_point_may(c, m->node));
    if (b == ELAB_NO) continue;
    int mo = lr_const_in(c, lr_mixin_class(c, m), name, p, late, top, out, depth + 1, key);
    if (mo == ELAB_YES && b == ELAB_YES) return unsure ? ELAB_UNKNOWN : ELAB_YES;
    if (mo != ELAB_NO) unsure = 1;
  }
  int sup = lr_super(c, cls);
  if (sup == LR_MAIN && !top) sup = LR_UNKNOWN_CLASS;
  o = sup >= LR_MAIN ? lr_const_in(c, sup, name, p, late, top, out, depth + 1, key) : ELAB_NO;
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
    if (k == NK_ClassNode || k == NK_ModuleNode) return elab_header_class(c, r->st[f].node, NULL, NULL);
  }
  return LR_MAIN;
}

/* An unknown singleton owner has no constant table the record can search.
   A source write in such a body can hide an enclosing constant; after
   lexical lookup, a source write can belong to an unsearched ancestor. */
static int lr_singleton_const_written(Compiler *c, const char *name, int own) {
  const LoadRecord *r = c->lrec;
  const int *par = NULL;
  for (int i = 0; i < r->nsrcw; i++) {
    if (!sp_streq(r->srcw[i].name, name)) continue;
    if (!own) return 1;
    if (!par) par = comp_parent_map(c);
    for (int a = par[r->srcw[i].node]; a >= 0; a = par[a]) {
      NodeKind k = nt_kind(c->nt, a);
      if (k == NK_SingletonClassNode) return 1;
      if (k == NK_ClassNode || k == NK_ModuleNode) break;
    }
  }
  return 0;
}

/* What constant `name` read in node's lexical scopes resolves to at p, as
   CRuby resolves it there: each lexical scope's own constants, innermost
   first, then the ancestors of the innermost (Object's last) */
static int lr_const_at(Compiler *c, int node, const char *name, LoadPoint p, const LrDef **out, int key) {
  LoadRecord *r = elab_fresh(c);
  int e = node >= 0 && node < r->n ? r->at[node] : -1, late = p.region < 0, first = LR_MAIN;
  int singleton = 0, sgowner = 0;
  if (e < 0 || !name) return ELAB_UNKNOWN;
  for (int g = r->st[e].frame; g >= 0; g = r->st[g].frame) {   /* the class and module bodies around it */
    NodeKind k = nt_kind(c->nt, r->st[g].node);
    if (k == NK_SingletonClassNode) {
      int owner = lr_singleton_owner(c, g);
      if (!owner && lr_singleton_const_written(c, name, 1)) return ELAB_UNKNOWN;
      if (first == LR_MAIN && !singleton) { singleton = 1; sgowner = owner; }
      int o = owner ? lr_owner_last(c, key, owner, name, p, late, out) : ELAB_NO;
      if (o != ELAB_NO) return o;
      continue;
    }
    if (k != NK_ClassNode && k != NK_ModuleNode) continue;
    int ci = elab_header_class(c, r->st[g].node, NULL, NULL);
    if (ci < 0) return ELAB_UNKNOWN;
    if (first == LR_MAIN && !singleton) first = ci;
    int o = lr_owner_last(c, key, lr_const_owner(ci), name, p, late, out);
    if (o != ELAB_NO) return o;
  }
  /* then the innermost scope's ancestors, and Object's for a module too:
     a lexical read falls back to Object */
  for (int cls = sgowner ? lr_super(c, -sgowner - 2) : LR_UNKNOWN_CLASS, depth = 0;
       singleton && cls >= 0 && depth < 64; cls = lr_super(c, cls), depth++) {
    int o = lr_owner_last(c, key, lr_sg_const_owner(cls), name, p, late, out);
    if (o != ELAB_NO) return o;
  }
  int o = lr_const_in(c, first, name, p, late, 1, out, 0, key);
  if (o == ELAB_NO && first != LR_MAIN && lr_super(c, first) == LR_UNKNOWN_CLASS) o = lr_const_in(c, LR_MAIN, name, p, late, 1, out, 0, key);
  if (o == ELAB_NO && singleton && lr_singleton_const_written(c, name, 0)) o = ELAB_UNKNOWN;
  return o;
}

/* What constant `name` of class owner's own (LR_MAIN: Object's) was
   before p, as CRuby's `class X` asks its cbase (rb_const_defined_at): no
   lexical scope's and no ancestor's. *out: on ELAB_YES the write or header
   found; on ELAB_UNKNOWN the last one that may have run before p (one that
   may not have run, or one in another region), else NULL. */
int elab_const_own_at(Compiler *c, int owner, const char *name, LoadPoint p, const LrDef **out) {
  if (out) *out = NULL;
  /* a point the walk has not recorded up to answers nothing; a header
     at no point (in a lambda or a block run later) may run after any
     write the walk has recorded */
  if (!c->lrec || owner < LR_MAIN || !name || (!lr_ready(c, p) && (p.region >= 0 || !c->lrec->walk))) return ELAB_UNKNOWN;
  const LrHist *h = lr_hist(c->lrec, -2 - (int)ELAB_CONST, lr_const_owner(owner), name, 0);
  const LrDef *d = NULL;
  int o = lr_ready(c, p) ? lr_last_before(c, h, p, p.region < 0, 0, &d) : ELAB_UNKNOWN;
  for (int i = h ? h->n - 1 : -1; o == ELAB_UNKNOWN && i >= 0; i--) {
    int at = elab_cmp(elab_point_of(c, h->v[i].node), p);
    if (at == PROG_BEFORE || at == PROG_UNKNOWN) { d = &h->v[i]; break; }
  }
  if (out && o != ELAB_NO) *out = d;
  return o;
}

/* Until N2b: reads after the write still name the header's class by
   name. So a header whose owner holds nothing of its leaf before it, and
   writes it later to hold a class (`class A ... end; A = Foo`), keeps the
   answer it had before the record: it reopens the class the write holds,
   as CRuby's header would not, and every read of A then answers for that
   class. The write the walk has not reached yet, by the source's writes
   (elab_note_const_writes): the first of the leaf whose lexical scope is
   owner's body, the top level for Object (main); -1 for none. */
static int lr_later_alias(Compiler *c, int owner, const char *name) {
  LoadRecord *r = c->lrec;
  const NodeTable *nt = c->nt;
  const int *par = NULL;
  for (int i = 0; i < r->nsrcw; i++) {
    int w = r->srcw[i].node, in = LR_MAIN;
    if (!sp_streq(r->srcw[i].name, name) || nt_kind(nt, w) != NK_ConstantWriteNode || !sp_streq(lr_written_name(nt, w), name)) continue;
    if (!par) par = comp_parent_map(c);
    for (int a = par[w]; a >= 0; a = par[a]) {
      NodeKind k = nt_kind(nt, a);
      if (k == NK_SingletonClassNode) { in = LR_UNKNOWN_CLASS; break; }
      if (k == NK_ClassNode || k == NK_ModuleNode) { in = elab_header_class(c, a, NULL, NULL); break; }
    }
    if (in == owner) return w;
  }
  return -1;
}

/* As the scope walk reaches class or module header `node`, which stands
   at *pos, before registration opens its class: its entity, (owner, leaf).
   `class X` names X in the class whose body is its lexical scope (a
   class_eval or class builder's block keeps the scope it is written in, as
   lr_cref reads it), `class A::B` B in the class A names there, `class ::B`
   B in Object. Then what the owner itself held of the leaf before the
   header: a header's class, which this one reopens; a write, an alias; or
   nothing, a new class. Where the record cannot tell (a write that may
   not have run, one in another region, a header at no point), the last
   write that may have run before it. Returns the write the header's name
   aliases (a ConstantWriteNode), -1 for none. */
int elab_header_entity(Compiler *c, int node, const ElabPos *pos) {
  LoadRecord *r = c->lrec;
  const NodeTable *nt = c->nt;
  int cp = nt_ref(nt, node, "constant_path");
  if (!r || !r->walk || pos->e < 0 || cp < 0) return -1;
  /* the constant path is the header's own: placed now, as the walk places
     it next, for the lookup of its parent */
  lr_fit(r, nt->count);
  ElabPos q = elab_child(c, node, cp, pos);
  for (int x = cp; x >= 0 && (nt_kind(nt, x) == NK_ConstantPathNode || nt_kind(nt, x) == NK_ConstantReadNode);
       x = nt_kind(nt, x) == NK_ConstantPathNode ? nt_ref(nt, x, "parent") : -1)
    lr_place(r, x, &q);
  LrHeader *h = lr_header(r, node, 1);
  NodeKind k = nt_kind(nt, cp);
  int par = k == NK_ConstantPathNode ? nt_ref(nt, cp, "parent") : -1;
  free(h->leaf);
  if (!(h->leaf = strdup(lr_written_name(nt, cp)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  h->owner = k == NK_ConstantReadNode ? lr_cref(c, r->at[node]) : par < 0 ? LR_MAIN : lr_class_ref_at(c, par, NULL);
  if (h->owner < LR_MAIN) h->owner = LR_UNKNOWN_CLASS;
  const LrDef *d = NULL;
  h->verdict = elab_const_own_at(c, h->owner, h->leaf, elab_point_of(c, node), &d);
  h->found = d ? d->node : -1;
  h->want = d ? lr_class_of(c, d) : h->verdict == ELAB_NO ? LR_NEW : LR_UNKNOWN_CLASS;
  /* Object's constant of a builtin's name the program has not written yet
     is the builtin's: the header reopens it, at the top level or in
     `class Object` */
  if (h->want == LR_NEW && (h->owner == LR_MAIN || h->owner == comp_core_class(c, CORE_OBJECT)) &&
      (is_builtin_class_name(h->leaf) || is_builtin_module_name(h->leaf)))
    h->want = LR_BUILTIN;
  if (h->verdict == ELAB_NO && (h->found = lr_later_alias(c, h->owner, h->leaf)) >= 0) h->want = LR_LATER_ALIAS;
  return h->found >= 0 && nt_kind(nt, h->found) == NK_ConstantWriteNode ? h->found : -1;
}

static const char *lr_class_name(Compiler *c, int cls) {
  return cls == LR_MAIN ? "main" : cls == LR_NEW ? "new" : cls >= 0 && cls < c->nclasses ? c->classes[cls].name : "?";
}

/* ...and the class registration opened there (made: a new one), which
   the map answers from here on. Registration picks it by qualify's
   spelling of the name; check mode reports a header whose entity names
   another class: a reopen where CRuby makes a class, a new class where
   CRuby reopens one, or another class than the one CRuby reopens. A
   header whose class the record does not know (an alias of a value it
   cannot follow, an owner it does not know) is counted apart. */
void elab_header_opened(Compiler *c, int node, int cls, int made) {
  LoadRecord *r = lr_get(c);
  LrHeader *h = lr_header(r, node, 1);
  h->cls = cls >= 0 ? cls : LR_UNKNOWN_CLASS;
  int core = cls >= 0 ? builtin_core_class_slot(c->classes[cls].name) : -1;
  if (core >= 0) r->core[core] = cls + 1;
  if (!r->check || !r->walk || !h->leaf) return;
  NodeKind fk = h->found >= 0 ? nt_kind(c->nt, h->found) : NK_NilNode;
  if (h->want == LR_LATER_ALIAS) { lr_family(r, "class header, alias written after it (until N2b)")->asked++; return; }
  if (h->want < 0 && h->want != LR_NEW && h->want != LR_BUILTIN) {
    lr_family(r, h->found >= 0 && fk != NK_ClassNode && fk != NK_ModuleNode ? "class header, alias" : "class header, not known")->asked++;
    return;
  }
  LrCheck *f = lr_family(r, "class header entity");
  f->asked++;
  const char *kind = h->want == LR_BUILTIN ? NULL : h->want == LR_NEW && !made ? "reopen-not-new" : h->want >= 0 && made ? "new-not-reopen" :
                     h->want >= 0 && h->cls != h->want ? "other-class" : NULL;
  if (!kind) return;
  f->differ++;
  if (r->shown++ >= LR_CHECK_SHOW) return;
  char d[96];
  lr_node_desc(c, node, d, sizeof d);
  fprintf(stderr, "spinel-elab-check: header node=%s kind=%s class=%s entity=%s::%s want=%s\n", d, kind,
          lr_class_name(c, h->cls), lr_class_name(c, h->owner), h->leaf, lr_class_name(c, h->want));
}

/* The class the class or module header at node opens, as registration
   holds it under cname (qualify's spelling): a new one, made here, whose
   body encl's encloses -- or, for `class A::B`, the class A names
   wherever the header stands, and for `class ::B` none -- else the class
   of that name it reopens, whose definition becomes this opening when it
   is the first to declare a superclass. The node -> class map answers it
   from here on (elab_header_opened); a header with no name opens none,
   and its body stays encl's. */
int elab_rec_open(Compiler *c, int node, int cp, const char *cname, int encl) {
  if (!cname) { elab_header_opened(c, node, -1, 0); return encl; }
  int ci = comp_class_index(c, cname), made = ci < 0;
  if (made) {
    comp_class_new(c, cname, node);
    ci = c->nclasses - 1;
    c->classes[ci].enclosing_class = encl;
    /* a name qualify_colliding_classes gave carries the Ruby path it
       resolved the header to (`AP = P; class AP::Foo` is P::Foo) */
    const char *qrn = cp >= 0 ? nt_str(c->nt, cp, "qc_ruby_name") : NULL;
    if (qrn) c->classes[ci].ruby_name_cache = strdup(qrn);
    /* `class A::B`: A encloses B wherever it is written. The leaf is the
       header's as written: a `__` in it is the program's own, not the
       nesting mark the walk strips */
    int path = cp >= 0 && (nt_kind(c->nt, cp) == NK_ConstantPathNode || nt_kind(c->nt, cp) == NK_ConstantPathTargetNode);
    int par = path ? nt_ref(c->nt, cp, "parent") : -1;
    if (par >= 0 && (nt_kind(c->nt, par) == NK_ConstantReadNode || nt_kind(c->nt, par) == NK_ConstantPathNode)) {
      header_parent_encloses(c, par, ci, cname, qrn != NULL);
      /* a path builder's leaf the constant pass qualified (`A::B` made
         `A__B`) is found by its Ruby path, as a header's the class pass
         qualified is (qc_ruby_name) */
      if (!qrn && nt_str(c->nt, cp, "qc_orig_name") && class_def_builder(c->nt, node) >= 0)
        c->classes[ci].ruby_name_cache = strdup(class_ruby_name(c, ci));
    }
    /* `class ::B` (`::B = Class.new do ... end`) names B at the top level
       wherever it is written: no module encloses it, and its name is B */
    else if (par < 0 && path)
      c->classes[ci].enclosing_class = -1;
  }
  /* A class can be opened bare first (no superclass -- e.g. just to hold a
     nested class) and reopened later with `< Super`. The parent link is read
     from def_node's "superclass" ref, so prefer the opening that declares one;
     otherwise the superclass is lost and a subclass's overrides aren't
     dispatched against the right ancestor chain (matz/spinel#1477). */
  else if (class_def_super(c->nt, node) >= 0 && c->classes[ci].def_node >= 0 &&
           class_def_super(c->nt, c->classes[ci].def_node) < 0)
    comp_class_set_def(c, ci, node);
  elab_header_opened(c, node, ci, made);
  return ci;
}

/* What a class builder's call makes (EB_*), noted where the builders are
   lowered, ahead of the walk (desugar_class_new_blocks): the node -> class
   map holds it beside the headers, for the walk and the call plan to read.
   A builder whose class the program cannot have (one built from a
   superclass only the run time holds) is noted at its call and at the
   constant write holding it. A copy of such a node (a clone: a module's
   method copied into each class that includes it) answers as the node it
   copies (nt_origin), along a chain of up to 64 copies. */
void elab_builder_note(Compiler *c, int node, int kind) {
  lr_header(lr_get(c), node, 1)->builder = (unsigned char)kind;
}
int elab_builder_kind(const Compiler *c, int node) {
  for (int depth = 0; c->lrec && node >= 0 && depth < 64; depth++) {
    const LrHeader *h = lr_header(c->lrec, node, 0);
    if (h) return h->builder;
    char how = 0; int a = 0, k = 0; unsigned pass = 0;
    int o = nt_origin(c->nt, node, &how, &a, &pass, &k);
    if (o < 0 || how != 'c') break;
    node = o;
  }
  return EB_NONE;
}

/* Header map entry h now keys `node`: moved to that node's bucket */
static void lr_header_rekey(LoadRecord *r, LrHeader *h, int node) {
  int i = (int)(h - r->hdr), *p = &r->hdr_head[(unsigned)h->node % LR_BUCKETS];
  while (*p != i) p = &r->hdr[*p].next;
  *p = h->next;
  h->node = node;
  h->next = r->hdr_head[(unsigned)node % LR_BUCKETS];
  r->hdr_head[(unsigned)node % LR_BUCKETS] = i;
}

/* Exchange the contents of nodes a and b (nt_swap_nodes), and with them
   what the record keeps by node: a header's or a builder's entry in the
   node -> class map, a source constant write, a def's method scope. A pass
   that numbers a new node ahead of an existing one swaps through here, so
   a fact noted of the existing node follows it to its new number. */
void elab_swap_nodes(Compiler *c, int a, int b) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (a < 0 || b < 0 || a >= nt->count || b >= nt->count || a == b) return;
  nt_swap_nodes(nt, a, b);
  LoadRecord *r = c->lrec;
  if (!r) return;
  LrHeader *ha = lr_header(r, a, 0), *hb = lr_header(r, b, 0);
  if (ha) lr_header_rekey(r, ha, b);
  if (hb) lr_header_rekey(r, hb, a);
  for (int i = 0; i < r->nsrcw; i++)
    r->srcw[i].node = r->srcw[i].node == a ? b : r->srcw[i].node == b ? a : r->srcw[i].node;
  int sa = lr_def_scope(r, a), sb = lr_def_scope(r, b);
  if (sa || sb) { elab_def_scope(c, a, sb); elab_def_scope(c, b, sa); }
}

int elab_const_at(Compiler *c, int node, const char *name, LoadPoint p, const LrDef **out) {
  if (out) *out = NULL;
  const char *nm = node >= 0 ? nt_str(c->nt, node, "name") : NULL;
  if (name && nm && sp_streq(name, nm)) name = lr_written_name(c->nt, node);
  if (p.region < 0) p = elab_point_of(c, node);
  if (!c->lrec || !lr_ready(c, p)) return ELAB_UNKNOWN;
  return lr_root_read(c->nt, node) ? lr_const_in(c, LR_MAIN, name, p, p.region < 0, 1, out, 0, LR_KEY_CONST)
                                 : lr_const_at(c, node, name, p, out, LR_KEY_CONST);
}

/* The constant name a node that writes a constant writes: a write of any
   form, a multiple assignment's target, a class or module header; NULL
   for any other node */
static const char *lr_const_written(const NodeTable *nt, int id) {
  switch (nt_kind(nt, id)) {
    case NK_ConstantWriteNode: case NK_ConstantOrWriteNode: case NK_ConstantAndWriteNode:
    case NK_ConstantOperatorWriteNode: case NK_ConstantTargetNode: case NK_ConstantPathTargetNode:
      return lr_written_name(nt, id);
    case NK_ConstantPathWriteNode: case NK_ConstantPathOrWriteNode: case NK_ConstantPathAndWriteNode:
    case NK_ConstantPathOperatorWriteNode: {
      int t = nt_ref(nt, id, "target");
      return t >= 0 ? lr_written_name(nt, t) : NULL;
    }
    case NK_ClassNode: case NK_ModuleNode: {
      int cp = nt_ref(nt, id, "constant_path");
      return cp >= 0 ? lr_written_name(nt, cp) : NULL;
    }
    default: return NULL;
  }
}

/* The node kinds that write a constant (lr_const_written) */
static const NodeKind *lr_const_write_kinds(int *n) {
  static const NodeKind kinds[] = {
    NK_ConstantWriteNode, NK_ConstantOrWriteNode, NK_ConstantAndWriteNode, NK_ConstantOperatorWriteNode,
    NK_ConstantTargetNode, NK_ConstantPathTargetNode, NK_ConstantPathWriteNode, NK_ConstantPathOrWriteNode,
    NK_ConstantPathAndWriteNode, NK_ConstantPathOperatorWriteNode, NK_ClassNode, NK_ModuleNode
  };
  *n = (int)(sizeof kinds / sizeof kinds[0]);
  return kinds;
}

/* The constant writes the program's source has, by node and name, taken
   before any pass runs: a pass before the walk may drop one or rewrite it
   into something else (desugar_constant_path_self_alias drops
   `A::B::C = remove_const(:C)`), and the record, which sees only the tree
   the walk reaches, must then not take the name for one nothing writes
   (lr_const_seen). */
void elab_note_const_writes(Compiler *c) {
  LoadRecord *r = lr_get(c);
  const NodeTable *nt = c->nt;
  int nk = 0, cap = 0;
  const NodeKind *kinds = lr_const_write_kinds(&nk);
  for (int k = 0; k < nk; k++) {
    int n = 0;
    const int *v = nt_nodes_of_kind(nt, kinds[k], &n);
    for (int i = 0; i < n; i++) {
      const char *nm = lr_const_written(nt, v[i]);
      if (!nm) continue;
      if (r->nsrcw == cap) {
        cap = cap ? cap * 2 : 64;
        r->srcw = realloc(r->srcw, sizeof *r->srcw * (size_t)cap);
        if (!r->srcw) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      }
      r->srcw[r->nsrcw].node = v[i];
      r->srcw[r->nsrcw].name = strdup(nm);
      if (!r->srcw[r->nsrcw].name) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      r->nsrcw++;
    }
  }
}

/* Does the record see every write of constant `name` there can be? Not
   when the program runs code it cannot follow (elab_runs_unseen), loads a
   C extension, or calls const_set, autoload or an eval of a String (also
   through a send), or defines const_missing; nor when a write the
   program's source has is gone or rewritten by a pass before the walk
   (elab_note_const_writes); nor when a write of the name is one it does
   not record: a path or operator write, a multiple assignment's target,
   or a write or header in a method body, a lambda or a block, which may
   run before any point, or in a file region 0 has not entered (yet, while
   the walk records: a file a later load enters, or one in a region of its
   own, whose points the record cannot order). */
static int lr_const_seen(Compiler *c, const char *name) {
  LoadRecord *r = c->lrec;
  const NodeTable *nt = c->nt;
  if (!r->consts) {
    int hit = elab_runs_unseen(c) || nt->has_cext_decls, n = 0;
    const int *v = nt_nodes_of_kind(nt, NK_CallNode, &n);
    for (int i = 0; !hit && i < n; i++) {
      const char *m = nt_str(nt, v[i], "name");
      int a = nt_ref(nt, v[i], "arguments"), an = 0;
      const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
      if (m && is_send_family(m)) m = an > 0 ? vis_arg_name(nt, av[0]) : NULL;
      hit = !m || is_const_set_name(m) || is_autoload_name(m) || (is_code_eval_name(m) && an > 0);
    }
    v = nt_nodes_of_kind(nt, NK_DefNode, &n);
    for (int i = 0; !hit && i < n; i++) hit = is_const_missing_name(nt_str(nt, v[i], "name"));
    r->consts = hit ? 2 : 1;
  }
  if (r->consts == 2) return 0;
  for (int i = 0; i < r->nsrcw; i++) {
    const char *now = sp_streq(r->srcw[i].name, name) ? lr_const_written(nt, r->srcw[i].node) : name;
    if (!now || !sp_streq(now, name)) return 0;
  }
  const int *par = comp_parent_map(c);
  int nk = 0;
  const NodeKind *kinds = lr_const_write_kinds(&nk);
  for (int k = 0; k < nk; k++) {
    int n = 0;
    const int *v = nt_nodes_of_kind(nt, kinds[k], &n);
    for (int i = 0; i < n; i++) {
      if (!sp_streq(lr_const_written(nt, v[i]), name)) continue;
      if (kinds[k] != NK_ConstantWriteNode && kinds[k] != NK_ConstantOrWriteNode &&
          kinds[k] != NK_ClassNode && kinds[k] != NK_ModuleNode) return 0;
      int top = v[i];
      for (int a = par[v[i]]; a >= 0; top = a, a = par[a]) {
        NodeKind ak = nt_kind(nt, a);
        if (ak == NK_DefNode || ak == NK_LambdaNode || ak == NK_BlockNode) return 0;
      }
      int e = top < r->n ? r->at[top] : -1;   /* the file's root's statement */
      if (e < 0 || r->st[e].region != 0) return 0;
    }
  }
  return 1;
}

/* May the constants of class cls (LR_MAIN: Object's) or of an ancestor
   include a builtin's own (a builtin class or module that has constants:
   File's Stat, Process's Status), which the record does not hold? */
static int lr_builtin_consts(Compiler *c, int cls) {
  for (int k = cls, g = 0; g < 64; g++) {
    if (k == LR_MAIN) return 0;
    if (k < 0 || k >= c->nclasses) return 1;
    const char *nm = c->classes[k].name;
    if ((is_builtin_class_name(nm) || is_builtin_module_name(nm) || is_builtin_exception_name(nm) ||
         is_toplevel_const_name(nm)) && !is_constless_builtin_name(nm)) return 1;
    /* a superclass the record does not know: a builtin without constants
       of its own (StandardError) adds none, any other may */
    LrClass *kc = lr_class(c->lrec, k);
    if (kc && kc->has_sup && lr_later(c, &kc->sup, kc->sup_ref) < 0) {
      NodeKind sk = nt_kind(c->nt, kc->sup_ref);
      return !((sk == NK_ConstantReadNode || sk == NK_ConstantPathNode) &&
               is_constless_builtin_name(lr_written_name(c->nt, kc->sup_ref)));
    }
    k = lr_super(c, k);
    if (k == LR_UNKNOWN_CLASS) return 0;
  }
  return 1;
}

/* What constant reference ref names, read where statement node `site`
   stands (its point and lexical scopes): ELAB_YES with *cls its class
   (LR_UNKNOWN_CLASS when it holds none the record knows), ELAB_NO, or
   ELAB_UNKNOWN; on ELAB_NO, *miss is the name that names nothing (ref's
   or a parent's) and *owner the class it was looked up in. */
static int lr_ref_at(Compiler *c, int ref, int site, int *cls, const char **miss, int *owner) {
  const NodeTable *nt = c->nt;
  LoadRecord *r = c->lrec;
  NodeKind k = ref >= 0 ? nt_kind(nt, ref) : NK_NilNode;
  const char *nm = k == NK_ConstantReadNode || k == NK_ConstantPathNode ? lr_written_name(nt, ref) : NULL;
  LoadPoint p = elab_point_of(c, site);
  int e = site >= 0 && site < r->n ? r->at[site] : -1, in = LR_MAIN, o, scoped = 0;
  const LrDef *d = NULL;
  *cls = LR_UNKNOWN_CLASS;
  if (!nm || e < 0) return ELAB_UNKNOWN;
  if (k == NK_ConstantReadNode && !lr_root_read(nt, ref)) {
    /* a lexical scope that is a builtin's reopening holds its constants */
    for (int g = r->st[e].frame; g >= 0; g = r->st[g].frame) {
      NodeKind gk = nt_kind(nt, r->st[g].node);
      if (gk != NK_ClassNode && gk != NK_ModuleNode) continue;
      int ci = elab_header_class(c, r->st[g].node, NULL, NULL);
      if (in == LR_MAIN) in = ci;
      if (ci < 0 || lr_builtin_consts(c, ci)) return ELAB_UNKNOWN;
    }
    o = lr_const_at(c, site, nm, p, &d, LR_KEY_CONST);
  }
  else {
    int par = k == NK_ConstantPathNode ? nt_ref(nt, ref, "parent") : -1;
    if (par >= 0) {
      o = lr_ref_at(c, par, site, &in, miss, owner);
      if (o != ELAB_YES) return o;
      if (in < 0 || lr_builtin_consts(c, in)) return ELAB_UNKNOWN;
      scoped = 1;
    }
    o = lr_const_in(c, in, nm, p, p.region < 0, !scoped, &d, 0, LR_KEY_CONST);
  }
  if (o == ELAB_YES) { *cls = lr_class_of(c, d); return o; }
  if (o != ELAB_NO) return o;
  /* a builtin's, or a class made where the record does not see it */
  int ci = comp_class_index(c, nm);
  if (is_toplevel_const_name(nm)) {
    if (scoped) return ELAB_UNKNOWN;
    *cls = ci >= 0 ? ci : LR_UNKNOWN_CLASS;
    return ELAB_YES;
  }
  const LrClass *kc = ci >= 0 ? lr_class(r, ci) : NULL;
  if ((ci >= 0 && (!kc || !kc->nopen)) || lr_reopened(r, nm) || !lr_const_seen(c, nm)) return ELAB_UNKNOWN;
  *miss = nm; *owner = in;
  return ELAB_NO;
}

int elab_const_ref_at(Compiler *c, int ref, int site, const char **miss, int *owner) {
  int cls;
  return c->lrec && lr_ready(c, elab_point_of(c, site)) ? lr_ref_at(c, ref, site, &cls, miss, owner) : ELAB_UNKNOWN;
}

/* The superclass the first header of cls naming one names, as the record
   resolved it there; -1 when it names none or one the record does not know */
int elab_class_super(Compiler *c, int cls) {
  const LrClass *k = c->lrec && c->lrec->recorded ? lr_class(c->lrec, cls) : NULL;
  return k && k->has_sup && k->sup >= 0 ? k->sup : -1;
}

int elab_body_visibility_at(Compiler *c, int node) {
  LoadRecord *r = c->lrec;
  if (!r || !lr_ready(c, elab_point_of(c, node))) return 0;
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
  return lr_ready(c, p) ? lr_vis_at(c, cls, name, sg, p) : -1;
}

int elab_method_vis_held_from(Compiler *c, int cls, const char *name, int sg, LoadPoint p) {
  int vis = elab_method_vis_at(c, cls, name, sg, p);
  for (int k = cls, n = 0; vis >= 0 && k >= LR_MAIN && n < 64; k = lr_super(c, k), n++) {
    const LrHist *h = lr_hist(c->lrec, k, sg != 0, name, 0);
    for (int i = 0; h && i < h->n; i++) {
      const LrDef *v = &h->v[i];
      if (v->kind > LR_VIS || v->kind == LR_UNDEF || v->kind == LR_REMOVE) continue;
      /* an unplaced entry (a visibility call in a method body) runs at a
         point the record does not know */
      if (v->vis != vis && (v->unplaced || elab_cmp(elab_point_of(c, v->node), p) != PROG_BEFORE)) return -1;
    }
  }
  /* ... and a call of a program's method that may define something (its
     body may change the name's visibility, `def unhide = public(:foo)`)
     on the class at or after p. A send or an eval is passed over: in Ruby
     it can change a visibility too, but the forms that do are raised or
     refused today (`send(:public, :foo)` raises NoMethodError; class_eval,
     instance_eval and eval of a visibility word do not compile), and
     keeping the ones that name nothing of the kind (`x.send(:foo, 3)`)
     would turn the arm off for every program that sends. The record
     holds a send as opaque whatever it names (lr_opaque_call): reading a
     literal one as the call it names belongs there, not here */
  const LrHist *oh = vis >= 0 ? lr_hist(c->lrec, LR_KEY_OPAQUE, lr_const_owner(cls), "", 0) : NULL;
  for (int i = 0; oh && i < oh->n; i++) {
    const char *nm = oh->v[i].kind == LR_OPAQUE ? nt_str(c->nt, oh->v[i].node, "name") : NULL;
    if (nm && !is_send_family(nm) && !is_eval_exec_family(nm) &&
        (oh->v[i].unplaced || elab_cmp(elab_point_of(c, oh->v[i].node), p) != PROG_BEFORE))
      return -1;
  }
  return vis;
}

/* Does the registration apply entry v as the load phase ends
   (lr_stmt_applied, at v's point now) */
static int lr_applied(Compiler *c, const LrDef *v) {
  LoadRecord *r = elab_fresh(c);
  return lr_stmt_applied(lr_stmt_of(r, v->node), v->unplaced, v->may);
}

/* A history's end-of-load entries (ElabSurface); 0 when it has none */
static int lr_surface_of(Compiler *c, const LrHist *h, ElabSurface *s) {
  *s = (ElabSurface){ h->name, NULL, -1, -1, -1, 0, 0, 0, 0 };
  for (int j = 0; j < h->n; j++) {
    const LrDef *v = &h->v[j];
    int ap = v->kind > LR_VIS ? 0 : lr_applied(c, v);
    if (!ap) continue;
    s->node = v->node;
    if (v->kind != LR_UNDEF && v->kind != LR_REMOVE) {
      s->vis = (signed char)v->vis;
      if (s->first < 0) s->first = v->node;
    }
    if (ap == 2) continue;
    s->reader |= v->kind == LR_READER; s->writer |= v->kind == LR_WRITER;
    s->undef |= v->kind == LR_UNDEF; s->copy |= v->kind == LR_COPY;
    if (v->kind == LR_ALIAS) s->alias = v->target;
  }
  return s->node >= 0;
}

/* The open bit of every method history, once for all of them, as of the
   entries recorded (a later entry, or the record made again, asks for it
   anew): some version of the history after its last applied-1 version is
   applied 2 (under a run-time condition in its class body) or partial (in
   an iterator's block or a begin body with rescue clauses, at a point),
   which the registration leaves to the run time. A visibility change is no
   version. */
static void lr_open_make(Compiler *c) {
  LoadRecord *r = c->lrec;
  if (r->opened == r->nentries) return;
  for (int i = 0; i < r->nhist; i++) {
    LrHist *h = &r->hist[i];
    h->open = 0;
    if (h->a < LR_MAIN) continue;
    for (int j = h->n - 1; j >= 0 && !h->open; j--) {
      const LrDef *v = &h->v[j];
      if (v->kind > LR_REMOVE) continue;
      int ap = lr_applied(c, v);
      if (ap == 1) break;
      const LrStmt *s = lr_stmt_of(r, v->node);
      h->open = ap == 2 || (s && s->region >= 0 && s->fpartial);
    }
  }
  r->opened = r->nentries;
}

int elab_history_open(Compiler *c, int cls, int sg, const char *name) {
  LoadRecord *r = c->lrec;
  if (!r || !r->recorded) return 0;
  const LrHist *h = lr_hist(r, cls, sg != 0, name, 0);
  if (!h) return 0;
  lr_open_make(c);
  return h->open;
}

int elab_history_absent(Compiler *c, int cls, int sg, const char *name) {
  LoadRecord *r = c->lrec;
  const LrHist *h = r && r->recorded ? lr_hist(r, cls, sg != 0, name, 0) : NULL;
  if (!h) return 0;
  lr_open_make(c);
  for (int j = 0; h->open && j < h->n; j++)
    if (lr_is_version(&h->v[j]) && lr_applied(c, &h->v[j]) == 1) return 0;
  return h->open;
}

void elab_open_histories(Compiler *c, void (*fn)(void *arg, int cls, int sg, const char *name), void *arg) {
  LoadRecord *r = c->lrec;
  if (!r || !r->recorded) return;
  lr_open_make(c);
  for (int i = 0; i < r->nhist; i++) {
    const LrHist *h = &r->hist[i];
    if (h->open) fn(arg, h->a, h->b, h->name);
  }
}

const ElabSurface *elab_class_surface(Compiler *c, int cls, int sg, int *n) {
  LoadRecord *r = c->lrec;
  *n = 0;
  if (!r || !r->recorded || !lr_class(r, cls)) return NULL;
  if (!r->surf_at) {   /* every class side's at once: a count, then a fill */
    int nk = 2 * r->ncls + 1;
    ElabSurface t;
    r->surf_at = calloc((size_t)nk, sizeof *r->surf_at);
    if (!r->surf_at) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < r->nhist; i++)
      if (r->hist[i].a >= LR_MAIN && lr_class(r, r->hist[i].a) && lr_surface_of(c, &r->hist[i], &t))
        r->surf_at[2 * (r->hist[i].a + 1) + r->hist[i].b + 1]++;
    for (int k = 1; k < nk; k++) r->surf_at[k] += r->surf_at[k - 1];
    r->surf = malloc(sizeof *r->surf * (size_t)(r->surf_at[nk - 1] + 1));
    int *fill = malloc(sizeof *fill * (size_t)nk);
    if (!r->surf || !fill) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memcpy(fill, r->surf_at, sizeof *fill * (size_t)nk);
    for (int i = 0; i < r->nhist; i++)
      if (r->hist[i].a >= LR_MAIN && lr_class(r, r->hist[i].a) && lr_surface_of(c, &r->hist[i], &t))
        r->surf[fill[2 * (r->hist[i].a + 1) + r->hist[i].b]++] = t;
    free(fill);
  }
  int k = 2 * (cls + 1) + (sg != 0);
  *n = r->surf_at[k + 1] - r->surf_at[k];
  return r->surf + r->surf_at[k];
}

/* elab_entries' order: class side, statement, argument, reader first */
typedef struct { ElabEntry e; long long at; int arg; } LrEntryKey;
static int lr_entry_cmp(const void *x, const void *y) {
  const LrEntryKey *a = x, *b = y;
  if (a->e.cls != b->e.cls) return a->e.cls < b->e.cls ? -1 : 1;
  if (a->e.sg != b->e.sg) return a->e.sg < b->e.sg ? -1 : 1;
  if (a->at != b->at) return a->at < b->at ? -1 : 1;
  if (a->arg != b->arg) return a->arg < b->arg ? -1 : 1;
  return a->e.kind - b->e.kind;
}

/* The place among the names form `node` lists (a call's arguments, an
   undef's names) of the one that gives `name` (an attr writer's by its
   base name); 0 for a form of one name */
static int lr_entry_arg(const NodeTable *nt, int node, const char *name, int writer) {
  int an = nt_kind(nt, node) == NK_CallNode ? nt_ref(nt, node, "arguments") : -1, ac = 0;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : nt_kind(nt, node) == NK_UndefNode ? nt_arr(nt, node, "names", &ac) : NULL;
  size_t bl = strlen(name) - (size_t)(writer != 0);
  for (int i = 0; i < ac; i++) {
    const char *a = vis_arg_name(nt, av[i]);
    if (a && strlen(a) == bl && !strncmp(a, name, bl)) return i;
  }
  return 0;
}

ElabEntry *elab_entries(Compiler *c, unsigned mask, int *n) {
  LoadRecord *r = c->lrec;
  int k = 0, cap = 0;
  LrEntryKey *v = NULL;
  for (int i = 0; r && r->recorded && i < r->nhist; i++) {
    const LrHist *h = &r->hist[i];
    if (h->a < LR_MAIN || !lr_class(r, h->a)) continue;
    for (int j = 0; j < h->n; j++) {
      const LrDef *d = &h->v[j];
      if (!(mask >> d->kind & 1) || lr_applied(c, d) != 1) continue;
      v = lr_grow(v, &cap, k, sizeof *v);
      v[k++] = (LrEntryKey){ { h->a, h->b, d->kind, d->node, h->name, d->target }, elab_sort_key(c, d->node),
                             lr_entry_arg(c->nt, d->node, h->name, d->kind == LR_WRITER) };
    }
  }
  qsort(v, (size_t)k, sizeof *v, lr_entry_cmp);
  ElabEntry *out = malloc(sizeof *out * (size_t)(k ? k : 1));
  if (!out) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < k; i++) out[i] = v[i].e;
  free(v);
  *n = k;
  return out;
}

int elab_opaque_before(Compiler *c, int cls, LoadPoint p) {
  if (!lr_ready(c, p)) return ELAB_UNKNOWN;
  return lr_last_before(c, lr_hist(c->lrec, LR_KEY_OPAQUE, lr_const_owner(cls), "", 0), p, p.region < 0, 0, NULL);
}

/* The bodies and mixins of a class, once the record is made */
const LrOpen *elab_class_bodies(Compiler *c, int cls, int *n) {
  LrClass *k = c->lrec && c->lrec->recorded ? lr_class(c->lrec, cls) : NULL;
  *n = k ? k->nopen : 0;
  /* each body as the tree has it now: a pass after the walk may give an
     empty class its statements */
  for (int i = 0; i < *n; i++) k->open[i].body = class_def_body(c->nt, k->open[i].node);
  return k ? k->open : NULL;
}

static int lr_body_cmp(const void *a, const void *b) {
  const int *x = a, *y = b;
  return x[0] < y[0] ? -1 : x[0] > y[0];
}

ElabBody *elab_all_bodies(Compiler *c, int *n) {
  LoadRecord *r = elab_fresh(c);
  int tot = 0, k = 0, nr = 0;
  for (int ci = 0; ci < c->nclasses; ci++) { int no = 0; elab_class_bodies(c, ci, &no); tot += no + 1; }
  ElabBody *v = malloc(sizeof *v * (size_t)(tot > 0 ? tot : 1));
  int (*rest)[3] = malloc(sizeof *rest * (size_t)(tot > 0 ? tot : 1));   /* (seq, class, body) */
  if (!v || !rest) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int ci = 0; ci < c->nclasses; ci++) {
    int no = 0, d = -1;
    const LrOpen *o = elab_class_bodies(c, ci, &no);
    int dn = c->classes[ci].def_node, own = 0;
    for (int i = 0; i < no; i++) if (o[i].node == dn) { d = i; own = 1; }
    /* every class has its definition's body, an empty one too, also where
       the record holds no opening of it (a class a pass makes) */
    if (!own) v[k++] = (ElabBody){ ci, class_def_body(c->nt, dn) };
    for (int i = 0; i < no; i++) {
      if (i <= d) v[k++] = (ElabBody){ ci, o[i].body };
      else { rest[nr][0] = r->at[o[i].node]; rest[nr][1] = ci; rest[nr][2] = o[i].body; nr++; }
    }
  }
  qsort(rest, (size_t)nr, sizeof *rest, lr_body_cmp);
  for (int i = 0; i < nr; i++) v[k++] = (ElabBody){ rest[i][1], rest[i][2] };
  free(rest);
  *n = k;
  return v;
}

const LrMixin *elab_class_mixins(Compiler *c, int cls, int *n) {
  const LrClass *k = c->lrec && c->lrec->recorded ? lr_class(c->lrec, cls) : NULL;
  *n = k ? k->nmix : 0;
  return k ? k->mix : NULL;
}

/* See analyze.h. A builtin module's name is asked before the alias chain,
   which reads every constant write: a class's includes of Comparable or
   Enumerable are asked for on every ancestor walk. */
int elab_module_named(Compiler *c, const char *nm) {
  int id = nm ? comp_class_index(c, nm) : -1;
  if (id >= 0 || !nm) return id;
  if (builtin_mixin_module_id(nm)) return builtin_mixin_module_id(nm);
  const char *al = resolve_class_alias_chain(c, nm);
  if (!al) return -1;
  id = comp_class_index(c, al);
  return id >= 0 ? id : builtin_mixin_module_id(al) ? builtin_mixin_module_id(al) : -1;
}

int elab_defines(Compiler *c, int cls, const char *name) {
  const LrHist *h = c->lrec && c->lrec->recorded ? lr_hist(c->lrec, cls, 0, name, 0) : NULL;
  for (int i = 0; h && i < h->n; i++)
    if (h->v[i].kind == LR_DEF) return 1;
  return 0;
}

int elab_mixin_module(Compiler *c, const LrMixin *m) {
  if (m->mod >= 0) return m->mod;
  NodeKind k = m->ref >= 0 ? nt_kind(c->nt, m->ref) : NK_NilNode;
  return k == NK_ConstantReadNode || k == NK_ConstantPathNode ? elab_module_named(c, nt_str(c->nt, m->ref, "name")) : -1;
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

/* The map the walk made as it went (was, wbits: each node's point at its
   first visit, -2 for one it did not reach) against the climb's */
static void lr_check_map(Compiler *c, const int *was, const unsigned char *wbits) {
  LoadRecord *r = c->lrec;
  LrCheck *f = lr_family(r, "the walk's map");
  for (int id = 0; id < r->n; id++) {
    if (was[id] == -2) continue;
    f->asked++;
    if (was[id] == r->at[id] && wbits[id] == r->bits[id]) continue;
    f->differ++;
    if (r->shown++ >= LR_CHECK_SHOW) continue;
    char d[96];
    lr_node_desc(c, id, d, sizeof d);
    fprintf(stderr, "spinel-elab-check: map node=%s walk=#%d bits=%d\n", d, was[id], wbits[id]);
  }
}

/* The version call_target decided for call id in the call plan's frame
   (vi, found from class dc) against the one the map as it is answers (now,
   from ndc), asked while the pass that appends nodes then runs. A
   lowering that moves a call so that its version changes shows here. */
void elab_check_version(Compiler *c, int id, int vi, int dc, int now, int ndc) {
  LoadRecord *r = lr_get(c);
  LrCheck *f = lr_family(r, "the call plan's versions");
  f->asked++;
  if (vi == now && dc == ndc) return;
  f->differ++;
  if (r->shown++ >= LR_CHECK_SHOW) return;
  char d[96];
  lr_node_desc(c, id, d, sizeof d);
  fprintf(stderr, "spinel-elab-check: plan pass=%s node=%s version=%d/%d class=%d/%d (plan/now)\n",
          c->nt->pass ? c->nt->pass : "-", d, vi, now, dc, ndc);
}

/* The class elab_class_ref_at answered in its frame for reference node
   (ci) against the one the map as it is answers */
static void lr_check_class_ref(Compiler *c, int node, int ci) {
  LoadRecord *r = c->lrec;
  LrCheck *f = lr_family(r, "class references in the frame");
  int now = lr_class_ref_at(c, node, NULL);
  f->asked++;
  if (ci == now) return;
  f->differ++;
  if (r->shown++ >= LR_CHECK_SHOW) return;
  char d[96];
  lr_node_desc(c, node, d, sizeof d);
  fprintf(stderr, "spinel-elab-check: class-ref pass=%s node=%s class=%d/%d (frame/now)\n",
          c->nt->pass ? c->nt->pass : "-", d, ci, now);
}

/* The record as --dump-load-record prints it */
static char *lr_dump_text(Compiler *c) {
  FILE *f = tmpfile();
  if (!f) return NULL;
  elab_dump(c, f);
  long n = ftell(f);
  char *t = malloc((size_t)n + 1);
  if (!t) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  rewind(f);
  t[fread(t, 1, (size_t)n, f)] = 0;
  fclose(f);
  return t;
}

/* The record the walk made as it went (walked, as dumped) against the one
   made again after it */
static void lr_check_walked(Compiler *c, const char *walked) {
  LoadRecord *r = c->lrec;
  char *again = lr_dump_text(c);
  LrCheck *f = lr_family(r, "the walk's record");
  f->asked++;
  if (again && !sp_streq(walked, again)) {
    f->differ++;
    const char *a = walked, *b = again;
    while (*a && *a == *b) a++, b++;
    while (a > walked && a[-1] != '\n') a--, b--;
    if (r->shown++ < LR_CHECK_SHOW)
      fprintf(stderr, "spinel-elab-check: record walk=\"%.*s\" after=\"%.*s\"\n", (int)strcspn(a, "\n"), a, (int)strcspn(b, "\n"), b);
  }
  free(again);
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
  /* when each placed block runs, as the syntax tells (elab_block_runs),
     where it tells more than "not known" */
  for (int id = 0; id < r->n; id++) {
    int pe = nt_kind(nt, id) == NK_PostExecutionNode, b = pe ? -1 : nt_kind(nt, id) == NK_CallNode ? nt_ref(nt, id, "block") : -1;
    int runs = r->at[id] < 0 || (!pe && (b < 0 || nt_kind(nt, b) != NK_BlockNode)) ? ELAB_BLK_UNKNOWN : elab_block_runs(c, id);
    if (runs != ELAB_BLK_UNKNOWN)
      fprintf(out, "block %s line %d: %s\n", pe ? "END" : nt_str(nt, id, "name"), (int)nt_int(nt, id, "node_line", 0),
              runs == ELAB_BLK_LATER ? "later" : runs == ELAB_BLK_NOW ? "now" : "assumed");
  }
  if (!r->recorded) return;
  lr_open_make(c);
  static const char *const kinds[] = { "def", "define_method", "reader", "writer", "alias", "copy", "undef", "remove", "vis", "write", "opaque" };
  static const char *const words[] = { "", "module_function", "public", "private", "protected" };
  static const char *const mixes[] = { "include", "extend", "prepend", "singleton_prepend" };
  static const char *const vars[] = { "$", "main ", "const " };
  for (int ci = LR_MAIN; ci + 1 < r->ncls; ci++) {
    const LrClass *k = lr_class(r, ci);
    if (!k || (!k->nopen && !k->nmix)) continue;
    fprintf(out, "class %s:", ci == LR_MAIN ? "main" : c->classes[ci].name);
    for (int i = 0; i < k->nopen; i++) fprintf(out, " body#%d", elab_point_of(c, k->open[i].node).seq);
    for (int i = 0; i < k->nmix; i++)
      fprintf(out, " %s %s#%d", mixes[k->mix[i].kind], k->mix[i].mod >= 0 ? c->classes[k->mix[i].mod].name : "?",
              elab_point_of(c, k->mix[i].node).seq);
    fputc('\n', out);
  }
  /* each class entity, where a header makes it */
  for (int i = 0; i < r->nhdr; i++)
    if (r->hdr[i].want == LR_NEW)
      fprintf(out, "entity %s: owner %s leaf %s\n", lr_class_name(c, r->hdr[i].cls), lr_class_name(c, r->hdr[i].owner), r->hdr[i].leaf);
  if (r->nhist) lr_run_points(c);
  for (int i = 0; i < r->nhist; i++) {
    const LrHist *h = &r->hist[i];
    if (h->a == LR_KEY_DEFINES) continue;
    if (h->a == LR_KEY_VIS) fprintf(out, "visibility in #%d:", elab_point_of(c, h->b).seq);
    else if (h->a == LR_KEY_OPAQUE) fprintf(out, "opaque %s:", h->b >= 2 ? c->classes[h->b - 2].name : "main");
    else if (h->a == LR_KEY_RUNS) {   /* only the forms that run the program's code */
      if (h->n && h->v[h->n - 1].vis != ELAB_NO)
        fprintf(out, "form #%d runs code%s\n", elab_point_of(c, h->b).seq, h->v[h->n - 1].vis == ELAB_YES && !h->v[h->n - 1].may ? "" : " (maybe)");
      continue;
    }
    else if ((h->a == LR_KEY_CONST || h->a == LR_KEY_AUTO) && h->b < 0)
      fprintf(out, "%s #<Class:%s>::%s:", h->a == LR_KEY_AUTO ? "autoload" : "const", lr_class_name(c, -h->b - 2), h->name);
    else if (h->a == LR_KEY_AUTO)   /* a registration, keyed by its owner as a constant is */
      fprintf(out, "autoload %s%s%s:", h->b >= 2 ? c->classes[h->b - 2].name : "", h->b >= 2 ? "::" : "", h->name);
    else if (h->a < LR_MAIN) fprintf(out, "%s%s%s%s:", vars[-2 - h->a], h->b >= 2 ? c->classes[h->b - 2].name : "",
                                h->b >= 2 ? "::" : h->b == 0 && h->a == -2 - (int)ELAB_CONST ? "?::" : "", h->name);
    else fprintf(out, "%s%s%s:", h->a == LR_MAIN ? "main" : c->classes[h->a].name, h->b ? "." : "#", h->name);
    for (int j = 0; j < h->n; j++) {
      const LrDef *v = &h->v[j];
      fprintf(out, " %s#%d%s%s%s", kinds[v->kind], elab_point_of(c, v->node).seq, v->may ? " may" : "", v->nil ? " nil" : "",
              v->unplaced ? " unplaced" : "");
      if (v->kind == LR_DEF && h->a >= LR_MAIN && r->nregion == 1 && v->node < r->n && elab_method_versions(c, h->name) > 1)   /* lr_rta */
        r->urun[v->node] == LR_NEVER ? fputs(" runs=never", out) : fprintf(out, " runs=#%d", r->urun[v->node]);
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
      fprintf(out, " end=%s", (const char *[]){ "no", "yes", "unknown" }[elab_method_at(c, h->a, h->name, h->b, (LoadPoint){ -1, -1, -1 }, NULL)]);
    if (h->open) fputs(" open", out);
    fputc('\n', out);
  }
  /* a history with two versions or more: each one's interval, up to the
     next version or the region's end, and the versions its own name
     reaches over it (elab_method_span; idle: no statement in it may run
     code of the program's); the versions of the name in every history;
     whether the program names it */
  for (int i = 0; i < r->nhist; i++) {
    const LrHist *h = &r->hist[i];
    int nv = 0;
    for (int j = 0; h->a >= LR_MAIN && j < h->n; j++) nv += lr_is_version(&h->v[j]);
    if (nv < 2) continue;
    fprintf(out, "versions %s%s%s:", h->a == LR_MAIN ? "main" : c->classes[h->a].name, h->b ? "." : "#", h->name);
    for (int j = 0, k = 0; j < h->n; j++) {
      const LrDef *v = &h->v[j];
      if (!lr_is_version(v)) continue;
      LoadPoint p = elab_point_of(c, v->node), end;
      int e = elab_version_end(c, h->a, h->name, h->b, v->node, &end);
      fprintf(out, "%s %s#%d", k++ ? "," : "", kinds[v->kind], p.seq);
      if (e == ELAB_UNKNOWN || p.region < 0) { fputs("..?", out); continue; }
      ElabWindow w = { p, e == ELAB_YES ? end : (LoadPoint){ p.region, r->rend[p.region] }, e == ELAB_YES, 0 };
      if (e == ELAB_YES) fprintf(out, "..#%d", end.seq);
      else fputs("..end", out);
      const LrDef *d[4];
      int idle = 0, nd = lr_span(c, h->a, h->name, h->b, &w, d, 4, &idle);
      fputs(" seen=", out);
      for (int q = 0; q < nd && q < 4; q++)
        if (d[q]) fprintf(out, "%s%s#%d", q ? "|" : "", kinds[d[q]->kind], elab_point_of(c, d[q]->node).seq);
        else fprintf(out, "%snone", q ? "|" : "");
      if (nd < 0) fputs("unknown", out);
      if (nd >= 0 && idle) fputs(" idle", out);
    }
    fprintf(out, "; %d of the name%s\n", elab_method_versions(c, h->name), elab_version_captured(c, h->name) ? ", named" : "");
  }
}

/* The record's visibility of each instance method at the end of the load
   phase against the class's own table (comp_method_vis), which the
   visibility pass fills after the scope walk: asked at the end of analysis
   (analyze_program), after every pass that makes or copies scopes, which the
   mixin lookup reads. */
void elab_check_visibility(Compiler *c) {
  LoadRecord *r = c->lrec;
  if (!r || !r->recorded) return;
  for (int i = 0; i < r->nhist; i++) {
    const LrHist *h = &r->hist[i];
    if (h->a < 0 || h->b || h->a >= c->nclasses) continue;
    int rec = elab_method_vis_at(c, h->a, h->name, 0, (LoadPoint){ -1, -1, -1 }), now = comp_method_vis(&c->classes[h->a], h->name);
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
    for (int j = 0; k && j < k->nmix; j++) {
      if (k->mix[j].mod < 0 || elab_mix_on_singleton(k->mix[j].kind)) continue;
      for (int i = 0; i < r->nhist; i++) {
        const LrHist *h = &r->hist[i];
        if (h->a != k->mix[j].mod || h->b || lr_hist(r, ci, 0, h->name, 0)) continue;
        int rec = elab_method_vis_at(c, ci, h->name, 0, (LoadPoint){ -1, -1, -1 }), now = comp_method_vis_in_chain(c, ci, h->name);
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

/* The class table's entry of name in v[0..n): its kind (kinds, else 1),
   0 for none */
static int lr_table_has(char **v, const int *kinds, int n, const char *name) {
  for (int i = 0; i < n; i++) if (sp_streq(v[i], name)) return kinds ? kinds[i] : 1;
  return 0;
}

enum { LR_SF_READER, LR_SF_WRITER, LR_SF_UNDEF, LR_SF_COPY, LR_SF_ALIAS, LR_SF_VIS, LR_SF_N };

/* The fields in mask of one name on class side (cls, sg): the class
   table's (k) against the record's (s, NULL for none). A writer goes by
   its base name, and a Struct member's is the builder's, not a body's; an
   alias with another original counts 2. */
static void lr_check_name(Compiler *c, ClassInfo *k, int cls, int sg, const char *name, const ElabSurface *s, int mask) {
  const char *fam[] = { "the record's surface: readers", "the record's surface: writers", "the record's surface: undefs",
                               /* the table's are still the module_function pass's (conditional-def
                                  arms by its own rule), not filled from the record */
                               "the record's surface: module_function copies, not filled from the record yet",
                               "the record's surface: aliases",
                               "the record's surface: visibility" };
  int now[LR_SF_N] = { 0 }, rec[LR_SF_N] = { 0 }, nl = (int)strlen(name), w = nl > 1 && name[nl - 1] == '=';
  char base[256];
  snprintf(base, sizeof base, "%.*s", w ? nl - 1 : nl, name);
  if (cls >= 0) {
    now[LR_SF_READER] = !w && (sg ? comp_is_sg_reader(k, name) : comp_is_own_reader(k, name));
    now[LR_SF_WRITER] = w && (sg ? comp_is_sg_writer(k, base)
                                 : comp_is_writer(k, base) && !(k->is_struct && comp_is_reader(k, base) && !comp_is_own_reader(k, base)));
    now[LR_SF_UNDEF] = !sg && lr_table_has(k->undefs, NULL, k->nundefs, name);
    int mf = 0;   /* a module_function copy: the table keeps its instance side's privacy as the Scope's flag */
    for (int i = 0; i < c->nscopes && !mf; i++)
      mf = c->scopes[i].is_module_function && c->scopes[i].class_id == cls && sp_streq(c->scopes[i].name, name);
    now[LR_SF_COPY] = sg && mf;
    int v = sg ? lr_table_has(k->cm_vis_names, k->cm_vis_kinds, k->ncm_vis, name) : lr_table_has(k->vis_names, k->vis_kinds, k->nvis, name);
    /* ...and an initialize's by its name: a public entry in the table may be
       a public section's, which those keep */
    now[LR_SF_VIS] = v ? v : !sg && (mf || is_implicitly_private_name(name)) ? SP_VIS_PRIVATE : SP_VIS_PUBLIC;
  }
  const char *old = NULL;
  for (int i = 0; i < k->naliases; i++) if (k->alias_sg[i] == sg && sp_streq(k->alias_new[i], name)) old = k->alias_old[i];
  now[LR_SF_ALIAS] = old != NULL;
  if (s && cls >= 0) {
    rec[LR_SF_READER] = s->reader; rec[LR_SF_WRITER] = s->writer; rec[LR_SF_UNDEF] = s->undef && !sg;
    rec[LR_SF_COPY] = s->copy && sg; rec[LR_SF_VIS] = s->vis > 0 ? s->vis : 0;
  }
  if (s && s->alias) rec[LR_SF_ALIAS] = old && !sp_streq(old, s->alias) ? 2 : 1;
  LoadRecord *r = c->lrec;
  for (int j = 0; j < LR_SF_N; j++) {
    if (!(mask >> j & 1) || (!now[j] && !rec[j])) continue;
    LrCheck *f = lr_family(r, fam[j]);
    f->asked++;
    if (now[j] == rec[j]) continue;
    f->differ++;
    if (r->shown++ < LR_CHECK_SHOW)
      fprintf(stderr, "spinel-elab-check: surface %s %s%s%s now=%d record=%d\n", fam[j] + sizeof "the record's surface:",
              cls == LR_MAIN ? "main" : k->name, sg ? "." : "#", name, now[j], rec[j]);
  }
}

/* The class table's own entries, as the registration leaves them (before
   a superclass's or a module's are copied in), against the record's
   surface (elab_class_surface), on every class side the record holds:
   each field of the record's names, then the table's names the record
   has no entry of, once per field. main's aliases are the Toplevel
   class's. */
void elab_check_surface(Compiler *c) {
  LoadRecord *r = c->lrec;
  if (!r || !r->recorded) return;
  int tl = c->toplevel_class;
  for (int cls = LR_MAIN; cls < c->nclasses; cls++)
    for (int sg = 0; sg < 2 && lr_class(r, cls) && (cls >= 0 || tl >= 0); sg++) {
      ClassInfo *k = &c->classes[cls >= 0 ? cls : tl];
      int n;
      const ElabSurface *v = elab_class_surface(c, cls, sg, &n);
      for (int i = 0; i < n; i++) lr_check_name(c, k, cls, sg, v[i].name, &v[i], -1);
      char **ls[] = { sg ? k->sg_readers : k->own_readers, sg ? k->sg_writers : k->writers, k->undefs, NULL, k->alias_new,
                      sg ? k->cm_vis_names : k->vis_names };
      int ln[] = { sg ? k->nsg_readers : k->nown_readers, sg ? k->nsg_writers : k->nwriters, sg ? 0 : k->nundefs,
                   sg && cls >= 0 ? c->nscopes : 0, k->naliases, sg ? k->ncm_vis : k->nvis };
      for (int f = cls >= 0 ? 0 : LR_SF_ALIAS; f < LR_SF_N; f++)
        for (int i = 0; i < ln[f]; i++) {
          const Scope *sc = f == LR_SF_COPY ? &c->scopes[i] : NULL;
          if ((f == LR_SF_ALIAS && k->alias_sg[i] != sg) || (sc && !(sc->is_module_function && sc->class_id == cls))) continue;
          char nm[256];
          snprintf(nm, sizeof nm, "%s%s", f == LR_SF_COPY ? sc->name : ls[f][i], f == LR_SF_WRITER ? "=" : "");
          const LrHist *h = lr_hist(r, cls, sg, nm, 0);
          ElabSurface e;
          if (!h || !lr_surface_of(c, h, &e)) lr_check_name(c, k, cls, sg, nm, NULL, 1 << f);
        }
    }
}

void elab_free(Compiler *c) {
  LoadRecord *r = c->lrec;
  if (!r) return;
  if (r->builds && sp_timing_on()) {
    char extra[64];
    snprintf(extra, sizeof extra, " builds=%d", r->builds);
    sp_timing_end(sp_timing_now() - r->ms, "an_elab_points", extra);
  }
  for (int i = 0; r->check && i < LR_CHECK_MAX && r->fam[i].reader; i++)
    fprintf(stderr, "spinel-elab-check: family=\"%s\" asked=%ld differ=%ld\n", r->fam[i].reader, r->fam[i].asked, r->fam[i].differ);
  lr_record_drop(r);
  free(r->plan.st); free(r->plan.at); free(r->plan.bits); free(r->plan_par); free(r->up);
  for (int i = 0; i < r->nnames; i++) free(r->names[i]);
  for (int i = 0; i < r->nreopens; i++) free(r->reopens[i]);
  free(r->names); free(r->defs); free(r->reopens);
  for (int i = 0; i < r->nsrcw; i++) free(r->srcw[i].name);
  free(r->srcw);
  free(r->slot); free(r->slot_head); free(r->slotp);
  free(r->hist); free(r->mixed); free(r->clsmod); free(r->hhead); free(r->cls); free(r->held); free(r->vers); free(r->vers_head); free(r->set);
  for (int i = 0; i < r->nhdr; i++) free(r->hdr[i].leaf);
  free(r->hdr); free(r->hdr_head);
  free(r->st); free(r->at); free(r->bits); free(r->dscope); free(r->rend); free(r->rinst); free(r->urun); free(r);
  c->lrec = NULL;
}

/* ---- the host (#9898) ----
   Code a step splices into a body once the load stage is under way is
   handed to the host, which makes it what the program's own code is at that
   point: the static class_eval text in it is grafted (sce_span), and
   at the end of the step its statements are lowered by every row that has
   run (lower_span), then the rows a span cannot be asked of run once over
   the program (lower_settle). Under the registration's walk (now), which
   reaches spliced statements next, each splice is lowered as it is made,
   and the step ends with the walk. Nothing here rebuilds anything of the whole
   program per splice: what the graft asks of the program is asked once per
   step (HostStep), and a span is lowered by a walk of its own nodes. */

void host_begin(HostStep *h, LowerCtx *lc) {
  memset(h, 0, sizeof *h);
  h->lc = lc;
}

static int host_id_cmp(const void *a, const void *b) {
  int x = *(const int *)a, y = *(const int *)b;
  return (x > y) - (x < y);
}
/* A node of the statements ids[0..n) in a shape a step before the
   registration's walk reads, one that is not a row and that no form or
   query of the walk has taken over (the precondition rule, H3-PLAN 2.4), or
   -1: an ffi declaration (desugar_ffi_library_functions,
   desugar_native_func_defs), a mixin (desugar_included_hooks,
   desugar_explicit_receiver_mixin, desugar_extend_self,
   desugar_extend_enumerable), a class builder (desugar_class_new_blocks,
   desugar_inline_struct_blocks), const_get (desugar_literal_const_get,
   desugar_self_const_get, desugar_dynamic_const_get), a write through a
   constant path (desugar_constant_path_self_alias, qualify_colliding_consts),
   instance_eval (desugar_instance_eval_defs), a method_defined? guard
   (desugar_def_unless_method_defined), an alias (desugar_inherited_aliases,
   desugar_reader_aliases_before_redef, desugar_module_function_aliases).
   A define_method block is the program's own code, which those steps read
   where it is written: its nodes are not asked. Code spliced in the walk
   carries none. */
int host_prewalk_shape(NodeTable *nt, const int *ids, int n) {
  int nn = 0, nb = 0, ns = 0, hit = -1;
  int *all = nt_subtree_ids(nt, ids, n, &nn), *blk = malloc(sizeof(int) * (size_t)(nn + 1));
  if (!blk) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < nn; i++) {
    const char *nm = nt_str(nt, all[i], "name");
    int b = nt_ref(nt, all[i], "block");
    if (nt_kind(nt, all[i]) == NK_CallNode && b >= 0 && (is_define_method_name(nm) || is_define_singleton_method_name(nm)))
      blk[nb++] = b;
  }
  int *own = nb ? nt_subtree_ids(nt, blk, nb, &ns) : NULL;
  if (own) qsort(own, (size_t)ns, sizeof(int), host_id_cmp);
  for (int i = 0; i < nn && hit < 0; i++) {
    int id = all[i], rv = nt_ref(nt, id, "receiver");
    const char *nm = nt_str(nt, id, "name");
    if (own && bsearch(&id, own, (size_t)ns, sizeof(int), host_id_cmp)) continue;
    switch (nt_kind(nt, id)) {
    case NK_CallNode:
      if (is_ffi_decl_name(nm) || is_mixin_call(nm) || is_const_query_name(nm) || is_instance_eval_family(nm) ||
          is_alias_method_name(nm) || is_method_defined_query(nm) ||
          ((nt_kind(nt, rv) == NK_ConstantReadNode || nt_kind(nt, rv) == NK_ConstantPathNode) &&
           is_class_builder(nt_str(nt, rv, "name"), nm)))
        hit = id;
      break;
    case NK_AliasMethodNode: case NK_ConstantPathWriteNode: case NK_ConstantPathOperatorWriteNode:
    case NK_ConstantPathOrWriteNode: case NK_ConstantPathAndWriteNode: case NK_ConstantPathTargetNode:
      hit = id;
      break;
    default: break;
    }
  }
  free(all); free(blk); free(own);
  return hit;
}

/* One splice is one scope: its first statement names it (lower_span). Its
   code runs in a frame of the locals `locals` (comma-joined; NULL: not
   known) but `gone`, which its class_eval text reads. */
int host_splice(Compiler *c, HostStep *h, int owner, int body, int at, int n, const char *locals, const char *gone) {
  n = sce_span(c, h, owner, body, at, n, locals, gone);
  int bn = 0;
  const int *st = nt_arr(c->nt, body, "body", &bn);
  if (h->now && h->lc->check && at + n <= bn && host_prewalk_shape((NodeTable *)c->nt, st + at, n) >= 0)
    lower_say(h->lc, "host_splice", "a statement spliced in the walk carries a shape a step before the walk reads");
  if (h->now && at + n <= bn) {   /* the walk reaches them next: lowered now, as one span */
    int *sp = malloc(sizeof(int) * (size_t)(2 * n + 1));   /* a row may replace the body's array */
    if (!sp) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < n; i++) { sp[i] = st[at + i]; sp[n + i] = st[at]; }
    h->lc->span_now = 1;
    lower_span(c, h->lc, owner, body, sp, sp + n, n);
    free(sp);
    return n;
  }
  for (int i = at; i < at + n && i < bn; i++) {
    if (h->nspans + 4 > h->cspans) {
      h->cspans = h->cspans ? h->cspans * 2 : 64;
      h->spans = realloc(h->spans, sizeof(int) * (size_t)h->cspans);
      if (!h->spans) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    h->spans[h->nspans++] = owner;
    h->spans[h->nspans++] = body;
    h->spans[h->nspans++] = st[i];
    h->spans[h->nspans++] = st[at];
  }
  return n;
}

void host_end(Compiler *c, HostStep *h) {
  int ns = h->nspans / 4;
  int *stmts = ns ? malloc(sizeof(int) * (size_t)ns) : NULL, *scopes = ns ? malloc(sizeof(int) * (size_t)ns) : NULL;
  if (ns && (!stmts || !scopes)) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  /* the statements of one body, spliced one after another, are one span */
  for (int i = 0; i < h->nspans;) {
    int owner = h->spans[i], body = h->spans[i + 1], n = 0;
    while (i < h->nspans && h->spans[i] == owner && h->spans[i + 1] == body) {
      stmts[n] = h->spans[i + 2];
      scopes[n++] = h->spans[i + 3];
      i += 4;
    }
    lower_span(c, h->lc, owner, body, stmts, scopes, n);
  }
  free(stmts);
  free(scopes);
  lower_settle(c, h->lc);
  free(h->spans);
  sce_facts_free(h->sce);
  ct_macro_facts_free(h->ct);
  memset(h, 0, sizeof *h);
}

/* Does a builtin class the record holds no class for, named by constant node
   `node`, reach Object? A core class below Object does (ELAB_YES); BasicObject
   and a builtin module do not; any other name is unknown. */
static int lr_named_reach(const NodeTable *nt, int node) {
  const char *sn = nt_kind_is_constant(nt_kind(nt, node)) ? lr_written_name(nt, node) : NULL;
  if (sn && (builtin_core_class_slot(sn) == CORE_BASIC_OBJECT || is_builtin_module_name(sn))) return ELAB_NO;
  return sn && (is_builtin_class_name(sn) || is_builtin_exception_name(sn)) ? ELAB_YES : ELAB_UNKNOWN;
}

/* Does the ancestry of class cls reach Object? A read scoped by such a class
   also looks in Object's constants, so it runs an autoload registered there
   before it raises NameError. ELAB_NO for a module and a BasicObject-rooted
   class, ELAB_UNKNOWN for a chain the record does not know. */
static int lr_reaches_object(Compiler *c, int cls) {
  for (int a = cls, g = 0; g < 64; g++) {
    if (a == LR_MAIN) return ELAB_YES;
    if (a < 0 || a >= c->nclasses) return ELAB_UNKNOWN;
    int dn = c->classes[a].def_node;
    if (a == comp_core_class(c, CORE_BASIC_OBJECT) ||
        (dn >= 0 ? class_def_is_module(c->nt, dn) : is_builtin_module_name(c->classes[a].name))) return ELAB_NO;
    if (a == cls && c->classes[a].is_builtin_const) return ELAB_UNKNOWN;   /* (a path to a builtin class reads a top-level class of the leaf's name) */
    LrClass *k = lr_class(c->lrec, a);
    if (k && k->has_sup && lr_later(c, &k->sup, k->sup_ref) < 0) return lr_named_reach(c->nt, k->sup_ref);   /* a builtin superclass has no class here */
    a = lr_super(c, a);
  }
  return ELAB_UNKNOWN;
}

/* The main-scope registration a read of `name` scoped by class owner reaches
   through owner's ancestry; *open when owner's chain is not known. */
static int lr_autoload_via_object(Compiler *c, int owner, const char *name, LoadPoint at, const LrDef **d, int *open) {
  int reach = owner == LR_MAIN ? ELAB_NO : lr_reaches_object(c, owner);
  if (reach == ELAB_NO) return ELAB_NO;
  int found = lr_const_in(c, LR_MAIN, name, at, at.region < 0, 1, d, 0, LR_KEY_AUTO);
  if (reach == ELAB_UNKNOWN && found != ELAB_NO) *open = 1;
  return reach == ELAB_YES ? found : ELAB_NO;
}

/* Is version d an autoload registration (not a constant's write)? */
static int lr_registration(const Compiler *c, const LrDef *d) {
  int n = 0;
  if (d) comp_autoloads(c, d->node, &n);
  return n > 0;
}

/* The registered file a read may trigger. Presence of the file's ordinary
   constant write proves that no autoload call is left to run here. */
int elab_autoload_read(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (!c->nautoloads) return 0;
  NodeKind k = nt_kind(nt, id);
  if (k != NK_ConstantReadNode && k != NK_ConstantPathNode) return 0;
  const char *name = lr_written_name(nt, id);
  LoadPoint p = elab_point_from(c, id), at = elab_point_of(c, id);
  int par = k == NK_ConstantPathNode ? nt_ref(nt, id, "parent") : -1;
  int lexical = k == NK_ConstantReadNode && !lr_root_read(nt, id), owner = LR_MAIN, found;
  if (!lexical && par >= 0 && (owner = lr_class_ref_at(c, par, NULL)) < LR_MAIN) return 0;
  /* a constant of the name proves no file is left to run, unless a
     registration comes first on the lookup's way (an inner scope's, or the
     scope's own ahead of an ancestor's constant) */
  const LrDef *d = NULL, *e = NULL;
  int first = (lexical ? lr_const_at(c, id, name, at, &e, LR_KEY_EITHER)
                       : lr_const_in(c, owner, name, at, at.region < 0, 0, &e, 0, LR_KEY_EITHER)) != ELAB_NO &&
              lr_registration(c, e);
  if (!first && elab_const_slot_present_at(c, nt_str(nt, id, "name"), p) == ELAB_YES) return 0;
  if (lexical) {
    if (!first && lr_const_at(c, id, name, p, &d, LR_KEY_CONST) == ELAB_YES) return 0;
    d = NULL;
    found = lr_const_at(c, id, name, at, &d, LR_KEY_AUTO);
  }
  else {
    if (!first && lr_const_in(c, owner, name, p, p.region < 0, 0, &d, 0, LR_KEY_CONST) == ELAB_YES) return 0;
    d = NULL;
    found = lr_const_in(c, owner, name, at, at.region < 0, 0, &d, 0, LR_KEY_AUTO);
    if (found == ELAB_NO) found = lr_autoload_via_object(c, owner, name, at, &d, &(int){ 0 });
  }
  if (found == ELAB_NO || !d) return 0;
  const LrStmt *s = p.seq >= 0 ? &c->lrec->st[p.seq] : NULL;
  if (s && nt_ref(nt, s->node, "constant_path") == id) {
    int n; const AutoloadRegistration *a = comp_autoloads(c, d->node, &n);
    for (int k = 0; k < n; k++) if (a[k].file == s->file) return 0;
  }
  return d->scope;
}

/* Is a scoped read's registration undecidable: its scope's superclass chain
   not being known, or the scope a builtin class? CRuby looks such a read up
   in Object and runs the file before the NameError; a path to a builtin class
   with a top-level class of the leaf's name reads that class instead (also
   without an autoload), so the program layer refuses it as master does. */
int elab_autoload_scope_open(Compiler *c, int id) {
  int par = nt_ref(c->nt, id, "parent"), open = 0;
  if (!c->nautoloads || par < 0) return 0;
  int owner = lr_class_ref_at(c, par, NULL);
  LoadPoint at = elab_point_of(c, id);
  const LrDef *d = NULL;
  if (owner >= LR_MAIN) lr_autoload_via_object(c, owner, lr_written_name(c->nt, id), at, &d, &open);
  else if (lr_named_reach(c->nt, par) == ELAB_YES)
    open = lr_const_in(c, LR_MAIN, lr_written_name(c->nt, id), at, at.region < 0, 1, &d, 0, LR_KEY_AUTO) != ELAB_NO;
  return open;
}

/* Does the file of the registration in slot write its constant, under its
   owner, whenever the file runs? A write under a condition, in another file
   or under another owner does not count. */
int elab_autoload_file_defines(Compiler *c, int slot, int file) {
  LoadRecord *r = c->lrec;
  if (!r || slot < 1 || slot > r->nhist) return 0;
  const LrHist *a = &r->hist[slot - 1], *h = lr_hist(r, LR_KEY_CONST, a->b, a->name, 0);
  for (int i = 0; h && i < h->n; i++) {
    int e = h->v[i].node < r->n ? r->at[h->v[i].node] : -1;
    if (e >= 0 && r->st[e].file == file && !h->v[i].may) return 1;
  }
  return 0;
}
