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
#include "call_plan.h"
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

/* A method the walk has recorded a version of that CRuby tells its class
   (sg: its singleton's) the hook of: recorded as the statement it stands in
   is (lr_method), consumed as the scope walk leaves that statement
   (elab_fire_after). kind is the version's LrKind, vis its visibility, node
   the form that made it (a def, a call), name the method's. A mixin's
   event (mix: 1 + its LrMixKind) is the module's: cls the includer, mod the
   module, arg the mixin's argument naming it, name the hook's. */
typedef struct { int stmt, cls, node, mod, arg; unsigned char sg, kind, vis, mix; char *name; } LrEvent;

/* A def of the program's, by its name (LoadRecord.defs) */
typedef struct { const char *name; int node; } LrSite;
typedef struct { const char *name; int node, owner; } LrCvarWrite;   /* a class variable written in owner's body */

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
     in while call_target asks its questions, with the parents it climbed and
     the run points made of it (lr_run_points) */
  struct LrPlan { LrStmt *st; int nst, cst, *at; unsigned char *bits; int n, ncap, runs_of, nrun, *rend, *rinst, *urun, *rentry; } plan;
  int *up;                   /* [n] the parent each node's point was climbed from */
  int *plan_par, plan_depth;
  unsigned char plan_made;
  unsigned char hold;        /* elab_hold: 1 asked for, 2 the map is made and kept as it is */
  /* the histories (elab_program) */
  int recorded;
  int frontier;              /* the statements recorded so far, in seq order: while the walk records, a query at p is complete for p.seq <= frontier (lr_ready) */
  struct ElabRec *rec;       /* the record's state while elab_program makes it */
  struct HostStep *host;     /* ...and the host a walk-time form splices through (elab_walk_host) */
  LrEvent *ev; int nev, cev;   /* the hook events of the statements recorded and not yet left by the scope walk */
  struct LrHist *hist; int nhist, chist, *hhead;
  /* [ncls] main's history (Object's) and each class the walk made, a
     record class's among them (elab_rec_open). One registration makes
     after the walk (an object's singleton class, which its extends and
     defs fill) has methods the record never saw, and no entry (lr_class). */
  struct LrClass *cls; int ncls;
  char **names; int nnames, cnames;     /* the names the program defines methods by, sorted (lr_names_take) */
  LrSite *defs; int ndefs;              /* ...and its defs, sorted by name */
  unsigned char callbacks;              /* lr_callbacks_define's memo: 1 + what they may do (LR_OPAQUE_*) */
  unsigned char evhooks;                /* lr_event_hooks' memo: 1 no, 2 yes */
  char **reopens; int nreopens, creopens;   /* the builtin classes and modules a header of the program reopens, sorted (lr_reopens_take) */
  char **refl; int nrefl, crefl;        /* the names a reflection reader of the source names, sorted (lr_refl_take) */
  char **refl_table[2]; int nrefl_table[2], crefl_table[2];   /* ...those a reader that answers from the table names, by the side it reports (lr_refl_sides) */
  char **refl_cell; int nrefl_cell, crefl_cell;      /* ...a respond_to? the version cells may answer */
  char **refl_capt; int nrefl_capt, crefl_capt;      /* ...a Method's maker that takes the entry, as a call */
  unsigned char refl_any;               /* ...and one names a name the record cannot read */
  unsigned char names_any;              /* ...and one it defines by a name the record cannot read */
  /* the method histories by name (lr_vers_index), as of nentries entries */
  long nentries, indexed;
  unsigned char *clsmod; int clsmod_all; struct { int nhist, ncls, recorded; unsigned ver; } clsmod_of;   /* lr_class_object_modules, and the record state it is of */
  int *mixed, nmixed; struct { int nhist, ncls, recorded; unsigned ver; } mixed_of;   /* lr_mixed */
  int *mby, *mby_at, mby_unknown; struct { int nhist, ncls, recorded; unsigned ver; } mby_of;   /* lr_mixed_by: who mixes each module in, as (class + 1) * 2 + singleton, CSR by module + 1 */
  int *rclass; struct { unsigned long hgen, nedits; unsigned shape, nver; int nclasses; } rclass_of;   /* [nclasses] by class: elab_record_class_of's answer + 2, 0 for not asked yet, and the state it is of */
  struct LrSgMaker { int node, cls; unsigned char sure; } *sgm; int nsgm, csgm;   /* lr_sg_makers, with the modules a singleton chain may hold (any of them: sgchain_any) */
  unsigned char *sgchain; int sgchain_any; struct { int recorded, nclasses; unsigned ver; } sgm_of;
  /* elab_class_unsettled's answers from the record, the tables and the
     nodes, by call, class and window start (key: the quadruples, -1 for
     none), asked again each pass, while the epochs in of hold
     (lr_unset_kept) */
  struct LrUnset { int *key; unsigned char *un; int n, cap; struct { unsigned long hgen, nedits; unsigned sgen, shape, tgen, nver; int ncls, nscopes, nclasses; } of; } unset;
  unsigned long hgen;        /* bumped by every change to the histories and the mixins, and by each scope owner the record sets */
  const LrDef **cand; int ncand, ccand, cand_bad; unsigned char collect;   /* elab_class_candidates: the writes lr_last_before passes */
  long opened;               /* LrHist.open is of this many entries (lr_open_make), -1 for none made */
  long lated;                /* LrVers.late is of this many entries (lr_late_make), -1 for none made */
  struct LrSettle *set; int nset, cset;   /* the answers recorded ahead of a later region, to recompute (lr_settle_regions) */
  unsigned char unseen;      /* elab_runs_unseen's memo: 1 no, 2 yes */
  unsigned char consts;      /* lr_const_seen's memo: 1 the program makes constants only as it writes them, 2 not */
  struct { int node; char *name; } *srcw; int nsrcw;   /* the constant writes the source has, before any pass (elab_note_const_writes) */
  /* the value constants' entities elab_const_keys keyed: each one's key,
     and by node, the entity + 1 a write is of or a read names (0: none, a
     class's, or a node made since) */
  char **ckey; int nckey; int *cent; int ncent; int ckey_gen;
  int *cwown;   /* by constant write (and its target): its class, as lr_const_owner numbers it */
  LrSlot *slot; int nslot, cslot, *slot_head, slot_builds;
  LrSlotPoint *slotp; int nslotp, cslotp; long slot_entries;
  /* lr_const_seen's and lr_const_final's facts of every written name (lr_final_index), and what they are of */
  struct LrFinal { const char *name; int writes, headers, next; unsigned char unseen, removed; } *fin; int nfin, cfin, *fin_head;
  struct { int builds, nst; long entries; const int *at; unsigned ver; unsigned long edits; } fin_of;
  unsigned char fin_calls;   /* ...1 when a remove_const or a send names no name the record reads: no name is final */
  int runs_of, nrun, *rend;  /* lr_run_points: the build its bits are of, how many statements run code, each region's end */
  int *rinst, *urun;         /* ...from which statement on an object of each class may exist, each def's body may run (lr_rta) */
  int *rentry;               /* ...and each region's load (all three statements of region 0) */
  unsigned char defs_computed;          /* ...and whether the program makes or copies a method by a computed name */
  struct LrVers *vers; int nvers, cvers, *vers_head;
  struct LrValias { int hist, j, next; } *valias; int nvalias, cvalias;   /* the aliases of each name the histories hold (LrVers.alias) */
  /* elab_version_met's readers of each name, filed by the classes their
     self may be (lr_readers), made at its first ask after a walk and kept
     with the map while it is held */
  struct LrReader { const char *name; int node, key, next; unsigned char kind, sg, copy; } *rdr; int nrdr, crdr, *rdr_head; struct { int n; unsigned ver; } rdr_of;
  int follow;                /* the aliases lr_class_of is following, to cap a chain */
  signed char *bread; int nbread;   /* [nbread] by def node: lr_def_block's reading + 3, 1 while it is read, 0 for not read yet */
  struct LrHeader *hdr; int nhdr, chdr, *hdr_head;   /* the class and module headers, in the order the walk reaches them (elab_header_class), and the class builders' calls and writes noted ahead of the walk (elab_builder_note) */
  ElabSurface *surf; int *surf_at;   /* elab_class_surface's, by class side: [2 * (cls + 1) + sg] .. [+ 1] */
  int *dscope, ndscope;      /* [ndscope] by node: the method scope the scope walk made of a def or define_method (elab_def_scope), 0 for none */
  int *ddef, nddef;          /* [nddef] by def node: a receiver-less def in an instance_eval frame, the class it defines on + 2 (LR_MAIN is -1) with bit 0 for its singleton; 0 for none (elab_def_definee) */
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
static int lr_named(Compiler *c, const char *name);

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
static const char *lr_literal_class(NodeKind k);
static int lr_lazy_later(Compiler *c, int call);
static int lr_target_runs_block(Compiler *c, int call);

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
   runs: elab_block_when with the lazy chain, and for a call the record
   binds to a method of the program's, that method's reading of its block
   (lr_target_runs_block): at once for certain when its body runs the
   block only before it answers; a body that may keep it takes a builtin
   iterator's reading back to ELAB_BLK_ASSUMED. The record places a lazy
   chain's block by its call's name (lazy 0) and an END body at its
   statement: that a block does not run now does not tell that it runs
   after a later def (the chain may be forced before it, the program may
   exit before it), so the record binds no later def to its calls; only
   a run-time check can. */
int elab_block_runs(Compiler *c, int node) {
  if (nt_kind(c->nt, node) == NK_PostExecutionNode) return ELAB_BLK_LATER;
  int runs = elab_block_when(c, node, 1);
  if (runs != ELAB_BLK_NOW && runs != ELAB_BLK_UNKNOWN) return runs;
  int t = lr_target_runs_block(c, node);
  return t > 0 ? ELAB_BLK_NOW : t < 0 && runs == ELAB_BLK_NOW ? ELAB_BLK_ASSUMED : runs;
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
  if (k == NK_CallNode || k == NK_RescueModifierNode || k == NK_LoadNode || k == NK_ParenthesesNode) q.kind = EP_SPINE;
  return q;
}

/* The writes whose value is placed where the write stands: a plain write's,
   and, with *may set, the value of `||=` and `&&=`, which may not run. */
int elab_write_value_kind(NodeKind k, int *may) {
  *may = 0;
  switch (k) {
    case NK_ConstantWriteNode: case NK_ConstantPathWriteNode: case NK_LocalVariableWriteNode:
    case NK_InstanceVariableWriteNode: case NK_GlobalVariableWriteNode: case NK_ClassVariableWriteNode:
      return 1;
    case NK_ConstantOrWriteNode: case NK_ConstantPathOrWriteNode: case NK_LocalVariableOrWriteNode:
    case NK_InstanceVariableOrWriteNode: case NK_GlobalVariableOrWriteNode: case NK_ClassVariableOrWriteNode:
    case NK_ConstantAndWriteNode: case NK_ConstantPathAndWriteNode: case NK_LocalVariableAndWriteNode:
    case NK_InstanceVariableAndWriteNode: case NK_GlobalVariableAndWriteNode: case NK_ClassVariableAndWriteNode:
      *may = 1;
      return 1;
    default: return 0;
  }
}

/* Where the load phase stands at child ch of node p, which stands at pp.
   On a statement's path: a class body, and the block of a call that runs
   it as one (class_eval and kin, a class builder), is a frame of its own,
   which the statement opened; a branch, a loop's body and a rescue clause
   may not run, and the next arm of an elsif ladder or a rescue chain is
   on the path; a branch whose condition is a constant (`x if true`,
   nt_pred_const, which the scope walk folds too) runs whenever the
   condition is reached, and the arms after one that always runs never do,
   which the record keeps as may not run; a rescue clause that runs
   whenever its begin completes (prog_rescue_runs: a require of no file in
   the body raises) runs as such a branch does; a value written under `||=` /
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
      int then_runs = k == NK_IfNode ? pc == 1 : k == NK_UnlessNode ? pc == 0 :
                      k == NK_RescueNode ? prog_rescue_runs(nt, p) == 1 : k == NK_ElseNode && pp->sure;
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
    case NK_AndNode: case NK_OrNode:   /* the right side runs only if the left decides so: a lone expression, which may not run */
      if (spine && ch == nt_ref(nt, p, "right")) { q.may = q.fmay = 1; return elab_body_pos(nt, ch, q); }
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
    default: {
      int may;
      if (spine && elab_write_value_kind(k, &may) && ch == nt_ref(nt, p, "value")) {
        if (may) q.may = q.fmay = 1;
        return elab_value_pos(nt, ch, q);
      }
      break;
    }
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
      /* (a top-level `K.include(M)` the record takes: M's hook fires after
         it, as after a body's own include, spliced next in the list) */
      if (w->reg) { walk_scope_stmt(c, v, k, sc, at); if (!seen && st >= 0) elab_fire_after(c, st, k); }
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
  if (!r->walk && !r->plan_depth && r->hold != 2 && (!r->at || r->n != c->nt->count || r->ver != c->nt->version)) elab_points_walk(c, 0);
  if (r->hold == 1 && !r->walk && !r->plan_depth) r->hold = 2;
  return r;
}

/* While held, the map is made at the first question and then kept as it
   is, whatever the table's nodes become: a pass that rewrites nodes as it
   asks (desugar_engine_branches folds a guard to true or false, which
   moves no statement; register_extends copies method bodies, which stand
   at no point) answers every question from the program as it was read,
   instead of rebuilding the map after each rewrite. A hold does not nest:
   releasing it ends it, so there is one holder at a time (the guard fold
   of desugar_engine_branches, or register_extends). */
void elab_hold(Compiler *c, int on) {
  lr_get(c)->hold = on ? 1 : 0;
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
   call's version of the map as it is too (elab_check_version). The run
   points (lr_run_points: each statement's run bits, each region's end, and
   lr_rta's first statements) are facts of one map's statement numbers, so
   they swap with it: the frame and the live map each make their own. */
static void lr_plan_swap(LoadRecord *r) {
  struct LrPlan live = { r->st, r->nst, r->cst, r->at, r->bits, r->n, r->ncap, r->runs_of, r->nrun, r->rend, r->rinst, r->urun, r->rentry };
  r->st = r->plan.st; r->nst = r->plan.nst; r->cst = r->plan.cst; r->at = r->plan.at;
  r->bits = r->plan.bits; r->n = r->plan.n; r->ncap = r->plan.ncap;
  r->runs_of = r->plan.runs_of; r->nrun = r->plan.nrun; r->rend = r->plan.rend; r->rinst = r->plan.rinst; r->urun = r->plan.urun;
  r->rentry = r->plan.rentry;
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
    r->plan.n = r->n; r->plan.ncap = r->ncap; r->plan.runs_of = -1;
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

/* Does node's statement run at once if at all but maybe not whole (in an
   iterator's block, or a begin body with rescue clauses, where a raise skips
   the rest)? */
int elab_point_partial(Compiler *c, int node) {
  LoadRecord *r = elab_fresh(c);
  int e = node >= 0 && node < r->n ? r->at[node] : -1;
  return e >= 0 && r->st[e].partial;
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
   (elab_history_open); late: lr_late_make's late bit of the history, 1
   no, 2 yes, 3 not asked */
typedef struct LrHist { int a, b; char *name; LrDef *v; int n, cap, next, nlink; unsigned char open, late; } LrHist;
/* sup: the superclass the first header naming one names, as resolved
   there (has_sup); LR_UNKNOWN_CLASS when it is not a class the record knows */
typedef struct LrClass { LrOpen *open; int nopen, copen; LrMixin *mix; int nmix, cmix, sup, sup_ref; unsigned char has_sup, unheld; } LrClass;   /* unheld: the module is mixed onto a receiver the record holds no class of (an object, `o.extend M`) */
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
   when it runs, keyed by the def (vis: the answer, lr_body_defines); and
   whether the record holds a def in a method body as the version that
   body makes, keyed by the def (vis: the answer, lr_body_held) */
enum { LR_KEY_VIS = -5, LR_KEY_OPAQUE = -6, LR_KEY_RUNS = -7, LR_KEY_DEFINES = -8, LR_KEY_AUTO = -9,
       LR_KEY_CONST = -2 - ELAB_CONST,   /* a constant's history, keyed as the variables' (ElabVar) are */
       LR_KEY_EITHER = -10,   /* a lookup's only: an owner's constant, else its registration, as CRuby's table holds both */
       LR_KEY_HELD = -11 };
/* Is key a the memo of an answer (LR_KEY_RUNS, LR_KEY_DEFINES,
   LR_KEY_HELD): a history keyed by a node, holding no entry of the
   program's, which a walk over the histories passes over? */
static int lr_key_memo(int a) {
  return a == LR_KEY_RUNS || a == LR_KEY_DEFINES || a == LR_KEY_HELD;
}

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
  r->hgen++;
  return x;
}

/* The entries changed (one made, one dropped): the by-name index, the late and
   open bits and elab_class_surface's are of the entries before */
static void lr_entries_changed(LoadRecord *r) {
  r->nentries++; r->hgen++;
  free(r->surf); free(r->surf_at); r->surf = NULL; r->surf_at = NULL;
}

static void lr_version(LoadRecord *r, int a, int b, const char *name, LrDef d) {
  LrHist *x = lr_hist(r, a, b, name, 1);
  if (!x) return;
  if (d.target && !(d.target = strdup(d.target))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  x->v = lr_grow(x->v, &x->cap, x->n, sizeof *x->v);
  x->v[x->n++] = d;
  lr_entries_changed(r);
}

/* cls's history (LR_MAIN: main's, Object's), NULL for a class the record
   does not hold (one registration made after the walk) */
static LrClass *lr_class(LoadRecord *r, int cls) {
  return cls >= LR_MAIN && cls + 1 < r->ncls ? &r->cls[cls + 1] : NULL;
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
typedef struct ElabRec { Compiler *c; LoadRecord *r; int *fc; unsigned char *fs, *vs, *vmain; int cap; unsigned char unplaced; int cur, cfc; unsigned char cfs; int body; } ElabRec;   /* cur: the statement being recorded, in the frame cfc / cfs; body: the def whose body makes the versions (LrDef.body) */

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

/* The class a builder write makes at the write (`X = Class.new do`, `X =
   Struct.new(:a) do`, `k = Data.define(:a) do`), the node -> class map's
   (elab_rec_open, record_write_class); LR_UNKNOWN_CLASS for a write with
   no such block */
static int lr_builder_class(Compiler *c, int node) {
  if (class_def_builder(c->nt, node) < 0 && class_def_body(c->nt, node) < 0) return LR_UNKNOWN_CLASS;
  return elab_header_class(c, node, NULL, NULL);
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
   aliases up to a cap; else none known. *core (when asked): the core
   class slot the chain ends in, as lr_class_ref_at gives it. */
static int lr_class_of(Compiler *c, const LrDef *d, int *core) {
  if (core) *core = -1;
  NodeKind k = d ? nt_kind(c->nt, d->node) : NK_NilNode;
  if (k == NK_ClassNode || k == NK_ModuleNode || (d && class_def_builder(c->nt, d->node) >= 0))
    return elab_header_class(c, d->node, NULL, NULL);
  if (k != NK_ConstantWriteNode) return LR_UNKNOWN_CLASS;
  int v = nt_ref(c->nt, d->node, "value");
  if (class_def_is_record_call(c->nt, v)) {   /* `S = Struct.new(:a)`, its block or none: the class made at the write */
    int ci = elab_header_class(c, d->node, NULL, NULL);
    return ci >= 0 ? ci : LR_UNKNOWN_CLASS;
  }
  NodeKind vk = v >= 0 ? nt_kind(c->nt, v) : NK_NilNode;
  LoadRecord *r = c->lrec;
  if ((vk != NK_ConstantReadNode && vk != NK_ConstantPathNode) || r->follow >= 16) return LR_UNKNOWN_CLASS;
  r->follow++;
  int ci = lr_class_ref_at(c, v, core);
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

/* *core (when asked): the slot of the core class the name is registered as,
   also through an alias of it, and -1 for any other. */
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
  if (o == ELAB_YES) return lr_class_of(c, d, core);
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
    int ci = lr_class_of(c, d, NULL);
    if (ci >= LR_MAIN) lr_cls_add(&s, ci);
    else if (nt_kind(nt, d->node) == NK_ConstantWriteNode) for_each_value_position(nt, nt_ref(nt, d->node, "value"), 0, lr_value_leaf, &s);
    else s.bad = 1;
  }
  if (s.bad) { free(s.v); return -1; }
  *out = s.v;
  return s.n;
}

/* The constant reference `ref` (a path, its parents too), which the walk
   has not placed yet, stands at statement e's point: placed now, as the walk
   places it next, for a lookup that cannot wait for it */
static void lr_place_ref(LoadRecord *r, const NodeTable *nt, int ref, int e, unsigned char bits) {
  for (int x = ref; e >= 0 && x >= 0 && x < r->n && (nt_kind(nt, x) == NK_ConstantPathNode || nt_kind(nt, x) == NK_ConstantReadNode);
       x = nt_kind(nt, x) == NK_ConstantPathNode ? nt_ref(nt, x, "parent") : -1)
    if (r->at[x] == -2) { r->at[x] = e; r->bits[x] = bits; }
}

/* The class a call's receiver names: none or self, the frame's (its
   singleton where self is a singleton class); a constant, its class (not
   singleton), read at its statement's point (a def's `def K.m` and a call's
   `K.attr_reader` receiver are not entered by the walk) */
static int lr_receiver(ElabRec *x, int recv, int fc, int fs, int *sg, unsigned char bits) {
  NodeKind k = recv >= 0 ? nt_kind(x->c->nt, recv) : NK_SelfNode;
  *sg = k == NK_SelfNode && (fs == LR_FS_SCLASS || fs == LR_FS_IEVAL_SCLASS);
  if (k == NK_SelfNode) return fc;
  if (!x->unplaced) lr_place_ref(x->r, x->c->nt, recv, x->cur, bits);
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
    if (nt_kind(nt, ex) != NK_SelfNode) lr_place_ref(x->r, nt, ex, x->cur, 0);
    cls = nt_kind(nt, ex) == NK_SelfNode ? cls : lr_class_ref_at(x->c, ex, NULL);
    sg = LR_FS_SCLASS;
  }
  else if ((k == NK_ConstantWriteNode || k == NK_ConstantPathWriteNode || k == NK_LocalVariableWriteNode) &&
           class_def_body(x->c->nt, node) >= 0) {
    /* `X = Struct.new(:a) do`, `A::B = Class.new do`: the class made at the write */
    cls = lr_builder_class(x->c, node); sg = 0;
  }
  else if (call == LR_UNKNOWN_CLASS) { cls = LR_UNKNOWN_CLASS; sg = 0; }
  else if (call >= 0) {
    const char *nm = nt_str(nt, call, "name");
    int rsg;
    cls = is_class_eval_family(nm) || is_instance_eval_family(nm) ? lr_receiver(x, nt_ref(nt, call, "receiver"), cls, sg, &rsg, 0) : LR_UNKNOWN_CLASS;
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
int elab_def_scope_of(Compiler *c, int node) { return lr_def_scope(lr_get(c), node); }

/* Does module_function copy a version of this kind to the singleton: a
   def, a define_method and a `module_function :n` of a name do; an alias
   (or the forwarder of one), an attr reader or writer and an undef do not
   (CRuby makes no copy for those under the bare word either) */
static int lr_mf_copies(LrKind kind, const char *target) {
  return !target && (kind == LR_DEF || kind == LR_DEFINE_METHOD || kind == LR_VIS);
}

/* A method version of (cls, sg, name) by node, under the visibility state
   bvis: the frame's where the version is the frame's own definee's (a
   `class << self` body's def is the singleton's), public for one on
   another receiver (`def self.m` in a class body, `K.attr_reader`); a
   module_function one also copies it to the singleton */
static int lr_vis_at(Compiler *c, int cls, const char *name, int sg, LoadPoint p);

static void lr_event(ElabRec *x, int cls, int sg, const char *name, LrKind kind, int node, int vis);

/* ...: a definition of initialize and its kin is private wherever it
   stands, and an alias, or an alias's forwarder (lr_def), takes its
   original's visibility */
static int lr_chain_open(Compiler *c, int cls);
static void lr_method(ElabRec *x, int cls, int sg, const char *name, int node, LrKind kind, int bvis, int may, const char *target) {
  if (cls < LR_MAIN || !name || (x->body && lr_chain_open(x->c, cls))) return;   /* (lr_body_versions) */
  int vis = lr_vis(bvis), ov = target ? lr_vis_at(x->c, cls, target, sg, elab_point_of(x->c, node)) : -1;
  const LrHist *th = x->unplaced && target ? lr_hist(x->r, cls, sg, target, 0) : NULL;
  for (int i = th ? th->n - 1 : -1; i >= 0; i--)   /* an unplaced body's own original, which no query at a point sees */
    if (th->v[i].unplaced && th->v[i].kind <= LR_VIS) { ov = th->v[i].vis; break; }
  if (!sg && (kind == LR_DEF || kind == LR_DEFINE_METHOD) && is_implicitly_private_name(name)) vis = SP_VIS_PRIVATE;
  if (target && !x->unplaced) lr_settle_add(x->r, SET_ALIAS, node, cls, sg, may, vis, name);
  if (ov >= 0) vis = ov;
  int sc = kind == LR_DEF || kind == LR_DEFINE_METHOD ? lr_def_scope(x->r, node) : 0;
  lr_version(x->r, cls, sg, name, (LrDef){ node, (unsigned char)kind, (unsigned char)vis, (unsigned char)may, 0, target, .unplaced = x->unplaced && !x->body, .scope = sc, .body = x->body });
  lr_event(x, cls, sg, name, kind, node, vis);
  if (!sg && bvis == BVIS_MODULE_FUNCTION && lr_mf_copies(kind, target)) {
    lr_version(x->r, cls, 1, name, (LrDef){ node, LR_COPY, SP_VIS_PUBLIC, (unsigned char)may, 0, name, .unplaced = x->unplaced, .scope = sc });
    lr_event(x, cls, 1, name, LR_COPY, node, SP_VIS_PUBLIC);
  }
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
/* A receiver-less def in a `K.instance_eval { }` frame defines on K's
   singleton: kept by def node for the scope pass, which placed the def's
   scope before the record saw the def (elab_def_definee reads it) */
static void lr_def_definee_set(LoadRecord *r, int def, int cls, int sg) {
  if (def < 0) return;
  if (def >= r->nddef) {
    int n = def + 1 > 2 * r->nddef ? def + 1 : 2 * r->nddef;
    r->ddef = realloc(r->ddef, sizeof *r->ddef * (size_t)n);
    if (!r->ddef) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memset(r->ddef + r->nddef, 0, sizeof *r->ddef * (size_t)(n - r->nddef));
    r->nddef = n;
  }
  r->ddef[def] = ((cls + 2) << 1) | (sg ? 1 : 0);
}
int elab_def_definee(Compiler *c, int node, int *sg) {
  LoadRecord *r = lr_get(c);
  if (node < 0 || node >= r->nddef || !r->ddef[node]) { if (sg) *sg = 0; return -1; }
  if (sg) *sg = r->ddef[node] & 1;
  return (r->ddef[node] >> 1) - 2;
}
static void lr_def(ElabRec *x, int def, int fc, int fs, int bvis, int may) {
  const NodeTable *nt = x->c->nt;
  int recv = nt_ref(nt, def, "receiver"), sg, cls = lr_definee(fc, fs, &sg);
  if (recv >= 0) { cls = lr_receiver(x, nt_def_receiver(nt, recv), fc, LR_FS_SCLASS, &sg, LR_LATE | LR_BODY); sg = 1; bvis = BVIS_PUBLIC; }
  else if (fs == LR_FS_IEVAL && cls >= 0) lr_def_definee_set(x->r, def, cls, sg);
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

/* Is p a path of constant names alone (`A::B`, `::A`, `A`)? */
static int lr_const_chain(const NodeTable *nt, int p) {
  for (; p >= 0; p = nt_ref(nt, p, "parent"))
    if (nt_kind(nt, p) != NK_ConstantPathNode && nt_kind(nt, p) != NK_ConstantReadNode) return 0;
  return 1;
}

/* The owner of the constant the write node sets through the path `path`
   (`path::X = v`, `path.const_set(:X, v)`): the class the reference names
   where it stands. Object, written `Object` or through an alias of it, is
   main's (LR_MAIN), where `::X` and a top-level write go, whether the
   program reopens it before, after or not at all (a record made again
   after the walk gives the walk's owner). The walk visits a path after a
   value that opens a statement of its own (a block run at once); a path of
   names it has not reached is placed with its write, whose statement it
   runs in. */
static int lr_path_const_owner(Compiler *c, int node, int path) {
  LoadRecord *r = c->lrec;
  if (node < r->n && r->at[node] >= 0 && lr_const_chain(c->nt, path))
    for (int p = path; p >= 0 && p < r->n && r->at[p] == -2; p = nt_ref(c->nt, p, "parent")) {
      r->at[p] = r->at[node];
      r->bits[p] = r->bits[node];
    }
  int core, cls = lr_class_ref_at(c, path, &core);
  return lr_const_owner(core == CORE_OBJECT ? LR_MAIN : cls);
}

/* A known class's singleton has its own constant history, disjoint from
   the class's. A singleton of an arbitrary object remains unknown. */
static int lr_singleton_class(Compiler *c, int f) {
  int ex = nt_ref(c->nt, c->lrec->st[f].node, "expression");
  return nt_kind(c->nt, ex) == NK_SelfNode ? lr_cref(c, f) : lr_class_ref_at(c, ex, NULL);
}
static int lr_singleton_owner(Compiler *c, int f) {
  return lr_sg_const_owner(lr_singleton_class(c, f));
}

/* A constant of a reopened Object is a top-level one: CRuby's Object is
   main's constant table, where a read at the top level looks, as for
   `Object::X = v` (lr_path_const_owner) */
static int lr_object_as_main(Compiler *c, int cls) {
  return cls >= 0 && cls == comp_core_class(c, CORE_OBJECT) ? LR_MAIN : cls;
}

static int lr_const_scope_owner(Compiler *c, int e) {
  const LoadRecord *r = c->lrec;
  for (int f = e >= 0 ? r->st[e].frame : -1; f >= 0; f = r->st[f].frame) {
    NodeKind k = nt_kind(c->nt, r->st[f].node);
    if (k == NK_SingletonClassNode) return lr_singleton_owner(c, f);
    if (k == NK_ClassNode || k == NK_ModuleNode) break;
  }
  return lr_const_owner(lr_object_as_main(c, lr_cref(c, e)));
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
    int in_def = 0;   /* (inside a method a `class << self` defines, self is the class) */
    for (int a = par[node]; a >= 0; a = par[a]) {
      NodeKind ak = nt_kind(nt, a);
      if (ak == NK_DefNode && nt_ref(nt, a, "receiver") < 0) in_def = 1;
      if (ak == NK_SingletonClassNode) {
        int f = a < c->lrec->n ? c->lrec->at[a] : -1;
        owner = f >= 0 && c->lrec->st[f].node == a ?
          (in_def ? lr_const_owner(lr_object_as_main(c, lr_singleton_class(c, f))) : lr_singleton_owner(c, f)) : 0;
        break;
      }
      if (ak == NK_ClassNode || ak == NK_ModuleNode) { owner = lr_const_owner(lr_object_as_main(c, elab_header_class(c, a, NULL, NULL))); break; }
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
        (fs == LR_FS_SCLASS ? lr_sg_const_owner(fc) : lr_const_owner(lr_object_as_main(c, fc))) : lr_path_const_owner(c, node, rv);
      lr_write(c->lrec, ELAB_CONST, owner, name, node, lr_nil_value(nt, av[1]), may);
      return;
    }
    default: return;
  }
  if (!(name = lr_const_written(nt, node))) return;
  if (target >= 0) {
    int par = nt_ref(nt, target, "parent");
    owner = par < 0 ? lr_const_owner(LR_MAIN) : nt_kind(nt, par) == NK_SelfNode ?
      (fs == LR_FS_SCLASS ? lr_sg_const_owner(fc) : lr_const_owner(lr_object_as_main(c, fc))) : lr_path_const_owner(c, node, par);
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
      const char *fn = call_sent_name(nt, n, NULL);
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
/* The names the source's reflection readers name, taken before the walk,
   ahead of the passes that rewrite readers (desugar_defined_method_call,
   desugar_respond_to_probe): respond_to?, method_defined? and their kin and
   a Method's makers (is_method_presence_query 1) by a literal name, called
   by their own names or through the send family, a send of a send too
   (whose first argument, when no literal, may name any of them), a call under defined?, and the queries a fold has answered already (def_asked,
   desugar_engine_branches). They answer from the final method tables, so a call
   they guard runs where they say the method is there (lr_late_make). A
   list of methods or a name the record cannot read may be any (refl_any).
   A respond_to? the version cells may answer (respond_to_cells_shape), and
   a defined? of a call on a constant or self, which becomes one, go to
   refl_cell instead, and a Method's maker of that shape
   (capture_cells_shape) to refl_capt: of a class's own class method, they
   read the cells of the entry its call takes (cplan_cmethod_presence), or
   take the entry. Every other reader but a Method's maker answers from
   the table (refl_table, elab_method_reflected), kept by the sides whose
   methods it may report (sides: 1 the instance side, 2 the class side). */
enum { LR_REFL_TABLE, LR_REFL_MAKER, LR_REFL_CELL, LR_REFL_CAPT };
static void lr_refl_list(LoadRecord *r, char ***l, int *n, int *cap, const char *name) {
  *l = lr_grow(*l, cap, *n, sizeof **l);
  if (!((*l)[(*n)++] = strdup(name))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
}
static void lr_refl_add(LoadRecord *r, const char *name, int kind, int sides) {
  if (!name) { r->refl_any = 1; return; }
  if (kind == LR_REFL_CELL) { lr_refl_list(r, &r->refl_cell, &r->nrefl_cell, &r->crefl_cell, name); return; }
  if (kind == LR_REFL_CAPT) { lr_refl_list(r, &r->refl_capt, &r->nrefl_capt, &r->crefl_capt, name); return; }
  lr_refl_list(r, &r->refl, &r->nrefl, &r->crefl, name);
  if (kind != LR_REFL_TABLE) return;
  for (int sg = 0; sg < 2; sg++)
    if (sides & (1 << sg)) lr_refl_list(r, &r->refl_table[sg], &r->nrefl_table[sg], &r->crefl_table[sg], name);
}
/* The sides whose methods table reader call n, asking rn, may report:
   method_defined? and its kin a receiver's instance methods (1), but a
   singleton class's class methods (2); any other reader an instance's on
   `X.new` (1), and either on any other receiver (3) */
static int lr_refl_sides(const NodeTable *nt, int n, const char *rn) {
  int recv = nt_ref(nt, n, "receiver");
  const char *rcn = recv >= 0 && nt_kind(nt, recv) == NK_CallNode ? nt_str(nt, recv, "name") : NULL;
  if (is_method_defined_query(rn)) return is_singleton_class_name(rcn) ? 2 : 1;
  return is_new_name(rcn) ? 1 : 3;
}
static void lr_refl_take(Compiler *c) {
  const NodeTable *nt = c->nt;
  LoadRecord *r = c->lrec;
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    int ac = 0, a = 0, pq;
    const int *av = call_args(nt, n, &ac);
    const char *rn = call_sent_name(nt, n, &a);
    if (!rn) {
      if (ac) lr_refl_add(r, NULL, LR_REFL_TABLE, 3);   /* (a send with no argument reads no name) */
      continue;
    }
    if ((pq = is_method_presence_query(rn)))
      lr_refl_add(r, pq == 1 && a < ac ? vis_arg_name(nt, av[a]) : NULL,
                  respond_to_cells_shape(nt, n) ? LR_REFL_CELL : capture_cells_shape(nt, n) ? LR_REFL_CAPT :
                  is_method_object_maker(rn) ? LR_REFL_MAKER : LR_REFL_TABLE, lr_refl_sides(nt, n, rn));
  }
  NT_FOREACH_KIND(nt, NK_DefinedNode, n) {
    int v = nt_ref(nt, n, "value"), rv = v >= 0 ? nt_ref(nt, v, "receiver") : -1;
    NodeKind rk = rv >= 0 ? nt_kind(nt, rv) : NK_SelfNode;
    if (v >= 0 && nt_kind(nt, v) == NK_CallNode)
      lr_refl_add(r, nt_str(nt, v, "name"),
                  rk == NK_SelfNode || rk == NK_ConstantReadNode || rk == NK_ConstantPathNode ? LR_REFL_CELL : LR_REFL_TABLE, 3);
  }
  for (int i = 0; i < c->ndef_asked; i++) lr_refl_add(r, c->def_asked[i], LR_REFL_TABLE, 3);
  if (r->nrefl > 1) qsort(r->refl, (size_t)r->nrefl, sizeof *r->refl, lr_name_cmp);
  for (int sg = 0; sg < 2; sg++)
    if (r->nrefl_table[sg] > 1) qsort(r->refl_table[sg], (size_t)r->nrefl_table[sg], sizeof *r->refl_table[sg], lr_name_cmp);
  if (r->nrefl_cell > 1) qsort(r->refl_cell, (size_t)r->nrefl_cell, sizeof *r->refl_cell, lr_name_cmp);
  if (r->nrefl_capt > 1) qsort(r->refl_capt, (size_t)r->nrefl_capt, sizeof *r->refl_capt, lr_name_cmp);
}

/* What code the record cannot follow may do, kept in its opaque entry's
   vis, and what a method body may do when it runs (lr_body_defines): 0
   nothing; LR_OPAQUE_WRITES, only write a variable (an instance variable,
   a global, a class variable), as an attr's method (LR_READER, LR_WRITER)
   or a def whose body does no more; LR_OPAQUE_DEFINES, anything (a send,
   an eval, a mixin a pass made, a program method that may define, one the
   record cannot pin). Only the defining ones may supply a method
   (lr_unordered_supply). */
enum { LR_OPAQUE_WRITES = 1, LR_OPAQUE_DEFINES };

static int lr_body_defines(Compiler *c, int def, int direct, int depth);
static int lr_callbacks_define(Compiler *c, int depth);

static int lr_body_def(const NodeTable *nt, const int *par, int n, int node);
static int lr_const_seen(Compiler *c, const char *name);
static int lr_super(Compiler *c, int cls);
static int lr_chain_bound(Compiler *c);

/* Is constant `name` bound once, so that what it names never changes: the
   record sees every write of it (lr_const_seen), the source writes it with
   class and module headers alone, which reopen what the first made, or
   with one write and no header, and no remove_const may take it away? */
static const struct LrFinal *lr_final_of(Compiler *c, const char *name);
static int lr_const_final(Compiler *c, const char *name) {
  if (!lr_const_seen(c, name)) return 0;
  const struct LrFinal *f = lr_final_of(c, name);
  int writes = f ? f->writes : 0, headers = f ? f->headers : 0;
  return !(writes > 1 || (writes && headers)) && !c->lrec->fin_calls && !(f && f->removed);
}

/* ...asked of a write of it after the walk (an_const_presence) */
int elab_const_write_final(Compiler *c, int write) {
  const char *nm = c->lrec ? lr_const_written(c->nt, write) : NULL;
  return nm && lr_const_final(c, nm);
}

/* Does the record hold def node `def` (a `def K.m` in a method body) as
   the version that body makes (lr_body_versions), on facts that cannot
   change later: K's constant is bound once (lr_const_final), and K's class
   and every class on its chain are in the record already, each superclass
   fixed by the class's first header (CRuby raises on another), so its
   chain is where it ends (lr_chain_open)? Its version opens where the
   body first runs, so the def is no definition the record cannot follow
   (lr_node_defines). Asked first while the walk records, where a class it
   has not reached yet answers no, and kept (LR_KEY_HELD): a yes is then
   the answer of the whole record, and a def it does not hold, which
   lr_body_versions may still record, counts as a definition the record
   cannot follow, so the calls that run its body stay opaque.
   SPINEL_ELAB_CHECK asks it again of the finished record (lr_check_held). */
static int lr_body_holds(Compiler *c, int def) {
  LoadRecord *r = c->lrec;
  const NodeTable *nt = c->nt;
  const int *par = r->plan_depth ? r->plan_par : r->up ? r->up : comp_parent_map(c);   /* (the walk's: the tree's) */
  int recv = nt_ref(nt, def, "receiver"), k = recv >= 0 ? nt_def_receiver(nt, recv) : -1, held = 0;
  int body = def < r->n ? lr_body_def(nt, par, r->n, def) : -1;
  if (body > 0 && k >= 0 && nt_kind(nt, k) == NK_ConstantReadNode && r->at[body] >= 0 && lr_const_final(c, lr_written_name(nt, k))) {
    lr_place_ref(r, nt, k, r->at[body], LR_LATE | LR_BODY);   /* (where the climb places it: a placed def's receiver, lr_receiver) */
    int cls = lr_class_ref_at(c, k, NULL);
    held = cls >= LR_MAIN && !lr_chain_open(c, cls);
    for (int a = cls, d = 0; held && a >= 0 && a < c->nclasses && d < lr_chain_bound(c); a = lr_super(c, a), d++) {
      const LrClass *kc = lr_class(r, a);
      if (!c->classes[a].is_builtin_const && (!kc || !kc->nopen)) held = 0;
    }
  }
  return held;
}
/* ...its first answer, kept */
static int lr_body_held(Compiler *c, int def) {
  const LrHist *h = lr_hist(c->lrec, LR_KEY_HELD, def, "", 0);
  if (h && h->n) return h->v[h->n - 1].vis;
  int held = lr_body_holds(c, def);
  lr_version(c->lrec, LR_KEY_HELD, def, "", (LrDef){ def, LR_OPAQUE, (unsigned char)held, 0, 0, NULL });
  return held;
}

/* The larger of two answers of what code may do (LR_OPAQUE_*) */
static int lr_most(int o, int w) {
  return w > o ? w : o;
}

/* What may a call of `name` in a method body do, by the methods of the
   program's it may run: 0 nothing, else the most any of them may
   (LR_OPAQUE_*)? The most of what each place the program defines the name
   may do (lr_body_defines), and define, where a def is not recorded yet
   (past the frontier, or in a method body, while the record is made), or
   the place is an attr, an alias or a define_method. A name the program
   defines nowhere is a builtin's, which lr_node_defines answers for by its
   name. */
static int lr_callee_defines(Compiler *c, const char *name, int direct, int depth) {
  LoadRecord *r = c->lrec;
  int nn = 0, nd = 0, o = 0;
  if (r->names_any) return LR_OPAQUE_DEFINES;
  lr_name_range(r->names, r->nnames, sizeof *r->names, name, &nn);
  const LrSite *d = lr_name_range(r->defs, r->ndefs, sizeof *r->defs, name, &nd);
  if (nd < nn) return LR_OPAQUE_DEFINES;
  for (int i = 0; i < nd; i++) {
    LoadPoint p = elab_point_of(c, d[i].node);
    if (!r->recorded && (p.region < 0 || p.seq >= r->frontier)) return LR_OPAQUE_DEFINES;
    o = lr_most(o, lr_body_defines(c, d[i].node, direct, depth + 1));
    if (o == LR_OPAQUE_DEFINES) return o;
  }
  return o;
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
    case NK_InstanceVariableWriteNode: case NK_ClassVariableWriteNode: case NK_InstanceVariableOrWriteNode:
    case NK_InstanceVariableAndWriteNode: case NK_ClassVariableOrWriteNode: case NK_ClassVariableAndWriteNode:
      return 0;
    default: return 1;
  }
}

/* Is k a write of a class variable or an instance variable? Neither calls
   back into the program (a global's setter may: $stdout= asks the value
   respond_to?, $0= asks it to_str). */
static int lr_variable_write(NodeKind k) {
  switch (k) {
    case NK_InstanceVariableWriteNode: case NK_InstanceVariableOrWriteNode: case NK_InstanceVariableAndWriteNode:
    case NK_InstanceVariableOperatorWriteNode: case NK_InstanceVariableTargetNode:
    case NK_ClassVariableWriteNode: case NK_ClassVariableOrWriteNode: case NK_ClassVariableAndWriteNode:
    case NK_ClassVariableOperatorWriteNode: case NK_ClassVariableTargetNode:
      return 1;
    default: return 0;
  }
}

/* What may a call of `nm` do, as lr_node_defines asks for a CallNode's: what
   the program's methods of that name may do, a call of `new` its initialize
   too; a load effect, or a name the record cannot read, may define */
static int lr_call_defines(Compiler *c, const char *nm, int direct, int depth) {
  if (!nm || is_load_effect_call(nm)) return LR_OPAQUE_DEFINES;
  int w = lr_named(c, nm) ? lr_callee_defines(c, nm, direct, depth) : 0;
  if (w != LR_OPAQUE_DEFINES && is_new_name(nm)) w = lr_most(w, lr_callee_defines(c, "initialize", direct, depth));
  return w;
}

/* Is k an operator write of an instance variable or a class variable
   (`@x **= 1`), which calls its operator on the value, and which
   lr_node_defines asks for the way it asks for a CallNode's name? A local's,
   one through a call or through an index calls a method too, and a global's
   setter may: those are answered as before, the record not counting what
   they call. */
static int lr_variable_op_write(NodeKind k) {
  return k == NK_InstanceVariableOperatorWriteNode || k == NK_ClassVariableOperatorWriteNode;
}

/* What may node n, run, do (lr_body_defines): 0 nothing, LR_OPAQUE_WRITES
   write an instance variable or a class variable, LR_OPAQUE_DEFINES define
   or write anything else. With direct, the runtime's own calls back into the
   program are left out */
static int lr_node_defines(Compiler *c, int n, int direct, int depth) {
  static const NodeKind kinds[] = {
    NK_DefNode, NK_SingletonClassNode, NK_AliasMethodNode, NK_AliasGlobalVariableNode, NK_UndefNode, NK_YieldNode,
    NK_SuperNode, NK_ForwardingSuperNode, NK_ForwardingArgumentsNode, NK_GlobalVariableWriteNode,
    NK_GlobalVariableOrWriteNode, NK_GlobalVariableAndWriteNode, NK_GlobalVariableOperatorWriteNode,
    NK_GlobalVariableTargetNode };
  const NodeTable *nt = c->nt;
  if (n < 0) return 0;
  NodeKind k = nt_kind(nt, n);
  int nc = 0, o = lr_variable_write(k) ? LR_OPAQUE_WRITES : 0;
  const NodeKind *ck = elab_const_write_kinds(&nc);
  if (k == NK_DefNode && lr_body_held(c, n)) return 0;   /* (the record follows it) */
  for (int i = 0; i < (int)(sizeof kinds / sizeof *kinds); i++) if (k == kinds[i]) return LR_OPAQUE_DEFINES;
  for (int i = 0; i < nc; i++) if (k == ck[i]) return LR_OPAQUE_DEFINES;
  if (k == NK_BlockArgumentNode && nt_kind(nt, nt_ref(nt, n, "expression")) != NK_SymbolNode) return LR_OPAQUE_DEFINES;   /* a block passed on */
  if (k == NK_CallNode) o = lr_most(o, lr_call_defines(c, nt_str(nt, n, "name"), direct, depth));
  if (lr_variable_op_write(k)) o = lr_most(o, lr_call_defines(c, nt_str(nt, n, "binary_operator"), direct, depth));
  if (o != LR_OPAQUE_DEFINES && !direct && lr_node_dispatches(nt, n)) o = lr_most(o, lr_callbacks_define(c, depth));
  for (int i = 0, nr = nt_num_refs(nt, n); i < nr && o != LR_OPAQUE_DEFINES; i++)
    o = lr_most(o, lr_node_defines(c, nt_ref_at(nt, n, i), direct, depth));
  for (int i = 0, na = nt_num_arrs(nt, n); i < na && o != LR_OPAQUE_DEFINES; i++) {
    int m = 0;
    const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m && o != LR_OPAQUE_DEFINES; j++) o = lr_most(o, lr_node_defines(c, ids[j], direct, depth));
  }
  return o;
}

/* What does the body of def node `def`, run, do: define anything (a def, a
   class, module or alias, a call that changes what the record holds or runs
   code it cannot follow (is_load_effect_call), a write of a constant, a
   yield, a super or a block passed on, a call of a method of the program's
   that may (lr_callee_defines; `new` runs initialize), or, but with direct,
   a node by which the runtime may call back one that may
   (lr_callbacks_define); a write of a global too, whose setter may call
   back: LR_OPAQUE_DEFINES), write a variable and no more (a class variable
   or an instance variable, its class's, run on a class, or a call of a
   method that does no more: LR_OPAQUE_WRITES), or neither (0)? A def asked again while it is asked (a cycle) may define.
   Memoized by the def (LR_KEY_DEFINES; "direct" for the direct answer). */
static int lr_body_defines(Compiler *c, int def, int direct, int depth) {
  LoadRecord *r = c->lrec;
  const char *key = direct ? "direct" : "";
  const LrHist *h = lr_hist(r, LR_KEY_DEFINES, def, key, 0);
  if (h && h->n) return h->v[h->n - 1].vis;
  if (depth > 64) return LR_OPAQUE_DEFINES;
  lr_version(r, LR_KEY_DEFINES, def, key, (LrDef){ def, LR_OPAQUE, LR_OPAQUE_DEFINES, 0, 0, NULL });   /* while it is asked */
  int o = lr_node_defines(c, nt_ref(c->nt, def, "parameters"), direct, depth);
  if (o != LR_OPAQUE_DEFINES) o = lr_most(o, lr_node_defines(c, nt_ref(c->nt, def, "body"), direct, depth));
  LrHist *m = lr_hist(r, LR_KEY_DEFINES, def, key, 0);
  m->v[m->n - 1].vis = (unsigned char)o;
  return o;
}

/* What may a method the runtime calls back by itself (a protocol's,
   method_name_implicitly_invoked; one a builtin or a node's syntax
   calls on an object, ic_core_callback) do: 0 nothing, else the most any
   may (LR_OPAQUE_*)? Its direct answer is enough: what it calls back in
   turn is one of these too. Kept once no such method may, or once the
   record is made. */
static int lr_callbacks_define(Compiler *c, int depth) {
  LoadRecord *r = c->lrec;
  if (r->callbacks) return r->callbacks - 1;
  int o = 0;
  for (int i = 0; i < r->nnames && o != LR_OPAQUE_DEFINES; i++)
    if ((i == 0 || strcmp(r->names[i - 1], r->names[i]) != 0) &&
        (method_name_implicitly_invoked(r->names[i]) || ic_core_callback(r->names[i], 0)))
      o = lr_most(o, lr_callee_defines(c, r->names[i], 1, depth));
  if (!o || r->recorded) r->callbacks = (unsigned char)(o + 1);
  return o;
}

/* Does owner k's opaque history hold code that may define (LR_OPAQUE_DEFINES)? */
static int lr_opaque_defines(LoadRecord *r, int k) {
  const LrHist *op = lr_hist(r, LR_KEY_OPAQUE, lr_const_owner(k), "", 0);
  for (int j = 0; op && j < op->n; j++)
    if (op->v[j].vis == LR_OPAQUE_DEFINES) return 1;
  return 0;
}
/* Does a call of `name` on class cls (its singleton when sg) at p run a
   method of the program's that may define or write something: any one
   it may find but a def whose body does neither (lr_body_defines)? 0 when
   not, else what it may do */
static int lr_runs_defining(Compiler *c, int cls, int sg, const char *name, LoadPoint p) {
  const LrDef *d = NULL;
  int o = lr_search(c, cls, sg, name, -1, p, 0, 0, &d, 0);
  if (o == ELAB_NO) return 0;
  if (o == ELAB_YES && d && d->kind == LR_DEF) return lr_body_defines(c, d->node, 0, 0);
  return o == ELAB_YES && d && (d->kind == LR_READER || d->kind == LR_WRITER) ? LR_OPAQUE_WRITES : LR_OPAQUE_DEFINES;
}

/* Does call `node` on class cls (in its singleton when sg) run code the
   record cannot follow: a send, an eval of a String, or a method of the
   program that may define or write something (lr_runs_defining), called
   on the class (its singleton method, from its body or through its
   constant) or from its body (a top-level one)? 0 when not, else what it
   may do (LR_OPAQUE_*) */
static int lr_opaque_call(Compiler *c, int node, int cls, int sg) {
  const NodeTable *nt = c->nt;
  if (nt_int(nt, node, "event_hook", 0)) return 0;   /* (the call of a hook the record fires: it asked for it) */
  const char *nm = nt_str(nt, node, "name");
  int recv = nt_ref(nt, node, "receiver");
  LoadPoint p = elab_point_of(c, node);
  int bare = recv < 0 || nt_kind(nt, recv) == NK_SelfNode;
  if (is_send_family(nm) || (is_eval_exec_family(nm) && nt_ref(nt, node, "block") < 0)) return LR_OPAQUE_DEFINES;
  int on = sg ? 0 : lr_runs_defining(c, cls, 1, nm, p), top = bare ? lr_runs_defining(c, LR_MAIN, 0, nm, p) : 0;
  return on > top ? on : top;
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

/* The hook a mixin of kind kind fires on its module (definition_form_hooks):
   included, extended, prepended; NULL for a singleton class's prepend */
static const char *lr_mix_hook(int kind) {
  return kind == LR_INCLUDE ? "included" : kind == LR_EXTEND ? "extended" : kind == LR_PREPEND ? "prepended" : NULL;
}

/* A mixin the walk has recorded at the statement it is recording, of
   module mod (named by the argument arg) into class cls, whose hook the
   program may define: noted for the scope walk to fire as it leaves the
   statement (elab_fire_after), unless the pre-walk splice took the mixin
   (desugar_included_hooks marks it) */
static void lr_mix_event(ElabRec *x, int cls, int node, int arg, int mod, int kind) {
  LoadRecord *r = x->r;
  const char *hook = lr_mix_hook(kind);
  if (x->unplaced || !r->walk || !hook || cls < 0 || nt_int(x->c->nt, arg, "incl_prewalk", 0) || !lr_named(x->c, hook)) return;
  r->ev = lr_grow(r->ev, &r->cev, r->nev, sizeof *r->ev);
  char *copy = strdup(hook);
  if (!copy) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  r->ev[r->nev] = (LrEvent){ .stmt = x->cur, .cls = cls, .node = node, .mod = mod, .arg = arg, .mix = (unsigned char)(kind + 1), .name = copy };
  r->nev++;
}

static int lr_runs(Compiler *c, int cls, const char *name, LoadPoint p);

/* Does a method of the program's answer the call nm (`remove_method`) at
   node, on class cls (its singleton when sg)? Then the call runs that,
   removes nothing and fires no hook. On a class the method is found on its
   singleton's chain. On a singleton class (a `class << X` body) the record
   does not hold the singleton class's own singleton, where an extend of a
   module would put one: a program that defines a method of the name is
   taken to answer it. */
static int lr_remove_shadowed(Compiler *c, int node, const char *nm, int cls, int sg) {
  return sg ? lr_defines(c->lrec, nm) : lr_runs(c, cls, nm, elab_point_of(c, node)) == ELAB_YES;
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
/* Does a mixin call name a class where a module is expected (`include K`,
   `extend String`): one of the program's, a builtin one the program reopens,
   or a builtin class by name? CRuby checks every argument before mixing any
   in, so the statement raises TypeError and brings nothing. */
static int lr_mix_names_class(Compiler *c, const int *av, int ac) {
  const NodeTable *nt = c->nt;
  for (int i = 0; i < ac; i++) {
    int m = lr_class_ref_at(c, av[i], NULL);
    if (m >= 0 && m < c->nclasses) { if (!comp_class_is_module(c, &c->classes[m])) return 1; continue; }
    const char *nm = nt_kind(nt, av[i]) == NK_ConstantReadNode ? nt_str(nt, av[i], "name") : NULL;
    if (nm && m == LR_UNKNOWN_CLASS && is_builtin_class_name(nm) && !is_builtin_module_name(nm)) return 1;
  }
  return 0;
}

/* The statement list a frame's statements are in: a file's, a class or
   module body's; -1 for any other frame */
static int lr_frame_body(const NodeTable *nt, int fnode) {
  if (fnode < 0) return -1;
  NodeKind k = nt_kind(nt, fnode);
  if (k == NK_ProgramNode) return nt_ref(nt, fnode, "statements");
  if (k == NK_ClassNode || k == NK_ModuleNode) return class_def_body(nt, fnode);
  if (k == NK_SingletonClassNode) return nt_ref(nt, fnode, "body");
  return -1;
}
/* Is mixin m, of call node on class cls, one an explicit receiver names
   -- `K.include M` at the top level, in another class's body or in K's
   own -- as a statement of that frame's list
   that always runs there, an include or a prepend of a module of the
   program or a core module, on a class of the program or a builtin's
   reopening (desugar_explicit_receiver_mixin makes an empty one for a
   builtin the program does not reopen)? */
static int lr_foreign_mixin(ElabRec *x, int node, const LrMixin *m, int cls, int fc, int sg, int nested) {
  const NodeTable *nt = x->c->nt;
  int recv = nt_ref(nt, node, "receiver");
  if (nested || x->unplaced || !m->applied || m->cond || cls < 0 || cls >= x->c->nclasses) return 0;
  if (sg) {
    /* a singleton's: `include M` in a `class << self` / `class << K` body,
       or `K.singleton_class.prepend M` */
    int sbody = m->open >= 0 && nt_kind(nt, m->open) == NK_SingletonClassNode;
    if (m->kind != LR_EXTEND && m->kind != LR_SG_PREPEND) return 0;
    if (sbody ? !(recv < 0 || nt_kind(nt, recv) == NK_SelfNode) : !nt_self_singleton_class(nt, recv) &&
        !(recv >= 0 && nt_kind(nt, recv) == NK_CallNode && is_singleton_class_name(nt_str(nt, recv, "name")))) return 0;
  }
  else {
    if (recv < 0 || nt_kind(nt, recv) == NK_SelfNode) return 0;
    if (nt_kind(nt, recv) != NK_ConstantReadNode && nt_kind(nt, recv) != NK_ConstantPathNode) return 0;
    if (m->kind != LR_INCLUDE && m->kind != LR_PREPEND && m->kind != LR_EXTEND) return 0;
  }
  /* a module of the program's, or a core module by its name (Comparable) */
  if (m->mod >= x->c->nclasses) return 0;
  if (m->mod >= 0 && !comp_class_is_module(x->c, &x->c->classes[m->mod])) return 0;
  if (m->mod < 0 && !(m->ref >= 0 && nt_kind(nt, m->ref) == NK_ConstantReadNode &&
                      is_builtin_module_name(nt_str(nt, m->ref, "name")))) return 0;
  const char *cn = x->c->classes[cls].name;
  if (!cn) return 0;
  int body = lr_frame_body(nt, m->open), bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  for (int i = 0; i < bn; i++) if (bb[i] == node) return 1;
  return 0;
}

static int lr_const_root_undefined(const NodeTable *nt, int n);
static int lr_sg_chain_runs(Compiler *c, int cls, const char *name, LoadPoint p, const LrDef **out);
static int lr_sg_chain_find(Compiler *c, int cls, const char *name, LoadPoint p, const LrDef **out, int *found);
static void lr_call(ElabRec *x, int node, int fc, int fs, unsigned char *vs, int fnode, int may, int nested) {
  const NodeTable *nt = x->c->nt;
  const char *nm = nt_str(nt, node, "name");
  int sg, recv = nt_ref(nt, node, "receiver"), cls = lr_receiver(x, recv, fc, fs, &sg, 0), an = nt_ref(nt, node, "arguments"), ac = 0;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  /* `K.send(:include, M)`: the mixin `K.include(M)` */
  int sent_args = 0;
  const char *sent = nm && is_send_family(nm) ? call_sent_name(nt, node, &sent_args) : NULL;
  if (sent && sent_args < ac && is_mixin_call(sent)) { nm = sent; av += sent_args; ac -= sent_args; }
  /* `singleton_class.attr_reader :x` in a class or module body: an attr of
     the body's singleton, as `class << self; attr_reader :x; end` records */
  if (nm && (is_attr_decl_reader(nm) || is_attr_decl_writer(nm)) && fc >= 0 && fs == LR_FS_NONE &&
      nt_self_singleton_class(nt, recv)) { cls = fc; sg = 1; }
  /* `singleton_class.include M` in a body, `K.singleton_class.prepend M`: a
     mixin of the singleton, as in `class << K` */
  else if (nm && is_mixin_call(nm) && recv >= 0 && nt_kind(nt, recv) == NK_CallNode &&
           is_singleton_class_name(nt_str(nt, recv, "name")) && nt_ref(nt, recv, "arguments") < 0 &&
           nt_ref(nt, recv, "block") < 0) {
    int in = nt_ref(nt, recv, "receiver");
    if (in < 0 || nt_kind(nt, in) == NK_SelfNode) { cls = fc >= 0 && fs == LR_FS_NONE ? fc : LR_UNKNOWN_CLASS; sg = 1; }
    else {
      if (!x->unplaced) lr_place_ref(x->r, nt, in, x->cur, 0);
      cls = lr_class_ref_at(x->c, in, NULL); sg = 1;
    }
  }
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
  /* a mixin onto a receiver the record holds no class of (`o.extend M` on an object): the module has a position the
     record cannot list, which the queries of a module's `super` take as not known */
  if (nm && cls < LR_MAIN && is_mixin_call(nm))
    for (int i = 0; i < ac; i++) {
      int m = lr_class_ref_at(x->c, av[i], NULL);
      LrClass *mk = m >= LR_MAIN ? lr_class(x->r, m) : NULL;
      if (mk) mk->unheld = 1;
    }
  if (!nm || cls < LR_MAIN) return;
  int attr_r = is_attr_decl_reader(nm), attr_w = is_attr_decl_writer(nm);
  int naming = bvis || cvis || attr_r || attr_w || is_method_naming_call(nm), dm = is_define_method_name(nm) || is_define_singleton_method_name(nm);
  /* `include A, B` puts A nearest the class (ancestors [C, A, B]), as
     `include B` then `include A` would, and so do prepend and extend: the
     arguments of a mixin are recorded last first */
  int mix = is_mixin_call(nm);
  /* `include M` where the class's own `def self.include` (or a
     superclass's) is what the call finds: that method runs, and it mixes
     nothing in by itself. The call is the program's, which may do anything
     (codegen reads the mark and emits it as a call) */
  const LrDef *own = NULL;
  int oc = cls;
  if (mix && !sg && cls >= 0 && !nested && !nt_int(nt, node, "sclass_mixin", 0) &&
      lr_sg_chain_find(x->c, cls, nm, elab_point_of(x->c, node), &own, &oc) == ELAB_YES) {
    /* an alias (`alias include ci` in `class << self`) runs the method it
       took: the version of its target on the class that holds the alias,
       as it stood at the alias. An alias of the builtin itself (`alias
       include prepend`) finds no method of the program's there, and is
       left to the builtin's mixin, as before. */
    for (int step = 0; own && own->kind == LR_ALIAS && own->target && step < 8; step++) {
      /* an alias found through an extended module is that module's: its
         target is the module's instance method, which this search does
         not walk, so it keeps the builtin mixin as before */
      int an = own->node;
      if (an < 0 || an >= x->c->node_cap || x->c->node_cbody[an] != oc) { own = NULL; break; }
      const LrDef *to = NULL;
      int tc = oc;
      if (lr_sg_chain_find(x->c, oc, own->target, elab_point_of(x->c, own->node), &to, &tc) != ELAB_YES) { own = NULL; break; }
      own = to; oc = tc;
    }
    /* the definition the call finds there: a def or a define_method, by
       its node (codegen reads its body) */
    int dn = own && (own->kind == LR_DEF || own->kind == LR_DEFINE_METHOD) ? own->node : -1;
    if (dn >= 0) {
      nt_node_set_int((NodeTable *)nt, node, "mixin_own_call", dn + 1);
      mix = 0; naming = 0;
    }
  }
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
      if (!vis_arg_name(nt, ev[k])) opaque = LR_OPAQUE_DEFINES;
    if (!an_ && ak != NK_DefNode && (ak != NK_ArrayNode || !(bvis || cvis)) && naming && (!dm || i == 0)) opaque = LR_OPAQUE_DEFINES;   /* a computed name */
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
      /* `extend self` in a module's own body: an extend of the module on
         itself, at its point (CRuby's model: the module's singleton gets
         the module as a mixin) */
      int mod = nt_kind(nt, av[i]) == NK_SelfNode && self && !sg && cls == fc && fs == LR_FS_NONE &&
                fnode >= 0 && class_def_is_module(nt, fnode) && is_extend_name(nm)
                ? cls : lr_class_ref_at(x->c, av[i], NULL);
      /* a class among the arguments: TypeError where the statement runs
         (codegen reads the mark), and no mixin, not of the modules beside it */
      if (lr_mix_names_class(x->c, av, ac)) { nt_node_set_int((NodeTable *)nt, node, "mixin_class_arg", 1); continue; }
      if (mod < 0 && mod != LR_LATER) opaque = LR_OPAQUE_DEFINES;
      /* a constant the program defines nowhere: the statement raises
         NameError where it runs, and mixes nothing in (codegen reads the
         mark) */
      if (mod < 0 && mod != LR_LATER && lr_const_root_undefined(nt, av[i]))
        nt_node_set_int((NodeTable *)nt, node, "mixin_undefined_const", 1);
      if (!k) continue;
      k->mix = lr_grow(k->mix, &k->cmix, k->nmix, sizeof *k->mix);
      x->r->hgen++;
      k->mix[k->nmix++] = (LrMixin){ node, lr_mix_kind(nt, node, nm, sg), mod, av[i], .cond = !x->unplaced && x->r->at[node] >= 0 && x->r->st[x->r->at[node]].fmay,
                                     .applied = lr_stmt_applied(x->unplaced ? NULL : lr_stmt_of(x->r, node), x->unplaced, may) == 1, .open = fnode };
      /* `K.include M` in another class's body or at the top level, on a
         class of the program: a mixin the passes take as one of K's
         bodies' (elab_all_bodies), so the statement itself does nothing
         where it runs (lr_foreign_mixin) */
      if (lr_foreign_mixin(x, node, &k->mix[k->nmix - 1], cls, fc, sg, nested))
        nt_node_set_int((NodeTable *)nt, node, "mixin_by_record", 1);
      /* include, extend or prepend on the body's own class: the module's
         hook fires after it (elab_fire_after) */
      if (mod >= 0 && !nested && fs == LR_FS_NONE && !sg &&
          ((cls == fc && (recv < 0 || nt_kind(nt, recv) == NK_SelfNode)) || nt_int(nt, node, "mixin_by_record", 0)))
        lr_mix_event(x, cls, node, av[i], mod, lr_mix_kind(nt, node, nm, sg));
    }
    else if (is_undef_method_name(nm) || is_remove_method_name(nm)) {
      if (is_remove_method_name(nm) && lr_remove_shadowed(x->c, node, nm, cls, sg)) opaque = LR_OPAQUE_DEFINES;   /* (a program method answers: it may do anything) */
      else lr_method(x, cls, sg, an_, node, is_undef_method_name(nm) ? LR_UNDEF : LR_REMOVE, *vs, may, NULL);
    }
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
    lr_version(x->r, LR_KEY_OPAQUE, lr_const_owner(cls), "", (LrDef){ node, LR_OPAQUE, (unsigned char)opaque, (unsigned char)may, 0, NULL });
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
    (fs == LR_FS_SCLASS ? lr_sg_const_owner(fc) : lr_const_owner(fc)) : lr_path_const_owner(x->c, node, recv);
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
  x->cur = e; x->cfc = fc; x->cfs = (unsigned char)fs;
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

/* A history for each class registration has made so far: one the walk
   made (the record is made as the walk goes) */
static void lr_fit_classes(LoadRecord *r, Compiler *c) {
  if (c->nclasses + 1 <= r->ncls) return;
  r->cls = realloc(r->cls, sizeof *r->cls * (size_t)(c->nclasses + 1));
  if (!r->cls) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memset(r->cls + r->ncls, 0, sizeof *r->cls * (size_t)(c->nclasses + 1 - r->ncls));
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
  for (int i = 0; i < r->nev; i++) free(r->ev[i].name);
  r->nev = 0;
  if (r->cls) memset(r->cls, 0, sizeof *r->cls * (size_t)r->ncls);
  for (int b = 0; r->hhead && b < LR_BUCKETS; b++) r->hhead[b] = -1;
  r->nhist = 0; r->frontier = 0; r->nset = 0;
  r->slot_builds = -1;
  r->nentries = 0; r->indexed = r->opened = r->lated = -1;   /* the by-name index and the open and late bits are of the dropped histories */
  r->hgen++;
  free(r->surf); free(r->surf_at); r->surf = NULL; r->surf_at = NULL;
  free(r->bread); r->bread = NULL; r->nbread = 0;   /* (read through the dropped histories) */
}

static char *lr_dump_text(Compiler *c);
static void lr_check_walked(Compiler *c, const char *walked);
enum { LR_OWNERS_WALK = 1, LR_OWNERS_HELD = 2 };   /* lr_def_owners' checks */
static void lr_def_owners(Compiler *c, int check);
static void lr_later_settle(Compiler *c);
static void lr_block_cond_dms(Compiler *c);

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

static int lr_body_def(const NodeTable *nt, const int *par, int n, int node);
/* The versions method bodies make (LrDef.body): a `def K.m` on a constant
   the record resolves, in the body of a def, with no class body between,
   is a version of K's singleton history of m wherever the walk stands,
   recorded after the placed ones. It has no load point but an extent
   (lr_version_order): from the first statement the def's body may run on,
   any number of times, as CRuby defines it each time the body runs. Not
   on a class whose chain the record does not hold (lr_chain_open: a
   class a builder call makes, `S = Struct.new(:a) { }`): no call on it is
   ordered against a version, so its calls would meet the version as if
   it had run from the start. */
static void lr_body_versions(Compiler *c) {
  const NodeTable *nt = c->nt;
  LoadRecord *r = c->lrec;
  const int *par = r->plan_depth ? r->plan_par : r->up;
  NT_FOREACH_KIND(nt, NK_DefNode, id) {
    int recv = nt_ref(nt, id, "receiver"), body = lr_body_def(nt, par, r->n, id);
    NodeKind rk = recv >= 0 ? nt_kind(nt, nt_def_receiver(nt, recv)) : NK_SelfNode;
    if (id >= r->n || body <= 0 || (rk != NK_ConstantReadNode && rk != NK_ConstantPathNode)) continue;
    ElabRec x = { c, r, .unplaced = 1, .body = body };
    lr_def(&x, id, LR_MAIN, LR_FS_NONE, BVIS_PUBLIC, 1);
  }
}

/* Each def the record answered held for (lr_body_held, its first answer
   kept) against the answer the finished record gives, and recorded as a
   body version: a held def that is not, or a kept yes the record now
   answers no to, is an input the first answer read before it was final.
   A kept no the record now answers yes to is the walk's (a class it had
   not reached), which lr_body_held keeps on purpose. */
static void lr_check_held(Compiler *c) {
  LoadRecord *r = c->lrec;
  const NodeTable *nt = c->nt;
  LrCheck *f = lr_family(r, "body held");
  for (int i = 0; i < r->nhist; i++) {
    if (r->hist[i].a != LR_KEY_HELD || !r->hist[i].n || !r->hist[i].v[r->hist[i].n - 1].vis) continue;
    int def = r->hist[i].b, recv = nt_ref(nt, def, "receiver"), found = 0, now;
    const LrHist *h = recv >= 0 ? lr_hist(r, lr_class_ref_at(c, nt_def_receiver(nt, recv), NULL), 1, nt_str(nt, def, "name"), 0) : NULL;
    for (int j = 0; h && j < h->n; j++) found |= h->v[j].node == def && h->v[j].body;
    f->asked++;
    if ((now = lr_body_holds(c, def)) && found) continue;
    f->differ++;
    if (r->shown++ >= LR_CHECK_SHOW) continue;
    char d[96];
    lr_node_desc(c, def, d, sizeof d);
    fprintf(stderr, "spinel-elab-check: body held def=%s recorded=%d now=%d\n", d, found, now);
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
  const NodeKind *kinds = elab_const_write_kinds(&nk);
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
  /* a record builder's block (`k = Struct.new(:a) do ... end`) that is no
     statement of the load phase (one in a method's body, in an expression):
     its class's body all the same, after the ones that have a point */
  for (int ci = 0; ci < c->nclasses; ci++) {
    int d = c->classes[ci].def_node;
    NodeKind k = d >= 0 ? nt_kind(nt, d) : NK_NilNode;
    int body = k == NK_ConstantWriteNode || k == NK_LocalVariableWriteNode ? class_def_body(nt, d) : -1;
    LrClass *kc = body >= 0 ? lr_class(r, ci) : NULL;
    int have = !kc;
    for (int i = 0; !have && i < kc->nopen; i++) have = kc->open[i].body == body;
    if (have) continue;
    kc->open = lr_grow(kc->open, &kc->copen, kc->nopen, sizeof *kc->open);
    kc->open[kc->nopen++] = (LrOpen){ d, body };
    lr_body_defs(c, ci, body, d);
  }
  lr_body_versions(c);
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

/* The method scope each named module_function copy copies: that of the def
   or define_method live where `module_function :n` stands, followed through
   the aliases to the one each captured (a bare copy took its def's scope as
   lr_method recorded it). Each step asks at an alias's own point, where only
   a version recorded before it is live, so the chain ends. Asked once the
   record is made, since what is live at a point depends on the entries after
   it; a copy of a method with no scope (an attribute, a method a mixin
   brought) keeps none. */
static void lr_copy_scopes(Compiler *c) {
  LoadRecord *r = c->lrec;
  for (int i = 0; i < r->nhist; i++) {
    LrHist *h = &r->hist[i];
    if (h->a < LR_MAIN || !h->b) continue;
    for (int j = 0; j < h->n; j++) {
      LrDef *v = &h->v[j];
      if (v->kind != LR_COPY || v->scope > 0 || !v->target) continue;
      const char *nm = v->target;
      LoadPoint p = elab_point_of(c, v->node);
      for (;;) {
        const LrDef *d = NULL;
        if (elab_method_at(c, h->a, nm, 0, p, &d) != ELAB_YES || !d) break;
        if (d->kind == LR_DEF || d->kind == LR_DEFINE_METHOD) { v->scope = d->scope; break; }
        if (d->kind != LR_ALIAS || !d->target) break;
        nm = d->target;
        p = elab_point_of(c, d->node);
      }
    }
  }
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

static void lr_mixin_dups_settle(Compiler *c);

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
  lr_refl_take(c);
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
    lr_check_held(c);
    char *walked = lr_dump_text(c);
    lr_record_again(&x);
    lr_check_walked(c, walked);
    free(walked);
  }
  lr_autoload_values(c);
  r->rec = NULL;
  free(x.fc); free(x.fs); free(x.vs); free(x.vmain);
  lr_mixin_dups_settle(c);
  lr_copy_scopes(c);
  lr_def_owners(c, r->check ? LR_OWNERS_WALK | LR_OWNERS_HELD : 0);
  elab_mark_versions(c);
  lr_block_cond_dms(c);
  sp_timing_end(t0, "an_elab_record", "");
}

static int lr_applied(Compiler *c, const LrDef *v);

/* A define_method in a builder's block (`S = Struct.new(:a) do ... end`)
   that the load phase does not apply whenever the block runs
   (elab_def_applied: under a run-time condition, in a loop, a case, a
   rescue) defines its method only when that code runs. Its open history's
   entry answers a call (ver_named_one); it is marked cond_lone, so that a
   question about the method by name is refused (ver_refl_named). One left
   without a class is refused, as a def there is. The block is the body of
   the class the write made, whose nodes the walk gave that class
   (node_cbody). */
static void lr_block_cond_dms(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  unsigned char *blk = calloc((size_t)c->nclasses + 1, 1);
  if (!blk) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int ci = 0; ci < c->nclasses; ci++) {
    int nb = 0;
    const LrOpen *o = elab_class_bodies(c, ci, &nb);
    for (int i = 0; i < nb && !blk[ci]; i++)
      blk[ci] = nt_kind(nt, o[i].node) == NK_ConstantWriteNode || nt_kind(nt, o[i].node) == NK_LocalVariableWriteNode;
  }
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    int dn = sc->def_node, ci = dn >= 0 && dn < c->node_cap ? c->node_cbody[dn] : -1;
    if (sc->is_cmethod || ci < 0 || ci >= c->nclasses || !blk[ci]) continue;
    if (nt_kind(nt, dn) != NK_CallNode || !is_define_method_name(nt_str(nt, dn, "name"))) continue;
    if ((sc->class_id >= 0 && elab_def_applied(c, dn)) || nt_int(nt, dn, "dm_host", 0)) continue;   /* (dm_host: in a method's body) */
    if (sc->class_id >= 0) { nt_node_set_int(nt, dn, "cond_lone", 1); continue; }
    nt_node_set_int(nt, dn, "struct_cond_dm", 1);
    nt_node_set_str(nt, dn, "name", "raise");
    nt_node_set_ref(nt, dn, "receiver", -1);
    nt_node_set_ref(nt, dn, "block", -1);
    nt_node_set_ref(nt, dn, "arguments", -1);
  }
  free(blk);
}

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
   scopes); LR_OWNERS_HELD, each def or define_method the walk made a scope
   of that the record has no owner for, and why. */
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
      if (sc->class_id != cls || sc->is_cmethod != h->b) { sc->class_id = cls; sc->is_cmethod = h->b; comp_touch_scope(sc->idx); c->lrec->hgen++; }
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
   class stays (lr_block_cond_dms refuses one in a class builder's block);
   in a body the walk does not place, one of its own statements, as
   lr_body_defs has them. */
static void lr_unrolled_owner(Compiler *c, int call, int ci) {
  int dn = nt_kind(c->nt, call) == NK_CallNode ? dm_each_inner(c->nt, call) : -1;
  for (int k = 0; dn >= 0 && k < c->nscopes; k++)
    if (c->scopes[k].def_node == dn && c->scopes[k].class_id != ci) { c->scopes[k].class_id = ci; comp_touch_scope(c->scopes[k].idx); c->lrec->hgen++; }
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
        lr_version(r, LR_KEY_OPAQUE, lr_const_owner(i - 1), "", (LrDef){ k->mix[j].node, LR_OPAQUE, LR_OPAQUE_DEFINES, (unsigned char)elab_point_may(c, k->mix[j].node), 0, NULL });
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
      int o = !seen && t->cls >= 0 ? lr_opaque_call(c, t->node, t->cls, t->sg) : 0;
      if (o) lr_version(r, LR_KEY_OPAQUE, lr_const_owner(t->cls), "", (LrDef){ t->node, LR_OPAQUE, (unsigned char)o, (unsigned char)t->may, 0, NULL });
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
typedef struct LrVers { const char *name; int first, nver, next, pend, alias; unsigned char captured, replaced, late; } LrVers;   /* captured: elab_version_captured's memo, 1 no, 2 yes; replaced: one history holds two versions, or an alias; late: lr_late_make's, 1 no history late, 2 one is, 3 none asked; pend: the histories asked that are not late yet; alias: the first alias of the name, in LoadRecord.valias, -1 for none */

static int lr_is_version(const LrDef *v) { return v->kind <= LR_REMOVE && !v->unplaced && !v->absent; }

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
      r->vers[k] = (LrVers){ h->name, -1, 0, r->vers_head[b], 0, -1 };
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
  /* each alias on its target's entry, where the target has a history */
  r->nvalias = 0;
  for (int i = 0; i < r->nhist; i++)
    for (int j = 0; r->hist[i].a >= LR_MAIN && j < r->hist[i].n; j++) {
      const LrDef *v = &r->hist[i].v[j];
      if (v->kind != LR_ALIAS || !v->target || v->unplaced || v->absent) continue;
      int k = r->vers_head[lr_hash(0, 0, v->target)];
      while (k >= 0 && !sp_streq(r->vers[k].name, v->target)) k = r->vers[k].next;
      if (k < 0) continue;
      r->valias = lr_grow(r->valias, &r->cvalias, r->nvalias, sizeof *r->valias);
      r->valias[r->nvalias] = (struct LrValias){ i, j, r->vers[k].alias };
      r->vers[k].alias = r->nvalias++;
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

static int lr_refl_has(char *const *l, int n, const char *name) {
  return n && bsearch(&name, l, (size_t)n, sizeof *l, lr_name_cmp);
}
/* Does a respond_to? the cells may answer (refl_cell) answer `name` from
   the table after all: where the side is not a class's own class side
   (cside 0), in a program that defines respond_to_missing?
   (respond_to_missing_defined), or where one of the name's versions
   undefines or removes it (cplan_cmethod_presence declines them)? */
static int lr_refl_cell_table(Compiler *c, const char *name, int cside) {
  LoadRecord *r = c->lrec;
  if (!lr_refl_has(r->refl_cell, r->nrefl_cell, name)) return 0;
  if (!cside || respond_to_missing_defined(c)) return 1;
  const LrVers *x = lr_vers(r, name);
  for (int h = x ? x->first : -1; h >= 0; h = r->hist[h].nlink)
    for (int j = 0; j < r->hist[h].n; j++)
      if (r->hist[h].v[j].kind == LR_UNDEF || r->hist[h].v[j].kind == LR_REMOVE) return 1;
  return 0;
}
/* the first version of history h, NULL for none */
static const LrDef *lr_first_version(const LrHist *h) {
  for (int j = 0; j < h->n; j++) if (lr_is_version(&h->v[j])) return &h->v[j];
  return NULL;
}
/* Is the late bit asked of history h of x's name, one of nh histories
   that hold a version (lr_late_make)? */
static int lr_late_asked(Compiler *c, const LrVers *x, const LrHist *h, int nh) {
  const LoadRecord *r = c->lrec;
  int cside = h->a >= 0 && h->b;
  if (!lr_first_version(h) || r->refl_any || lr_refl_has(r->refl, r->nrefl, x->name) || lr_refl_cell_table(c, x->name, cside) ||
      (!cside && lr_refl_has(r->refl_capt, r->nrefl_capt, x->name)) || (nh > 1 && !cside)) return 0;
  return (cside && (bop_class_answers(x->name, 0, 0) || elab_record_cmethod(c, h->a, x->name))) ||
         !(builtin_object_method_known(x->name) || builtin_instance_name_known(x->name));
}
/* The late bit of every method history (LrHist.late), once for all of
   them, as of the entries recorded: a call of the name may run before the
   history's first version is made, its statement, or the window of the
   bodies around it (elab_call_window), opening ahead of that version's
   point in its region; any call, where a method body makes that version
   (LrDef.body). The first version opens the method; before it the name
   is absent on that class and side. Whether a call meets that is the
   typed receiver's fact (ct_version_at), asked of the histories on its
   chain (elab_versions_late). Not asked of a history of a name a
   builtin's method answers (Object's, or any builtin class's): such a
   call may reach the builtin's, which no entry calls; but a class's (or
   module's) own class-side history is, of a name the class answers on
   itself as a reader, with no argument or block (bop_class_answers), or
   of a record class's own builtin (elab_record_cmethod), whose entry's
   else arm calls that builtin (VK_BUILTIN). The ask is of
   the name's shape, not one call's; each entry asks again at its own
   arity and keeps the table's answer where the builtin does not take it.
   Of a name of two or more histories, only the class-side ones are
   asked: an instance's chain keeps the table's. Nor one a reflection
   reader names (lr_refl_take): a call it guards would take the entry
   where the reader answers the method is there, and raise where the
   table's answer runs the def. A class's own class-side history is
   asked all the same where the reader is a respond_to? the cells answer
   (refl_cell), which reads them as the call does
   (cplan_cmethod_presence), unless the table answers it after all
   (lr_refl_cell_table), or a Method's maker that takes the entry
   (refl_capt), which binds as the call does. One the record cannot read
   (refl_any) stops every name. LrVers.late is 2 where some history of
   the name is late. */
static void lr_late_make(Compiler *c) {
  LoadRecord *r = c->lrec;
  lr_vers_index(r);
  if (r->lated == r->nentries) return;
  r->lated = r->nentries;
  for (int k = 0; k < r->nvers; k++) {
    LrVers *x = &r->vers[k];
    int nh = 0;
    for (int h = x->first; h >= 0; h = r->hist[h].nlink) nh += lr_first_version(&r->hist[h]) != NULL;
    x->pend = 0;
    x->late = 3;
    for (int h = x->first; h >= 0; h = r->hist[h].nlink) {
      LrHist *y = &r->hist[h];
      y->late = lr_late_asked(c, x, y, nh) ? 1 : 3;
      if (y->late == 3) continue;
      if (lr_first_version(y)->body) y->late = x->late = 2;   /* (a method body's: before its extent opens, lr_version_order) */
      else { x->pend++; if (x->late == 3) x->late = 1; }
    }
  }
  NT_FOREACH_KIND(c->nt, NK_CallNode, id) {
    const char *nm[2];
    int nn = call_node_names(c, id, nm);
    ElabWindow w;
    int win = -1;   /* the node's window: not opened yet, none, or open */
    for (int k = 0; k < nn; k++) {
      LrVers *x = lr_vers(r, nm[k]);
      for (int h = x ? x->first : -1; h >= 0 && x->pend; h = r->hist[h].nlink) {
        LrHist *y = &r->hist[h];
        if (y->late != 1) continue;
        LoadPoint q = elab_point_of(c, lr_first_version(y)->node);
        if (q.region < 0) continue;
        if (win < 0) win = elab_call_window(c, id, &w) == ELAB_YES;
        if (!win) break;
        if (w.from.region == q.region && w.from.seq < q.seq) { y->late = x->late = 2; x->pend--; }
      }
    }
  }
}
int elab_method_late(Compiler *c, const char *name) {
  LoadRecord *r = c->lrec;
  const LrVers *x = r && r->recorded ? lr_vers(r, name) : NULL;
  if (!x || !x->nver) return 0;
  lr_late_make(c);
  return x->late == 2;
}
int elab_versions_late(Compiler *c, const char *name, const ElabVersion *vs, int n) {
  LoadRecord *r = c->lrec;
  const LrVers *x = r && r->recorded ? lr_vers(r, name) : NULL;
  int late = 1;
  if (!x || !x->nver) return 1;
  lr_late_make(c);
  if (n < 0) return x->late == 2 ? 2 : 3;
  for (int i = 0; i < n && late != 2; i++)
    if (r->hist[vs[i].hist].late != 1) late = r->hist[vs[i].hist].late;
  return late;
}

/* Does the record hold an extend, or a singleton class's include or
   prepend: a module's instance methods become an object's own, a class's
   class methods among them? */
static int lr_singleton_mixed(const LoadRecord *r) {
  for (int k = 0; k < r->ncls; k++)
    for (int j = 0; j < r->cls[k].nmix; j++)
      if (elab_mix_on_singleton(r->cls[k].mix[j].kind)) return 1;
  return 0;
}
int elab_method_reflected(Compiler *c, const char *name, int sg) {
  LoadRecord *r = c->lrec;
  if (!r || !r->recorded || !name) return 0;
  sg = sg != 0;
  return r->refl_any || lr_refl_has(r->refl_table[sg], r->nrefl_table[sg], name) ||
         (sg && lr_refl_has(r->refl_table[0], r->nrefl_table[0], name) && lr_singleton_mixed(r)) ||
         lr_refl_cell_table(c, name, sg);
}

int elab_method_asked(Compiler *c, const char *name) {
  const LoadRecord *r = c->lrec;
  if (!r || !r->recorded || !name) return 0;
  return r->refl_any || lr_refl_has(r->refl, r->nrefl, name) || lr_refl_has(r->refl_cell, r->nrefl_cell, name) ||
         lr_refl_has(r->refl_capt, r->nrefl_capt, name);
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
   Scope.versioned when the record holds more than one version of its name
   (a mixin's hidden copy's: the program's name of it), or one that a call
   may run before (elab_method_late);
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
    /* (a mixin's hidden copy, a layer below a later one, by the program's
       name: its versions are the module's, told apart under its own) */
    const char *name = sc->mixin_guarded ? comp_scope_user_name(sc) : sc->name;
    const char *vname = (sc->is_include_copy || sc->is_extend_copy) && sc->shadow ? comp_scope_user_name(sc) : name;
    const LrVers *x = vname && sc->def_node >= 0 ? lr_vers(r, vname) : NULL;
    if (!x || (x->nver < 2 && !elab_method_late(c, vname))) continue;
    { sc->versioned = 1; comp_touch_scope(sc->idx); }
    /* an accessor's version (elab_attr_scope) is one the def that ends its
       history replaces; its scope, made after that def's, takes no key */
    if (sc->attr_version) { sc->replaced = 1; continue; }
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

/* Does node run when its statement does, for certain: is every block
   around it one that runs at once where its call runs (elab_block_runs:
   ELAB_BLK_NOW, a builtin iterator on a literal), with no END body around
   it, up to a file's root? A block taken to run at once (ELAB_BLK_ASSUMED)
   or not known may run later (a program's `each` may keep it), and a node
   whose climb does not reach a root is not known either. */
int elab_blocks_run_now(Compiler *c, int node) {
  int n;
  const int *par = elab_node_parents(c, &n);
  int v = node;
  for (int p; v >= 0 && v < n && (p = par[v]) >= 0 && p < n; v = p) {
    NodeKind k = nt_kind(c->nt, p);
    if (k == NK_PostExecutionNode) return 0;
    if (k == NK_BlockNode && (par[p] < 0 || elab_block_runs(c, par[p]) != ELAB_BLK_NOW)) return 0;
  }
  return v >= 0 && v < n && par[v] < 0 && nt_kind(c->nt, v) == NK_ProgramNode;
}

/* The record's readings of a method body's block (lr_def_block): kept for
   later, for certain (a store, a closure that escapes, a hand-on to a call
   that keeps it); not proven; not kept, the block dropped or tested; run
   before the body answers. Combined over a body's uses by lr_def_block;
   one use's reading is the lowest of its parts'. */
enum { LRB_KEEPS = -1, LRB_UNSURE = 0, LRB_NOW = 1, LRB_RUNS = 2 };
static int lr_def_block(Compiler *c, int def);
static int lr_block_value(Compiler *c, int x, int def, const char **names, int *nn);

/* When call q, in the body of def node `def`, runs the block the body
   was given, which q gets as its block's code (a yield or a read of the
   parameter in it) or handed on (`&b`, `&`, `...`, a super's): LRB_KEEPS
   for a call that runs it later than it answers (is_deferred_block_call,
   a lazy chain) or a method of the program's whose body keeps it; LRB_RUNS
   for a builtin iterator on a literal, a method of the program's that does
   not keep it, and a super that reaches Object's builtin of an iterator's
   name that runs it at once (object_iter_runs_now); else LRB_UNSURE. A
   receiverless callee is main's method only where no builtin class
   answers the name (the body runs on a literal, whose class would answer
   first), and is read only when the program defines it once: the body is
   read for every call that reaches it, not as of one. */
static int lr_hand_on(Compiler *c, int q, int def) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, q);
  if (k == NK_SuperNode || k == NK_ForwardingSuperNode)
    return object_iter_runs_now(nt_str(nt, def, "name")) ? LRB_RUNS : LRB_UNSURE;
  if (k != NK_CallNode) return LRB_UNSURE;
  int w = elab_block_when(c, q, 1);
  if (w == ELAB_BLK_LATER) return LRB_KEEPS;
  int t = lr_target_runs_block(c, q);
  if (t) return t > 0 ? LRB_RUNS : LRB_KEEPS;
  const char *nm = nt_str(nt, q, "name");
  int rv = nt_ref(nt, q, "receiver");
  if ((rv < 0 || nt_kind(nt, rv) == NK_SelfNode) && nm && lr_defines(c->lrec, nm)) {
    ElabVersion v[2];
    if (builtin_instance_name_known(nm) || elab_history_versions(c, LR_MAIN, 0, nm, v, 2) != 1 ||
        v[0].v->kind != LR_DEF) return LRB_UNSURE;
    return lr_def_block(c, v[0].v->node);
  }
  return w == ELAB_BLK_NOW ? LRB_RUNS : LRB_UNSURE;
}

/* Does call q make a Proc of its block (`proc { }`, `lambda { }`,
   `Proc.new { }`), whose value then decides when the block runs? */
static int lr_proc_made(Compiler *c, int q) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_kind(nt, q) == NK_CallNode ? nt_str(nt, q, "name") : NULL;
  int rv = nm ? nt_ref(nt, q, "receiver") : -1;
  if (!nm) return 0;
  if (rv < 0) return is_proc_constructor(nm);
  return nt_kind(nt, rv) == NK_ConstantReadNode && is_proc_new(nt_str(nt, rv, "name"), nm);
}

/* How node x, in the body of def node `def`, holds the block the body was
   given, through the blocks and closures around it up to the def: a
   closure made there (a lambda, a Proc made of a block) as its value does
   (lr_block_value); a block as its call does (lr_hand_on); a nested def or
   an END keeps it. LRB_RUNS when every one runs it at once. */
static int lr_body_capture(Compiler *c, int x, int def, const char **names, int *nn) {
  int n, res = LRB_RUNS;
  const int *par = elab_node_parents(c, &n);
  for (int p = x >= 0 && x < n ? par[x] : -1; p >= 0 && p < n && p != def; p = par[p]) {
    NodeKind k = nt_kind(c->nt, p);
    int h;
    if (k == NK_DefNode || k == NK_PostExecutionNode) return LRB_KEEPS;
    if (k == NK_LambdaNode) h = lr_block_value(c, p, def, names, nn);
    else if (k != NK_BlockNode) continue;
    else if (par[p] < 0) h = LRB_UNSURE;
    else h = lr_proc_made(c, par[p]) ? lr_block_value(c, par[p], def, names, nn) : lr_hand_on(c, par[p], def);
    if (h == LRB_KEEPS) return h;
    if (h < res) res = h;
  }
  return res;
}

/* How the value of node x (a read of the block parameter, a closure that
   holds it, or a call on either), in the body of def node `def`, holds the
   block, climbed by the value edges (vp_climb): the receiver of call, (),
   [] or yield: run there; of another call: as that call's value
   (`b.to_proc.call`); handed on as `&b`: as the call does (lr_hand_on);
   written whole to a local: that local's reads stand for it too (names,
   *nn); written anywhere else, an element of an Array or Hash literal, an
   argument, a return's value or the body's: kept; a statement whose value
   is dropped, a truth test or a loop body: not kept. Any other place is
   not proven (LRB_UNSURE). */
static int lr_block_value(Compiler *c, int x, int def, const char **names, int *nn) {
  const NodeTable *nt = c->nt;
  int n;
  const int *par = elab_node_parents(c, &n);
  for (;;) {
    VpUp up;
    VpUpKind k = vp_climb(nt, par, x, VPS_ALL, &up);
    if (k == VPU_DROPPED) return LRB_NOW;
    if (k != VPU_ENDED) return LRB_UNSURE;
    int p = up.parent;
    if (vp_up_def(nt, &up) == def) return LRB_KEEPS;   /* the body's value */
    switch (nt_kind(nt, p)) {
      case NK_CallNode:
        if (nt_ref(nt, p, "receiver") != up.node) return LRB_UNSURE;
        if (is_proc_invoke(nt_str(nt, p, "name"))) return LRB_RUNS;
        x = p;
        continue;
      case NK_BlockArgumentNode:
        if (p >= n || par[p] < 0) return LRB_UNSURE;
        /* a Proc made of the block (`proc(&b)`, `Proc.new(&b)`) is a closure
           like a literal block's: its own value decides, as lr_body_capture
           reads it */
        return lr_proc_made(c, par[p]) ? lr_block_value(c, par[p], def, names, nn) : lr_hand_on(c, par[p], def);
      case NK_LocalVariableWriteNode: {
        const char *wn = nt_str(nt, p, "name");
        for (int i = 0; wn && i < *nn; i++) if (sp_streq(names[i], wn)) return LRB_NOW;
        if (!wn || *nn >= 4) return LRB_UNSURE;
        names[(*nn)++] = wn;
        return LRB_NOW;
      }
      case NK_InstanceVariableWriteNode: case NK_InstanceVariableOrWriteNode: case NK_InstanceVariableAndWriteNode:
      case NK_InstanceVariableOperatorWriteNode: case NK_GlobalVariableWriteNode: case NK_GlobalVariableOrWriteNode:
      case NK_GlobalVariableAndWriteNode: case NK_GlobalVariableOperatorWriteNode: case NK_ClassVariableWriteNode:
      case NK_ClassVariableOrWriteNode: case NK_ClassVariableAndWriteNode: case NK_ClassVariableOperatorWriteNode:
      case NK_ConstantWriteNode: case NK_ConstantPathWriteNode: case NK_MultiWriteNode:
      case NK_ArrayNode: case NK_HashNode: case NK_KeywordHashNode: case NK_AssocNode: case NK_SplatNode:
      case NK_ArgumentsNode: case NK_ReturnNode:
        return LRB_KEEPS;
      case NK_IfNode: case NK_UnlessNode: case NK_WhileNode: case NK_UntilNode: case NK_AndNode: case NK_OrNode:
        return LRB_NOW;   /* (a predicate, a loop body, `&&`'s left operand) */
      default:
        return LRB_UNSURE;
    }
  }
}

/* Is node x in a closure (a lambda, `proc { }`) before any def? Where that
   closure runs is not where x is. */
static int lr_in_closure(Compiler *c, const int *par, int n, int x) {
  for (int p = x >= 0 && x < n ? par[x] : -1; p >= 0 && p < n; p = par[p]) {
    NodeKind k = nt_kind(c->nt, p);
    if (k == NK_DefNode) return 0;
    if (k == NK_LambdaNode || (k == NK_BlockNode && par[p] >= 0 && lr_proc_made(c, par[p]))) return 1;
  }
  return 0;
}
/* The call that runs value x as its receiver (is_proc_invoke), for
   elab_closure_runs, where that call is not itself in a closure; -1 where
   x goes anywhere else */
static int lr_closure_use(Compiler *c, const int *par, int n, int x) {
  VpUp up;
  VpUpKind k = vp_climb(c->nt, par, x, VPS_ALL, &up);
  return k == VPU_ENDED && nt_kind(c->nt, up.parent) == NK_CallNode && nt_ref(c->nt, up.parent, "receiver") == up.node &&
         is_proc_invoke(nt_str(c->nt, up.parent, "name")) && !lr_in_closure(c, par, n, up.parent) ? up.parent : -1;
}
int elab_closure_runs(Compiler *c, int id, int *sites, int cap) {
  const NodeTable *nt = c->nt;
  int n, lam = -1, ns = 0, at;
  const int *par = elab_node_parents(c, &n);
  for (int p = id >= 0 && id < n ? par[id] : -1; p >= 0 && p < n && lam < 0; p = par[p]) {
    NodeKind k = nt_kind(nt, p);
    if (k == NK_DefNode || (k == NK_BlockNode && !(par[p] >= 0 && lr_proc_made(c, par[p])))) return -1;
    if (k == NK_LambdaNode) lam = p;
    else if (k == NK_BlockNode) lam = par[p];
  }
  VpUp up;
  if (lam < 0) return -1;
  if (vp_climb(nt, par, lam, VPS_ALL, &up) != VPU_ENDED || nt_kind(nt, up.parent) != NK_LocalVariableWriteNode) {
    if ((at = lr_closure_use(c, par, n, lam)) >= 0) {
      if (cap < 1) return -1;
      sites[ns++] = at;
    }
    return ns;
  }
  /* written whole to a local: each read of it in the same body that
     invokes it (a write that reads it too, `l ||= ...`, does not) */
  static const NodeKind reads[] = { NK_LocalVariableReadNode, NK_LocalVariableOperatorWriteNode,
                                    NK_LocalVariableOrWriteNode, NK_LocalVariableAndWriteNode };
  const char *ln = nt_str(nt, up.parent, "name");
  int body = elab_body_def(c, up.parent);
  for (int j = 0; ln && j < 4; j++)
    NT_FOREACH_KIND(nt, reads[j], x) {
      if (x >= n || !sp_streq(nt_str(nt, x, "name"), ln) || elab_body_def(c, x) != body) continue;
      if (!j && (at = lr_closure_use(c, par, n, x)) >= 0) {
        if (ns >= cap) return -1;
        sites[ns++] = at;
      }
    }
  return ns;
}

/* How the body of def node `def` holds the block it is given (LRB_*), from
   its uses: a yield, a read of the block parameter (or of a local it, or a
   closure holding it, is written to whole: `blk = b`, the &-parameter
   rewrite's `__bpv_b = b`, `l = lambda { yield }`) as a value
   (lr_block_value), a hand-on of the block it does not name (`&`, `...`, a
   super that takes no block of its own), each through the blocks and
   closures around it (lr_body_capture). It keeps the block when some use
   keeps it and none runs it now or is unsure: a body that also runs it now
   (or may) keeps the call's reading by name, which a run now answers
   right. It runs it now when every use does or drops it, at least one
   runs it, and none keeps it or is unsure. A write of the parameter drops
   the block, and a yield under a condition runs now or not at all, as an
   iterator's zero runs. Read through the def's own nodes, once per def
   (bread); a def asked while its own reading is in progress (a recursive
   hand-on) answers LRB_UNSURE. A def read inside such a cycle (two helpers
   that hand on to each other) caches the LRB_UNSURE it got, and a later ask
   for that def alone gets it where a fresh reading might be definite. That
   is safe, for LRB_UNSURE keeps the call's reading by name, and the answer
   is the same on every compile of a program, though it depends on which
   def is asked first. */
static int lr_def_block(Compiler *c, int def) {
  NodeTable *nt = (NodeTable *)c->nt;
  LoadRecord *r = c->lrec;
  if (def < 0 || def >= nt->count || nt_kind(nt, def) != NK_DefNode) return LRB_UNSURE;
  if (def >= r->nbread) {
    signed char *nb = realloc(r->bread, (size_t)nt->count);
    if (!nb) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memset(nb + r->nbread, 0, (size_t)(nt->count - r->nbread));
    r->bread = nb; r->nbread = nt->count;
  }
  if (r->bread[def]) return r->bread[def] == 1 ? LRB_UNSURE : r->bread[def] - 3;
  r->bread[def] = 1;
  int ps = nt_ref(nt, def, "parameters"), bp = ps >= 0 ? nt_ref(nt, ps, "block") : -1;
  const char *names[4] = { bp >= 0 ? nt_str(nt, bp, "name") : NULL };
  int nn = names[0] ? 1 : 0, nids = 0, keeps = 0, runs = 0, unsure = 0;
  int *ids = nt_subtree_ids(nt, &def, 1, &nids);
  for (int pass = 0, grew = 1; grew && !(keeps && (runs || unsure)); pass++) {
    int before = nn;
    for (int i = 0; i < nids && !(keeps && (runs || unsure)); i++) {
      int x = ids[i], h = LRB_NOW;
      if (elab_body_def(c, x) != def) continue;
      switch (nt_kind(nt, x)) {
        case NK_YieldNode:
          if (pass == 0) h = lr_body_capture(c, x, def, names, &nn);
          break;
        case NK_SuperNode: case NK_ForwardingSuperNode:
          if (pass == 0 && nt_ref(nt, x, "block") < 0) {
            h = lr_body_capture(c, x, def, names, &nn);
            if (h != LRB_KEEPS) { int o = lr_hand_on(c, x, def); h = o < h ? o : h; }
          }
          break;
        case NK_ForwardingArgumentsNode: case NK_BlockArgumentNode: {
          if (pass || (nt_kind(nt, x) == NK_BlockArgumentNode && nt_ref(nt, x, "expression") >= 0)) break;
          int n, q = -1;
          const int *par = elab_node_parents(c, &n);
          for (q = x < n ? par[x] : -1; q >= 0 && q < n && nt_kind(nt, q) == NK_ArgumentsNode; q = par[q]) {}
          h = lr_body_capture(c, x, def, names, &nn);
          if (h != LRB_KEEPS) { int o = q >= 0 ? lr_hand_on(c, q, def) : LRB_UNSURE; h = o < h ? o : h; }
          break;
        }
        case NK_LocalVariableReadNode: {
          const char *vn = nt_str(nt, x, "name");
          int in = -1;
          for (int j = 0; vn && j < nn; j++) if (sp_streq(names[j], vn)) in = j;
          if (in < 0) break;
          h = lr_body_capture(c, x, def, names, &nn);
          if (h != LRB_KEEPS) { int o = lr_block_value(c, x, def, names, &nn); h = o < h ? o : h; }
          break;
        }
        default: break;
      }
      if (h == LRB_KEEPS) keeps = 1;
      else if (h == LRB_UNSURE) unsure = 1;
      else if (h == LRB_RUNS) runs = 1;
    }
    grew = nn > before;
  }
  free(ids);
  int res = keeps ? (runs || unsure ? LRB_UNSURE : LRB_KEEPS) : unsure ? LRB_UNSURE : runs ? LRB_RUNS : LRB_NOW;
  r->bread[def] = (signed char)(res + 3);
  return res;
}

/* The method of the program's that call `call` runs, as the record binds
   it: on a literal of a builtin class no header of the program reopens, a
   name the class does not answer itself reaches Object's, main's methods
   (a public top-level def): the version live at the call (an alias's
   original), whose body the record reads for how it holds the block
   (lr_def_block). 1 when it runs it only before it answers, -1 when it
   keeps it; 0 for none (the builtin's; a private one, which CRuby refuses
   to call there, so the block never runs), and for a body whose reading
   proves neither or a version the record cannot place at the call (a def
   in a loop's block): the call keeps its reading by name. */
static int lr_target_runs_block(Compiler *c, int call) {
  const NodeTable *nt = c->nt;
  LoadRecord *r = c->lrec;
  const char *nm = nt_kind(nt, call) == NK_CallNode ? nt_str(nt, call, "name") : NULL;
  int rv = nm ? nt_ref(nt, call, "receiver") : -1;
  const char *cls = rv >= 0 ? lr_literal_class(nt_kind(nt, rv)) : NULL;
  if (!r || r->nreopens || !cls || !lr_defines(r, nm) || builtin_answers_ahead_of(cls, nm, "Object")) return 0;
  LoadPoint p = elab_point_of(c, call);
  for (int hop = 0; hop < 8; hop++) {
    const LrDef *d = NULL;
    /* A call in a method body has no place in the load (no region): the
       version live there is the one at the load's end, which the body may
       not see when it runs. Read the callee only when main defines the name
       once and Object has no method of that name of its own, as lr_hand_on
       reads a receiverless callee: where Object answers it, the call
       reaches that until the def, and it runs the block at once. */
    if (p.region < 0) {
      ElabVersion hv[2];
      if (object_instance_method_name(nm) || elab_history_versions(c, LR_MAIN, 0, nm, hv, 2) != 1) return 0;
    }
    if (elab_method_at(c, LR_MAIN, nm, 0, p, &d) != ELAB_YES || !d) return 0;
    int vis = hop ? SP_VIS_PUBLIC : elab_method_vis_at(c, LR_MAIN, nm, 0, p);   /* (the called name's, not an alias's original's) */
    if (vis == SP_VIS_PRIVATE || vis == SP_VIS_PROTECTED) return 0;
    if (d->kind != LR_ALIAS || !d->target) return d->kind == LR_DEF ? lr_def_block(c, d->node) : 0;
    nm = d->target;
    p = elab_point_of(c, d->node);
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
static int lr_version_order(Compiler *c, const LrDef *v, LoadPoint p, int late);
static int lr_last_before(Compiler *c, const LrHist *h, LoadPoint p, int late, int skip_vis, const LrDef **out) {
  int unsure = 0;
  LoadRecord *r = c->lrec;
  int take = r->collect && h && h->a == -2 - (int)ELAB_CONST;   /* a constant's writes, for elab_class_candidates */
  int at = !late && p.region >= 0 && p.node >= 0 && p.node < r->n && r->at[p.node] == p.seq;
  for (int i = h ? h->n - 1 : -1; i >= 0; i--) {
    const LrDef *v = &h->v[i];
    if (take && v->unplaced) r->cand_bad = 1;
    if ((skip_vis && v->kind == LR_VIS) || v->unplaced || v->absent) continue;
    int in = at && !v->body && v->node >= 0 && v->node < r->n && r->at[v->node] == p.seq;
    int o = in ? lr_run_order(c, v->node, p.node) : PROG_UNKNOWN;
    if (o == PROG_UNKNOWN) o = lr_version_order(c, v, p, late);
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
  return comp_class_is_module(c, &c->classes[cls]) ? LR_UNKNOWN_CLASS : LR_MAIN;
}
/* The depth bound of the walks along a chain: a chain, with the modules
   mixed into it, never repeats a class or module, and the modules are
   among the program's classes (nclasses), so nclasses + 1 (main) bounds
   every path a walk takes, a superclass step, a mixin or the singleton
   tail alike. A guard against a cycle in a malformed record only; a walk
   under it reaches the chain's end. */
static int lr_chain_bound(Compiler *c) {
  return c->nclasses + 1;
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
   mixin. Answers given while the record is made are dropped at its end,
   and every mixin's is asked then (lr_mixin_dups_settle). The search
   starts at depth 0, so the kept answer does not depend on where it was
   first asked; an ask while it runs (modules
   that include each other) answers unknown. The modules its own call
   mixed in ahead of it count too, which the search at the call's point
   does not take as run: `extend M1, M2, M1` extends M1, then M2, and its
   first M1 stays behind M2 (the record holds a call's modules the last
   first). A prepend is a no-op only among the class's earlier prepends (CRuby
   prepends a module the class includes). main's top-level include is an include into Object: its ancestors
   are the program's reopening of Object's. */
static int lr_mixin_dup(Compiler *c, int cls, int sg, LrClass *k, int i) {
  LrMixin *m = &k->mix[i];
  if (lr_mixin_class(c, m) < 0) return ELAB_NO;
  if (!m->dup) {
    m->dup = 1 + ELAB_UNKNOWN;
    LoadPoint p = elab_point_of(c, m->node);
    int o = ELAB_NO, pre = m->kind == LR_PREPEND || m->kind == LR_SG_PREPEND;
    for (int j = 0; j < i && o != ELAB_YES; j++) {
      LrMixin *e = &k->mix[j];
      if ((pre || e->node == m->node) && e->kind == m->kind && lr_mixin_class(c, e) >= 0)
        o = e->mod == m->mod || lr_search(c, e->mod, 0, NULL, m->mod, p, 0, 0, NULL, 0) == ELAB_YES ? ELAB_YES : ELAB_NO;
    }
    if (o != ELAB_YES && !pre) o = lr_search(c, cls == LR_MAIN && lr_core(c->lrec, CORE_OBJECT) >= 0 ? lr_core(c->lrec, CORE_OBJECT) : cls, sg, NULL, m->mod, p, 0, 0, NULL, 0);
    m->dup = (unsigned char)(1 + o);
  }
  return m->dup - 1;
}
/* Every mixin's duplicate answer, asked once the record is made: an answer
   given while it was made saw only what was recorded by then, and one left
   to its first ask reads the point map as the table then is, which a pass
   that appends nodes between its asks (ver_entries_build's versions
   readers) has made again for every mixin */
static void lr_mixin_dups_settle(Compiler *c) {
  LoadRecord *r = c->lrec;
  for (int k = 0; k < r->ncls; k++)
    for (int i = 0; i < r->cls[k].nmix; i++) r->cls[k].mix[i].dup = 0;
  for (int k = 0; k < r->ncls; k++)
    for (int i = 0; i < r->cls[k].nmix; i++)
      lr_mixin_dup(c, k - 1, elab_mix_on_singleton(r->cls[k].mix[i].kind), &r->cls[k], i);
}

/* One ancestor's part of the search of `name` (or of module `mod`): cls's
   prepends (the last first), its own entries, its includes (the last
   first); a singleton's prepends, own methods and extends. ELAB_YES found,
   LR_STOP an undef stops the search there, ELAB_NO go on to the next,
   ELAB_UNKNOWN for a class the record does not hold (lr_class);
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
      int dup = lr_mixin_dup(c, cls, sg, k, i);
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
   entry before its header. The tail goes on to BasicObject's, which only the
   walk of the whole chain (lr_walk) reads. */
enum { LR_TAIL_N = 6 };
static int lr_sg_tail_of(Compiler *c, int cls, int tail[LR_TAIL_N]) {   /* where in tail cls's starts */
  const LoadRecord *r = c->lrec;
  int reopen = comp_core_class(c, CORE_CLASS_REOPEN);   /* (-1 with none: not main's) */
  tail[0] = reopen >= 0 ? reopen : LR_UNKNOWN_CLASS, tail[1] = lr_core(r, CORE_MODULE), tail[2] = lr_core(r, CORE_OBJECT);
  tail[3] = LR_MAIN, tail[4] = lr_core(r, CORE_KERNEL), tail[5] = lr_core(r, CORE_BASIC_OBJECT);
  return cls == LR_MAIN ? 2 : cls >= 0 && comp_class_is_module(c, &c->classes[cls]) ? 1 : 0;
}
/* ...for a hook the core defines on Class or Module (is_module_hook_name)
   only up to where Object's begin (tail[2]): theirs stand before Object's,
   and before main's top level's, which a `def inherited` there does not
   shadow */
static int lr_sg_tail_end(const char *name) {
  return name && is_module_hook_name(name) ? 2 : 5;
}
static int lr_sg_tail(Compiler *c, int cls, const char *name, int mod, LoadPoint p, int late, int vis, const LrDef **out, int depth, int *unsure) {
  int tail[LR_TAIL_N];
  for (int i = lr_sg_tail_of(c, cls, tail), end = lr_sg_tail_end(name); i < end; i++) {
    int t = tail[i];
    if (t < LR_MAIN || (t == LR_MAIN && lr_sg_tail_end(name) < 5)) continue;
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
  /* the superclass steps are a loop, not a call each: a chain is as deep as
     the program makes it, and the native stack is not */
  int unsure = 0, o;
  for (;; depth++) {
    if (depth > lr_chain_bound(c) || cls < LR_MAIN) { o = ELAB_UNKNOWN; break; }
    if (!name && cls == mod && !sg) { o = ELAB_YES; break; }
    if ((o = lr_search_one(c, cls, sg, name, mod, p, late, vis, out, depth, &unsure, 0)) != ELAB_NO) break;
    /* a class's chain ends at Object, whose methods main's top level
       defines (main's own singleton is not Object's) */
    int sup = lr_super(c, cls);
    if (sg && sup == LR_MAIN) sup = LR_UNKNOWN_CLASS;
    if (sup < LR_MAIN) { o = sg ? lr_sg_tail(c, cls, name, mod, p, late, vis, out, depth, &unsure) : ELAB_NO; break; }
    cls = sup;
  }
  if (o == LR_STOP) o = ELAB_NO;
  return unsure ? ELAB_UNKNOWN : o;
}

/* Does a call of `name` on class cls at p find a class method of the
   program's: the class's own singleton methods or its extends', then its
   superclasses' (not the instance methods of Class, Module, Object and
   Kernel every singleton chain ends in)? *out: the definition it finds. */
static int lr_sg_chain_find(Compiler *c, int cls, const char *name, LoadPoint p, const LrDef **out, int *found) {
  for (int d = 0; cls >= 0 && d <= lr_chain_bound(c); d++) {
    int unsure = 0, o = lr_search_one(c, cls, 1, name, -1, p, 0, 0, out, d, &unsure, 0);
    if (o == LR_STOP) return ELAB_NO;
    if (o == ELAB_YES && found) *found = cls;
    if (o == ELAB_YES) return unsure ? ELAB_UNKNOWN : ELAB_YES;
    if (o == ELAB_UNKNOWN || unsure) return ELAB_UNKNOWN;
    int sup = lr_super(c, cls);
    if (sup < 0) return ELAB_NO;
    cls = sup;
  }
  return ELAB_UNKNOWN;
}
static int lr_sg_chain_runs(Compiler *c, int cls, const char *name, LoadPoint p, const LrDef **out) {
  return lr_sg_chain_find(c, cls, name, p, out, NULL);
}
/* The chain of a class (its singleton's when sg), in lookup order, one walk for every reader of it (lr_search's
   order): on each class, its prepends', its own, its includes' or extends' (a module's own list in turn); then its
   superclass's; and where a singleton's chain ends, the instance methods of lr_sg_tail's classes, each one's own
   (main's own singleton: Object's, main's top level's, Kernel's). A reader is a visitor: own is asked each owner's own
   methods, mixin each mixin whose module the record holds no class of, builtin a class whose chain goes on in a
   builtin (the program's reopening of a core class, a record class, a class naming a builtin superclass; NULL: the
   record's superclass goes on), unsure a mixin whose module may already be among the ancestors (NULL: walked as it is),
   done the end of a class's or singleton's chain, past the tail. The chain's end is
   CRuby's core classes one at a time, each one's program reopening and then that class's own table: a singleton's
   Class, Module, Object (main's top level with it), Kernel, BasicObject, an instance's Object, Kernel, BasicObject.
   core (NULL: the reopenings only) is asked each one's own table, by CORE_*, right after its reopening. A visitor
   returns 0 to go on, else the walk ends with its code; the walk answers -1 where a class is not the record's or the
   chain is longer than it can be. tail_end: how much of the singleton tail is read (lr_sg_tail_end, LR_TAIL_N: all). */
static int lr_hist_versions(const LoadRecord *r, const LrHist *h, ElabVersion *out, int n, int cap);
enum { LR_W_STOP = 1, LR_W_END = 2 };
typedef struct LrWalker {
  int (*own)(void *ud, int cls, int sg);
  int (*mixin)(void *ud, int cls, int ref);
  int (*builtin)(void *ud, int cls, int sg);
  int (*done)(void *ud, int cls, int sg);
  int (*core)(void *ud, int core);
  int (*unsure)(void *ud, int cls, int ref);
  int tail_end;
} LrWalker;
static int lr_walk(Compiler *c, const LrWalker *w, void *ud, int cls, int sg, int depth);
/* mixin i of cls (class record k): nothing where its module is already among the ancestors (CRuby's no-op re-include,
   lr_mixin_dup as lr_search_one asks it), the visitor's unsure step (if it has one) where that is unknown; else its module's chain, or
   the visitor's mixin step for a module the record holds no class of */
static int lr_walk_mixin(Compiler *c, const LrWalker *w, void *ud, int cls, int sg, LrClass *k, int i, int depth) {
  LrMixin *m = &k->mix[i];
  int dup = lr_mixin_dup(c, cls, sg, k, i), mod, rc;
  if (dup == ELAB_YES) return 0;
  if (dup == ELAB_UNKNOWN && w->unsure && (rc = w->unsure(ud, cls, m->ref))) return rc;
  return (mod = lr_later(c, &m->mod, m->ref)) < 0 ? w->mixin(ud, cls, m->ref) : lr_walk(c, w, ud, mod, 0, depth + 1);
}
/* the mixins of one kind a class holds, latest first: pass 0 its prepends (a singleton's), 2 its includes (extends) */
static int lr_walk_mixins(Compiler *c, const LrWalker *w, void *ud, int cls, int sg, int pass, int depth) {
  LrClass *k = lr_class(c->lrec, cls);
  for (int i = k ? k->nmix - 1 : -1; i >= 0; i--) {
    int want = sg ? (pass == 0 ? LR_SG_PREPEND : LR_EXTEND) : pass == 0 ? LR_PREPEND : LR_INCLUDE, rc;
    if (k->mix[i].kind == want && (rc = lr_walk_mixin(c, w, ud, cls, sg, k, i, depth))) return rc;
  }
  return 0;
}
static int lr_walk_one(Compiler *c, const LrWalker *w, void *ud, int cls, int sg, int depth) {
  int rc;
  if (depth > lr_chain_bound(c) || !lr_class(c->lrec, cls)) return -1;
  if ((rc = lr_walk_mixins(c, w, ud, cls, sg, 0, depth)) || (rc = w->own(ud, cls, sg))) return rc;   /* prepends, own, includes or extends, as lr_search */
  return lr_walk_mixins(c, w, ud, cls, sg, 2, depth);
}
/* Object's methods are one table: the program's reopening of Object and main's top level define into it (own is asked
   each, the reopening first). Object's step is its reopening's prepends (a top-level `prepend` does not exist), that
   table, then the includes of the reopening and of main (a top-level include is an include into Object), latest first
   in the record's order of the two statements (elab_order; the visitor's mixin step answers where it has none). main
   (0: not) is read only when the tail reads that far. */
static int lr_object_table(Compiler *c, int cls) {
  return cls == LR_MAIN || (cls >= 0 && cls == lr_core(c->lrec, CORE_OBJECT));
}
static int lr_next_include(const LrClass *k, int i) {
  while (k && i >= 0 && k->mix[i].kind != LR_INCLUDE) i--;
  return k ? i : -1;
}
static int lr_walk_object(Compiler *c, const LrWalker *w, void *ud, int main, int depth) {
  int o = lr_core(c->lrec, CORE_OBJECT), rc;
  LrClass *ko = o >= 0 ? lr_class(c->lrec, o) : NULL, *km = main ? lr_class(c->lrec, LR_MAIN) : NULL;
  if (depth > lr_chain_bound(c)) return -1;
  if (o >= 0 && ((rc = lr_walk_mixins(c, w, ud, o, 0, 0, depth)) || (rc = w->own(ud, o, 0)))) return rc;
  if (main && (rc = w->own(ud, LR_MAIN, 0))) return rc;
  for (int i = lr_next_include(ko, ko ? ko->nmix - 1 : -1), j = lr_next_include(km, km ? km->nmix - 1 : -1); i >= 0 || j >= 0;) {
    int ord = i < 0 ? PROG_BEFORE : j < 0 ? PROG_AFTER : elab_order(c, ko->mix[i].node, km->mix[j].node);
    if (ord == PROG_UNKNOWN && (rc = w->mixin(ud, LR_MAIN, km->mix[j].ref))) return rc;   /* (no order of the two: the visitor's step) */
    if (ord == PROG_BEFORE) {
      if ((rc = lr_walk_mixin(c, w, ud, LR_MAIN, 0, km, j, depth))) return rc;
      j = lr_next_include(km, j - 1);
    }
    else {
      if ((rc = lr_walk_mixin(c, w, ud, o, 0, ko, i, depth))) return rc;
      i = lr_next_include(ko, i - 1);
    }
  }
  return 0;
}
/* the core class whose own table follows tail[i] (-1: none; main's top level is Object's: tail[LR_TAIL_MAIN] is no step) */
enum { LR_TAIL_OBJECT = 2, LR_TAIL_MAIN = 3 };
static int lr_tail_core(int i) {
  switch (i) {
    case 0: return CORE_CLASS;
    case 1: return CORE_MODULE;
    case LR_TAIL_OBJECT: return CORE_OBJECT;
    case 4: return CORE_KERNEL;
    case 5: return CORE_BASIC_OBJECT;
    default: return -1;
  }
}
static int lr_walk(Compiler *c, const LrWalker *w, void *ud, int cls, int sg, int depth) {
  int sup, tail[LR_TAIL_N], rc;
  for (;; depth++, cls = sup) {   /* (the superclass steps a loop, as lr_search's) */
    if (w->core && !sg && lr_object_table(c, cls)) break;   /* (Object's step is the tail's) */
    if (depth > lr_chain_bound(c) || (rc = lr_walk_one(c, w, ud, cls, sg, depth))) return rc ? rc : -1;
    if (w->builtin && (rc = w->builtin(ud, cls, sg))) return rc;
    sup = lr_super(c, cls);
    if (sg && sup == LR_MAIN) sup = LR_UNKNOWN_CLASS;
    if (sup < LR_MAIN) break;
  }
  int ends = sg || !(cls >= 0 && comp_class_is_module(c, &c->classes[cls]));   /* (a module's instance chain has no end) */
  if (ends && (sg || w->core)) {
    int first = lr_sg_tail_of(c, cls, tail);
    for (int i = sg ? first : LR_TAIL_OBJECT; i < w->tail_end; i++) {
      if (i == LR_TAIL_MAIN) continue;
      rc = i == LR_TAIL_OBJECT ? lr_walk_object(c, w, ud, w->tail_end > LR_TAIL_MAIN, depth + 1) : tail[i] >= LR_MAIN ? lr_walk_one(c, w, ud, tail[i], 0, depth + 1) : 0;
      if (rc || (w->core && lr_tail_core(i) >= 0 && (rc = w->core(ud, lr_tail_core(i))))) return rc;
    }
  }
  return w->done && ends ? w->done(ud, cls, sg) : 0;
}
/* the versions of `name` along a chain: the visitor that collects them */
typedef struct { Compiler *c; const char *name; ElabVersion *out; int n, cap; } LrVers_;
static int lr_vv_own(void *ud, int cls, int sg) {
  LrVers_ *v = ud;
  v->n = lr_hist_versions(v->c->lrec, lr_hist(v->c->lrec, cls, sg, v->name, 0), v->out, v->n, v->cap);
  return v->n < 0 ? LR_W_STOP : 0;
}
static int lr_vv_mixin(void *ud, int cls, int ref) {
  (void)cls; (void)ref;
  ((LrVers_ *)ud)->n = -1;
  return LR_W_STOP;
}
static int lr_chain_versions(Compiler *c, int cls, int sg, const char *name, ElabVersion *out, int cap) {
  LrVers_ v = { c, name, out, 0, cap };
  LrWalker w = { lr_vv_own, lr_vv_mixin, NULL, NULL, NULL, NULL, lr_sg_tail_end(name) };
  return lr_walk(c, &w, &v, cls, sg, 0) < 0 || v.n < 0 ? -1 : v.n;
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
  return r && r->recorded && name && cls >= LR_MAIN ? lr_chain_versions(c, cls, sg, name, out, cap) : -1;
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

/* Is the version recorded at node in cls's history of `name` on its
   singleton (an alias in a `class << self`), not on its instance side? */
int elab_version_on_singleton(Compiler *c, int cls, const char *name, int node) {
  LoadRecord *r = c->lrec;
  const LrHist *h = r && r->recorded ? lr_hist(r, cls, 1, name, 0) : NULL;
  for (int i = 0; h && i < h->n; i++)
    if (h->v[i].node == node) return 1;
  return 0;
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
  for (int a = cls, d = 0; a >= LR_MAIN && d < lr_chain_bound(c); a = lr_super(c, a), d++)
    if (a == k) return 1;
  return 0;
}

/* Is class or module k one of cls's ancestors (its singleton's when sg)
   at p, or may it be: cls, a superclass, a module mixed into one of them?
   Only a superclass puts a class on a chain, whatever is mixed in, so the
   answer for a class is exact: cls or a superclass of it; Object and
   BasicObject, which end every class's chain whatever its superclass walk
   says (lr_super ends at main), and a module's singleton's; and on a
   singleton's chain the classes lr_sg_tail ends it in. */
static int lr_in_ancestry(Compiler *c, int cls, int sg, int k, LoadPoint p) {
  int tail[LR_TAIL_N];
  if (lr_under(c, cls, k)) return 1;
  if (k >= 0 && k < c->nclasses && !comp_class_is_module(c, &c->classes[k])) {
    if ((k == comp_core_class(c, CORE_OBJECT) || k == comp_core_class(c, CORE_BASIC_OBJECT)) &&
        (sg || cls < 0 || !comp_class_is_module(c, &c->classes[cls]))) return 1;
    for (int i = sg ? lr_sg_tail_of(c, cls, tail) : 5; i < 5; i++) if (tail[i] == k) return 1;
    return 0;
  }
  return k >= 0 && lr_search(c, cls, sg, NULL, k, p, 0, 0, NULL, 0) != ELAB_NO;
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
   class on it, code the record cannot follow run on one of them that may
   define (LR_OPAQUE_DEFINES)? */
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
      if (op->v[j].vis == LR_OPAQUE_DEFINES && lr_inside(c, op->v[j].node, p0, p1)) return 1;
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
    if (!lr_in_ancestry(c, cls, sg, k, end)) continue;
    if (lr_opaque_defines(r, k)) return 0;
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
  return lr_literal_class(k) != NULL;
}
/* the builtin class a literal of kind k makes, NULL for no literal */
static const char *lr_literal_class(NodeKind k) {
  switch (k) {
    case NK_IntegerNode: return "Integer";
    case NK_FloatNode: return "Float";
    case NK_StringNode: case NK_InterpolatedStringNode: return "String";
    case NK_SymbolNode: return "Symbol";
    case NK_ArrayNode: return "Array";
    case NK_HashNode: return "Hash";
    case NK_RangeNode: return "Range";
    case NK_NilNode: return "NilClass";
    case NK_TrueNode: return "TrueClass";
    case NK_FalseNode: return "FalseClass";
    default: return NULL;
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
   object (and runs its initialize); a definition form, def, alias, class
   header or constant write whose hook the program defines; a mixin into an object
   the program holds; code, or a method made or called, by a name the
   record cannot read; a callable made to run later (a lambda, a kept
   block, a method object, a block argument). A require, or the read of a
   constant an autoload is registered for, runs the code of the file it loads
   (a file of another region: lr_region_runs adds what that region runs).
   Or may it (LR_DISPATCH): any other call, and any node but those that
   only hold, read or write values, may run a method of the program's the
   runtime calls by itself (method_name_implicitly_invoked) on an object it
   meets, a callable, or one made by a name the record cannot read. */
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

/* The loads of lr_region_runs: edge lists the regions other than 0 that
   the loading nodes read (a node's own run from its estart), 0 for any
   region (an open set); au, the autoload registrations by name */
typedef struct {
  Compiler *c;
  const AutoloadRegistration **au;
  int nau, *edge, nedge, cedge;
} LrLoads;

static int lr_au_cmp(const void *a, const void *b) {
  return strcmp((*(const AutoloadRegistration *const *)a)->name, (*(const AutoloadRegistration *const *)b)->name);
}

/* Add the region of file f to the edges: none for a file of region 0, which the walk places */
static void lr_load_file(LrLoads *t, int f) {
  const NodeTable *nt = t->c->nt;
  const LoadRecord *r = t->c->lrec;
  int root = f > 0 && f < nt_nroots(nt) ? nt_root(nt, f) : -1;
  int g = root >= 0 && root < r->n && r->at[root] >= 0 ? r->st[r->at[root]].region : 0;
  if (g <= 0) return;
  t->edge = lr_grow(t->edge, &t->cedge, t->nedge, sizeof *t->edge);
  t->edge[t->nedge++] = g;
}

/* Add the regions of the files that node id may load: a require's target, a
   computed path's candidates, any for an open set; for a constant read, those
   of the autoloads registered under its name. (The registration's owner is
   not asked: a read's owner is a lookup that reads the run points this pass
   makes, and a name is a superset of it.) */
static void lr_load_regions(LrLoads *t, int id) {
  const NodeTable *nt = t->c->nt;
  if (nt_kind(nt, id) != NK_LoadNode) {
    const char *nm = lr_written_name(nt, id);
    int lo = 0, hi = t->nau;
    while (nm && lo < hi) { int mid = lo + (hi - lo) / 2; if (strcmp(t->au[mid]->name, nm) < 0) lo = mid + 1; else hi = mid; }
    for (; nm && lo < t->nau && sp_streq(t->au[lo]->name, nm); lo++) lr_load_file(t, t->au[lo]->file);
  }
  else if (prog_load_open(nt, id)) {
    t->edge = lr_grow(t->edge, &t->cedge, t->nedge, sizeof *t->edge);
    t->edge[t->nedge++] = 0;
  }
  else
    for (int k = -1, nc = prog_load_ncand(nt, id); k < nc; k++)
      lr_load_file(t, k < 0 ? (int)nt_int(nt, id, "target_file", -1) : prog_load_cand(nt, id, k));
}

static int lr_node_runs(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const LoadRecord *r = c->lrec;
  const int *par = comp_parent_map(c);
  const char *nm = NULL, *sent = NULL, *hook[2];
  int recv, an, ac = 0, hooks = r->defs_computed ? LR_DISPATCH : 0;
  const int *av;
  switch (nt_kind(nt, id)) {
    case NK_CallNode:
      nm = nt_str(nt, id, "name");
      sent = call_sent_name(nt, id, NULL);
      recv = nt_ref(nt, id, "receiver");
      an = nt_ref(nt, id, "arguments");
      av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
      if (!nm || is_new_name(nm) || is_allocate_name(nm) || is_raise_alias(nm) || lr_defines(r, nm) || is_method_object_maker(nm) ||
          (is_mixin_call(nm) && recv >= 0 && nt_kind(nt, recv) != NK_SelfNode && nt_kind(nt, recv) != NK_ConstantReadNode) ||
          (ac && (is_code_eval_name(nm) || (is_method_naming_call(nm) && !vis_arg_name(nt, av[0])) ||
                  (is_send_family(nm) && (!sent || lr_defines(r, sent)))))) return LR_RUN;
      for (int sg = 0; sg < 2; sg++)
        for (int i = definition_form_hooks(nm, sg, hook) - 1; i >= 0; i--)
          if (lr_defines(r, hook[i])) return LR_RUN;
      return lr_call_on_literals(c, id) ? 0 : LR_DISPATCH;
    case NK_AssocNode: recv = nt_ref(nt, id, "key"); return recv >= 0 && lr_literal_kind(nt_kind(nt, recv)) ? 0 : LR_DISPATCH;
    case NK_SuperNode: case NK_ForwardingSuperNode: case NK_YieldNode: case NK_LambdaNode: case NK_BlockArgumentNode: return LR_RUN;
    case NK_BlockNode:
      nm = par[id] >= 0 && nt_kind(nt, par[id]) == NK_CallNode ? nt_str(nt, par[id], "name") : NULL;
      return par[id] >= 0 && elab_block_when(c, par[id], 0) == ELAB_BLK_LATER && !(nm && is_method_naming_call(nm)) ? LR_RUN : 0;
    case NK_DefNode: case NK_AliasMethodNode: return lr_defines(r, "method_added") || lr_defines(r, "singleton_method_added") ? LR_RUN : hooks;
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
   classes have instances from the start. A region no load places runs
   inside the statement of region 0 that loads it, so the walk goes over
   region 0's statements and scans a region's at the first one that may
   load it: a require naming one of its files, a call of every name or of
   a file loader's name (which may load any file); and from the start
   where no keyed require names it (one in any body, even a method never
   called). Each region is scanned once: a require loads once, and what a
   scan finds holds from its statement on.
   rinst[k + 1]: the first statement an object with k among its ancestors
   may exist (LR_NEVER: none); urun[def]: the first statement a recorded
   def's body may run, 0 for one the record did not record; rentry[k]: the
   first statement region k's load may run (LR_NEVER: none). All three are
   statements of region 0, the one time every region's code shares: a
   statement of another region is not ordered against them. */
enum { LR_NEVER = INT_MAX, LR_CORE = -4, RQ_INST = 1, RQ_CLASS = 2, LR_RG_NAMED = 1, LR_RG_IN = 2 };
/* The order of version v against point p (late == 1: as of the load phase's
   end): a version at a point, its point's; one a method body makes
   (LrDef.body), its extent's: after p where p comes before the first
   statement that body may run on (lr_rta's urun) and that statement does
   not run again before p (lr_runs_again), not known from there on, since
   the body may run any number of times, and after every point for a body
   that never runs */
static int lr_version_order(Compiler *c, const LrDef *v, LoadPoint p, int late) {
  if (!v->body)
    return late == 1 ? (elab_point_of(c, v->node).region == 0 ? PROG_BEFORE : PROG_UNKNOWN) : elab_cmp(elab_point_of(c, v->node), p);
  LoadRecord *r = c->lrec;
  lr_run_points(c);
  int from = r->urun[v->body];
  if (from == LR_NEVER) return PROG_AFTER;
  return late != 1 && p.region >= 0 && p.seq < from && !lr_runs_again(c, r->st[from].node, p) ? PROG_AFTER : PROG_UNKNOWN;
}
/* The class a builder's constant write w makes (`P = Struct.new(:x)`,
   block or not): the class the walk made at the write (record_write_class),
   or LR_CORE for one it made none of (a name no C identifier spells: only
   core ancestors); LR_UNKNOWN_CLASS for any other write */
static int lr_rta_builder(Compiler *c, int w) {
  if (nt_kind(c->nt, w) != NK_ConstantWriteNode || class_def_record(c->nt, w) < 0) return LR_UNKNOWN_CLASS;
  int k = elab_header_class(c, w, NULL, NULL);
  return k >= 0 ? k : LR_CORE;
}
typedef struct { const char *name; int cls; } LrRtaName;
typedef struct {
  Compiler *c; LoadRecord *r;
  int s, n, *key, *ustart, *ulist, *stack, nstack, *hnext, *hfirst, *pend, npend;
  unsigned char *hreach, *rq, *sub, *ext, *seen, *rbits;   /* (rbits: by region, LR_RG_*) */
  int allinst, cb, allcls, any, builders, ncname, allreg;
  LrRtaName *cname;
  int *kid, *kids;   /* the classes by superclass: kids[kid[p + 1] .. kid[p + 2]) are p's (main's at p = LR_MAIN) */
} LrRta;

/* name's index in the program's method names (r->names), -1 for none */
static int lr_name_ix(const LoadRecord *r, const char *nm) {
  char **p = nm ? bsearch(&nm, r->names, (size_t)r->nnames, sizeof *r->names, lr_name_cmp) : NULL;
  return p ? (int)(p - r->names) : -1;
}
static void lr_rta_reach(LrRta *t, int hi);
static void lr_rta_add(LrRta *t, int k);
static void lr_rta_recv(LrRta *t, int a) {   /* an object with a among its ancestors may exist */
  LoadRecord *r = t->r;
  LrClass *kc = lr_class(r, a);
  if (a < LR_MAIN || a >= t->c->nclasses || r->rinst[a + 1] != LR_NEVER) return;
  r->rinst[a + 1] = t->s;
  for (int h = t->hfirst[a + 1]; h >= 0; h = t->hnext[h])
    if (t->allinst || t->rq[lr_name_ix(r, r->hist[h].name)] & RQ_INST) lr_rta_reach(t, h);
  for (int j = 0; kc && j < kc->nmix; j++)
    if (!elab_mix_on_singleton(kc->mix[j].kind)) lr_rta_add(t, lr_later(t->c, &kc->mix[j].mod, kc->mix[j].ref));
}
/* k and its superclasses into E. A class lr_rta_recv has reached got its
   superclasses with it (every way in is through here, a mixin's too), so
   the walk stops at the first one reached: the rest of the chain is in
   already. That is also what ends a chain that loops. */
static void lr_rta_add(LrRta *t, int k) {   /* k into E */
  for (int a = k; a >= LR_MAIN && a < t->c->nclasses && t->r->rinst[a + 1] == LR_NEVER; a = lr_super(t->c, a)) lr_rta_recv(t, a);
}
static void lr_rta_ext(LrRta *t, int m, int d) {   /* module m extended onto a class, or included into one that is */
  LrClass *mc = lr_class(t->r, m);
  if (m < 0 || m >= t->c->nclasses || t->ext[m + 1] || d > 64) return;
  t->ext[m + 1] = 1;
  for (int j = 0; mc && j < mc->nmix; j++)
    if (!elab_mix_on_singleton(mc->mix[j].kind)) lr_rta_ext(t, lr_later(t->c, &mc->mix[j].mod, mc->mix[j].ref), d + 1);
}
/* Region k's load may run from statement t->s on: its statements are
   scanned there (lr_rta_drain), once */
static void lr_rta_enter(LrRta *t, int k) {
  if (k <= 0 || k >= t->r->nregion || (t->rbits[k] & LR_RG_IN)) return;
  t->rbits[k] |= LR_RG_IN;
  t->r->rentry[k] = t->s;
  t->pend[t->npend++] = k;
}
static void lr_rta_enter_all(LrRta *t) {
  for (int k = 1; !t->allreg && k < t->r->nregion; k++) lr_rta_enter(t, k);
  t->allreg = 1;
}
/* The region file f's statements are in, -1 for none */
static int lr_rta_region(const LrRta *t, int f) {
  const NodeTable *nt = t->c->nt;
  int root = f > 0 && f < nt_nroots(nt) ? nt_root(nt, f) : -1;
  return root >= 0 && root < t->n && t->r->at[root] >= 0 ? t->r->st[t->r->at[root]].region : -1;
}
/* The regions require id may load: its file's, those of the set its
   computed path may name, every one for a set of any file; entered
   (enter), or else only marked as named by a require */
static void lr_rta_loads(LrRta *t, int id, int enter) {
  const NodeTable *nt = t->c->nt;
  if (prog_load_open(nt, id)) {
    if (enter) lr_rta_enter_all(t);
    for (int k = 1; !enter && k < t->r->nregion; k++) t->rbits[k] |= LR_RG_NAMED;
    return;
  }
  for (int k = -1, nc = prog_load_ncand(nt, id); k < nc; k++) {
    int g = lr_rta_region(t, k < 0 ? (int)nt_int(nt, id, "target_file", -1) : prog_load_cand(nt, id, k));
    if (g > 0 && enter) lr_rta_enter(t, g);
    else if (g > 0) t->rbits[g] |= LR_RG_NAMED;
  }
}
/* name called on an object of E (RQ_INST) or on any class object (RQ_CLASS);
   a file loader's name (a send of require, its method object, an alias of
   it) may load any file */
static void lr_rta_call(LrRta *t, const char *name, int how) {
  LoadRecord *r = t->r;
  if (!t->allreg && is_file_load_name(name)) lr_rta_enter_all(t);
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
static void lr_rta_reach_by(LrRta *t, int hi);
/* name called on class k: its singleton chain's, its extended modules'.
   once: by a call that runs when statement t->s does and at no other time
   (lr_rta_once), which reaches only the singleton versions made by then */
static void lr_rta_call_on(LrRta *t, const char *name, int k, int once) {
  LoadRecord *r = t->r;
  lr_rta_call(t, name, RQ_INST);
  for (int a = k, d = 0; a >= LR_MAIN && a < t->c->nclasses && d < lr_chain_bound(t->c); a = lr_super(t->c, a), d++) {
    LrClass *kc = lr_class(r, a);
    LrHist *h = lr_hist(r, a, 1, name, 0);
    if (h && once) lr_rta_reach_by(t, (int)(h - r->hist));
    else if (h) lr_rta_reach(t, (int)(h - r->hist));
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
/* The instance versions of `name` on module a, and of the originals its
   aliases take, as the copy of a module_function runs them though no object
   of the module may exist to call them */
static void lr_rta_inst(LrRta *t, int a, const char *name, int depth) {
  const LrHist *h = depth < 64 ? lr_hist(t->r, a, 0, name, 0) : NULL;
  if (!h || t->hreach[h - t->r->hist]) return;
  lr_rta_reach(t, (int)(h - t->r->hist));
  for (int j = 0; j < h->n; j++)
    if (h->v[j].kind == LR_ALIAS && h->v[j].target) lr_rta_inst(t, a, h->v[j].target, depth + 1);
}
/* A module_function copy (on h, the singleton's history) runs the instance
   method as it stood at the copy: the def of the copy's scope (lr_copy_scopes
   gives a named copy one); for a copy of a method with none, the instance
   history of the name */
static void lr_rta_copy(LrRta *t, const LrHist *h, const LrDef *v) {
  if (v->scope > 0) lr_rta_unit(t, t->c->scopes[v->scope].def_node);
  else lr_rta_inst(t, h->a, v->target, 0);
}
static void lr_rta_version(LrRta *t, const LrHist *h, const LrDef *v) {
  if (v->kind == LR_DEF) lr_rta_unit(t, v->node);
  else if ((v->kind == LR_ALIAS || v->kind == LR_COPY) && !v->target) lr_rta_all(t, 0);
  else if (v->kind == LR_ALIAS || v->kind == LR_COPY) {
    h->b ? lr_rta_call_on(t, v->target, h->a, 0) : lr_rta_call(t, v->target, RQ_INST);
    if (v->kind == LR_COPY) lr_rta_copy(t, h, v);
  }
}
static void lr_rta_reach(LrRta *t, int hi) {
  const LrHist *h = &t->r->hist[hi];
  if (t->hreach[hi]) return;
  t->hreach[hi] = 1;
  for (int j = 0; j < h->n; j++) lr_rta_version(t, h, &h->v[j]);
}
/* ...its versions made by statement t->s: not one at a later statement of
   region 0, which a call reaches once it is made. The history counts as
   reached once none is left. */
static void lr_rta_reach_by(LrRta *t, int hi) {
  const LrHist *h = &t->r->hist[hi];
  int all = 1;
  if (t->hreach[hi]) return;
  for (int j = 0; j < h->n; j++) {
    LoadPoint q = h->v[j].body ? (LoadPoint){ -1, -1 } : elab_point_of(t->c, h->v[j].node);
    if (q.region == 0 && q.seq > t->s) all = 0;
    else lr_rta_version(t, h, &h->v[j]);
  }
  t->hreach[hi] = (unsigned char)all;
}
/* Does call id run when statement t->s runs, and at no other time: a node
   of the statement's own (no def around it), in no lambda and in no block
   but one that runs at once (elab_blocks_run_now), the statement in no
   body that runs again (a loop's)? */
static int lr_rta_once(LrRta *t, int id) {
  const LrStmt *st = &t->r->st[t->s];
  NodeKind sk = nt_kind(t->c->nt, st->node);
  if (t->key[id] != t->s || st->rep >= 0 || sk == NK_WhileNode || sk == NK_UntilNode || !elab_blocks_run_now(t->c, id)) return 0;
  const int *par = comp_parent_map(t->c);
  for (int p = par[id]; p >= 0 && p != st->node; p = par[p]) if (nt_kind(t->c->nt, p) == NK_LambdaNode) return 0;
  return 1;
}
/* every name (disp 0: every instance method of E, every singleton
   method), or what the runtime may call back (disp 1: the implicitly
   invoked names, and on class objects its callbacks) */
static void lr_rta_all(LrRta *t, int disp) {
  LoadRecord *r = t->r;
  if (!disp) lr_rta_enter_all(t);   /* require among them */
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
   core class the program binds nothing to, or a builder's write the walk
   made no class at (lr_rta_builder) */
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

/* The class self is at call id, when id is a statement's of a class or
   module body (or a builder's block) itself, as elab_self_value_at reads
   it: a bare or `self.` call there is a call on that class.
   LR_UNKNOWN_CLASS elsewhere (a method body, a block run later, the top
   level, a `class << X` body). */
static int lr_rta_self(const LrRta *t, int id) {
  const NodeTable *nt = t->c->nt;
  int recv = nt_ref(nt, id, "receiver");
  if (recv >= 0 && (nt_kind(nt, recv) != NK_SelfNode || nt_int(nt, recv, "sclass_value", 0))) return LR_UNKNOWN_CLASS;
  const LrStmt *s = lr_stmt_of(t->r, id);
  if (!s || s->region < 0 || s->frame < 0 || s->up != s->frame) return LR_UNKNOWN_CLASS;
  int f = t->r->st[s->frame].node;
  if (nt_kind(nt, f) == NK_SingletonClassNode || !class_def_opens_frame(nt, f)) return LR_UNKNOWN_CLASS;
  int cls = elab_header_class(t->c, f, NULL, NULL);
  return cls >= 0 ? cls : LR_UNKNOWN_CLASS;
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
  if (!v || t->sub[k + 1]) return;
  t->sub[k + 1] = 1;
  /* a class method's new makes an object of the class self is: k or any
     subclass of it. The subclasses come from the index of the classes by
     superclass, collected before any is added. sub marks the classes whose subclasses are in (or
     are being added by the call that collected them), so a class under
     one is not walked again. */
  int *low = malloc(sizeof(int) * (size_t)(t->c->nclasses + 1)), n = 0;
  if (!low) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  low[n++] = k;
  for (int i = 0; i < n; i++)
    for (int j = t->kid[low[i] + 1]; j < t->kid[low[i] + 2]; j++)
      if (!t->sub[t->kids[j] + 1]) { t->sub[t->kids[j] + 1] = 1; low[n++] = t->kids[j]; }
  for (int i = 1; i < n; i++) lr_rta_add(t, low[i]);
  free(low);
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
      /* a def the record did not record: from here; and a C extension's
         dispatch stub, the way its C code calls the program (a method's,
         an allocator's), which it may from the extension's load on */
      if (t->r->urun[id] == 0 || nt_int(nt, id, "cext_stub", 0)) lr_rta_unit(t, id);
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
    case NK_LoadNode:
      lr_rta_loads(t, id, 1);
      /* fallthrough */
    default:
      if (lr_node_runs(t->c, id) == LR_DISPATCH) lr_rta_all(t, 1);
      return;
  }
  nm = nt_str(nt, id, "name");
  recv = nt_ref(nt, id, "receiver");
  an = nt_ref(nt, id, "arguments");
  av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_SelfNode, ak;
  int rc = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode ? lr_rta_const(t, recv)
         : rk == NK_SelfNode ? lr_rta_self(t, id) : LR_UNKNOWN_CLASS;
  by = nm && is_send_family(nm) ? (ac ? call_sent_name(nt, id, NULL) : "")
     : nm && (is_method_object_maker(nm) || is_enumerator_maker(nm)) ? (ac ? vis_arg_name(nt, av[0]) : "") : nm;
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
    if ((m = m >= 0 ? elab_header_class(t->c, m, NULL, NULL) : LR_UNKNOWN_CLASS) >= 0) lr_rta_add(t, m);
    else lr_rta_any(t);
  }
  if (rc >= LR_MAIN) lr_rta_call_on(t, by, rc, by == nm && lr_rta_once(t, id));   /* a method called, sent or made a callable by its name */
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
    if ((d = lr_rta_const(t, av[ka])) >= LR_MAIN) lr_rta_call_on(t, "exception", d, 0);
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
    if (m >= 0) lr_rta_add(t, m);
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
      t->cname[n++] = (LrRtaName){ lr_written_name(nt, path ? nt_ref(nt, id, "target") : id), b != LR_UNKNOWN_CLASS ? b : lr_class_of(t->c, &(LrDef){ .node = id }, NULL) };
    }
    if ((cn && (is_const_set_name(cn) || is_remove_const_name(cn))) || (n && !t->cname[n - 1].name)) { t->ncname = -1; return; }
  }
  qsort(t->cname, (size_t)n, sizeof *t->cname, lr_rta_name_cmp);
  for (int i = 0; i < n; i++)   /* a name bound to two classes, or otherwise too: unknown */
    if (m && !strcmp(t->cname[m - 1].name, t->cname[i].name)) { if (t->cname[m - 1].cls != t->cname[i].cls) t->cname[m - 1].cls = LR_UNKNOWN_CLASS; }
    else t->cname[m++] = t->cname[i];
  t->ncname = m;
}

/* The code reached at statement t->s and not scanned yet: each body first
   reached (lr_rta_unit), and each region entered (lr_rta_enter), whose
   statements' nodes are one run of the groups */
static void lr_rta_drain(LrRta *t) {
  const LoadRecord *r = t->r;
  for (;;) {
    int lo, hi;
    if (t->nstack) { lo = r->nst + t->stack[--t->nstack]; hi = lo + 1; }
    else if (t->npend) { int k = t->pend[--t->npend]; lo = r->rend[k - 1]; hi = r->rend[k]; }
    else return;
    for (int j = t->ustart[lo]; j < t->ustart[hi]; j++) lr_rta_node(t, t->ulist[j]);
  }
}

static void lr_rta(Compiler *c) {
  LoadRecord *r = c->lrec;
  const int *par = comp_parent_map(c);
  LrRta t = { .c = c, .r = r, .n = c->nt->count < r->n ? c->nt->count : r->n, .allreg = r->nregion < 2 };
  int n = t.n, ncl = c->nclasses + 1, nk = r->nst + n;
  t.key = malloc(sizeof(int) * (size_t)(4 * n + r->nst + 2 + r->nhist + ncl + r->nregion));
  t.hreach = calloc((size_t)(r->nhist + r->nnames + 3 * ncl + n + r->nregion) + 1, 1);
  if (!t.key || !t.hreach) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  t.ustart = t.key + n; t.ulist = t.ustart + nk + 2; t.stack = t.ulist + n; t.hnext = t.stack + n; t.hfirst = t.hnext + r->nhist; t.pend = t.hfirst + ncl;
  t.rq = t.hreach + r->nhist; t.sub = t.rq + r->nnames; t.ext = t.sub + ncl; t.seen = t.ext + ncl; t.rbits = t.seen + n;
  for (int i = 0; i < n; i++) {   /* each node's innermost def (the def itself is its outer one's) */
    int p = par[i];
    while (p >= 0 && p < n && p > i && nt_kind(c->nt, p) != NK_DefNode) p = par[p];
    t.key[i] = p < 0 || p >= n ? -1 : nt_kind(c->nt, p) == NK_DefNode ? p : t.key[p];
  }
  for (int i = 0; i < n; i++)   /* grouped: a def's nodes; the others by their statement */
    t.key[i] = t.key[i] >= 0 ? r->nst + t.key[i] : r->at[i] >= 0 ? r->at[i] : -1;
  memset(t.ustart, 0, sizeof(int) * (size_t)(nk + 2));
  for (int i = 0; i < n; i++) if (t.key[i] >= 0) t.ustart[t.key[i] + 2]++;
  for (int i = 1; i < nk + 2; i++) t.ustart[i] += t.ustart[i - 1];
  for (int i = 0; i < n; i++) if (t.key[i] >= 0) t.ulist[t.ustart[t.key[i] + 1]++] = i;
  for (int k = 0; k < ncl; k++) { t.hfirst[k] = -1; r->rinst[k] = LR_NEVER; }
  t.kid = calloc((size_t)(2 * ncl + 2), sizeof(int));   /* the classes by superclass: counted, summed, filled as ustart is */
  if (!t.kid) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  t.kids = t.kid + ncl + 2;
  for (int k = 0; k < c->nclasses; k++) { int p = lr_super(c, k); if (p >= LR_MAIN && p < c->nclasses) t.kid[p + 3]++; }
  for (int i = 1; i < ncl + 2; i++) t.kid[i] += t.kid[i - 1];
  for (int k = 0; k < c->nclasses; k++) { int p = lr_super(c, k); if (p >= LR_MAIN && p < c->nclasses) t.kids[t.kid[p + 2]++] = k; }
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
  for (int k = 1; k < r->nregion; k++) r->rentry[k] = LR_NEVER;
  NT_FOREACH_KIND(c->nt, NK_LoadNode, id)   /* a region no keyed require names, in any body: from the start */
    if (id < n && t.key[id] >= 0) lr_rta_loads(&t, id, 0);
  for (int k = 1; k < r->nregion; k++) if (!t.rbits[k]) lr_rta_enter(&t, k);
  lr_rta_drain(&t);
  for (t.s = 0; t.s < r->rend[0]; t.s++)
    for (int i = t.ustart[t.s]; i < t.ustart[t.s + 1]; i++) {
      lr_rta_node(&t, t.ulist[i]);
      lr_rta_drain(&t);
    }
  free(t.key); free(t.hreach); free(t.cname); free(t.kid);
}

/* Add to each statement that loads a file of another region (lr_load_regions)
   what that region runs: the union of the run bits of its statements, itself
   including those of the regions its own statements load, so a region that
   only dispatches makes a require of it dispatch, not run. The loading nodes
   and the regions they read are collected once; a region's bits (rr; rr[0],
   all regions' for an open set) only grow, each at most twice, and each time
   they do only the loads that read the region are visited again (rd, by
   region), so the work is linear in the loads whatever their order. */
static void lr_region_runs(Compiler *c) {
  LoadRecord *r = c->lrec;
  if (r->nregion < 2) return;
  int nr = r->nregion, ncand = 0, nstack = 0, *cand = malloc(sizeof(int) * (size_t)(r->n + 1)), *est = malloc(sizeof(int) * (size_t)(r->n + 2));
  LrLoads t = { c, NULL, 0, NULL, 0, 0 };
  if (!cand || !est) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  if (c->nautoloads) {
    t.au = malloc(sizeof *t.au * (size_t)c->nautoloads);
    if (!t.au) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int k = 0; k < c->nautoloads; k++) t.au[t.nau++] = &c->autoloads[k];
    qsort(t.au, (size_t)t.nau, sizeof *t.au, lr_au_cmp);
  }
  for (int id = 0; id < r->n; id++) {
    NodeKind k = nt_kind(c->nt, id);
    if (r->at[id] < 0 || (r->bits[id] & LR_LATE) || (k != NK_LoadNode && !(t.nau && nt_kind_is_constant(k)))) continue;
    est[ncand] = t.nedge;
    lr_load_regions(&t, id);
    if (t.nedge > est[ncand]) cand[ncand++] = id;   /* (a node that reads no region is overwritten by the next) */
  }
  est[ncand] = t.nedge;
  unsigned char *rr = calloc((size_t)nr, 1), *b = calloc((size_t)ncand + 1, 1), *queued = calloc((size_t)nr, 1);
  int *rd = malloc(sizeof(int) * (size_t)(t.nedge + 1)), *off = calloc((size_t)nr + 1, sizeof *off);
  int *fill = calloc((size_t)nr, sizeof *fill), *stack = malloc(sizeof(int) * (size_t)nr);
  if (!rr || !b || !queued || !rd || !off || !fill || !stack) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int j = 0; j < t.nedge; j++) off[t.edge[j] + 1]++;
  for (int g = 0; g < nr; g++) off[g + 1] += off[g];
  for (int i = 0; i < ncand; i++)
    for (int j = est[i]; j < est[i + 1]; j++) rd[off[t.edge[j]] + fill[t.edge[j]]++] = i;
  for (int s = 0; s < r->nst; s++)
    if (r->st[s].region > 0) rr[r->st[s].region] |= r->st[s].runs, rr[0] |= r->st[s].runs;
  for (int g = 0; g < nr; g++) if (rr[g]) queued[g] = 1, stack[nstack++] = g;
  while (nstack) {
    int h = stack[--nstack];
    queued[h] = 0;
    for (int j = off[h]; j < off[h + 1]; j++) {
      int i = rd[j], g = r->st[r->at[cand[i]]].region;
      unsigned char nb = b[i] | rr[h];
      if (nb == b[i]) continue;
      b[i] = nb;
      int grows[2] = { g, 0 };   /* the statement's region grows, and all regions' */
      for (int q = 0; g > 0 && q < 2; q++) {
        int x = grows[q];
        if ((rr[x] | nb) == rr[x]) continue;
        rr[x] |= nb;
        if (!queued[x]) queued[x] = 1, stack[nstack++] = x;
      }
    }
  }
  for (int i = 0; i < ncand; i++) r->st[r->at[cand[i]]].runs |= b[i];
  free(cand); free(est); free(t.au); free(t.edge); free(rr); free(b); free(queued); free(rd); free(off); free(fill); free(stack);
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
  lr_region_runs(c);
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
  free(r->rinst); free(r->urun); free(r->rentry);
  r->rinst = calloc((size_t)(r->ncls > c->nclasses ? r->ncls : c->nclasses + 1) + 1, sizeof(int));
  r->urun = calloc((size_t)r->n + 1, sizeof(int));
  r->rentry = calloc((size_t)r->nregion, sizeof(int));
  if (!r->rinst || !r->urun || !r->rentry) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  lr_rta(c);
  r->runs_of = r->builds;
}

/* How many statements in seq [a, b) may run code of the program's */
static int lr_runs_in(const LoadRecord *r, int a, int b) {
  return (b < r->nst ? r->st[b].nrun : r->nrun) - (a < r->nst ? r->st[a].nrun : r->nrun);
}

/* The node the climb to a point goes on to from a: its parent; from a
   clone no file's root reaches (a method body a pass copied: a mixin's
   copy, a module_function's), the node it copies, where it stands as the
   call plan's frame stands it (lr_plan_place) */
static int lr_climb_up(Compiler *c, const int *par, int a) {
  if (par[a] >= 0) return par[a];
  /* a method a pass made for a node, which runs no earlier than it */
  int rf = nt_kind(c->nt, a) == NK_DefNode ? (int)nt_int(c->nt, a, "runs_from", 0) - 1 : -1;
  if (rf >= 0) return rf;
  int o = nt_clone_source(c->nt, a);
  return o >= 0 && o < a ? o : -1;
}

int elab_call_window(Compiler *c, int id, ElabWindow *w) {
  LoadRecord *r = elab_fresh(c);
  const int *par = comp_parent_map(c);
  int a = id;
  *w = (ElabWindow){ { -1, -1 }, { -1, -1 }, 0, 0, 0 };
  while (a >= 0 && a < r->n && elab_point_of(c, a).region < 0) a = lr_climb_up(c, par, a);
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
  /* (the collapse below keeps a window, not a proof; lr_rta walks the load
     only, and under --ext-entry the host calls into the program after it) */
  w->never = !g_ext_entries && nt_kind(c->nt, a) == NK_DefNode && r->urun[a] == LR_NEVER;
  if (nt_kind(c->nt, a) == NK_DefNode && r->urun[a] > w->from.seq)   /* nor before the body may first run (lr_rta) */
    w->from.seq = r->urun[a] < w->to.seq ? r->urun[a] : w->to.seq - 1;
  if (nt_kind(c->nt, a) == NK_DefNode && !lr_runs_in(r, w->from.seq, w->from.seq + 1)) {
    /* nor at a statement that runs no code of the program's (lr_run_points):
       from the first that may, w's last when none does */
    int lo = w->from.seq + 1, hi = w->to.seq;
    while (lo < hi) { int mid = lo + (hi - lo) / 2; if (lr_runs_in(r, w->from.seq, mid) > 0) hi = mid; else lo = mid + 1; }
    w->from.seq = lo - 1;
  }
  return ELAB_YES;
}

/* ...in a block the record takes to run at once, one whose typed call
   target keeps it when kept (blk_kept), any when not: a call there may be
   one a target keeps, before the typed pass has marked it */
static int lr_in_assumed_block(Compiler *c, const int *par, int n, int id, int kept) {
  const NodeTable *nt = c->nt;
  for (int a = id; a >= 0 && a < n && par[a] >= 0; a = par[a]) {
    NodeKind k = nt_kind(nt, a);
    if (k == NK_DefNode || k == NK_LambdaNode) break;   /* (a body of its own: its window is the def's) */
    if (k == NK_BlockNode && nt_ref(nt, par[a], "block") == a && (!kept || nt_int(nt, par[a], "blk_kept", 0)) &&
        elab_block_runs(c, par[a]) == ELAB_BLK_ASSUMED) return 1;
  }
  return 0;
}
int elab_in_kept_block(Compiler *c, int id) {
  return lr_in_assumed_block(c, comp_parent_map(c), c->nt->count, id, 1);
}
/* Does node id run later than its point, at a time the syntax does not
   tell: in a block whose call the record answers ELAB_BLK_LATER for (a
   lazy chain's, placed at its call's point), or in an END body (placed at
   its statement), with no def or lambda between (a body of its own: its
   window is the def's)? It may then run any time after its point. */
int elab_runs_later(Compiler *c, int id) {
  int n;
  const int *par = elab_node_parents(c, &n);
  for (int a = id; a >= 0 && a < n && par[a] >= 0; a = par[a]) {
    NodeKind k = nt_kind(c->nt, a);
    if (k == NK_DefNode || k == NK_LambdaNode) return 0;
    if (k == NK_PostExecutionNode ||
        (k == NK_BlockNode && nt_ref(c->nt, par[a], "block") == a && elab_block_runs(c, par[a]) == ELAB_BLK_LATER)) return 1;
  }
  return 0;
}
void elab_window_open(Compiler *c, ElabWindow *w) {
  LoadRecord *r = elab_fresh(c);
  lr_run_points(c);
  w->to = (LoadPoint){ w->from.region, r->rend[w->from.region] };
  w->closed = 0;
}

/* The owners whose histories can be in cls's ancestry: its superclass chain
   and the modules mixed into any of those, a module's own mixins too (at any
   time, so a superset of lr_in_ancestry's). -1 when that is not known: a
   superclass or a mixin the record cannot resolve, code it cannot follow
   that may define (not one that only writes) run on one of them, or more
   owners than fit. A module's (mod) is itself and
   Object, whose constants its lookup reaches, and their mixins. */
static int lr_ancestry_owners(Compiler *c, int cls, int *out, int cap, int mod) {
  LoadRecord *r = c->lrec;
  int n = 0, a = cls;
  for (int d = 0; a >= LR_MAIN; a = lr_super(c, a), d++) {
    if (d >= 64 || n >= cap) return -1;
    out[n++] = a;
  }
  if (mod && n == 1 && a == LR_UNKNOWN_CLASS && n < cap) out[n++] = LR_MAIN;
  if (a != LR_UNKNOWN_CLASS || out[n - 1] != LR_MAIN) return -1;
  for (int i = 0; i < n; i++) {
    if (lr_opaque_defines(r, out[i])) return -1;
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

/* Does the statement holding node run for sure before p: placed, on no
   condition and in no body that may not run or be cut short, and ordered
   before p? */
static int lr_settled(Compiler *c, int node, LoadPoint p) {
  const LrStmt *s = node >= 0 ? lr_stmt_of(c->lrec, node) : NULL;
  return s && s->region >= 0 && !s->may && !s->partial && elab_cmp(elab_point_of(c, node), p) == PROG_BEFORE;
}

/* Is class k Object, Kernel or BasicObject, whose program methods and
   mixins no Class emitter lists among a class's inherited ones? */
static int lr_core_owner(Compiler *c, int k) {
  return k >= 0 && (k == comp_core_class(c, CORE_OBJECT) || k == comp_core_class(c, CORE_KERNEL) ||
                    k == comp_core_class(c, CORE_BASIC_OBJECT));
}

/* a call that sets or removes a constant or a class variable by name, or
   names a constant to autoload */
static int lr_setter_name(const char *n) {
  return is_const_set_name(n) || is_remove_const_name(n) || is_cvar_write_name(n) || is_autoload_name(n);
}

/* What of `kinds` the record's histories and the class and scope tables
   leave unsettled for a reflection on cls at p (owners: its ancestry,
   lr_ancestry_owners): every entry the record holds of them, and every
   contributor the tables hold -- an instance method's def, an alias, an
   attr the record has an entry of, a mixin. The inherited kind is
   unsettled too while main (a public top-level def), Object, Kernel or
   BasicObject holds any: the emitters do not list theirs. */
static unsigned lr_unsettled_tables(Compiler *c, int cls, const int *owners, int n, unsigned kinds, LoadPoint p) {
  LoadRecord *r = c->lrec;
  unsigned un = 0;
  for (int i = 0; i < r->nhist; i++) {
    const LrHist *h = &r->hist[i];
    if (lr_key_memo(h->a)) continue;
    int own = h->a == cls ? 1 : 0, core = h->a == LR_MAIN || lr_core_owner(c, h->a);
    for (int o = 0; o < n && !own && h->a >= LR_MAIN; o++) own = owners[o] == h->a ? 2 : 0;
    int sg = h->a == LR_KEY_CONST && h->b <= 0;   /* (a singleton class body's, or an owner not known) */
    unsigned k = h->a == LR_KEY_CONST ? ELAB_GAIN_CONSTS : h->b || !own ? (h->b || !core ? 0 : ELAB_GAIN_INHERITED)
               : own == 1 ? ELAB_GAIN_METHODS : ELAB_GAIN_INHERITED;
    for (int j = 0; (kinds & k & ~un) && j < h->n; j++) {
      const LrDef *d = &h->v[j];
      /* a top-level def is Object's private method, which no listing of
         public and protected methods holds */
      if (h->a == LR_MAIN && d->kind <= LR_DEFINE_METHOD && d->vis == SP_VIS_PRIVATE) continue;
      if (core || sg || d->may || !lr_settled(c, d->node, p)) un |= k;
    }
  }
  for (int si = 0; (kinds & (ELAB_GAIN_METHODS | ELAB_GAIN_INHERITED) & ~un) && si < c->nscopes; si++) {
    const Scope *sc = &c->scopes[si];
    if (sc->class_id < 0 || sc->is_cmethod || !sc->name) continue;
    int core = lr_core_owner(c, sc->class_id), own = sc->class_id == cls;
    for (int o = 0; o < n && !own; o++) own = owners[o] == sc->class_id ? 2 : 0;
    if (core) un |= ELAB_GAIN_INHERITED;
    if (own && !lr_settled(c, sc->def_node, p)) un |= own == 1 ? ELAB_GAIN_METHODS : ELAB_GAIN_INHERITED;
  }
  const int cores[] = { CORE_OBJECT, CORE_KERNEL, CORE_BASIC_OBJECT };
  for (int o = -3; o < n; o++) {   /* (the core classes first, then the owners) */
    int a = o < 0 ? comp_core_class(c, cores[o + 3]) : owners[o];
    if (o < 0 && a < 0) continue;
    const LrClass *kc = lr_class(r, a);
    for (int j = 0; kc && j < kc->nmix; j++)
      if (o < 0 || a == LR_MAIN || !lr_settled(c, kc->mix[j].node, p)) un |= kinds & ~ELAB_GAIN_METHODS;
    if (a < 0) continue;
    const ClassInfo *ki = &c->classes[a];
    unsigned k = a == cls ? ELAB_GAIN_METHODS : ELAB_GAIN_INHERITED;
    if (o < 0 && ki->naliases + ki->nreaders + ki->nwriters) un |= ELAB_GAIN_INHERITED;
    for (int j = 0; o >= 0 && (kinds & k & ~un) && j < ki->naliases; j++)
      if (!(ki->alias_sg && ki->alias_sg[j]) && !lr_settled(c, ki->alias_node[j], p)) un |= k;
    for (int j = 0; o >= 0 && (kinds & k & ~un) && j < ki->nreaders + ki->nwriters; j++) {
      char wn[256];
      snprintf(wn, sizeof wn, j < ki->nreaders ? "%s" : "%s=", j < ki->nreaders ? ki->readers[j] : ki->writers[j - ki->nreaders]);
      if (!lr_hist(r, a, 0, wn, 0)) un |= k;
    }
  }
  return un & kinds;
}

static int lr_owner_in(const int *owners, int n, int a) {
  for (int o = 0; o < n; o++) if (owners[o] == a) return 1;
  return 0;
}

/* Does a class_variable_set or remove_class_variable call, its receiver
   naming `on` (LR_UNKNOWN_CLASS: not known), set or remove one the emitters
   see: a class variable off cls's ancestry, one the owner it names holds
   itself (not one an ancestor overtook), or, on a receiver not known, one
   cls's ancestry holds? */
static int lr_cvar_set_seen(Compiler *c, int cls, const int *owners, int n, int call, int on) {
  int an = 0, a = nt_ref(c->nt, call, "arguments");
  const int *av = a >= 0 ? nt_arr(c->nt, a, "arguments", &an) : NULL;
  const char *nm = an ? vis_arg_name(c->nt, av[0]) : NULL;
  if (!nm) return 0;
  if (on >= 0 && !lr_owner_in(owners, n, on)) return 1;
  int k = comp_cvar_owner(c, on >= 0 ? on : cls, nm);
  return (on < 0 || k == on) && comp_cvar_index(&c->classes[k], nm) >= 0;
}

/* ...and what the nodes as they are leave unsettled: a class variable the
   tables hold with no write the record places in its class body itself;
   one name two owners' bodies write (which owner keeps it, or whether a
   read raises as overtaken, depends on the order of the writes); a call
   that sets or removes a class variable the tables do not hold; a call
   that sets or removes a constant by name on any receiver but cls, and an
   autoload; any of those calls the record cannot settle, call id aside */
static unsigned lr_unsettled_nodes(Compiler *c, int cls, const int *owners, int n, unsigned kinds, int id, LoadPoint p) {
  const NodeTable *nt = c->nt;
  unsigned un = 0;
  /* (the owners' body writes, sorted by name) */
  int nw = 0, cw = 0;
  LrCvarWrite *wr = NULL;
  for (int u = kinds & ELAB_GAIN_CVARS ? comp_kind_first(c, NK_ClassVariableWriteNode) : -1; u >= 0; u = comp_kind_next(c, u)) {
    int s = elab_self_value_at(c, u);
    const char *nm = nt_str(nt, u, "name");
    if (!nm || !lr_owner_in(owners, n, s)) continue;
    wr = lr_grow(wr, &cw, nw, sizeof *wr);
    wr[nw++] = (LrCvarWrite){ nm, u, s };
  }
  if (nw > 1) qsort(wr, (size_t)nw, sizeof *wr, lr_name_cmp);
  for (int o = 0; o < n; o++) {
    const ClassInfo *ki = owners[o] >= 0 ? &c->classes[owners[o]] : NULL;
    for (int j = 0; ki && (kinds & ELAB_GAIN_CVARS & ~un) && j < ki->ncvars; j++) {
      int k, body = 0;
      const LrCvarWrite *e = lr_name_range(wr, nw, sizeof *wr, ki->cvars[j], &k);
      for (int i = 0; i < k && !body; i++) body = e[i].owner == owners[o] && lr_settled(c, e[i].node, p);
      if (!body) un |= ELAB_GAIN_CVARS;
    }
  }
  for (int x = 1; x < nw && !(un & ELAB_GAIN_CVARS); x++)
    if (lr_name_cmp(&wr[x], &wr[x - 1]) == 0 && wr[x].owner != wr[x - 1].owner) un |= ELAB_GAIN_CVARS;
  free(wr);
  int *calls = NULL, nc = kinds & (ELAB_GAIN_CONSTS | ELAB_GAIN_CVARS) & ~un ? an_calls_named_if(c, lr_setter_name, nt->count, &calls) : 0;
  for (int i = 0; i < nc; i++) {
    const char *cn = nt_str(nt, calls[i], "name");
    unsigned k = is_cvar_write_name(cn) ? ELAB_GAIN_CVARS : ELAB_GAIN_CONSTS;
    if (calls[i] == id) continue;
    int rv = nt_ref(nt, calls[i], "receiver");
    int on = rv < 0 || nt_kind(nt, rv) == NK_SelfNode ? elab_self_value_at(c, calls[i]) : lr_class_ref_at(c, rv, NULL);
    /* (what the emitters do not see: an autoload, a constant set or
       removed on another receiver than cls, a class variable the tables
       do not hold) */
    int seen = is_autoload_name(cn) ? 0 : k == ELAB_GAIN_CONSTS ? on == cls : lr_cvar_set_seen(c, cls, owners, n, calls[i], on);
    if (!seen || !lr_settled(c, calls[i], p)) un |= k;
  }
  free(calls);
  return un & kinds;
}

/* Both, for call id */
static unsigned lr_unsettled_of(Compiler *c, int cls, const int *owners, int n, unsigned kinds, int id, LoadPoint p) {
  unsigned un = lr_unsettled_tables(c, cls, owners, n, kinds, p);
  return un | lr_unsettled_nodes(c, cls, owners, n, kinds & ~un, id, p);
}

/* An open-addressed slot of the memo for (id, cls) asked from window
   start w: its own, or the empty one it goes in */
static int lr_unset_slot(const int *key, int cap, int id, int cls, LoadPoint w) {
  unsigned h = ((unsigned)id * 31u + (unsigned)cls * 7u + (unsigned)w.seq) & (unsigned)(cap - 1);
  while (key[4 * h] >= 0 && (key[4 * h] != id || key[4 * h + 1] != cls || key[4 * h + 2] != w.region || key[4 * h + 3] != w.seq))
    h = (h + 1) & (unsigned)(cap - 1);
  return (int)h;
}

/* lr_unsettled_of for call id from window start w, kept while nothing it
   reads can have moved: the histories, the mixins and the scope owners
   the record sets (LoadRecord.hgen), the scopes' classes and names (the
   frozen scope index's epoch, the scope count), the aliases and parents
   (comp_class_shape_gen), the attrs and class variables (comp_table_gen),
   the class count and the nodes' names, kinds and refs (the node table's
   version and edits) */
static unsigned lr_unset_kept(Compiler *c, int cls, const int *owners, int n, int id, LoadPoint w) {
  LoadRecord *r = c->lrec;
  const unsigned all = ELAB_GAIN_METHODS | ELAB_GAIN_INHERITED | ELAB_GAIN_CONSTS | ELAB_GAIN_CVARS;
  if (!comp_scope_index_is_frozen()) return lr_unsettled_of(c, cls, owners, n, all, id, w);
  struct LrUnset *m = &r->unset;
  int same = m->key && m->of.hgen == r->hgen && m->of.sgen == comp_scope_index_gen() && m->of.shape == comp_class_shape_gen() &&
             m->of.tgen == comp_table_gen && m->of.ncls == r->ncls && m->of.nscopes == c->nscopes && m->of.nclasses == c->nclasses &&
             m->of.nver == c->nt->version && m->of.nedits == c->nt->edits;
  if (!same || 2 * (m->n + 1) > m->cap) {   /* start afresh, twice as large when full */
    int cap = same ? 2 * m->cap : 256;
    free(m->key); free(m->un);
    m->key = malloc(sizeof(int) * 4 * (size_t)cap); m->un = malloc((size_t)cap);
    if (!m->key || !m->un) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memset(m->key, -1, sizeof(int) * 4 * (size_t)cap);
    m->cap = cap; m->n = 0;
    m->of.hgen = r->hgen; m->of.sgen = comp_scope_index_gen(); m->of.shape = comp_class_shape_gen();
    m->of.tgen = comp_table_gen; m->of.ncls = r->ncls; m->of.nscopes = c->nscopes; m->of.nclasses = c->nclasses;
    m->of.nver = c->nt->version; m->of.nedits = c->nt->edits;
  }
  int h = lr_unset_slot(m->key, m->cap, id, cls, w);
  if (m->key[4 * h] < 0) {
    m->un[h] = (unsigned char)lr_unsettled_of(c, cls, owners, n, all, id, w);
    m->key[4 * h] = id; m->key[4 * h + 1] = cls; m->key[4 * h + 2] = w.region; m->key[4 * h + 3] = w.seq; m->n++;
  }
  return m->un[h];
}

unsigned elab_class_unsettled(Compiler *c, int cls, unsigned kinds, int id, int keep) {
  LoadRecord *r = elab_fresh(c);
  ElabWindow w;
  int owners[64], n;
  /* (a file the compiler did not read, an autoload's too, can give the class anything) */
  if (g_require_unread || !r->recorded || cls < 0 || cls >= c->nclasses || elab_call_window(c, id, &w) != ELAB_YES ||
      (n = lr_ancestry_owners(c, cls, owners, 64, comp_class_is_module(c, &c->classes[cls]))) < 0) return kinds;
  return keep && r->plan_depth ? lr_unset_kept(c, cls, owners, n, id, w.from) & kinds
                               : lr_unsettled_of(c, cls, owners, n, kinds, id, w.from);
}

typedef struct { int m, nh, any, *cp; } LrSpanAcc;
/* history h's versions inside the window (p0, p1), when it is one of cls's ancestors' */
static void lr_span_take(Compiler *c, const LrHist *h, int cls, int sg, LoadPoint p0, LoadPoint p1, LrSpanAcc *s) {
  if ((h->b != sg && !sg) || !lr_in_ancestry(c, cls, sg, h->a, p1)) return;
  int had = s->m;
  for (int j = 0; j < h->n; j++) {
    const LrDef *v = &h->v[j];
    if (!lr_is_version(v) || (v->body ? lr_version_order(c, v, p1, 0) == PROG_AFTER : !lr_inside(c, v->node, p0, p1))) continue;
    LoadPoint q = elab_point_of(c, v->node);
    if (v->may || elab_cmp(p0, q) != PROG_BEFORE || elab_cmp(q, p1) != PROG_BEFORE) { s->any = 1; continue; }
    if (!(s->m & (s->m - 1)) && !(s->cp = realloc(s->cp, sizeof(int) * (size_t)(s->m ? 2 * s->m : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    int k = s->m++;
    while (k > 0 && s->cp[k - 1] > q.seq) { s->cp[k] = s->cp[k - 1]; k--; }
    s->cp[k] = q.seq;
  }
  s->nh += s->m > had;
}

/* The first statement of region `region` from which a call may run on
   cls: on its class object (sg) from the header that opens it, on an
   instance from the first statement one may exist (lr_rta); -1 where the
   record cannot tell (a builtin class, one no header opens, one opened in
   another region). In a program of more regions only an instance's, in a
   call of region 0: the openings do not bound a class there (another
   region may open it first, a write in a kept block may make it), while
   lr_rta's first instance stands for every way one is made. */
static int lr_live_from(Compiler *c, int cls, int sg, int region) {
  LoadRecord *r = c->lrec;
  lr_run_points(c);
  const LrClass *kc = cls >= 0 && !c->classes[cls].is_builtin_const ? lr_class(r, cls) : NULL;
  if (r->nregion > 1) return region != 0 || sg || !kc || !kc->nopen ? -1 : r->rinst[cls + 1];
  LoadPoint b = kc && kc->nopen ? elab_point_of(c, kc->open[0].node) : (LoadPoint){ -1, -1 };
  /* (a builder's write with no block opens no body, but makes the class
     there: a header that reopens it later is not where it opens) */
  int d = kc ? c->classes[cls].def_node : -1;
  NodeKind dk = d >= 0 ? nt_kind(c->nt, d) : NK_NilNode;
  if (b.region >= 0 && (dk == NK_ConstantWriteNode || dk == NK_LocalVariableWriteNode || dk == NK_ConstantPathWriteNode)) {
    LoadPoint wp = elab_point_of(c, d);
    if (wp.region == b.region && wp.seq < b.seq) b = wp;
  }
  return b.region != region ? -1 : sg || r->rinst[cls + 1] < b.seq ? b.seq : r->rinst[cls + 1];
}
int elab_class_live_at(Compiler *c, int cls, int sg, LoadPoint p) {
  LoadRecord *r = elab_fresh(c);
  return !r->recorded || p.region < 0 || p.seq >= lr_live_from(c, cls, sg, p.region);
}
/* elab_method_span; *idle when no statement of w that may run code of the
   program's sees a version (the one version counts all the same) */
static int lr_span(Compiler *c, int cls, const char *name, int sg, const ElabWindow *w, const LrDef **out, int cap, int *idle) {
  LoadRecord *r = c->lrec;
  LoadPoint p0 = w->from, p1 = w->to;
  if (!r || !r->recorded || !name || cls < LR_MAIN || p0.region < 0 || elab_cmp(p0, p1) != PROG_BEFORE) return -1;
  /* no call on a class before it opens, nor on an instance of it before
     the first statement one may exist (lr_live_from); none in w: w's last
     statement. In a program of more regions, on a class object or in a
     region other than 0, from w's start: more versions, never fewer */
  int bl = lr_live_from(c, cls, sg, p0.region);
  if (bl >= p1.seq) bl = p1.seq - 1;
  if (p0.seq < bl && bl < p1.seq) p0.seq = bl;
  int n = 0, obs0 = 0, owners[64], no = lr_ancestry_owners(c, cls, owners, 64, 0);
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
void elab_call_meet(Compiler *c, int id, int cls, const char *name, int sg, int how, ElabMeet *m, const LrDef **out, int cap) {
  LoadPoint p = elab_point_of(c, id);
  int after = how & 1;
  memset(m, 0, sizeof *m);
  m->at = p.region >= 0 && !after && !(how & ELAB_MEET_COPY) ? elab_method_at(c, cls, name, sg, p, &m->d) : ELAB_UNKNOWN;
  m->win = ELAB_NO;
  if (m->at != ELAB_UNKNOWN) return;
  m->n = elab_call_span(c, id, cls, name, sg, after, &m->w, out, cap);
  m->win = m->n == ELAB_SPAN_NO_WINDOW ? ELAB_UNKNOWN : ELAB_YES;
  if (m->win != ELAB_YES) m->n = 0;
}
int elab_call_span(Compiler *c, int id, int cls, const char *name, int sg, int open, ElabWindow *w, const LrDef **out, int cap) {
  ElabWindow own;
  if (!w) w = &own;
  if (elab_call_window(c, id, w) != ELAB_YES) return ELAB_SPAN_NO_WINDOW;
  if (open) elab_window_open(c, w);
  return lr_span(c, cls, name, sg, w, out, cap, NULL);
}

/* ---- the readers of a version (elab_version_met) ----
   Who may meet a version on cls's singleton chain: each call of the name
   the map was made of, and each Method's maker naming it (`C.method(:v)`),
   whose self may be cls or a class under it. lr_readers files each once
   per walk by the classes its self may be, under the keys a question about
   cls looks up:
     - RD_UNDER at each ancestor of x: self is x, so a question about cls
       finds it where x is cls or a class under it;
     - RD_AT at x: self is x or a class under it, so a question about cls
       finds it, looking up cls's superclasses, where cls is under x too;
     - RD_ANY: self may be any class, or the record cannot say.
   Who self is: for a constant receiver, its class, and for self in a class
   body, that class (RD_UNDER). For a bare call or one on self in a def,
   where the def is written directly in a class or module body (or in a
   `class << self` there) on no receiver or self, with no block or lambda
   between the call and the def, the def's owner says:
     - a class method of K runs on K or a class under it (both, at K);
     - a module's own method runs on the module (RD_UNDER at it);
     - a module's instance method (a module function's too) runs on what
       the module is mixed into: on a class object, through a mixin's copy
       of the method in a class whose singleton chain holds the module or
       a class under one (both, at each holder: lr_holders_make), or any
       where Class, Module, Object, BasicObject or Kernel holds it. Such a
       reader is asked as the copy's call (ELAB_MEET_COPY), as the call
       plan asks it;
     - a class's instance method runs on a class object only where the
       class is Class, Module, Object or BasicObject (RD_ANY), else never.
   A Method made of the def's name or of an alias of it may run the body on
   any object (`instance_method(:f).bind_call(x)`), as may a maker or a
   send of one whose name the record cannot read: RD_ANY. A question about
   cls reads its RD_UNDER bucket, the RD_AT bucket of each of its
   superclasses and the RD_ANY one, not every call of the name. The
   instance side is not filed by class yet: there each reader is RD_ANY. */
enum { RD_UNDER, RD_AT, RD_ANY, RD_MAKER };

static void lr_reader_add(LoadRecord *r, const char *name, int node, int kind, int key, int sg, int copy) {
  unsigned b = lr_hash(key, kind * 2 + sg, name);
  r->rdr = lr_grow(r->rdr, &r->crdr, r->nrdr, sizeof *r->rdr);
  r->rdr[r->nrdr] = (struct LrReader){ name, node, key, r->rdr_head[b], (unsigned char)kind, (unsigned char)sg, (unsigned char)copy };
  r->rdr_head[b] = r->nrdr++;
}
/* ...a reader whose self is x (RD_UNDER at x's ancestors), or x or a class
   under it as well (at: RD_AT at x) */
static void lr_reader_under(Compiler *c, LoadRecord *r, const char *name, int node, int x, int at, int copy) {
  for (int a = x, d = 0; a >= LR_MAIN && d < lr_chain_bound(c); a = lr_super(c, a), d++) lr_reader_add(r, name, node, RD_UNDER, a, 1, copy);
  if (at) lr_reader_add(r, name, node, RD_AT, x, 1, copy);
}
static int lr_reader_has(LoadRecord *r, const char *name, int kind, int key, int sg) {
  for (int v = r->rdr_head[lr_hash(key, kind * 2 + sg, name)]; v >= 0; v = r->rdr[v].next)
    if (r->rdr[v].kind == kind && r->rdr[v].key == key && sp_streq(r->rdr[v].name, name)) return 1;
  return 0;
}

/* The modules on module m's own chain (m, the modules it includes or
   prepends, theirs), into out[0..cap); -1 where one is a module the record
   does not know, or past cap */
static int lr_module_closure(Compiler *c, int m, int *out, int cap) {
  int n = 0;
  if (cap < 1) return -1;
  out[n++] = m;
  for (int i = 0; i < n; i++) {
    LrClass *kc = lr_class(c->lrec, out[i]);
    for (int j = 0; kc && j < kc->nmix; j++) {
      if (elab_mix_on_singleton(kc->mix[j].kind)) continue;
      int mod = lr_mixin_class(c, &kc->mix[j]), have = 0;
      if (mod < LR_MAIN) return -1;
      for (int k = 0; k < n && !have; k++) have = out[k] == mod;
      if (have) continue;
      if (n >= cap) return -1;
      out[n++] = mod;
    }
  }
  return n;
}

/* Which classes' singleton chains may hold each module, made once per
   walk for lr_readers: a class or module holds m by a mixin of its own (an
   extend, a singleton prepend, an include in `class << self`) of m or of a
   module whose chain holds m (lr_module_closure); it may hold any (wild)
   where one is of a module the record does not know, or code it cannot
   follow that may define runs on it; and every class's chain holds m
   (every) where Class, Module, Object, BasicObject, main's (Object's) or
   Kernel does, or holds any where the program may run code the record has
   not seen or one of those holds a module it does not know (all). */
typedef struct { int *head, n, cap, *wild, nwild; struct { int who, next; } *by; unsigned char *every, all; } LrHolders;
static void lr_holders_make(Compiler *c, LrHolders *h) {
  LoadRecord *r = c->lrec;
  int mods[64], nk = r->ncls;
  memset(h, 0, sizeof *h);
  h->head = malloc(sizeof(int) * (size_t)(nk > 0 ? nk : 1));
  h->every = calloc((size_t)(nk > 0 ? nk : 1), 1);
  h->wild = malloc(sizeof(int) * (size_t)(nk > 0 ? nk : 1));
  if (!h->head || !h->every || !h->wild) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < nk; i++) h->head[i] = -1;
  int core[6] = { comp_core_class(c, CORE_CLASS_REOPEN), lr_core(r, CORE_MODULE), lr_core(r, CORE_OBJECT), LR_MAIN,
                  lr_core(r, CORE_KERNEL), comp_core_class(c, CORE_BASIC_OBJECT) };
  h->all = (unsigned char)elab_runs_unseen(c);
  for (int i = 0; i < 6 && !h->all; i++) {
    if (core[i] < LR_MAIN) continue;
    int nm = lr_module_closure(c, core[i], mods, 64);
    h->all = nm < 0 || lr_opaque_defines(r, core[i]);
    for (int k = 0; k < nm; k++) if (mods[k] + 1 < nk) h->every[mods[k] + 1] = 1;
  }
  for (int e = LR_MAIN; e + 1 < nk; e++) {
    const LrClass *kc = lr_class(r, e);
    int wild = lr_opaque_defines(r, e);
    for (int j = 0; kc && j < kc->nmix && !wild; j++) {
      if (!elab_mix_on_singleton(kc->mix[j].kind)) continue;
      int mod = lr_mixin_class(c, &kc->mix[j]), nm = mod >= LR_MAIN ? lr_module_closure(c, mod, mods, 64) : -1;
      wild = nm < 0;
      for (int k = 0; k < nm && !wild; k++) {
        if (mods[k] + 1 >= nk) continue;
        h->by = lr_grow(h->by, &h->cap, h->n, sizeof *h->by);
        h->by[h->n].who = e; h->by[h->n].next = h->head[mods[k] + 1]; h->head[mods[k] + 1] = h->n++;
      }
    }
    if (wild) h->wild[h->nwild++] = e;
  }
}
static void lr_holders_free(LrHolders *h) {
  free(h->head); free(h->by); free(h->wild); free(h->every);
}

/* ...the reader at call id whose receiver is self and whose value the
   record does not hold, filed by its def's owner (see above) */
static void lr_reader_self(Compiler *c, LoadRecord *r, const LrHolders *hd, int id, const char *name) {
  const NodeTable *nt = c->nt;
  int np, def = -1, body = -1;
  const int *par = elab_node_parents(c, &np);
  for (int a = id < np ? par[id] : -1; a >= 0 && a < np && body < 0; a = par[a]) {
    NodeKind k = nt_kind(nt, a);
    if (k == NK_BlockNode || k == NK_LambdaNode || (k == NK_DefNode && def >= 0) ||
        (k == NK_SingletonClassNode && (def < 0 || nt_kind(nt, nt_ref(nt, a, "expression")) != NK_SelfNode))) {
      def = -1;
      break;
    }
    if (k == NK_DefNode) def = a;
    else if (k == NK_ClassNode || k == NK_ModuleNode) body = a;
  }
  int dr = def >= 0 ? nt_ref(nt, def, "receiver") : -1;
  const Scope *sc = def >= 0 && body >= 0 && (dr < 0 || nt_kind(nt, dr) == NK_SelfNode) ? comp_scope_of(c, id) : NULL;
  int k = sc && sc->def_node == def ? sc->class_id : -1;
  /* a Method made of the def's name, or of an alias of it */
  const char *names[8] = { def >= 0 ? nt_str(nt, def, "name") : NULL };
  int nn = names[0] ? 1 : 0, made = 0;
  for (int i = 0; i < nn && !made; i++) {
    made = lr_reader_has(r, names[i], RD_MAKER, -3, 1);
    const LrVers *x = lr_vers(r, names[i]);
    for (int a = x ? x->alias : -1; a >= 0 && nn < 8; a = r->valias[a].next) {
      const char *an = r->hist[r->valias[a].hist].name;
      int have = 0;
      for (int j = 0; j < nn && !have; j++) have = sp_streq(names[j], an);
      if (!have) names[nn++] = an;
    }
    if (nn == 8) made = 1;
  }
  if (k < 0 || k >= c->nclasses || made) { lr_reader_add(r, name, id, RD_ANY, -2, 1, 0); return; }
  if (!comp_class_is_module(c, &c->classes[k])) {
    int core = k == comp_core_class(c, CORE_CLASS_REOPEN) || k == lr_core(r, CORE_MODULE) || k == lr_core(r, CORE_OBJECT) ||
               k == comp_core_class(c, CORE_BASIC_OBJECT);
    if (sc->is_cmethod) lr_reader_under(c, r, name, id, k, 1, 0);
    else if (core) lr_reader_add(r, name, id, RD_ANY, -2, 1, 0);
    return;
  }
  if (sc->is_cmethod) lr_reader_under(c, r, name, id, k, 0, 0);
  if (sc->is_cmethod && !sc->is_module_function) return;
  if (hd->all || k + 1 >= r->ncls || hd->every[k + 1]) { lr_reader_add(r, name, id, RD_ANY, -2, 1, 1); return; }
  for (int v = hd->head[k + 1]; v >= 0; v = hd->by[v].next) lr_reader_under(c, r, name, id, hd->by[v].who, 1, 1);
  for (int i = 0; i < hd->nwild; i++) lr_reader_under(c, r, name, id, hd->wild[i], 1, 1);
}

/* The readers' buckets, filed once per walk: a pass that asks while it
   copies method bodies (register_extends, under a hold) asks of the calls
   the program was read with, a mixin's copy's call as the call it copies,
   asked as a copy's (ELAB_MEET_COPY). */
static void lr_readers(Compiler *c, LoadRecord *r) {
  const NodeTable *nt = c->nt;
  if (r->rdr_head && r->rdr_of.n == r->n && r->rdr_of.ver == r->ver) return;
  if (!r->rdr_head && !(r->rdr_head = malloc(sizeof(int) * LR_BUCKETS))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int b = 0; b < LR_BUCKETS; b++) r->rdr_head[b] = -1;
  r->nrdr = 0;
  r->rdr_of.n = r->n; r->rdr_of.ver = r->ver;
  int anymaker = 0;
  /* the makers first, by the name they take: a def whose name one takes
     runs on any self */
  for (int id = 0; id < r->n; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (nm && is_send_family(nm)) {
      const char *by = call_sent_name(nt, id, NULL);
      anymaker |= !by || is_method_object_maker(by);
    }
    if (!nm || !is_method_object_maker(nm)) continue;
    const char *sym = method_sym_arg(c, id);
    if (sym) lr_reader_add(r, sym, id, RD_MAKER, -3, 1, 0);
    else anymaker = 1;
  }
  LrHolders hd;
  lr_holders_make(c, &hd);
  for (int id = 0; id < r->n; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name"), *keys[2] = { nm, nm && is_method_object_maker(nm) ? method_sym_arg(c, id) : NULL };
    int recv = nt_ref(nt, id, "receiver");
    NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_SelfNode;
    int k = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode ? lr_class_ref_at(c, recv, NULL)
          : rk == NK_SelfNode ? elab_self_value_at(c, id) : LR_UNKNOWN_CLASS;
    for (int i = 0; i < 2; i++) {
      if (!keys[i]) continue;
      lr_reader_add(r, keys[i], id, RD_ANY, -2, 0, 0);
      if (k >= 0) lr_reader_under(c, r, keys[i], id, k, 0, 0);
      else if (k == LR_MAIN) continue;   /* (main is no class) */
      else if (rk == NK_SelfNode && !anymaker) lr_reader_self(c, r, &hd, id, keys[i]);
      else lr_reader_add(r, keys[i], id, RD_ANY, -2, 1, 0);
    }
  }
  lr_holders_free(&hd);
}

/* ...call id's part of elab_version_met: does the call plan bind it to the
   version made at def node `def` on cls's chain, or to an entry that holds
   it, from the versions it may meet (elab_call_meet): the one at its
   point, the one over its window, or, over a chain that holds still from
   the window's start, any of several (an entry, ct_version_at); copy: as
   the call in a mixin's copy of its body. In a block the record takes to
   run at once, a call may be one its typed target keeps
   (elab_in_kept_block), which no pass has marked yet: it is asked as
   running after its point as well. */
static int lr_meet_holds(Compiler *c, const ElabMeet *m, const LrDef **ds, int cap, int cls, int sg, const char *name, int def) {
  if (m->at == ELAB_YES) return m->d && m->d->node == def;
  if (m->at == ELAB_NO || m->win != ELAB_YES || (m->n < 1 && m->n != ELAB_SPAN_ANY)) return 0;
  if (m->n == 1) return ds[0] && ds[0]->node == def;
  if (!elab_chain_settled(c, cls, sg, name, m->w.from)) return 0;
  if (m->n == ELAB_SPAN_ANY || m->n > cap) return 1;
  for (int k = 0; k < m->n; k++) if (ds[k] && ds[k]->node == def) return 1;
  return 0;
}
static int lr_call_meets(Compiler *c, int id, int copy, int cls, int sg, const char *name, int def) {
  int np;
  const int *par = elab_node_parents(c, &np);
  int later = elab_runs_later(c, id), held = lr_in_assumed_block(c, par, np, id, 0);
  const LrDef *ds[16] = { NULL };
  ElabMeet m;
  elab_call_meet(c, id, cls, name, sg, later | (copy ? ELAB_MEET_COPY : 0), &m, ds, 16);
  if (lr_meet_holds(c, &m, ds, 16, cls, sg, name, def)) return 1;
  if (later || !held) return 0;
  elab_call_meet(c, id, cls, name, sg, 1 | (copy ? ELAB_MEET_COPY : 0), &m, ds, 16);
  return lr_meet_holds(c, &m, ds, 16, cls, sg, name, def);
}
/* ...one bucket's part */
static int lr_readers_meet(Compiler *c, LoadRecord *r, int kind, int key, int cls, int sg, const char *name, int def) {
  for (int v = r->rdr_head[lr_hash(key, kind * 2 + sg, name)]; v >= 0; v = r->rdr[v].next) {
    const struct LrReader *e = &r->rdr[v];
    if (e->kind == kind && e->key == key && e->sg == sg && sp_streq(e->name, name) && lr_call_meets(c, e->node, e->copy, cls, sg, name, def)) return 1;
  }
  return 0;
}
/* Does the program reach the version made at def node `def` on cls's
   chain (its singleton's when sg) under `name`: does the call plan bind a
   call of the name, or a Method's maker naming it, whose self may be cls
   or a class under it (lr_readers) to it or to an entry holding it, from
   the versions the call may meet (elab_call_meet, lr_call_meets), or does
   an alias of the name on cls or a class under it capture it where it is
   made? */
int elab_version_met(Compiler *c, int cls, int sg, const char *name, int def) {
  LoadRecord *r = elab_fresh(c);
  if (!r->recorded || !name || def < 0 || cls < 0) return 0;
  sg = sg ? 1 : 0;
  lr_readers(c, r);
  /* the readers whose self is cls or a class under it, or a class cls is
     under, or any */
  int met = lr_readers_meet(c, r, RD_UNDER, cls, cls, sg, name, def);
  for (int a = cls, d = 0; sg && !met && a >= LR_MAIN && d < lr_chain_bound(c); a = lr_super(c, a), d++)
    met = lr_readers_meet(c, r, RD_AT, a, cls, sg, name, def);
  if (!met) met = lr_readers_meet(c, r, RD_ANY, -2, cls, sg, name, def);
  const LrVers *x = lr_vers(r, name);
  for (int a = x ? x->alias : -1; a >= 0 && !met; a = r->valias[a].next) {
    const LrHist *h = &r->hist[r->valias[a].hist];
    const LrDef *d = NULL;
    LoadPoint p = elab_point_of(c, h->v[r->valias[a].j].node);
    met = h->b == sg && lr_under(c, h->a, cls) && p.region >= 0 &&
          elab_method_at(c, h->a, name, sg, p, &d) == ELAB_YES && d && d->node == def;
  }
  return met;
}

/* Can the program run code the record has not seen? Only through a load
   whose target it could not resolve: under --load-model=graph a LoadNode
   whose path is computed when it runs, inline a require, require_relative,
   load or autoload the parse left in the tree (one it resolved it spliced
   away). A require or a load is Kernel's on no receiver or on Object or
   Kernel (`YAML.load` reads no file); an autoload on any receiver is one
   (`Mod.autoload(:X, path)`, which the parse leaves); and a require the
   inline model ignored, which no node stands for (the root's
   dropped_require). The files of a closed
   set a computed path may name are seen: each is a region of its own.
   Read from the tree, once. */
int elab_runs_unseen(Compiler *c) {
  LoadRecord *r = lr_get(c);
  if (!r->unseen) {
    const NodeTable *nt = c->nt;
    int hit = nt_int(nt, nt->root_id, "dropped_require", 0), n = 0;   /* (a require the inline model ignored, whose library CRuby would load) */
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
   the builtin prelude (node_bi)? Such a node stands where the pass linked
   it, which for a def or a mixin a lowering appends, or a method body it
   copies into a helper, is after every file: no point in CRuby's load
   order the record holds. A library file's nodes are not among them: the
   load model places the file (a boot library at the program's start, any
   other at its require), and the record reads the point as it does for the
   program's own. Nor is a node a pass made to stand and run where its
   source stood (node_in_place: the source became the parentheses that run
   it, or a copy of it is one of the branches that do, every node of the copy
   marked), which stands where it is linked, in its frame, once; nor a node of
   an eval's text the graft put in place of the eval (node_eval), which runs
   where the eval runs, as the program's own code does. The marks decide, a
   clone or not: a copy that is not marked is unplaced. */
static int lr_unplaced(Compiler *c, int node) {
  char how = 0; int a = 0, k = 0; unsigned pass = 0;
  int made = nt_origin(c->nt, node, &how, &a, &pass, &k) >= 0;
  return nt_int(c->nt, node, "node_bi", 0) ||
         (made && !nt_int(c->nt, node, "node_in_place", 0) && !nt_int(c->nt, node, "node_eval", 0));
}

/* May what the record cannot order supply `name` on cls's chain (its
   singleton's when sg) at p: a version of the name a pass made, a mixin a
   pass made into a class on the chain, or code the record cannot follow
   (a send, an eval of a String) run on one before p, but code that only
   writes (LR_OPAQUE_WRITES)? */
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
      if (op->v[j].vis == LR_OPAQUE_DEFINES && elab_cmp(elab_point_of(c, op->v[j].node), p) != PROG_AFTER) return 1;
  }
  return 0;
}

/* Is cls a class a record builder's write makes (`Foo = Struct.new(:a)`,
   `k = Data.define(:a)`, made at the write: record_write_class)? Its
   superclass is the builtin Struct or Data, Object's subclass, whose
   methods the builtin tables answer; its own singleton answers new, [],
   members and inspect (is_record_class_cmethod). */
static int lr_record_class(Compiler *c, int cls) {
  int d = cls >= 0 && cls < c->nclasses ? c->classes[cls].def_node : -1;
  return class_def_record(c->nt, d) >= 0;
}

/* Does cls's chain pass a class whose superclass the record does not
   hold: one whose header names a superclass the record does not know (`<
   Struct.new(:a)`, a core class), a class with no header? Its ancestors
   past it are unknown. A record class's is the builtin Struct or Data,
   then Object (lr_record_class). */
static int lr_chain_open(Compiler *c, int cls) {
  for (int a = cls, d = 0; a >= 0 && a < c->nclasses; a = lr_super(c, a)) {
    if (++d > lr_chain_bound(c)) return 1;
    LrClass *k = lr_class(c->lrec, a);
    int dn = c->classes[a].def_node;
    if (k && k->has_sup) { if (lr_later(c, &k->sup, k->sup_ref) < 0) return 1; continue; }
    if (c->classes[a].parent >= 0 || class_def_is_module(c->nt, dn) || lr_record_class(c, a)) continue;
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
  for (int a = cls, g = 0; g < lr_chain_bound(c); g++, a = lr_super(c, a)) {
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

/* The memo of elab_record_class_of, kept while what it reads holds: the
   histories (the record's superclasses), the parents (comp_class_shape_gen),
   the class count and the nodes' refs (the node table's version and edits),
   as lr_unset_kept's. NULL while the record is still being made, whose
   superclasses are not all set. */
static int *lr_rclass_memo(Compiler *c) {
  LoadRecord *r = c->lrec;
  if (!r->recorded || r->rec) return NULL;
  if (!r->rclass || r->rclass_of.hgen != r->hgen || r->rclass_of.shape != comp_class_shape_gen() || r->rclass_of.nclasses != c->nclasses ||
      r->rclass_of.nver != c->nt->version || r->rclass_of.nedits != c->nt->edits) {
    free(r->rclass);
    if (!(r->rclass = calloc((size_t)c->nclasses + 1, sizeof(int)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    r->rclass_of.hgen = r->hgen; r->rclass_of.shape = comp_class_shape_gen(); r->rclass_of.nclasses = c->nclasses;
    r->rclass_of.nver = c->nt->version; r->rclass_of.nedits = c->nt->edits;
  }
  return r->rclass;
}

/* The record class cls is or inherits from (-1 for none): a class of the
   program that is a Struct or Data builder's, or whose superclass is such
   a call. It is a fact of the class, kept per class, so asking it of every
   class down a chain walks the chain once: the walk goes up to the first
   class that answers or is kept, and keeps the answer for each class it
   passed. */
int elab_record_class_of(Compiler *c, int cls) {
  int *memo = lr_rclass_memo(c), ans = -1, a = cls, n = 0;
  for (; a >= 0 && a < c->nclasses && n < lr_chain_bound(c); a = lr_super(c, a), n++) {
    int d = c->classes[a].def_node;
    if (memo && memo[a]) { ans = memo[a] - 2; break; }
    if (lr_record_class(c, a) || (d >= 0 && class_def_is_record_call(c->nt, class_def_super(c->nt, d)))) { ans = a; if (memo) memo[a] = a + 2; break; }
  }
  for (int b = cls; memo && n-- > 0; b = lr_super(c, b)) memo[b] = ans + 2;
  return ans;
}

int elab_record_cmethod(Compiler *c, int cls, const char *name) {
  int a = elab_record_class_of(c, cls);
  if (a < 0) return 0;
  /* (a Data class by its record call, the write's or the superclass's: the
     walk asks before register_structs sets is_data, lr_late_make) */
  int d = c->classes[a].def_node, call = class_def_record(c->nt, d);
  return is_record_class_cmethod(name, class_def_record_is_data(c->nt, call >= 0 ? call : class_def_super(c->nt, d)));
}

int elab_call_method(Compiler *c, int id, int cls, const char *name, int sg, int mm) {
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
  /* an instance's chain passes Object's reopenings: a module they include */
  int ob = o == ELAB_NO && !sg ? lr_core(r, CORE_OBJECT) : -1;
  if (ob >= 0 && ob != cls) o = elab_method_at(c, ob, name, 0, p, NULL);
  /* a method_missing may answer the call */
  if (mm && o == ELAB_NO && (elab_method_versions(c, "method_missing") || elab_method_versions(c, "respond_to_missing?"))) return ELAB_UNKNOWN;
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

int elab_self_value_at(Compiler *c, int node) {
  LoadRecord *r = elab_fresh(c);
  const LrStmt *s = lr_stmt_of(r, node);
  if (!s || s->region < 0 || s->frame < 0 || s->up != s->frame) return LR_UNKNOWN_CLASS;
  return elab_self_at(c, node);
}

/* ---- a module's singleton class (elab_singleton_made) ---- */
/* Which module a form makes the singleton class of: a class id, or */
enum { LR_SG_ANY = -1, LR_SG_NONE = -2, LR_SG_MAIN = -3 };   /* any module it may be; none (an instance, a class); main, as self */
static int lr_sg_at(Compiler *c, int node, int *sing);
static int lr_cvar_cref_at(Compiler *c, int node);
/* Is cls, whose methods a def or a define_method block becomes, a class a
   module may be an instance of: Object, BasicObject, Kernel, Module, the
   Class reopening, or a module mixed into one of them (or none known) */
static int lr_sg_core_host(Compiler *c, int cls) {
  int ks[5] = { comp_core_class(c, CORE_OBJECT), comp_core_class(c, CORE_BASIC_OBJECT), comp_core_class(c, CORE_KERNEL),
                comp_core_class(c, CORE_MODULE), comp_core_class(c, CORE_CLASS_REOPEN) };
  if (cls < 0) return 1;
  for (int i = 0; i < 5; i++) {
    if (ks[i] < 0) continue;
    if (ks[i] == cls) return 1;
    const ClassInfo *k = &c->classes[ks[i]];
    for (int j = 0; j < k->nincluded_mods; j++) if (k->included_mods[j] == cls) return 1;
  }
  return 0;
}
/* ...the self of the body of the method def node `def` makes (a def, or a
   define_method call whose block is the body): an instance of the class
   it defines on, which may be a module only where that is Object, Kernel,
   Module or the like (lr_sg_core_host), as for main's top-level defs,
   which are Object's; none for a singleton method, whose def made the
   singleton class (`def self.m`, a def in a `class << x` body or an
   instance_eval block: lr_sg_at's *sing) */
static int lr_sg_body_self(Compiler *c, int def) {
  int sing = 0;
  if (nt_kind(c->nt, def) == NK_DefNode && nt_ref(c->nt, def, "receiver") >= 0) return LR_SG_NONE;
  int s = lr_sg_at(c, def, &sing);
  return !sing && (s == LR_SG_MAIN || s == LR_SG_ANY || (s >= 0 && lr_sg_core_host(c, s))) ? LR_SG_ANY : LR_SG_NONE;
}
/* ...expression e at node at (-1 for self): self's (lr_sg_at; main is no
   module), a constant's class (lr_class_ref_at), none for a builtin class
   or module the program has no class of, and any module for any other
   value */
static int lr_sg_who(Compiler *c, int at, int e) {
  NodeKind k = e >= 0 ? nt_kind(c->nt, e) : NK_SelfNode;
  int sing, core, ci;
  if (k == NK_SelfNode) return (ci = lr_sg_at(c, at, &sing)) == LR_SG_MAIN ? LR_SG_NONE : ci;
  if (k != NK_ConstantReadNode && k != NK_ConstantPathNode) return LR_SG_ANY;
  ci = lr_class_ref_at(c, e, &core);
  if (ci == LR_LATER) lr_later(c, &ci, e);
  return ci >= 0 ? ci : core >= 0 ? LR_SG_NONE : LR_SG_ANY;
}
/* The class body the analysis puts placed node in (node_cbody) where the
   walk does not open it as a frame, whose self the record's frame (self,
   elab_self_at) is not: a body in a value, as `module V; @x = 1; end
   rescue nil`; -1 otherwise */
static int lr_sg_unframed(Compiler *c, int node, int self) {
  int cb = holder_cbody(c, node);
  return cb >= 0 && (self >= 0 || self == LR_MAIN) && cb != self ? cb : -1;
}
/* ...self where node runs, and in *sing whether a def there makes a
   singleton method of it. A placed node's is the record's (elab_self_at:
   a class or module body's, a constant builder's block's, main's), or in
   a frame that names none, a `class << x` body's (a singleton class) or
   an eval form's receiver (lr_frame_call), on which an instance_eval's
   defs are singleton methods. A node run later (in a method body, a
   lambda, a block kept) runs on the receiver of the nearest eval form
   around it, in a define_method block or a def on that body's self
   (lr_sg_body_self), else on the self of the placed node around it that
   makes the closure. */
static int lr_sg_at(Compiler *c, int node, int *sing) {
  LoadRecord *r = c->lrec;
  const NodeTable *nt = c->nt;
  const LrStmt *s = lr_stmt_of(r, node);
  *sing = 0;
  if (!s) {
    int n;
    const int *par = elab_node_parents(c, &n);
    for (int v = node, p; v >= 0 && v < n && (p = par[v]) >= 0 && p < n; v = p) {
      if (nt_kind(nt, p) == NK_DefNode) return lr_sg_body_self(c, p);
      int call = nt_kind(nt, p) == NK_BlockNode ? par[p] : -1;
      const char *nm = call >= 0 && call < n && nt_kind(nt, call) == NK_CallNode ? nt_str(nt, call, "name") : NULL;
      if (nm && is_eval_exec_family(nm)) { *sing = is_instance_eval_family(nm); return lr_sg_who(c, call, nt_ref(nt, call, "receiver")); }
      if (nm && is_define_method_name(nm)) return lr_sg_body_self(c, call);
      if (nm && is_define_singleton_method_name(nm)) return LR_SG_NONE;
      if (lr_stmt_of(r, p)) return lr_sg_at(c, p, sing);
    }
    return LR_SG_ANY;
  }
  int self = elab_self_at(c, node), u = lr_sg_unframed(c, node, self), f = s->frame >= 0 ? r->st[s->frame].node : -1;
  if (u >= 0) return u;
  if (self >= 0) return self;
  if (self == LR_MAIN) return LR_SG_MAIN;
  if (f >= 0 && nt_kind(nt, f) == NK_SingletonClassNode) { *sing = 1; return LR_SG_NONE; }
  int call = f >= 0 ? lr_frame_call(nt, f) : -1;
  const char *nm = call >= 0 ? nt_str(nt, call, "name") : NULL;
  if (!nm || !is_eval_exec_family(nm)) return LR_SG_ANY;
  *sing = is_instance_eval_family(nm);
  return lr_sg_who(c, call, nt_ref(nt, call, "receiver"));
}
/* The writes that set a variable whatever it holds: `@x &&= v` and
   `@x += v` read it first, which raises or keeps it unset where no write
   made it before */
static int lr_ivar_write_kind(NodeKind k) {
  return k == NK_InstanceVariableWriteNode || k == NK_InstanceVariableOrWriteNode || k == NK_InstanceVariableTargetNode;
}
static int lr_cvar_write_kind(NodeKind k) {
  return k == NK_ClassVariableWriteNode || k == NK_ClassVariableOrWriteNode || k == NK_ClassVariableTargetNode;
}
/* Is a call of `name` on module who the class-level ivar write a desugar
   made of an `@x = v` or an instance_variable_set (civ_ensure_accessor)? */
static int lr_civ_setter_call(Compiler *c, int who, const char *name) {
  int si = who >= 0 ? comp_cmethod_in_class(c, who, name) : -1;
  return si >= 0 && c->scopes[si].def_node >= 0 && nt_int(c->nt, c->scopes[si].def_node, "civ_accessor", 0) == 2;
}
static void lr_sgm_add(Compiler *c, int node, int who, int sure) {
  LoadRecord *r = c->lrec;
  if (who < LR_SG_ANY || (who >= 0 && !comp_class_is_module(c, &c->classes[who]))) return;
  if (r->nsgm == r->csgm) {
    r->csgm = r->csgm ? 2 * r->csgm : 16;
    r->sgm = realloc(r->sgm, sizeof *r->sgm * (size_t)r->csgm);
    if (!r->sgm) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  r->sgm[r->nsgm++] = (struct LrSgMaker){ node, who, (unsigned char)sure };
}
/* The forms of the program that may make a module's singleton class, but
   for the singleton's own entries the histories hold: a maker call
   (is_singleton_class_maker), also through a send of its name, or a
   Method made of it (whose call may come any time: not sure); a send or a
   Method maker of a computed name (which may name another method: not
   sure); `class << x`; an instance variable
   write on a module; a class variable write in one; a def in an
   instance_eval block, the receiver's singleton method; and an eval of a
   String, which may do any of these on any module. Each with the module
   it makes it on (lr_sg_who) and whether it does whenever its node runs.
   With them, the modules a singleton class's chain may hold
   (elab_module_on_singleton_chain): the core hosts' (lr_sg_core_host),
   each extend's and singleton prepend's, and, for an include or a prepend
   into a receiver the record cannot hold as a class
   (`m.singleton_class.include(M)`), any. Made once per map. */
static void lr_sg_makers(Compiler *c) {
  LoadRecord *r = c->lrec;
  if (r->sgm && r->sgm_of.recorded == r->recorded && r->sgm_of.ver == r->ver && r->sgm_of.nclasses == c->nclasses) return;
  r->nsgm = 0;
  r->sgm_of.recorded = r->recorded; r->sgm_of.ver = r->ver; r->sgm_of.nclasses = c->nclasses;
  if (!r->sgm && !(r->sgm = malloc(sizeof *r->sgm * (size_t)(r->csgm = 16)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  free(r->sgchain);
  if (!(r->sgchain = calloc((size_t)c->nclasses + 1, 1))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  r->sgchain_any = 0;
  for (int k = 0; k < c->nclasses; k++) r->sgchain[k] = (unsigned char)lr_sg_core_host(c, k);
  for (int k = LR_MAIN; k + 1 < r->ncls; k++) {
    const LrClass *lc = lr_class(r, k);
    for (int i = 0; lc && i < lc->nmix; i++) {
      int mod = lc->mix[i].mod;
      if (lc->mix[i].kind != LR_EXTEND && lc->mix[i].kind != LR_SG_PREPEND) continue;
      if (mod >= 0 && mod < c->nclasses) r->sgchain[mod] = 1;
      else r->sgchain_any = 1;
    }
  }
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k == NK_CallNode) {
      const char *nm = nt_str(nt, id, "name");
      int a = nt_ref(nt, id, "arguments"), an = 0, recv = nt_ref(nt, id, "receiver");
      const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
      if (!nm) continue;
      NodeKind ak = an > 0 ? nt_kind(nt, av[0]) : NK_ProgramNode, rk = recv >= 0 ? nt_kind(nt, recv) : NK_SelfNode;
      if ((is_include_name(nm) || is_prepend_name(nm)) && rk != NK_ConstantReadNode && rk != NK_ConstantPathNode && rk != NK_SelfNode)
        r->sgchain_any = 1;
      if (id >= r->n) continue;   /* (appended since the map: a desugar's, not the program's) */
      const char *ln = ak == NK_SymbolNode ? nt_str(nt, av[0], "value") : ak == NK_StringNode ? nt_str(nt, av[0], "unescaped") : NULL;
      int lit = ak == NK_SymbolNode || ak == NK_StringNode, sure = 1;
      if (is_code_eval_name(nm) && an > 0) { lr_sgm_add(c, id, LR_SG_ANY, 0); continue; }
      int who = lr_sg_who(c, id, recv);
      if (!is_singleton_class_maker(nm, an)) {
        if (an > 0 && is_send_family(nm) && (!lit || is_singleton_class_maker(ln, an - 1))) sure = lit;
        else if (an > 0 && is_method_object_maker(nm) && (!lit || is_singleton_class_maker(ln, 1))) sure = 0;
        else if (!lr_civ_setter_call(c, who, nm)) continue;
      }
      lr_sgm_add(c, id, who, sure);
    }
    else if (id >= r->n) continue;
    else if (k == NK_SingletonClassNode) lr_sgm_add(c, id, lr_sg_who(c, id, nt_ref(nt, id, "expression")), 1);
    else if (lr_ivar_write_kind(k)) lr_sgm_add(c, id, lr_sg_who(c, id, -1), 1);
    else if (lr_cvar_write_kind(k)) {
      int u = lr_stmt_of(r, id) ? lr_sg_unframed(c, id, elab_self_at(c, id)) : -1, ci = u >= 0 ? u : lr_cvar_cref_at(c, id);
      lr_sgm_add(c, id, ci >= 0 ? ci : ci == LR_MAIN ? LR_SG_NONE : LR_SG_ANY, 1);
    }
    else if (k == NK_DefNode && nt_ref(nt, id, "receiver") < 0) {
      int sing, who = lr_sg_at(c, id, &sing);
      if (sing) lr_sgm_add(c, id, who, 1);
    }
  }
}
/* When form node, which makes a module's singleton class whenever it runs
   (sure) or may, runs as of p: ELAB_YES surely before p, ELAB_NO surely
   not before it, ELAB_UNKNOWN. Surely: a statement placed under no
   condition, which is the form itself or holds it as a value only (an
   assignment's, a multiple assignment's, a call's argument or receiver,
   parentheses), but for a block, which its call runs when it decides,
   and the arguments of a `&.` call, which run only on a receiver that is
   not nil. */
static int lr_sg_when(Compiler *c, int node, int may, int sure, LoadPoint p) {
  LoadRecord *r = c->lrec;
  const NodeTable *nt = c->nt;
  const LrStmt *s = lr_stmt_of(r, node);
  if (!s || s->region < 0 || lr_unplaced(c, node)) return ELAB_UNKNOWN;
  LoadPoint q = { s->region, (int)(s - r->st), node };
  int o = elab_cmp(q, p);
  if (o == PROG_AFTER) return lr_runs_again(c, node, p) ? ELAB_UNKNOWN : ELAB_NO;
  if (o != PROG_BEFORE || may || !sure || s->may || s->partial || s->kept) return ELAB_UNKNOWN;
  int n;
  const int *par = elab_node_parents(c, &n);
  for (int v = node; v != s->node; v = par[v]) {
    if (v < 0 || v >= n || par[v] < 0) return ELAB_UNKNOWN;
    int pv = par[v];
    NodeKind k = nt_kind(nt, pv);
    if (k == NK_CallNode && (nt_ref(nt, pv, "block") == v || (nt_safe_nav(nt, pv) && nt_ref(nt, pv, "receiver") != v))) return ELAB_UNKNOWN;
    if (k != NK_CallNode && k != NK_ArgumentsNode && k != NK_ParenthesesNode && k != NK_StatementsNode && k != NK_MultiWriteNode &&
        k != NK_LocalVariableWriteNode && k != NK_InstanceVariableWriteNode && k != NK_ConstantWriteNode) return ELAB_UNKNOWN;
  }
  return ELAB_YES;
}
int elab_module_on_singleton_chain(Compiler *c, int cls) {
  LoadRecord *r = elab_fresh(c);
  if (!r->recorded || cls < 0 || cls >= c->nclasses) return 1;
  lr_sg_makers(c);
  return r->sgchain_any || r->sgchain[cls];
}
int elab_singleton_made(Compiler *c, int cls, LoadPoint p) {
  LoadRecord *r = elab_fresh(c);
  if (cls < 0 || cls >= c->nclasses) return ELAB_UNKNOWN;
  /* a builtin module CRuby boots with its singleton class made (Math's,
     Process's: their module functions) */
  int slot = builtin_core_class_slot(c->classes[cls].name);
  if (slot >= 0 && comp_core_class(c, slot) == cls && builtin_singleton_at_boot(slot)) return ELAB_YES;
  if (!r->recorded || p.region < 0 || elab_runs_unseen(c)) return ELAB_UNKNOWN;
  int o = ELAB_NO;
  /* the singleton's own entries: its defs and accessors, a
     module_function's copies, define_singleton_method's */
  for (int i = 0; i < r->nhist && o != ELAB_YES; i++) {
    const LrHist *h = &r->hist[i];
    if (h->a != cls || !h->b) continue;
    for (int j = 0; j < h->n && o != ELAB_YES; j++) {
      const LrDef *v = &h->v[j];
      if (v->node >= 0 && nt_int(c->nt, v->node, "civ_accessor", 0)) continue;   /* a class-level ivar's accessor: its calls are the forms */
      int w = v->unplaced || v->body ? ELAB_UNKNOWN : lr_sg_when(c, v->node, v->may, 1, p);
      if (w != ELAB_NO) o = w;
    }
  }
  lr_sg_makers(c);
  for (int i = 0; i < r->nsgm && o != ELAB_YES; i++) {
    const struct LrSgMaker *m = &r->sgm[i];
    if (m->cls != cls && m->cls != LR_SG_ANY) continue;
    int w = lr_sg_when(c, m->node, 0, m->sure, p);
    if (w == ELAB_YES && m->cls == LR_SG_ANY) w = ELAB_UNKNOWN;
    if (w != ELAB_NO) o = w;
  }
  return o;
}

int elab_cref_at(Compiler *c, int node) {
  (void)elab_fresh(c);
  LoadPoint p = elab_point_of(c, node);
  return p.region < 0 ? LR_UNKNOWN_CLASS : lr_cref(c, p.seq);
}

/* A class variable's lookup (CRuby's cvar base) skips the singleton class
   bodies around it and the cref an eval form's block pushes; a builder's
   block pushes none, nor does the anonymous class header a builder's block
   was rewritten to (block_made_body). So: the innermost class or module body
   around the statement node runs from, which for a method body or a block
   run later is the statement that defines or makes it. A clone (a module's
   method copied into each class that includes it) looks up from where the
   node it copies was written, along a chain of up to 64 copies. */
static int lr_cvar_cref_at(Compiler *c, int node) {
  LoadRecord *r = elab_fresh(c);
  LoadPoint p = elab_point_from(c, node);
  for (int depth = 0; p.region < 0 && depth < 64; depth++) {
    node = nt_clone_source(c->nt, node);
    if (node < 0) return LR_UNKNOWN_CLASS;
    p = elab_point_from(c, node);
  }
  if (p.region < 0) return LR_UNKNOWN_CLASS;
  for (int f = r->st[p.seq].frame; f >= 0; f = r->st[f].frame) {
    int h = r->st[f].node;
    NodeKind k = nt_kind(c->nt, h);
    if ((k == NK_ClassNode || k == NK_ModuleNode) && !nt_int(c->nt, h, "block_made_body", 0))
      return elab_header_class(c, h, NULL, NULL);
  }
  return LR_MAIN;
}
/* Asked once the record is made, as elab_class_ref_at is: in the call
   plan's frame, so that a node a pass (or a String mutator's emission)
   appended since answers at its source position, and the map is not
   rebuilt per ask. A node the frame cannot place answers not known. */
int elab_cvar_cref_at(Compiler *c, int node) {
  LoadRecord *r = lr_get(c);
  if (!r->recorded || r->walk || r->plan_depth) return lr_cvar_cref_at(c, node);
  int framed = elab_plan_enter(c, node), ci = LR_UNKNOWN_CLASS;
  if (framed) {
    ci = lr_cvar_cref_at(c, node);
    elab_plan_leave(c);
  }
  return ci;
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
  int kn = target >= 0 ? target : node;
  return nt_const_key_or_name(nt, kn);   /* (const_key_at) */
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
  /* the superclass steps are a loop, as lr_search's; an unsure mixin on
     the way makes the answer unknown */
  int unsure = 0, o;
  for (;; depth++) {
    if (depth > lr_chain_bound(c)) { o = ELAB_UNKNOWN; break; }
    if (cls < LR_MAIN) {
      o = ELAB_UNKNOWN;
      for (int i = 0; builtin_class_at(i); i++) {
        const BuiltinClass *b = builtin_class_at(i);
        if (b->id == cls && builtin_mixin_module_id(b->name)) { o = is_constless_builtin_name(b->name) ? ELAB_NO : ELAB_UNKNOWN; break; }
      }
      break;
    }
    if ((o = lr_owner_last(c, key, lr_const_owner(cls), name, p, late, out)) != ELAB_NO) break;
    if (cls >= 0 && builtin_mixin_module_id(c->classes[cls].name) &&
        !is_constless_builtin_name(c->classes[cls].name)) { o = ELAB_UNKNOWN; break; }
    const LrClass *k = lr_class(r, cls);
    int found = 0;
    for (int i = k ? k->nmix - 1 : -1; i >= 0; i--) {
      LrMixin *m = &k->mix[i];
      int b = elab_mix_on_singleton(m->kind) ? ELAB_NO : lr_mixed_before(c, m, p, late, elab_point_may(c, m->node));
      if (b == ELAB_NO) continue;
      int mo = lr_const_in(c, lr_mixin_class(c, m), name, p, late, top, out, depth + 1, key);
      if (mo == ELAB_YES && b == ELAB_YES) { found = 1; break; }
      if (mo != ELAB_NO) unsure = 1;
    }
    if (found) { o = ELAB_YES; break; }
    int sup = lr_super(c, cls);
    if (sup == LR_MAIN && !top) sup = LR_UNKNOWN_CLASS;
    if (sup < LR_MAIN) { o = ELAB_NO; break; }
    cls = sup;
  }
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
       singleton && cls >= 0 && depth < lr_chain_bound(c); cls = lr_super(c, cls), depth++) {
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
/* The class or module header (or `class << x`) whose body is node id's
   lexical scope, by the parent map par; -1 for a file's top level. A
   method's or a block's body keeps the scope it is written in; a header's
   superclass expression is in the scope around the header. */
int elab_lexical_header(const NodeTable *nt, const int *par, int id) {
  for (int a = par[id], from = id; a >= 0; from = a, a = par[a]) {
    NodeKind k = nt_kind(nt, a);
    if ((k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode) && nt_ref(nt, a, "body") == from) return a;
  }
  return -1;
}

static int lr_later_alias(Compiler *c, int owner, const char *name) {
  LoadRecord *r = c->lrec;
  const NodeTable *nt = c->nt;
  const int *par = NULL;
  for (int i = 0; i < r->nsrcw; i++) {
    int w = r->srcw[i].node, in = LR_MAIN;
    if (!sp_streq(r->srcw[i].name, name) || nt_kind(nt, w) != NK_ConstantWriteNode || !sp_streq(lr_written_name(nt, w), name)) continue;
    if (!par) par = comp_parent_map(c);
    int a = elab_lexical_header(nt, par, w);
    if (a >= 0) in = nt_kind(nt, a) == NK_SingletonClassNode ? LR_UNKNOWN_CLASS : elab_header_class(c, a, NULL, NULL);
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
  h->want = d ? lr_class_of(c, d, NULL) : h->verdict == ELAB_NO ? LR_NEW : LR_UNKNOWN_CLASS;
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

/* Is the superclass node `sup` the constant Object? */
static int elab_names_object(const NodeTable *nt, int sup) {
  return nt_kind(nt, sup) == NK_ConstantReadNode && nt_str(nt, sup, "name") && is_object_class_name(nt_str(nt, sup, "name"));
}

/* ---- hooks fire per point (#9898) ----
   CRuby calls a class's hooks where their event happens: `inherited` on the
   superclass as a class is made, `method_added` as a def, an attr, an
   alias or a define_method adds a method. Whether a hook is live then is
   the record's question at the event's point (elab_method_at): a version
   the program defined before it, an ancestor's reached through a mixin or
   an extend the walk has passed, or none. ELAB_YES and ELAB_UNKNOWN (a
   conditional version, an unplaced file) fire, as the program has always
   run them; ELAB_NO fires nothing, as Class#inherited and the other core
   hooks are no-ops. A fired hook is a call spliced through the walk's host,
   which the walk reaches next and records at the point it stands, and the
   call plan binds there (call_target). */

/* A node of type ty at the line and file of node like */
static int lr_made(NodeTable *nt, int like, const char *ty) {
  int id = nt_new_node(nt, ty);
  if (id < 0) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  nt_node_set_int(nt, id, "node_line", nt_int(nt, like, "node_line", 0));
  nt_node_set_int(nt, id, "node_file", nt_int(nt, like, "node_file", 0));
  nt_node_set_int(nt, id, "node_col", nt_int(nt, like, "node_col", 0));
  return id;
}

/* The call of hook `hook` with one argument (the node arg, or a symbol named
   argname), at the line of node like: `recv.__send__(:hook, arg)` on an
   explicit receiver (a private hook runs too, as CRuby runs it), or the bare
   call `hook(arg)` for recv -1, which runs for the class whose body it
   stands in */
static int lr_hook_call(Compiler *c, int like, int recv, const char *hook, int arg, const char *argname) {
  NodeTable *nt = (NodeTable *)c->nt;
  int call = lr_made(nt, like, "CallNode"), args = lr_made(nt, like, "ArgumentsNode");
  if (!arg) {
    arg = lr_made(nt, like, "SymbolNode");
    nt_node_set_str(nt, arg, "value", argname);
  }
  if (recv < 0) {
    nt_node_set_arr(nt, args, "arguments", &arg, 1);
    nt_node_set_str(nt, call, "name", hook);
  }
  else {
    int sym = lr_made(nt, like, "SymbolNode"), av[2] = { sym, arg };
    nt_node_set_str(nt, sym, "value", hook);
    nt_node_set_arr(nt, args, "arguments", av, 2);
    nt_node_set_str(nt, call, "name", "__send__");
    nt_node_set_ref(nt, call, "receiver", recv);
  }
  nt_node_set_ref(nt, call, "arguments", args);
  return call;
}

/* `Super.__send__(:inherited, self)` first in the body of the class header
   or builder at node, as the walk makes the class (elab_rec_open) with a
   constant superclass: the call CRuby makes before the class's body runs, or,
   for Class.new(B), before its block does and before the constant is
   written. A reopening makes no class and calls nothing. The superclass's
   `inherited` at the header's point decides (a hook the program removed, or
   defines after the class, is not live there). A version the record cannot
   place fires, and so does a superclass it does not hold a class for (a
   Struct or Data class, registered after the walk): the call binds to the
   hook the program has, or is a no-op row (builtin_ops.c). A core class the
   program does not reopen fires nothing. */
static void lr_fire_inherited(Compiler *c, int node) {
  LoadRecord *r = c->lrec;
  NodeTable *nt = (NodeTable *)c->nt;
  HostStep *h = elab_walk_host(c);
  int sc = class_def_super(nt, node);
  if (!h || sc < 0 || !class_def_is_class(nt, node) || !lr_named(c, "inherited") ||
      (nt_kind(nt, sc) != NK_ConstantReadNode && nt_kind(nt, sc) != NK_ConstantPathNode))
    return;
  /* the superclass reference stands at the header's point, which the walk
     places it at next */
  lr_fit(r, nt->count);
  lr_place_ref(r, nt, sc, r->at[node], 0);
  int sup = lr_class_ref_at(c, sc, NULL);
  const char *sn = lr_written_name(nt, sc);
  if (sup < 0 && sup != LR_LATER && (is_builtin_class_name(sn) || is_builtin_exception_name(sn)) && !lr_reopened(r, sn)) return;
  if (sup >= 0 && elab_method_at(c, sup, "inherited", 1, elab_point_of(c, node), NULL) == ELAB_NO) return;
  int body = class_def_body(nt, node), bn = 0;
  if (body < 0) {
    body = lr_made(nt, node, "StatementsNode");
    nt_node_set_arr(nt, body, "body", NULL, 0);
    class_def_set_body(nt, node, body);
  }
  const int *bv = nt_arr(nt, body, "body", &bn);
  int *out = malloc(sizeof(int) * (size_t)(bn + 1)), recv = nt_clone_subtree(nt, sc);
  if (!out || recv < 0) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int call = lr_hook_call(c, node, recv, "inherited", lr_made(nt, node, "SelfNode"), NULL);
  nt_node_set_int(nt, call, "class_recv", 1);   /* (a superclass is a class: the receiver is proven one) */
  out[0] = call;
  if (bn) memcpy(out + 1, bv, sizeof(int) * (size_t)bn);
  nt_node_set_arr(nt, body, "body", out, bn + 1);
  free(out);
  comp_grow_node_arrays(c);
  host_splice(c, h, node, body, 0, 1, NULL, NULL);
  comp_grow_node_arrays(c);
}

/* The definition form whose hooks a version of kind kind fires
   (definition_form_hooks); NULL for a version it calls none for. A
   visibility word (`private :a`) is one only where it gives the class an
   entry of its own, which elab_fire_after decides. */
static const char *lr_event_form(LrKind kind) {
  switch (kind) {
    case LR_DEF: case LR_DEFINE_METHOD: case LR_READER: case LR_WRITER: case LR_ALIAS: case LR_VIS: return "define_method";
    case LR_COPY: return "module_function";
    case LR_UNDEF: return "undef_method";
    case LR_REMOVE: return "remove_method";
    default: return NULL;
  }
}

/* The hook CRuby calls for a version of kind kind added to a class
   (singleton class when sg), NULL for none */
static const char *lr_event_hook(LrKind kind, int sg) {
  const char *form = lr_event_form(kind), *hook[2];
  return form && definition_form_hooks(form, sg, hook) ? hook[0] : NULL;
}

/* Does the program define a hook a version fires (lr_event_hook)? */
static int lr_event_hooks(Compiler *c) {
  LoadRecord *r = c->lrec;
  if (!r->evhooks) {
    const LrKind kinds[] = { LR_DEF, LR_UNDEF, LR_REMOVE };
    r->evhooks = 1;
    for (int sg = 0; sg < 2; sg++)
      for (int k = 0; k < (int)(sizeof kinds / sizeof *kinds); k++)
        if (lr_named(c, lr_event_hook(kinds[k], sg))) r->evhooks = 2;
  }
  return r->evhooks == 2;
}

/* A version the walk has recorded at the statement it is recording
   (ElabRec.cur) whose hook the program may define, noted for the scope walk
   to fire as it leaves the statement. Only a version on the class whose
   body stands: its `self` names the receiver. */
static void lr_event(ElabRec *x, int cls, int sg, const char *name, LrKind kind, int node, int vis) {
  LoadRecord *r = x->r;
  const char *hook = lr_event_hook(kind, sg);
  if (x->unplaced || !r->walk || !hook || !name || cls < 0 || cls != x->cfc || (x->cfs != LR_FS_NONE && !(x->cfs == LR_FS_SCLASS && sg)) ||
      !lr_named(x->c, hook)) return;   /* (a hook the program defines nowhere is the core's no-op) */
  /* a conditional def is its branches' arms (named `m#__condN`), which run
     where the branch does, and a dispatcher that is no def of the program's:
     the hook is told the name the program wrote, once the arm ran */
  if (kind == LR_DEF && nt_int(x->c->nt, node, "cond_lone_disp", 0)) return;
  int arm = kind == LR_DEF && nt_int(x->c->nt, node, "cond_arm", 0) ? mf_arm_base_len(name) : -1;
  size_t len = arm >= 0 ? (size_t)arm : strlen(name);
  r->ev = lr_grow(r->ev, &r->cev, r->nev, sizeof *r->ev);
  char *copy = malloc(len + 1);
  if (!copy) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(copy, name, len);
  copy[len] = 0;
  r->ev[r->nev] = (LrEvent){ .stmt = x->cur, .cls = cls, .node = node, .mod = -1, .arg = -1, .sg = (unsigned char)sg,
                             .kind = (unsigned char)kind, .vis = (unsigned char)vis, .name = copy };
  r->nev++;
}

/* A node naming the class or module the header, or the class builder's write
   (`K = Class.new do`), `def` opens, for a call's receiver: the constant it
   is written under, cloned or made (-1: none to be had, as for a builder held
   in a local or written under a path, whose constant a read in its block
   cannot name: a bare call there). A bare call would bind to a top-level
   function of the hook's name before the class's own hook, as the call of a
   hook the class defines reaches it by the constant; the constant also runs
   the hook for the class whose body the call stands in (self.m would run it
   for the class that defines it). */
static int lr_frame_const(Compiler *c, int def) {
  NodeTable *nt = (NodeTable *)c->nt;
  int cp = class_def_path(nt, def), r;
  if (class_def_builder(nt, def) < 0)
    return nt_kind(nt, cp) == NK_ConstantReadNode || nt_kind(nt, cp) == NK_ConstantPathNode ? nt_clone_subtree(nt, cp) : -1;
  if (!class_def_bare_name(nt, def)) return -1;
  r = lr_made(nt, def, "ConstantReadNode");
  nt_node_set_str(nt, r, "name", class_def_bare_name(nt, def));
  return r;
}

/* The receiver of a hook call after a statement in the body of `frame` (the
   header of a class or module, a class builder's write, or a `class << X`):
   the constant the class, module or builder is written under (lr_frame_const;
   -1, a bare call, for a body with none), or, in a singleton class's body, the
   constant `class << K` names, or, for `class << self`, the one the class
   around it is written under (self is the singleton class there). -2 for none
   to be had. */
static int lr_hook_recv(Compiler *c, int frame) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (nt_kind(nt, frame) != NK_SingletonClassNode) return lr_frame_const(c, frame);
  int ex = nt_ref(nt, frame, "expression"), recv = -1;
  if (nt_kind(nt, ex) == NK_SelfNode) {
    const LrStmt *s = lr_stmt_of(c->lrec, frame);
    int up = s && s->frame >= 0 ? c->lrec->st[s->frame].node : -1;
    recv = up >= 0 && class_def_opens_frame(nt, up) ? lr_frame_const(c, up) : -1;
  }
  else if (nt_kind(nt, ex) == NK_ConstantReadNode || nt_kind(nt, ex) == NK_ConstantPathNode) recv = nt_clone_subtree(nt, ex);
  return recv < 0 ? -2 : recv;
}

/* Is the form at node `form` the statement itself, or an argument of the
   call that is (`private def m`, `memoize def m`, `private attr_accessor
   :a`)? A form deeper in a statement (a def used as a value) runs before the
   rest of its statement, whose effects the hook would follow. */
static int lr_form_leads(const NodeTable *nt, int stmt, int form) {
  if (form == stmt) return 1;
  int an = nt_kind(nt, stmt) == NK_CallNode ? nt_ref(nt, stmt, "arguments") : -1, n = 0;
  const int *v = an >= 0 ? nt_arr(nt, an, "arguments", &n) : NULL;
  for (int i = 0; i < n; i++) if (v[i] == form) return 1;
  return 0;
}

/* Is `hook` live on cls's singleton at p, or may it be: the record says so or
   does not know, or code it cannot follow ran on cls or a superclass before
   p (a class method that defines one), in a program that defines a method of
   that name somewhere */
/* Does owner k's opaque history hold code that may give it a hook: one
   that may define (LR_OPAQUE_DEFINES), but not an include, extend or
   prepend of core modules alone (`include Comparable`), which the record
   keeps as opaque since it does not hold the module, and which defines no
   method of the program's on the includer (#11040) */
/* Is constant read n (`Missing`, `Missing::Thing`) one whose first name
   the program defines nowhere -- no class or module of it, no constant
   write, no const_set or autoload naming it, no const_missing to answer
   it -- and no builtin has? Reading it raises NameError, so a mixin of it
   mixes nothing in (#11132). */
static int lr_const_root_undefined(const NodeTable *nt, int n) {
  while (n >= 0 && nt_kind(nt, n) == NK_ConstantPathNode && nt_ref(nt, n, "parent") >= 0) n = nt_ref(nt, n, "parent");
  NodeKind k = n >= 0 ? nt_kind(nt, n) : NK_NONE;
  const char *nm = k == NK_ConstantReadNode || k == NK_ConstantPathNode ? nt_str(nt, n, "name") : NULL;
  if (!nm || is_builtin_class_name(nm) || is_builtin_module_const_name(nm)) return 0;
  for (int id = 0; id < nt->count; id++) {
    NodeKind ik = nt_kind(nt, id);
    const char *in = NULL;
    if (ik == NK_ClassNode || ik == NK_ModuleNode) in = class_def_name(nt, id);
    else if (ik == NK_ConstantWriteNode || ik == NK_ConstantOrWriteNode || ik == NK_ConstantTargetNode) in = nt_str(nt, id, "name");
    else if (ik == NK_ConstantPathTargetNode) in = nt_str(nt, id, "name");
    else if (ik == NK_DefNode) { const char *dn = nt_str(nt, id, "name"); if (dn && sp_streq(dn, "const_missing")) return 0; }
    else if (ik == NK_CallNode) {
      const char *cn = nt_str(nt, id, "name");
      if (cn && (sp_streq(cn, "const_set") || sp_streq(cn, "autoload"))) {
        int an = nt_ref(nt, id, "arguments"), ac = 0;
        const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
        if (ac < 1) return 0;
        NodeKind ak = nt_kind(nt, av[0]);
        in = ak == NK_SymbolNode ? nt_str(nt, av[0], "value") : ak == NK_StringNode ? nt_str(nt, av[0], "unescaped") : NULL;
        if (!in) return 0;   /* a computed name may be any */
      }
    }
    if (in && sp_streq(in, nm)) return 0;
  }
  return 1;
}
static int lr_opaque_may_define_hook(Compiler *c, int k) {
  const NodeTable *nt = c->nt;
  const LrHist *op = lr_hist(c->lrec, LR_KEY_OPAQUE, lr_const_owner(k), "", 0);
  for (int j = 0; op && j < op->n; j++) {
    if (op->v[j].vis != LR_OPAQUE_DEFINES) continue;
    int n = op->v[j].node, core = 0;
    const char *nm = n >= 0 && nt_kind(nt, n) == NK_CallNode ? nt_str(nt, n, "name") : NULL;
    if (nm && (sp_streq(nm, "include") || sp_streq(nm, "extend") || sp_streq(nm, "prepend"))) {
      int an = nt_ref(nt, n, "arguments"), ac = 0;
      const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
      core = ac > 0;
      /* (core modules, or constants the program defines nowhere: a NameError) */
      for (int q = 0; q < ac && core; q++)
        core = (nt_kind(nt, av[q]) == NK_ConstantReadNode && nt_str(nt, av[q], "name") &&
                is_builtin_module_const_name(nt_str(nt, av[q], "name")) &&
                lr_class_ref_at(c, av[q], NULL) < 0) || lr_const_root_undefined(nt, av[q]);
    }
    if (!core) return 1;
  }
  return 0;
}
static int lr_hook_live(Compiler *c, int cls, const char *hook, LoadPoint p, const LrDef **d) {
  if (elab_method_at(c, cls, hook, 1, p, d) != ELAB_NO) return 1;
  if (!lr_defines(c->lrec, hook)) return 0;
  for (int a = cls, i = 0; a >= 0 && i < lr_chain_bound(c); a = lr_super(c, a), i++)
    if (elab_opaque_before(c, a, p) != ELAB_NO && lr_opaque_may_define_hook(c, a)) return 1;
  return 0;
}

/* Does the core define method `name` on a class (its singleton when sg): a
   Class row, or one of Object's instance methods */
static int lr_core_method(int sg, const char *name) {
  if (!sg) return builtin_object_method_known(name);
  for (int i = 0; i < bop_row_count(); i++) {
    const BuiltinOp *r = bop_row(i);
    if (r->recv == TY_CLASS && sp_streq(r->name, name)) return 1;
  }
  return 0;
}

/* What a mixin's hook puts after the mixin: one span of statements, each a
   scope of its own (host_splice), run in a frame of the locals `locals`
   but `gone` (NULL: not known, none) */
typedef struct { int *ids; int n; char *locals, *gone; } LrSpan;
typedef struct { LrSpan *v; int n, cap; } LrSpans;

static void lr_span_add(LrSpans *s, int *ids, int n, char *locals, const char *gone) {
  if (n <= 0) { free(ids); free(locals); return; }
  s->v = lr_grow(s->v, &s->cap, s->n, sizeof *s->v);
  s->v[s->n].ids = ids; s->v[s->n].n = n; s->v[s->n].locals = locals;
  s->v[s->n].gone = gone ? strdup(gone) : NULL;
  if (gone && !s->v[s->n].gone) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  s->n++;
}

/* Does any name the program defines a mixin hook (lr_mix_hook)? */
static int lr_mix_hooks(Compiler *c) {
  return lr_named(c, "included") || lr_named(c, "extended") || lr_named(c, "prepended");
}

/* The hook a mixin event v fires, after the mixin statement node in the body
   of frame (a class or module header), at point p: the version live on the
   module's singleton there (lr_hook_live) or none. A `def self.<hook>(base)`
   of the module's own body, run by a receiverless mixin, is spliced as the
   pre-walk splice splices it (incl_subst, incl_emit_stmt): base reads the
   includer's constant, self the module. One with a `return` or a `super` of
   its own, the conditional dispatcher, a hook reached another way (`class <<
   self`, a module the module extends, define_singleton_method) or a version
   the record cannot name is called: `M.__send__(:included, K)`. A version
   that carries a shape a step before the walk reads (incl_hook_declined)
   fires nothing, as it never did where the pre-walk splice does not take
   the mixin; nor does a core class's reopening, whose mixins the builtin
   reopen steps read. */
static void lr_fire_mixin(Compiler *c, const LrEvent *v, int node, int frame, LoadPoint p, LrSpans *out) {
  NodeTable *nt = (NodeTable *)c->nt;
  const LrDef *d = NULL;
  if (v->cls < 0 || v->cls >= c->nclasses || v->mod < 0 || v->mod >= c->nclasses) return;
  if (is_builtin_class_name(c->classes[v->cls].name) || is_builtin_exception_name(c->classes[v->cls].name)) return;
  if (!lr_hook_live(c, v->mod, v->name, p, &d)) return;
  int dn = d ? d->node : -1;
  if (dn >= 0 && nt_kind(nt, dn) == NK_DefNode && incl_hook_declined(nt, dn)) return;
  /* the includer: the frame's class, or the receiver a record-taken
     mixin names (`K.include(M)` at the top level or in another body) */
  int recv = nt_int(nt, node, "mixin_by_record", 0) ? nt_ref(nt, node, "receiver") : -1;
  int base = recv >= 0 ? nt_clone_subtree(nt, recv) : lr_frame_const(c, frame);
  if (base < 0) return;   /* (a body with no constant to name its class by) */
  /* spliced: the module's own def self.<hook>(base), one required parameter
     (a keyword or a block parameter beside it as the pre-walk splice takes
     it: the mixin passes neither) */
  int ps = dn >= 0 && nt_kind(nt, dn) == NK_DefNode ? nt_ref(nt, dn, "parameters") : -1, rn = 0;
  const int *rq = ps >= 0 ? nt_arr(nt, ps, "requireds", &rn) : NULL;
  const LrStmt *ds = dn >= 0 ? lr_stmt_of(c->lrec, dn) : NULL;
  int hdr = ds && ds->frame >= 0 ? c->lrec->st[ds->frame].node : -1;
  int dr = dn >= 0 ? nt_ref(nt, dn, "receiver") : -1, hb = dn >= 0 ? nt_ref(nt, dn, "body") : -1;
  const char *param = rn == 1 ? nt_str(nt, rq[0], "name") : NULL;
  int splice = param && dr >= 0 && nt_kind(nt, dr) == NK_SelfNode && hdr >= 0 && class_def_is_module(nt, hdr) &&
               nt_ref(nt, node, "receiver") < 0 && !nt_int(nt, dn, "cond_lone_disp", 0) &&
               (nt_kind(nt, frame) == NK_ClassNode || nt_kind(nt, frame) == NK_ModuleNode) &&
               !incl_hook_returns(nt, hb) && !incl_hook_supers(nt, hb);
  if (!splice) {
    int call = lr_hook_call(c, node, nt_clone_subtree(nt, v->arg), v->name, base, NULL);
    nt_node_set_int(nt, call, "class_recv", 1);   /* (a module is a class value: the receiver is proven one) */
    nt_node_set_int(nt, call, "event_hook", 1);
    int *ids = malloc(sizeof(int));
    if (!ids) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    ids[0] = call;
    lr_span_add(out, ids, 1, NULL, NULL);
    return;
  }
  if (hb < 0) return;   /* an empty hook: nothing to run */
  int clone = nt_clone_subtree(nt, hb);
  if (clone < 0) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  InclSubst is = { param, NULL, v->arg, hdr, base };
  incl_subst(nt, clone, &is, 1);
  const char *hl = nt_str(nt, dn, "locals");
  char *fr = strdup(hl ? hl : "");   /* the hook's frame, then the blocks' it unwraps */
  if (!fr) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int *ids = NULL, n = 0, cap = 0, hn = 0;
  const int *hs = nt_kind(nt, clone) == NK_StatementsNode ? nt_arr(nt, clone, "body", &hn) : NULL;
  if (!hs) incl_emit_stmt(nt, clone, &ids, &n, &cap, &fr);
  else {
    int *cp = malloc(sizeof(int) * (size_t)(hn + 1));
    if (!cp) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memcpy(cp, hs, sizeof(int) * (size_t)hn);
    for (int q = 0; q < hn; q++) incl_emit_stmt(nt, cp[q], &ids, &n, &cap, &fr);
    free(cp);
  }
  lr_span_add(out, ids, n, fr, param);
}

/* Does an event before ev[i] in the statement at e (events from first on)
   take the same name on the same class, as `remove_method :x, :x` does? */
static int lr_event_repeats(const LoadRecord *r, int first, int i, int e) {
  const LrEvent *v = &r->ev[i];
  for (int q = first; q < i; q++)
    if (r->ev[q].stmt == e && r->ev[q].kind == v->kind && r->ev[q].cls == v->cls && r->ev[q].sg == v->sg &&
        strcmp(r->ev[q].name, v->name) == 0) return 1;
  return 0;
}

/* The hook calls for the versions statement list[j] added, spliced after it:
   `C.method_added(:name)`, or `C.__send__(:singleton_method_added, :name)`
   (BasicObject's: a send reaches a top-level def of the name), for each hook
   live on the class at the point just after the statement. The walk reaches
   them next, as the statements after it in the list. The number spliced. */
int elab_fire_after(Compiler *c, int list, int j) {
  LoadRecord *r = c->lrec;
  NodeTable *nt = (NodeTable *)c->nt;
  HostStep *h = elab_walk_host(c);
  int bn = 0;
  if (!h || (!lr_event_hooks(c) && !lr_mix_hooks(c))) return 0;
  lr_record_upto(r, r->nst);   /* the statement is recorded: the hooks are asked after its versions */
  const int *st = r->nev ? nt_arr(nt, list, "body", &bn) : NULL;
  if (!st || j >= bn) return 0;
  int node = st[j], frame = -1;
  int e = node < r->n ? r->at[node] : -1;
  if (e < 0 || r->st[e].node != node || elab_stmt_list(c, node, &frame) != list) return 0;
  /* in a class or module body, or a class builder's block: a class_eval
     block run in a frame of its own (a top-level one) has the caller's self */
  int body = frame >= 0 && (class_def_opens_frame(nt, frame) || nt_kind(nt, frame) == NK_SingletonClassNode);
  int first = r->nev;
  while (first > 0 && r->ev[first - 1].stmt >= e) first--;
  int *calls = malloc(sizeof(int) * (size_t)(r->nev - first + 1)), n = 0;
  if (!calls) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  LoadPoint p = { r->st[e].region, e + 1, -1 };
  LrSpans mix = { 0 };
  int dead = 0;   /* a name of the statement raised NameError, which ends it */
  for (int i = first; i < r->nev; i++) {
    LrEvent *v = &r->ev[i];
    if (v->mix) {   /* a mixin's: the module's hook, after the mixin (lr_fire_mixin) */
      if ((body || nt_int(nt, node, "mixin_by_record", 0)) && v->stmt == e && v->node == node)
        lr_fire_mixin(c, v, node, frame, p, &mix);
      continue;
    }
    const char *hook = lr_event_hook((LrKind)v->kind, v->sg);
    const LrDef *d = NULL;
    int live = body && v->stmt == e && lr_form_leads(nt, node, v->node) && lr_hook_live(c, v->cls, hook, p, &d);
    /* a visibility word gives the class an entry of its own, and calls the
       hook, only for a method it does not hold itself and whose visibility
       it changes */
    if (live && v->kind == LR_VIS) {
      int was = elab_method_vis_at(c, v->cls, v->name, 0, (LoadPoint){ p.region, e, -1 });
      live = !v->sg && elab_own_method_at(c, v->cls, v->name, p) == ELAB_NO && was >= 0 && was != v->vis;
    }
    /* an undef of a name that is not defined raises NameError, and fires
       nothing: one the record shows undefined already (`undef a` a second
       time) or finds nowhere, unless the core defines it */
    if (live && v->kind == LR_UNDEF) {
      LoadPoint before = { p.region, e, -1 };
      int unsure = 0;
      live = !dead && !lr_event_repeats(r, first, i, e) &&
             lr_search_one(c, v->cls, v->sg, v->name, -1, before, p.region < 0, 0, NULL, 0, &unsure, 1) != LR_STOP &&
             (elab_method_at(c, v->cls, v->name, v->sg, before, NULL) != ELAB_NO || lr_core_method(v->sg, v->name));
      dead |= !live;
    }
    /* a removal takes the class's own entry: it raises NameError, and fires
       nothing, for a name the class does not hold itself (one it inherits,
       one removed or undefined already). One under a condition may be held. */
    if (live && v->kind == LR_REMOVE) {
      int own = lr_own(c, lr_hist(r, v->cls, v->sg, v->name, 0), (LoadPoint){ p.region, e, -1 }, p.region < 0, 0, NULL);
      live = !dead && !lr_event_repeats(r, first, i, e) && (own == ELAB_YES || own == ELAB_UNKNOWN);
      dead |= !live;
    }
    int recv = live ? lr_hook_recv(c, frame) : -2;
    if (recv > -2) {
      int mod = recv >= 0 && is_module_hook_name(hook);
      calls[n] = lr_hook_call(c, node, mod ? -1 : recv, hook, 0, v->name);
      if (mod) {   /* (a class body's call of its own class method: C.m, still a call on self) */
        nt_node_set_ref(nt, calls[n], "receiver", recv);
        nt_node_set_int(nt, calls[n], "self_call", 1);
        nt_set_ignore_visibility(nt, calls[n], 1);
      }
      nt_node_set_int(nt, calls[n++], "event_hook", 1);
    }
  }
  for (int i = first; i < r->nev; i++) free(r->ev[i].name);
  r->nev = first;
  /* the mixins' hooks first (a mixin statement adds no method), then the
     method hooks' calls, each span a scope of its own */
  if (n) lr_span_add(&mix, calls, n, NULL, NULL);
  else free(calls);
  int total = 0;
  for (int k = 0; k < mix.n; k++) total += mix.v[k].n;
  if (total) {
    int *out = malloc(sizeof(int) * (size_t)(bn + total)), at = j + 1;
    if (!out) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memcpy(out, st, sizeof(int) * (size_t)(j + 1));
    for (int k = 0; k < mix.n; k++) { memcpy(out + at, mix.v[k].ids, sizeof(int) * (size_t)mix.v[k].n); at += mix.v[k].n; }
    memcpy(out + at, st + j + 1, sizeof(int) * (size_t)(bn - j - 1));
    nt_node_set_arr(nt, list, "body", out, bn + total);
    free(out);
    comp_grow_node_arrays(c);
    total = 0;
    for (int k = 0, at2 = j + 1; k < mix.n; k++) {
      int got = host_splice(c, h, frame, list, at2, mix.v[k].n, mix.v[k].locals, mix.v[k].gone);
      comp_grow_node_arrays(c);
      at2 += got; total += got;
    }
  }
  for (int k = 0; k < mix.n; k++) { free(mix.v[k].ids); free(mix.v[k].locals); free(mix.v[k].gone); }
  free(mix.v);
  return total;
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
    c->classes[ci].enclosing_class = encl; comp_class_path_moved();
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
    }
    /* `class ::B` (`::B = Class.new do ... end`) names B at the top level
       wherever it is written: no module encloses it, and its name is B */
    else if (par < 0 && path) {
      c->classes[ci].enclosing_class = -1; comp_class_path_moved();
    }
  }
  /* A class can be opened bare first (no superclass -- e.g. just to hold a
     nested class) and reopened later with `< Super`. The parent link is read
     from def_node's "superclass" ref, so prefer the opening that declares one;
     otherwise the superclass is lost and a subclass's overrides aren't
     dispatched against the right ancestor chain (matz/spinel#1477). A
     class builder's definition stays when the header names Object: the
     builder's call states the superclass (none means Object), so that header
     only reopens the class and the builder is still how the class is made
     (the path write the presence logic reads). Any other superclass is a
     superclass mismatch in CRuby (TypeError), and so is a class header over
     a module builder; neither is raised here, and the header takes the
     definition as it did. */
  else if (class_def_super(c->nt, node) >= 0 && c->classes[ci].def_node >= 0 &&
           class_def_super(c->nt, c->classes[ci].def_node) < 0 &&
           (class_def_builder(c->nt, c->classes[ci].def_node) < 0 ||
            !class_def_is_class(c->nt, c->classes[ci].def_node) ||
            !elab_names_object(c->nt, class_def_super(c->nt, node))))
    { comp_class_set_def(c, ci, node); if (c->lrec) { free(c->lrec->rclass); c->lrec->rclass = NULL; } }   /* (the record class of a class is of its definition: elab_record_class_of) */
  elab_header_opened(c, node, ci, made);
  if (made) lr_fire_inherited(c, node);
  return ci;
}

/* What a class builder's call makes (EB_*), noted where the builders are
   lowered, ahead of the walk (desugar_class_new_blocks): the node -> class
   map holds it beside the headers, for the walk and the call plan to read.
   A builder whose class the program cannot have (one built from a
   superclass only the run time holds) is noted at its call and at the
   constant write holding it. A copy of such a node (a clone: a module's
   method copied into each class that includes it) answers as the node it
   copies (nt_clone_source), along a chain of up to 64 copies. */
void elab_builder_note(Compiler *c, int node, int kind) {
  lr_header(lr_get(c), node, 1)->builder = (unsigned char)kind;
}
int elab_builder_kind(const Compiler *c, int node) {
  for (int depth = 0; c->lrec && node >= 0 && depth < 64; depth++) {
    const LrHeader *h = lr_header(c->lrec, node, 0);
    if (h) return h->builder;
    int o = nt_clone_source(c->nt, node);
    if (o < 0) break;
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
  const char *nm = node >= 0 ? nt_str(c->nt, node, "name") : NULL, *ky = node >= 0 ? nt_const_key(c->nt, node) : NULL;
  /* (the name the node carries, or its key: the record has the name written) */
  if (name && ((nm && sp_streq(name, nm)) || (ky && sp_streq(name, ky)))) name = lr_written_name(c->nt, node);
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
const NodeKind *elab_const_write_kinds(int *n) {
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
  const NodeKind *kinds = elab_const_write_kinds(&nk);
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

/* The path a constant write w names its class by (`A::X = 1`, `class
   A::X`): the ConstantPath whose parent is that class; -1 where w writes
   the bare name, in the class whose body is its lexical scope */
static int lr_write_path(const NodeTable *nt, int w) {
  NodeKind k = nt_kind(nt, w);
  int p = k == NK_ConstantPathTargetNode ? w
        : k == NK_ClassNode || k == NK_ModuleNode ? nt_ref(nt, w, "constant_path")
        : k == NK_ConstantPathWriteNode || k == NK_ConstantPathOrWriteNode || k == NK_ConstantPathAndWriteNode ||
          k == NK_ConstantPathOperatorWriteNode ? nt_ref(nt, w, "target") : -1;
  return p >= 0 && (p == w || nt_kind(nt, p) == NK_ConstantPathNode) ? p : -1;
}

/* May constant cp (a header's name, a path's parent) hold Object? Where it
   is no constant, is Object's name, or is a name a write other than a
   header gives (`O = Object; class O`) */
static int lr_may_name_object(const LoadRecord *r, const NodeTable *nt, int cp) {
  NodeKind k = cp >= 0 ? nt_kind(nt, cp) : NK_NONE;
  const char *leaf = k == NK_ConstantReadNode || k == NK_ConstantPathNode ? lr_written_name(nt, cp) : NULL;
  if (!leaf || is_object_class_name(leaf)) return 1;
  for (int i = 0; i < r->nsrcw; i++) {
    NodeKind wk = nt_kind(nt, r->srcw[i].node);
    if (wk != NK_ClassNode && wk != NK_ModuleNode && sp_streq(r->srcw[i].name, leaf)) return 1;
  }
  return 0;
}

/* Does the program's source write constant `name` (a write of any form, a
   class or module header; elab_note_const_writes)? With `top`, only where
   a lookup from the top level finds it: in Object, or a class the source
   does not tell apart from it. A write in the body of a header that names
   another class (lr_may_name_object), in a singleton class's body, or by a
   path whose parent is such a class, is that class's; one at a file's top
   level, in a method or a block there (a block keeps the lexical scope it
   is written in), or by `::X`, is Object's. A `rescue` naming a builtin
   exception class may name the program's own then (prog_note_rescues). */
int elab_source_writes_const(Compiler *c, const char *name, int top) {
  LoadRecord *r = lr_get(c);
  const NodeTable *nt = c->nt;
  const int *par = top ? comp_parent_map(c) : NULL;
  for (int i = 0; i < r->nsrcw; i++) {
    if (!sp_streq(r->srcw[i].name, name)) continue;
    int w = r->srcw[i].node, p = top ? lr_write_path(nt, w) : -1, in_top = 1;
    if (p >= 0) in_top = nt_ref(nt, p, "parent") < 0 || lr_may_name_object(r, nt, nt_ref(nt, p, "parent"));
    else if (top) {
      int a = elab_lexical_header(nt, par, w);
      in_top = a < 0 || (nt_kind(nt, a) != NK_SingletonClassNode && lr_may_name_object(r, nt, nt_ref(nt, a, "constant_path")));
    }
    if (in_top) return 1;
  }
  return 0;
}

/* May the program write a constant, one already held included, under a
   name its source does not spell: by const_set or an eval of a String
   (also through a send), or by a send of a name it computes? With `unheld`,
   also where it defines one only while the name holds nothing: by an
   autoload (one of a held name does nothing), or in a const_missing it
   defines. Read from the tree as it stands (the caller keeps the answer). */
int elab_consts_by_value(Compiler *c, int unheld) {
  const NodeTable *nt = c->nt;
  int hit = 0, n = 0;
  const int *v = nt_nodes_of_kind(nt, NK_CallNode, &n);
  for (int i = 0; !hit && i < n; i++) {
    const char *m = call_sent_name(nt, v[i], NULL);
    int an = 0;
    call_args(nt, v[i], &an);
    hit = !m || is_const_set_name(m) || (unheld && is_autoload_name(m)) || (is_code_eval_name(m) && an > 0);
  }
  v = unheld ? nt_nodes_of_kind(nt, NK_DefNode, &n) : NULL;
  for (int i = 0; !hit && v && i < n; i++) hit = is_const_missing_name(nt_str(nt, v[i], "name"));
  return hit;
}

static struct LrFinal *lr_final(LoadRecord *r, const char *name, int make) {
  unsigned b = lr_hash(0, 1, name);
  for (int k = r->fin_head[b]; k >= 0; k = r->fin[k].next)
    if (sp_streq(r->fin[k].name, name)) return &r->fin[k];
  if (!make) return NULL;
  r->fin = lr_grow(r->fin, &r->cfin, r->nfin, sizeof *r->fin);
  int k = r->nfin++;
  r->fin[k] = (struct LrFinal){ .name = name, .next = r->fin_head[b] };
  r->fin_head[b] = k;
  return &r->fin[k];
}

/* The facts lr_const_seen and lr_const_final read, of every written name
   at once: one pass over the source's writes, the tree's writes and its
   calls, kept while the tree, the recorded statements and the histories
   stand. Asked per constant, a scan per name was quadratic in the
   program's constants. */
static void lr_final_index(Compiler *c) {
  LoadRecord *r = c->lrec;
  const NodeTable *nt = c->nt;
  if (r->fin_head && r->fin_of.builds == r->builds && r->fin_of.entries == r->nentries && r->fin_of.nst == r->nst &&
      r->fin_of.at == r->at && r->fin_of.ver == nt->version && r->fin_of.edits == nt->edits) return;
  if (!r->fin_head && !(r->fin_head = malloc(sizeof(int) * LR_BUCKETS))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int b = 0; b < LR_BUCKETS; b++) r->fin_head[b] = -1;
  r->nfin = 0;
  r->fin_calls = 0;
  /* a write the source has that a pass since renamed or dropped */
  for (int i = 0; i < r->nsrcw; i++) {
    struct LrFinal *f = lr_final(r, r->srcw[i].name, 1);
    NodeKind wk = nt_kind(nt, r->srcw[i].node);
    if (wk == NK_ClassNode || wk == NK_ModuleNode) f->headers++;
    else f->writes++;
    const char *now = lr_const_written(nt, r->srcw[i].node);
    if (!now || !sp_streq(now, r->srcw[i].name)) f->unseen = 1;
  }
  /* a write the record does not order */
  const int *par = comp_parent_map(c);
  int nk = 0;
  const NodeKind *kinds = elab_const_write_kinds(&nk);
  for (int k = 0; k < nk; k++) {
    int n = 0;
    const int *v = nt_nodes_of_kind(nt, kinds[k], &n);
    for (int i = 0; i < n; i++) {
      const char *nm = lr_const_written(nt, v[i]);
      if (!nm) continue;
      struct LrFinal *f = lr_final(r, nm, 1);
      if (kinds[k] != NK_ConstantWriteNode && kinds[k] != NK_ConstantOrWriteNode &&
          kinds[k] != NK_ClassNode && kinds[k] != NK_ModuleNode) { f->unseen = 1; continue; }
      int top = v[i];
      for (int a = par[v[i]]; a >= 0; top = a, a = par[a]) {
        NodeKind ak = nt_kind(nt, a);
        if (ak == NK_DefNode || ak == NK_LambdaNode || ak == NK_BlockNode) f->unseen = 1;
      }
      int e = top < r->n ? r->at[top] : -1;   /* the file's root's statement */
      if (e < 0 || r->st[e].region != 0) f->unseen = 1;
    }
  }
  /* a remove_const, and a send of a name the record cannot read */
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    int ac = 0, a = 0;
    const int *av = call_args(nt, n, &ac);
    const char *m = call_sent_name(nt, n, &a);
    if (!m) { r->fin_calls = 1; continue; }
    if (!is_remove_const_name(m)) continue;
    const char *k = a < ac ? vis_arg_name(nt, av[a]) : NULL;
    if (k) lr_final(r, k, 1)->removed = 1;
    else r->fin_calls = 1;
  }
  r->fin_of.builds = r->builds; r->fin_of.entries = r->nentries; r->fin_of.nst = r->nst;
  r->fin_of.at = r->at; r->fin_of.ver = nt->version; r->fin_of.edits = nt->edits;
}
static const struct LrFinal *lr_final_of(Compiler *c, const char *name) {
  lr_final_index(c);
  return lr_final(c->lrec, name, 0);
}

/* Does the record see every write of constant `name` there can be? Not
   when the program runs code it cannot follow (elab_runs_unseen), loads a
   C extension, or may make a constant by a name it computes, a held one or
   not (elab_consts_by_value); nor when a write the program's source has is
   gone or rewritten by a pass before the walk (elab_note_const_writes);
   nor when a write of the name is one it does not record: a path or
   operator write, a multiple assignment's target, or a write or header in
   a method body, a lambda or a block, which may run before any point, or
   in a file region 0 has not entered (yet, while the walk records: a file
   a later load enters, or one in a region of its own, whose points the
   record cannot order). */
static int lr_const_seen(Compiler *c, const char *name) {
  LoadRecord *r = c->lrec;
  const NodeTable *nt = c->nt;
  if (!r->consts) r->consts = elab_runs_unseen(c) || nt->has_cext_decls || elab_consts_by_value(c, 1) ? 2 : 1;
  if (r->consts == 2) return 0;
  const struct LrFinal *f = lr_final_of(c, name);
  return !(f && f->unseen);
}

/* May the constants of class cls (LR_MAIN: Object's) or of an ancestor
   include a builtin's own (a builtin class or module that has constants:
   File's Stat, Process's Status), which the record does not hold? */
static int lr_builtin_consts(Compiler *c, int cls) {
  for (int k = cls, g = 0; g < lr_chain_bound(c); g++) {
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
  if (o == ELAB_YES) { *cls = lr_class_of(c, d, NULL); return o; }
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

/* See analyze.h. */
int elab_main_method_vis_at(Compiler *c, int node, const char *name) {
  LoadPoint p = elab_point_of(c, node);
  const LrDef *d = NULL;
  if (p.region < 0 || !name) return -1;
  int at = elab_method_at(c, LR_MAIN, name, 0, p, &d);
  if (at == ELAB_NO) return d ? -1 : 0;   /* (an undef or a removal: not read here) */
  if (at != ELAB_YES || !d || (d->kind != LR_DEF && d->kind != LR_ALIAS) ||
      elab_own_method_at(c, LR_MAIN, name, p) != ELAB_YES)
    return -1;
  int v = elab_method_vis_at(c, LR_MAIN, name, 0, p);
  /* A visibility word in a reopened Object (`class Object; private :m;
     end`) changes main's method too, but the record files it in Object's
     own history: the later of the two last entries before p counts.
     Where Object's entries are not sure, main's answer stays. */
  int oc = comp_core_class(c, CORE_OBJECT);
  const LrHist *oh = oc >= 0 ? lr_hist(c->lrec, oc, 0, name, 0) : NULL;
  const LrDef *od = NULL, *md = NULL;
  int oo = oh ? lr_last_before(c, oh, p, 0, 0, &od) : ELAB_NO;
  /* (a change under a condition: main's own answer, as before) */
  if (oo == ELAB_YES) {
    if (lr_last_before(c, lr_hist(c->lrec, LR_MAIN, 0, name, 0), p, 0, 0, &md) != ELAB_YES || !md) return -1;
    int ord = elab_cmp(elab_point_of(c, md->node), elab_point_of(c, od->node));
    if (ord != PROG_BEFORE && ord != PROG_AFTER) return -1;
    /* An Object entry ahead of main's def (`alias_method :m, :to_s`)
       leaves main's def (or main's visibility word) the answer. Main's
       alias is not read here: it takes the visibility of its source
       method. An Object entry after main's last entry that is no
       visibility word is not read here either. */
    if (ord == PROG_AFTER && md->kind != LR_DEF && md->kind != LR_VIS) return -1;
    if (ord == PROG_BEFORE && od->kind != LR_VIS) return -1;
    if (ord == PROG_BEFORE) v = od->vis;
  }
  return v == SP_VIS_PUBLIC ? 1 : v == SP_VIS_PROTECTED ? 2 : v == SP_VIS_PRIVATE ? 4 : -1;
}

int elab_method_vis_held_from(Compiler *c, int cls, const char *name, int sg, LoadPoint p) {
  int vis = elab_method_vis_at(c, cls, name, sg, p);
  for (int k = cls, n = 0; vis >= 0 && k >= LR_MAIN && n < lr_chain_bound(c); k = lr_super(c, k), n++) {
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
  if (v->absent) return 0;
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
      if (!(mask >> d->kind & 1)) continue;
      int ap = lr_applied(c, d);
      if (ap != 1 && !(ap == 2 && (mask & ELAB_ENTRY_MAY))) continue;
      v = lr_grow(v, &cap, k, sizeof *v);
      v[k++] = (LrEntryKey){ { h->a, h->b, d->kind, d->node, h->name, d->target, d->scope }, elab_sort_key(c, d->node),
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

/* An alias or alias_method whose original the class has no method of where
   it runs raises NameError there, and makes no method (`undefined method 'm'
   for class 'K'`, 'module' for a module's, `class '#<Class:K>'` for a
   singleton's): the record answers none at the alias's point as it does for
   a call (elab_call_method: a def after it, a name nothing defines; not an
   undef, a name that code the record cannot follow or a version it does not
   place may supply), and no builtin on the chain answers the name
   (cplan_other_answers, which reads the registered accessors: asked after
   them). Such a version is dropped from the
   histories (LrDef.absent) and the statement carries its message, which
   codegen raises where the statement runs. An alias made in a method body
   runs when the body does, and one in a class that has no name, or in one the
   record cannot follow, is not asked. */
void elab_absent_aliases(Compiler *c) {
  LoadRecord *r = c->lrec;
  NodeTable *nt = (NodeTable *)c->nt;
  int dropped = 0;
  for (int i = 0; r && r->recorded && i < r->nhist; i++) {
    const LrHist *h = &r->hist[i];
    if (h->a < LR_MAIN || h->a >= c->nclasses || !lr_class(r, h->a)) continue;
    for (int j = 0; j < h->n; j++) {
      LrDef *v = &h->v[j];
      if (v->kind != LR_ALIAS || !v->target || v->unplaced || v->body || v->absent) continue;
      if (elab_call_method(c, v->node, h->a, v->target, h->b, 0) != ELAB_NO || cplan_other_answers(c, h->a, h->b, v->target, 0)) continue;
      /* (a `class Class` reopening is registered as the module Class__reopen) */
      int reopen = h->a != LR_MAIN && h->a == comp_core_class(c, CORE_CLASS_REOPEN);
      const char *cn = h->a == LR_MAIN ? "Object" : reopen ? "Class" : comp_class_anonymous(c, h->a) ? NULL : class_ruby_name(c, h->a);
      if (!cn) continue;
      char msg[512];
      int mod = h->a != LR_MAIN && !reopen && comp_class_is_module(c, &c->classes[h->a]);
      if (h->b) snprintf(msg, sizeof msg, "undefined method '%s' for class '#<Class:%s>'", v->target, cn);
      else snprintf(msg, sizeof msg, "undefined method '%s' for %s '%s'", v->target, mod ? "module" : "class", cn);
      nt_node_set_str(nt, v->node, "alias_absent", msg);
      v->absent = 1;
      dropped = 1;
    }
  }
  if (!dropped) return;
  lr_entries_changed(r);
}

/* The kind of a class side's last version, -1 for none, and whether any
   version is an accessor (*attr). `c` set: only the versions the load
   applies at their point (lr_applied 1), and no undef or removal. */
static int lr_attr_last_kind(Compiler *c, const LrHist *h, int *attr) {
  int last = -1;
  *attr = 0;
  for (int j = 0; h && j < h->n; j++) {
    const LrDef *v = &h->v[j];
    if (!lr_is_version(v)) continue;
    if (c && (v->kind == LR_UNDEF || v->kind == LR_REMOVE || lr_applied(c, v) != 1)) continue;
    *attr |= v->kind == LR_READER || v->kind == LR_WRITER;
    last = v->kind;
  }
  return last;
}

/* Does a class side's history hold an accessor version and end in a def? */
static int lr_attr_then_def(const LrHist *h) {
  int attr = 0, last = lr_attr_last_kind(NULL, h, &attr);
  return attr && last == LR_DEF;
}

/* See analyze.h. */
ElabEntry *elab_replaced_attrs(Compiler *c, int *n) {
  LoadRecord *r = c->lrec;
  ElabEntry *out = NULL;
  int k = 0, cap = 0;
  for (int i = 0; r && r->recorded && i < r->nhist; i++) {
    const LrHist *h = &r->hist[i];
    if (h->a < 0 || !h->b || !lr_attr_then_def(h)) continue;
    for (int j = 0; j < h->n; j++) {
      const LrDef *d = &h->v[j];
      if ((d->kind != LR_READER && d->kind != LR_WRITER) || !lr_is_version(d)) continue;
      out = lr_grow(out, &cap, k, sizeof *out);
      out[k++] = (ElabEntry){ h->a, h->b, d->kind, d->node, h->name, NULL, d->scope };
    }
  }
  *n = k;
  return out;
}

/* See analyze.h. */
void elab_attr_scope(Compiler *c, const ElabEntry *e, int scope) {
  LrHist *h = c->lrec ? lr_hist(c->lrec, e->cls, e->sg, e->name, 0) : NULL;
  for (int j = 0; h && j < h->n; j++)
    if (h->v[j].node == e->node && h->v[j].kind == e->kind) h->v[j].scope = scope;
}

/* See analyze.h. */
int elab_attr_replaced(Compiler *c, int cls, const char *name) {
  LoadRecord *r = c->lrec;
  const LrHist *h = r && r->recorded && cls >= 0 ? lr_hist(r, cls, 1, name, 0) : NULL;
  for (int j = 0; h && j < h->n; j++)
    if ((h->v[j].kind == LR_READER || h->v[j].kind == LR_WRITER) && h->v[j].scope) return 1;
  return 0;
}

/* See analyze.h. */
int elab_inst_attr_replaced(Compiler *c, int cls, const char *name) {
  LoadRecord *r = c->lrec;
  const LrHist *h = r && r->recorded && cls >= 0 ? lr_hist(r, cls, 0, name, 0) : NULL;
  int attr = 0, last = lr_attr_last_kind(c, h, &attr);
  return attr && last != LR_READER && last != LR_WRITER;
}

/* The node of the last attr_* entry for name on cls's instance side, or of
   a module's it prepends or includes, the way a lookup meets them; -1 */
static int lr_accessor_in(Compiler *c, int cls, const char *name, int depth) {
  const ClassInfo *k = &c->classes[cls];
  const LrHist *h = lr_hist(c->lrec, cls, 0, name, 0);
  int node = -1;
  for (int m = k->nprepended_mods - 1; node < 0 && depth < 16 && m >= 0; m--)
    node = lr_accessor_in(c, k->prepended_mods[m], name, depth + 1);
  for (int j = h ? h->n - 1 : -1; node < 0 && j >= 0; j--)
    if ((h->v[j].kind == LR_READER || h->v[j].kind == LR_WRITER) && lr_applied(c, &h->v[j]) == 1) node = h->v[j].node;
  for (int m = k->nincluded_mods - 1; node < 0 && depth < 16 && m >= 0; m--)
    node = lr_accessor_in(c, k->included_mods[m], name, depth + 1);
  return node;
}
/* See analyze.h. */
int elab_accessor_node(Compiler *c, int cls, const char *name) {
  LoadRecord *r = c->lrec;
  int node = -1;
  for (int k = cls; node < 0 && r && r->recorded && k >= 0 && k < c->nclasses; k = c->classes[k].parent)
    node = lr_accessor_in(c, k, name, 0);
  return node;
}

/* (every entry, one that only writes too: the macro splice in analyze_ct.c
   runs code at compile time that may read the instance variable it wrote) */
int elab_opaque_before(Compiler *c, int cls, LoadPoint p) {
  if (!lr_ready(c, p)) return ELAB_UNKNOWN;
  return lr_last_before(c, lr_hist(c->lrec, LR_KEY_OPAQUE, lr_const_owner(cls), "", 0), p, p.region < 0, 0, NULL);
}

/* ...leaving out a class-body macro call the walk ran and left as written,
   whose run wrote the class's instance variables alone (ct_state_note marks
   it ct_state_only): the record keeps it as code it does not follow, and
   the macro form follows that state itself */
int elab_opaque_before_bar_state(Compiler *c, int cls, LoadPoint p) {
  int o = elab_opaque_before(c, cls, p);
  const LrHist *h = o != ELAB_NO ? lr_hist(c->lrec, LR_KEY_OPAQUE, lr_const_owner(cls), "", 0) : NULL;
  for (int i = 0; h && i < h->n; i++)
    if (h->v[i].node < 0 || !nt_int(c->nt, h->v[i].node, "ct_state_only", 0)) return o;
  return h ? ELAB_NO : o;
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
    int no = 0, d = -1, from0 = nr;
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
    /* the frames whose `K.include M` the record takes as K's (a statement
       marked mixin_by_record), each once, where the statement runs */
    int nmx = 0;   /* (ci's own later bodies and frames are rest[from0..]) */
    const LrMixin *mx = elab_class_mixins(c, ci, &nmx);
    for (int i = 0; i < nmx; i++) {
      if (!nt_int(c->nt, mx[i].node, "mixin_by_record", 0) || (mx[i].kind != LR_INCLUDE && mx[i].kind != LR_PREPEND)) continue;
      int fb = lr_frame_body(c->nt, mx[i].open), seen = 0;
      for (int q = from0; q < nr && !seen; q++) seen = rest[q][1] == ci && rest[q][2] == fb;
      if (fb < 0 || seen) continue;
      if (nr >= tot) {
        tot = tot * 2 + 4;
        v = realloc(v, sizeof *v * (size_t)tot);
        rest = realloc(rest, sizeof *rest * (size_t)tot);
        if (!v || !rest) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      }
      rest[nr][0] = r->at[mx[i].node]; rest[nr][1] = ci; rest[nr][2] = fb; nr++;
    }
  }
  qsort(rest, (size_t)nr, sizeof *rest, lr_body_cmp);
  for (int i = 0; i < nr; i++) v[k++] = (ElabBody){ rest[i][1], rest[i][2] };
  free(rest);
  *n = k;
  return v;
}

/* The frame of class cls's record-taken mixins (mixin_by_record) whose
   statements are in list body, or -1 */
int elab_foreign_mixin_open(Compiler *c, int cls, int body) {
  int nmx = 0;
  const LrMixin *mx = elab_class_mixins(c, cls, &nmx);
  for (int i = 0; i < nmx; i++)
    if (nt_int(c->nt, mx[i].node, "mixin_by_record", 0) && lr_frame_body(c->nt, mx[i].open) == body) return mx[i].open;
  return -1;
}

/* The statement lists of the frames whose mixins of cls the record takes
   (mixin_by_record) that are a singleton's (a `class << self` body), each
   once, in the order they run; *n of them; the caller frees */
int *elab_foreign_mixin_bodies(Compiler *c, int cls, int *n) {
  int nmx = 0, k = 0;
  const LrMixin *mx = elab_class_mixins(c, cls, &nmx);
  int *v = malloc(sizeof(int) * (size_t)(nmx ? nmx : 1));
  if (!v) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < nmx; i++) {
    if (!nt_int(c->nt, mx[i].node, "mixin_by_record", 0) || (mx[i].kind != LR_EXTEND && mx[i].kind != LR_SG_PREPEND)) continue;
    int fb = lr_frame_body(c->nt, mx[i].open), seen = 0;
    for (int q = 0; q < k && !seen; q++) seen = v[q] == fb;
    if (fb >= 0 && !seen) v[k++] = fb;
  }
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
  if (r->nhist || r->nregion > 1) lr_run_points(c);
  for (int k = 1; k < r->nregion; k++)   /* the first statement of region 0 each other region's load may run on (lr_rta) */
    r->rentry[k] == LR_NEVER ? fprintf(out, "region %d: never\n", k) : fprintf(out, "region %d: entered #%d\n", k, r->rentry[k]);
  for (int i = 0; i < r->nhist; i++) {
    const LrHist *h = &r->hist[i];
    if (h->a == LR_KEY_RUNS) {   /* only the forms that run the program's code */
      if (h->n && h->v[h->n - 1].vis != ELAB_NO)
        fprintf(out, "form #%d runs code%s\n", elab_point_of(c, h->b).seq, h->v[h->n - 1].vis == ELAB_YES && !h->v[h->n - 1].may ? "" : " (maybe)");
      continue;
    }
    if (lr_key_memo(h->a)) continue;
    if (h->a == LR_KEY_VIS) fprintf(out, "visibility in #%d:", elab_point_of(c, h->b).seq);
    else if (h->a == LR_KEY_OPAQUE) fprintf(out, "opaque %s:", h->b >= 2 ? c->classes[h->b - 2].name : "main");
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
              v->unplaced ? " unplaced" : v->absent ? " absent" : "");
      if (v->body) {   /* a method body's (LrDef.body): its extent, from that body's first statement on */
        lr_run_points(c);
        fprintf(out, " in def#%d", elab_point_of(c, v->body).seq);
        int from = r->urun[v->body];   /* (0: a body lr_rta did not record, whose extent opens anywhere) */
        from == LR_NEVER ? fputs(" from=never", out) : from ? fprintf(out, " from=#%d", from) : fputs(" from=?", out);
      }
      if (v->kind == LR_DEF && !v->body && h->a >= LR_MAIN && v->node < r->n && elab_method_versions(c, h->name) > 1)   /* lr_rta */
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
      ElabWindow w = { p, e == ELAB_YES ? end : (LoadPoint){ p.region, r->rend[p.region] }, e == ELAB_YES, 0, 0 };
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
                               /* the table's are the registration's, from the record's copies
                                  (conditional-def arms by their own rule) */
                               "the record's surface: module_function copies",
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
    /* ...or, for a copy of a method the module holds under another name or
       through an include, a class method of its own (mf_copy_entries) */
    now[LR_SF_COPY] = sg && (mf || (s && s->copy && comp_cmethod_in_class(c, cls, name) >= 0));
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
  free(r->plan.rend); free(r->plan.rinst); free(r->plan.urun); free(r->plan.rentry);
  for (int i = 0; i < r->nnames; i++) free(r->names[i]);
  for (int i = 0; i < r->nreopens; i++) free(r->reopens[i]);
  for (int i = 0; i < r->nrefl; i++) free(r->refl[i]);
  for (int sg = 0; sg < 2; sg++) {
    for (int i = 0; i < r->nrefl_table[sg]; i++) free(r->refl_table[sg][i]);
    free(r->refl_table[sg]);
  }
  for (int i = 0; i < r->nrefl_cell; i++) free(r->refl_cell[i]);
  for (int i = 0; i < r->nrefl_capt; i++) free(r->refl_capt[i]);
  free(r->refl_cell); free(r->refl_capt);
  free(r->names); free(r->defs); free(r->reopens); free(r->refl);
  for (int i = 0; i < r->nsrcw; i++) free(r->srcw[i].name);
  free(r->srcw);
  free(r->slot); free(r->slot_head); free(r->slotp);
  free(r->fin); free(r->fin_head);
  free(r->hist); free(r->mixed); free(r->mby); free(r->mby_at); free(r->rclass); free(r->sgm); free(r->sgchain); free(r->unset.key); free(r->unset.un); free(r->clsmod); free(r->hhead); free(r->cls); free(r->vers); free(r->vers_head); free(r->valias); free(r->rdr); free(r->rdr_head); free(r->set);
  for (int i = 0; i < r->nhdr; i++) free(r->hdr[i].leaf);
  free(r->hdr); free(r->hdr_head);
  free(r->st); free(r->at); free(r->bits); free(r->dscope); free(r->ddef); free(r->rend); free(r->rinst); free(r->urun); free(r->rentry); free(r);
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
   desugar_explicit_receiver_mixin, desugar_extend_enumerable), a class builder (desugar_class_new_blocks,
   desugar_inline_struct_blocks), const_get (desugar_literal_const_get,
   desugar_self_const_get, desugar_dynamic_const_get), a write through a
   constant path (desugar_constant_path_self_alias, qualify_colliding_classes),
   instance_eval (desugar_instance_eval_defs), a method_defined? guard
   (desugar_def_unless_method_defined), an alias (desugar_inherited_aliases).
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
  for (int a = cls, g = 0; g < lr_chain_bound(c); g++) {
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

/* ---- Constant keys from the record (#10787) ----------------------------
   Each constant entity, a history keyed (owner, leaf) with a known owner,
   takes one key: the leaf, or, for a nested entity whose leaf another
   entity writes under another owner path (or a builtin class's leaf),
   the owner's Ruby path joined by `__`, then `__` and the leaf. This is
   qc_value_collisions' rule, run over the record's entities instead of the
   lexical writes. */
typedef struct { int h, cls_ent; char *path, *key; } LrKeyEnt;

/* The Ruby path of a constant owner (lr_const_owner's numbering), its
   segments joined by `__`; "" for main */
static void lr_owner_path(Compiler *c, int owner, char *buf, size_t n) {
  buf[0] = 0;
  int cls = owner > 0 ? owner - 2 : -owner - 2;
  if (cls < 0 || cls >= c->nclasses) return;
  /* the classes around it, out to the top level (a reopened Object's
     constants are top-level ones); a builtin's reopening gives no segment
     (`class Regexp::Syntax::V1_9_1`: Syntax__V1_9_1) */
  int chain[32], d = 0, obj = comp_core_class(c, CORE_OBJECT);
  const char *rn = NULL;   /* a class that carries its Ruby path (an aliased or rebased header): the path above it */
  for (int k = cls; k >= 0 && k != obj && d < 32; k = c->classes[k].enclosing_class) {
    if (c->classes[k].ruby_name_cache) { rn = c->classes[k].ruby_name_cache; break; }
    chain[d++] = k;
  }
  size_t o = 0;
  int head = 1;
  for (const char *p = rn; p && *p; head = 0) {
    const char *q = strstr(p, "::");
    size_t l = q ? (size_t)(q - p) : strlen(p);
    char seg[128]; snprintf(seg, sizeof seg, "%.*s", (int)l, p);
    p += l; if (q) p += 2;
    if (head && (q || d) && (builtin_class_id(seg) != 0 || is_builtin_class_name(seg) || is_builtin_module_name(seg))) continue;
    if (o + l + 3 >= n) break;
    if (o) { buf[o++] = '_'; buf[o++] = '_'; }
    memcpy(buf + o, seg, l); o += l;
  }
  for (int i = d - 1; i >= 0; i--, head = 0) {
    const char *nm = c->classes[chain[i]].name, *leaf = nm;
    if (!nm) continue;
    for (const char *q = strstr(nm, "__"); q; q = strstr(q + 2, "__")) leaf = q + 2;
    if (head && i > 0 && leaf == nm && (builtin_class_id(nm) != 0 || is_builtin_class_name(nm) || is_builtin_module_name(nm))) continue;
    size_t l = strlen(leaf);
    if (o + l + 3 >= n) break;
    if (o) { buf[o++] = '_'; buf[o++] = '_'; }
    memcpy(buf + o, leaf, l); o += l;
  }
  buf[o] = 0;
}

static int lr_write_is_class(Compiler *c, int node) {
  NodeKind k = nt_kind(c->nt, node);
  return k == NK_ClassNode || k == NK_ModuleNode || class_def_builder(c->nt, node) >= 0 || class_def_record(c->nt, node) >= 0;
}

/* The node whose name a write's key goes on: a path write's target */
static int lr_key_node(const NodeTable *nt, int node) {
  switch (nt_kind(nt, node)) {
    case NK_ConstantPathWriteNode: case NK_ConstantPathOrWriteNode: case NK_ConstantPathAndWriteNode:
    case NK_ConstantPathOperatorWriteNode: return nt_ref(nt, node, "target");
    case NK_ConstantWriteNode: case NK_ConstantOrWriteNode: case NK_ConstantAndWriteNode:
    case NK_ConstantOperatorWriteNode: case NK_ConstantTargetNode: case NK_ConstantPathTargetNode: return node;
    default: return -1;
  }
}

static const LrHist *lr_key_hists;   /* the histories lr_key_ent_cmp sorts the entities of */
static int lr_key_ent_cmp(const void *a, const void *b) {
  const LrKeyEnt *x = a, *y = b;
  int o = strcmp(lr_key_hists[x->h].name, lr_key_hists[y->h].name);
  return o ? o : strcmp(x->path, y->path);
}

/* A rename the keys make, applied after the last ask */
typedef struct { int node; char *key; } LrKeyRename;
static void lr_key_queue(LrKeyRename **q, int *n, int *cap, int node, const char *key) {
  *q = lr_grow(*q, cap, *n, sizeof **q);
  (*q)[(*n)++] = (LrKeyRename){ node, strdup(key) };
}

/* Is history h a path write's onto a Struct or Data holder (`Rim::LIMIT =
   v`) that a subclass of the holder writes too (an entity of the same leaf
   whose owner descends from it)? */
static int lr_holder_path_write(Compiler *c, const LrHist *h) {
  const NodeTable *nt = c->nt;
  const LoadRecord *r = c->lrec;
  int cls = h->b - 2, path = 0;
  if (cls < 0 || cls >= c->nclasses || c->classes[cls].def_node < 0 || class_def_record(nt, c->classes[cls].def_node) < 0) return 0;
  for (int v = 0; v < h->n && !path; v++) {
    NodeKind k = h->v[v].node >= 0 ? nt_kind(nt, h->v[v].node) : NK_NilNode;
    path = k == NK_ConstantPathWriteNode || k == NK_ConstantPathOrWriteNode || k == NK_ConstantPathAndWriteNode ||
           k == NK_ConstantPathOperatorWriteNode;
  }
  for (int i = 0; path && i < r->nhist; i++) {
    const LrHist *o = &r->hist[i];
    if (o == h || o->a != LR_KEY_CONST || o->b < 2 || !sp_streq(o->name, h->name)) continue;
    for (int k = lr_super(c, o->b - 2), d = 0; k >= 0 && d < 64; k = lr_super(c, k), d++) if (k == cls) return 1;
  }
  return 0;
}

/* The class of the program's whose Ruby name is the path p spells (a path
   through a builtin the program does not reopen, `Regexp::Syntax::Token`,
   which the record cannot step through), LR_UNKNOWN_CLASS for none */
static int lr_class_by_written_path_at(Compiler *c, int p, int read);
static int lr_class_by_written_path(Compiler *c, int p) { return lr_class_by_written_path_at(c, p, -1); }
/* ...where a relative path names a class under one of read's lexical scopes
   (`Constants::Open` in `module SQLite3`'s class Database), innermost first */
static int lr_class_by_written_path_at(Compiler *c, int p, int read) {
  const NodeTable *nt = c->nt;
  char path[512] = "", seg[QC_MAXDEPTH + 2][128];
  int n = 0;
  for (int q = p; q >= 0 && n < QC_MAXDEPTH + 2; q = nt_kind(nt, q) == NK_ConstantPathNode ? nt_ref(nt, q, "parent") : -1) {
    NodeKind k = nt_kind(nt, q);
    if (k != NK_ConstantPathNode && k != NK_ConstantReadNode) return LR_UNKNOWN_CLASS;
    const char *w = lr_written_name(nt, q);
    if (!w) return LR_UNKNOWN_CLASS;
    snprintf(seg[n++], sizeof seg[0], "%s", w);
  }
  for (int i = n - 1; i >= 0; i--) {
    if (path[0]) strncat(path, "::", sizeof path - strlen(path) - 1);
    strncat(path, seg[i], sizeof path - strlen(path) - 1);
  }
  LoadRecord *r = c->lrec;
  int e = read >= 0 && read < r->n ? r->at[read] : -1;
  for (int g = e >= 0 ? r->st[e].frame : -1; g >= 0 && n > 1; g = r->st[g].frame) {
    NodeKind gk = nt_kind(nt, r->st[g].node);
    if (gk != NK_ClassNode && gk != NK_ModuleNode) continue;
    int ci = elab_header_class(c, r->st[g].node, NULL, NULL);
    const char *cn = ci >= 0 ? class_ruby_name(c, ci) : NULL;
    if (!cn) continue;
    char full[640]; snprintf(full, sizeof full, "%s::%s", cn, path);
    for (int k = 0; k < c->nclasses; k++) {
      const char *rn = class_ruby_name(c, k);
      if (rn && sp_streq(rn, full)) return k;
    }
  }
  int leaf = LR_UNKNOWN_CLASS, nleaf = 0;
  for (int k = 0; path[0] && k < c->nclasses; k++) {
    const char *rn = class_ruby_name(c, k), *l = rn;
    if (rn && sp_streq(rn, path)) return k;
    for (const char *q = rn ? strstr(rn, "::") : NULL; q; q = strstr(q + 2, "::")) l = q + 2;
    if (n == 1 && l && sp_streq(l, path)) { leaf = k; nleaf++; }
  }
  /* a bare name (`UnicodeProperty`, found through an ancestor's module the
     record cannot place either): the one class of the program by that leaf */
  return nleaf == 1 ? leaf : LR_UNKNOWN_CLASS;
}

/* The entity a constant read names when the record cannot say whether it
   is bound yet (a write that may not have run, one in another region): the
   lookup's order, lexical scopes innermost first, then the innermost
   class's ancestors and Object's, the first history of the name with any
   write, sure or not. A scoped read (`A::X`) looks in A and its
   superclasses. NULL where none is. */
static const LrHist *lr_const_cand(Compiler *c, int node, const char *name) {
  LoadRecord *r = c->lrec;
  const NodeTable *nt = c->nt;
  const LrHist *h;
  if (!name) return NULL;
  if (nt_kind(nt, node) == NK_ConstantPathNode && !lr_root_read(nt, node)) {
    int par = nt_ref(nt, node, "parent"), pc = par >= 0 ? lr_class_ref_at(c, par, NULL) : LR_MAIN;
    if (pc < LR_MAIN && par >= 0) pc = lr_class_by_written_path_at(c, par, node);
    for (int d = 0; pc >= LR_MAIN && d < 64; pc = pc == LR_MAIN ? LR_UNKNOWN_CLASS : lr_super(c, pc), d++)
      if ((h = lr_hist(r, LR_KEY_CONST, lr_const_owner(pc), name, 0)) && h->n) return h;
    return NULL;
  }
  int e = node >= 0 && node < r->n ? r->at[node] : -1, first = LR_MAIN;
  if (e < 0) return NULL;
  if (!lr_root_read(nt, node))
    for (int g = r->st[e].frame; g >= 0; g = r->st[g].frame) {
      NodeKind k = nt_kind(nt, r->st[g].node);
      if (k == NK_SingletonClassNode) {
        int owner = lr_singleton_owner(c, g);
        if (owner && (h = lr_hist(r, LR_KEY_CONST, owner, name, 0)) && h->n) return h;
        continue;
      }
      if (k != NK_ClassNode && k != NK_ModuleNode) continue;
      int ci = elab_header_class(c, r->st[g].node, NULL, NULL);
      if (ci < 0) return NULL;
      if (first == LR_MAIN) first = ci;
      if ((h = lr_hist(r, LR_KEY_CONST, lr_const_owner(ci), name, 0)) && h->n) return h;
    }
  for (int cls = first == LR_MAIN ? LR_UNKNOWN_CLASS : lr_super(c, first), d = 0; cls >= 0 && d < 64; cls = lr_super(c, cls), d++)
    if ((h = lr_hist(r, LR_KEY_CONST, lr_const_owner(cls), name, 0)) && h->n) return h;
  return (h = lr_hist(r, LR_KEY_CONST, lr_const_owner(LR_MAIN), name, 0)) && h->n ? h : NULL;
}

/* A node's key (elab_const_node_key), and the name it carried then: a pass
   that renames the node later (the class half's qualifying) leaves the key
   behind */
static void lr_set_ckey(NodeTable *nt, int node, const char *key) {
  const char *n = nt_str(nt, node, "name");
  if (!n) return;
  char *was = strdup(n);
  if (!was) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  nt_node_set_str(nt, node, "ckey", key);
  nt_node_set_str(nt, node, "ckey_at", was);
  free(was);
}
void elab_const_keys(Compiler *c) {
  LoadRecord *r = c->lrec;
  const char *e = getenv("SPINEL_CONST_KEY_CHECK");
  int check = e && *e;
  if (!r || !r->recorded) return;
  LrKeyRename *q = NULL; int nq = 0, cq = 0;
  const NodeTable *nt = c->nt;
  LrKeyEnt *es = calloc((size_t)r->nhist + 1, sizeof *es);
  int ne = 0;
  char buf[512];
  for (int i = 0; i < r->nhist; i++) {
    const LrHist *h = &r->hist[i];
    if (h->a != LR_KEY_CONST || h->b == 0) continue;
    int cls_ent = 0;
    for (int v = 0; v < h->n; v++) if (h->v[v].node >= 0 && lr_write_is_class(c, h->v[v].node)) cls_ent = 1;
    lr_owner_path(c, h->b, buf, sizeof buf);
    es[ne++] = (LrKeyEnt){ i, cls_ent, strdup(buf) };
  }
  lr_key_hists = r->hist;
  qsort(es, (size_t)ne, sizeof *es, lr_key_ent_cmp);
  long same = 0, differ = 0;
  for (int i = 0, g0 = 0, g1 = 0; i < ne; i++) {
    const LrHist *h = &r->hist[es[i].h];
    /* the entities of the leaf, [g0, g1): es is sorted by leaf, then path */
    if (i == g1) {
      g0 = i;
      for (g1 = i + 1; g1 < ne && sp_streq(r->hist[es[g1].h].name, h->name); g1++) ;
    }
    if (es[i].cls_ent) continue;
    int nested = es[i].path[0] != 0;
    int collide = 0, sgc = 0;
    collide = !sp_streq(es[g0].path, es[i].path) || !sp_streq(es[g1 - 1].path, es[i].path);
    /* a singleton's constant beside its class's own of the leaf */
    for (int j = g0; h->b < 0 && j < g1 && !sgc; j++) sgc = j != i && r->hist[es[j].h].b == -h->b;
    if (!collide && nested && builtin_class_id(h->name) != 0) collide = 1;
    /* a Struct or Data holder's constant a subclass also writes keeps its
       leaf: `self::NAME` in the holder's block stays a read through the
       class value (desugar_holder_block_self_const), which finds it so */
    if (collide && nested && h->b > 0 && lr_holder_path_write(c, h)) collide = 0;
    char key[640];
    if (sgc) snprintf(key, sizeof key, "%s__singleton__%s", es[i].path, h->name);
    else if (collide && nested) snprintf(key, sizeof key, "%s__%s", es[i].path, h->name);
    else snprintf(key, sizeof key, "%s", h->name);
    es[i].key = strdup(key);
    for (int v = 0; v < h->n; v++) {
      int kn = h->v[v].node >= 0 ? lr_key_node(nt, h->v[v].node) : -1;
      if (kn < 0) continue;
      const char *qn = nt_str(nt, kn, "name");
      if (!qn) continue;
      if (sp_streq(qn, key)) { same++; continue; }
      differ++;
      lr_key_queue(&q, &nq, &cq, kn, key);
      if (check) fprintf(stderr, "constkey: %s line %lld owner=%s qualify=%s record=%s\n", nt_type(nt, h->v[v].node),
                         (long long)nt_int(nt, h->v[v].node, "node_line", 0), es[i].path[0] ? es[i].path : "main", qn, key);
    }
  }
  if (check) fprintf(stderr, "constkey: same=%ld differ=%ld\n", same, differ);
  /* the reads: the write each resolves to, by the record, and that write's key */
  int *ent = calloc((size_t)nt->count + 1, sizeof *ent);
  for (int i = 0; i < ne; i++) {
    const LrHist *h = &r->hist[es[i].h];
    for (int v = 0; v < h->n; v++) if (h->v[v].node >= 0 && h->v[v].node < nt->count) ent[h->v[v].node] = i + 1;
  }
  unsigned char *skip = calloc((size_t)nt->count + 1, 1);
  int *rent = calloc((size_t)nt->count + 1, sizeof *rent);   /* by read: the entity + 1 it names */
  if (!ent || !skip || !rent) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int id = 0; id < nt->count; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k == NK_ConstantPathNode) { int pa = nt_ref(nt, id, "parent"); if (pa >= 0) skip[pa] = 1; }
    else if (k == NK_ClassNode || k == NK_ModuleNode) { int cp = nt_ref(nt, id, "constant_path"); if (cp >= 0) skip[cp] = 1; }
    else if (lr_key_node(nt, id) >= 0 && lr_key_node(nt, id) != id) skip[lr_key_node(nt, id)] = 1;
  }
  long ragree = 0, rdiffer = 0, rno = 0, runk = 0, rcls = 0;
  for (int id = 0; id < nt->count; id++) {
    NodeKind k = nt_kind(nt, id);
    if ((k != NK_ConstantReadNode && k != NK_ConstantPathNode) || skip[id]) continue;
    if (id >= r->n || r->at[id] < 0) continue;   /* (no point: the name as written) */
    const LrDef *d = NULL;
    const char *nm = lr_written_name(nt, id);
    int o;
    if (k == NK_ConstantReadNode && !lr_root_read(nt, id)) o = lr_const_at(c, id, nm, elab_point_of(c, id), &d, LR_KEY_CONST);
    else {
      int par = k == NK_ConstantPathNode ? nt_ref(nt, id, "parent") : -1, pc = LR_MAIN;
      LoadPoint p = elab_point_of(c, id);
      if (par >= 0) pc = lr_class_ref_at(c, par, NULL);
      o = pc < LR_MAIN ? ELAB_UNKNOWN : lr_const_in(c, pc, nm, p, p.region < 0, pc == LR_MAIN, &d, 0, LR_KEY_CONST);
    }
    if (o != ELAB_YES || !d) {
      if (o == ELAB_NO) rno++; else runk++;
      /* a read the record cannot bind yet names the entity the lookup
         meets first (lr_const_cand) */
      const LrHist *ch = lr_const_cand(c, id, nm);
      for (int i = 0; ch && i < ne; i++)
        if (&r->hist[es[i].h] == ch) {
          if (!es[i].cls_ent && es[i].key) rent[id] = i + 1;
          if (!es[i].cls_ent && es[i].key && !sp_streq(es[i].key, nt_str(nt, id, "name") ? nt_str(nt, id, "name") : ""))
            lr_key_queue(&q, &nq, &cq, id, es[i].key);
          break;
        }
      const char *rn = nt_str(nt, id, "name");
      if (check && rn && nm && !sp_streq(rn, nm))
        fprintf(stderr, "constkey: unresolved-renamed %s %s line %lld name=%s written=%s\n", o == ELAB_NO ? "no" : "unknown",
                nt_type(nt, id), (long long)nt_int(nt, id, "node_line", 0), rn, nm);
      continue;
    }
    int ei = d->node >= 0 && d->node < nt->count ? ent[d->node] - 1 : -1;
    if (ei < 0 || es[ei].cls_ent || !es[ei].key) { rcls++; continue; }
    rent[id] = ei + 1;
    const char *rn = nt_str(nt, id, "name");
    if (rn && sp_streq(rn, es[ei].key)) { ragree++; continue; }
    rdiffer++;
    lr_key_queue(&q, &nq, &cq, id, es[ei].key);
    if (check) fprintf(stderr, "constkey: read %s line %lld name=%s record=%s\n", nt_type(nt, id),
                       (long long)nt_int(nt, id, "node_line", 0), rn ? rn : "?", es[ei].key);
  }
  if (check) fprintf(stderr, "constkey: reads agree=%ld differ=%ld no=%ld unknown=%ld class=%ld\n", ragree, rdiffer, rno, runk, rcls);
  /* the entities and the node map, kept for the slots and the readers
     that ask by node (elab_const_node_key) */
  for (int i = 0; i < r->nckey; i++) free(r->ckey[i]);
  free(r->ckey); free(r->cent); free(r->cwown);
  r->ckey = calloc((size_t)ne + 1, sizeof *r->ckey);
  r->cent = calloc((size_t)nt->count + 1, sizeof *r->cent);
  r->cwown = calloc((size_t)nt->count + 1, sizeof *r->cwown);
  if (!r->ckey || !r->cent || !r->cwown) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  r->nckey = ne; r->ncent = nt->count; r->ckey_gen++;
  /* (a class's entity too: `A::B = Class.new` is on A) */
  for (int id = 0; id < nt->count; id++) {
    if (!ent[id]) continue;
    int own = r->hist[es[ent[id] - 1].h].b, kn = lr_key_node(nt, id);
    r->cwown[id] = own;
    if (kn >= 0 && kn < nt->count) r->cwown[kn] = own;
  }
  for (int i = 0; i < ne; i++) r->ckey[i] = !es[i].cls_ent && es[i].key ? strdup(es[i].key) : NULL;
  for (int id = 0; id < nt->count; id++) {
    int ei = ent[id] - 1;
    if (ei < 0) ei = rent[id] - 1;
    if (ei < 0 || !r->ckey[ei]) continue;
    r->cent[id] = ei + 1;
    /* (and the node that carries a write's name: a path write's target) */
    int kn = ent[id] ? lr_key_node(nt, id) : -1;
    if (kn >= 0 && kn < nt->count && kn != id && !r->cent[kn]) r->cent[kn] = ei + 1;
  }

  free(rent);
  /* No node is renamed: each keeps the name the program wrote, and carries
     its slot's key beside it ("ckey", const_key_at), which a copy a later
     pass makes of the node (a method body copied into another class)
     carries too. The entities' writes and resolved reads first, then the
     ones the queue names (a candidate's read). */
  for (int id = 0; id < nt->count; id++)
    if (r->cent[id]) lr_set_ckey((NodeTable *)nt, id, r->ckey[r->cent[id] - 1]);
  for (int i = 0; i < nq; i++) { lr_set_ckey((NodeTable *)nt, q[i].node, q[i].key); free(q[i].key); }
  free(q);
  free(ent); free(skip);
  for (int i = 0; i < ne; i++) { free(es[i].path); free(es[i].key); }
  free(es);
}

/* Does a descendant of class cls write leaf `name` as its own value
   constant? A read of `self::NAME` from cls's class method then goes by
   the run-time class (const_get_recv_class_for, the switch of
   codegen_expr.c). A class entity it writes (`class NAME`) is not one. */
int elab_const_descendant_writes(Compiler *c, int cls, const char *name) {
  LoadRecord *r = c->lrec;
  if (!r || !name || cls < 0) return 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (k == cls) continue;
    int anc = 0;
    for (int x = c->classes[k].parent, d = 0; x >= 0 && d < 64 && !anc; x = c->classes[x].parent, d++) anc = x == cls;
    const LrHist *h = anc ? lr_hist(r, LR_KEY_CONST, lr_const_owner(k), name, 0) : NULL;
    /* (a value's write: one elab_const_keys keyed as a value entity) */
    for (int j = 0; h && j < h->n; j++) {
      int n = h->v[j].node, ei = n >= 0 && n < r->ncent ? r->cent[n] - 1 : -1;
      if (ei >= 0 && ei < r->nckey && r->ckey[ei]) return 1;
    }
  }
  return 0;
}
/* The key of the value constant class cls (LR_MAIN for Object's own) holds
   under leaf `name`: the entity of the record's constant history of (cls,
   name), through the key elab_const_keys gave its writes; NULL for none
   (no write of it, or an entity it keyed as a class) */
const char *elab_const_owned_key(Compiler *c, int cls, const char *name) {
  LoadRecord *r = c->lrec;
  if (!r || !r->ckey || !name) return NULL;
  /* (the program's Object, reopened, writes main's constants) */
  if (cls >= 0 && cls == comp_core_class(c, CORE_OBJECT)) cls = LR_MAIN;
  const LrHist *h = lr_hist(r, LR_KEY_CONST, lr_const_owner(cls), name, 0);
  for (int j = 0; h && j < h->n; j++) {
    int n = h->v[j].node;
    int ei = n >= 0 && n < r->ncent ? r->cent[n] - 1 : -1;
    if (ei >= 0 && ei < r->nckey && r->ckey[ei]) return r->ckey[ei];
  }
  return NULL;
}
/* The class a constant write (its node or its target) puts the constant on,
   as the record found it: Object's own is the program's reopened Object.
   -2 for none: a singleton's, an owner the record does not know, Object
   not reopened. */
int elab_const_write_owner(Compiler *c, int node) {
  const LoadRecord *r = c->lrec;
  if (!r || !r->cwown || node < 0 || node >= r->ncent || r->cwown[node] <= 0) return -2;
  int cls = r->cwown[node] - 2;
  if (cls == LR_MAIN) cls = comp_core_class(c, CORE_OBJECT);
  return cls >= 0 ? cls : -2;
}
/* The key of the value constant entity node is a write of or a read
   names, as elab_const_keys found it; NULL for none (a class's, one the
   record does not bind, a node made since) */
int elab_const_keys_gen(Compiler *c) { return c->lrec ? c->lrec->ckey_gen : 0; }
const char *elab_const_node_key(Compiler *c, int node) {
  return nt_const_key(c->nt, node);
}

/* The statements of the i-th opening of class cls the record holds, in
   the order the walk reached them (-1 for one with no body); -2 past the
   last. *header: its header */
int elab_class_opening_body(Compiler *c, int cls, int i, int *header) {
  LrClass *k = c->lrec ? lr_class(c->lrec, cls) : NULL;
  if (header) *header = k && i >= 0 && i < k->nopen ? k->open[i].node : -1;
  return k && i >= 0 && i < k->nopen ? k->open[i].body : -2;
}

/* ---- what a `super` reaches, from the record ----
   The forwarding lowering asks the record, once it is made, which method the
   `super` of a def reaches: a module's `def m(...) = super` at each position
   the module has (elab_module_super_owners), a class's after its own def
   (elab_class_super_owner). A position is a class side whose ancestors hold
   the module; its chain is lr_walk's order: prepends, own, includes or
   extends, a module's own list, then the superclass. Once the walk is past
   the def asking, the first owner of the name answers: a version of the
   program's (MX_PROGRAM, its def in reach; an attr reader in *reader), a
   builtin's method read through the names layer (MX_BUILTIN), an undef (MX_NONE),
   or what the record cannot follow (MX_UNKNOWN). */

/* The builtin class cls's superclass names when the record holds no class of it (`class SI < StopIteration`),
   NULL otherwise */
static const char *lr_builtin_super(Compiler *c, int cls) {
  LrClass *k = lr_class(c->lrec, cls);
  if (!k || !k->has_sup || lr_later(c, &k->sup, k->sup_ref) >= LR_MAIN || !nt_kind_is_constant(nt_kind(c->nt, k->sup_ref))) return NULL;
  const char *bn = lr_written_name(c->nt, k->sup_ref);
  return bn && (is_builtin_class_name(bn) || is_builtin_exception_name(bn)) ? bn : NULL;
}
/* The builtin a record class (a Struct or Data builder's, or one whose superclass is such a call) inherits from, NULL for others */
static const char *lr_record_root(Compiler *c, int cls) {
  int d = cls >= 0 && cls < c->nclasses ? c->classes[cls].def_node : -1;
  int call = d < 0 ? -1 : lr_record_class(c, cls) ? class_def_record(c->nt, d) : class_def_is_record_call(c->nt, class_def_super(c->nt, d)) ? class_def_super(c->nt, d) : -1;
  return call < 0 ? NULL : class_def_record_is_data(c->nt, call) ? "Data" : "Struct";
}

/* Who mixes each module in, once per record state: for module m, the positions (class + 1) * 2 + singleton whose own mixins
   name it, in mby[mby_at[m + 1] .. mby_at[m + 2]); mby_unknown when some mixin names a module the record cannot */
static void lr_mixed_by(Compiler *c) {
  LoadRecord *r = c->lrec;
  if (r->mby_at && r->mby_of.nhist == r->nhist && r->mby_of.ncls == r->ncls && r->mby_of.recorded == r->recorded && r->mby_of.ver == r->ver) return;
  free(r->mby); free(r->mby_at);
  int n = r->ncls + 1, total = 0, *cur = NULL;
  int *cnt = calloc((size_t)n + 1, sizeof(int));
  r->mby_at = malloc(sizeof(int) * ((size_t)n + 1));
  cur = malloc(sizeof(int) * ((size_t)n + 1));
  if (!cnt || !r->mby_at || !cur) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  r->mby_unknown = 0;
  r->mby = NULL;
  for (int pass = 0; pass < 2; pass++) {
    for (int k = LR_MAIN; k + 1 < r->ncls; k++) {
      LrClass *kc = lr_class(r, k);
      for (int i = 0; kc && i < kc->nmix; i++) {
        int m = lr_later(c, &kc->mix[i].mod, kc->mix[i].ref);
        if (m < LR_MAIN) {
          const char *bn = nt_kind_is_constant(nt_kind(c->nt, kc->mix[i].ref)) ? lr_written_name(c->nt, kc->mix[i].ref) : NULL;
          if (!(bn && is_builtin_module_name(bn))) r->mby_unknown = 1;
          continue;
        }
        if (!pass) cnt[m + 1]++;
        else r->mby[cur[m + 1]++] = (k + 1) * 2 + elab_mix_on_singleton(kc->mix[i].kind);
      }
    }
    if (pass) break;
    r->mby_at[0] = 0;
    for (int m = 0; m < n; m++) r->mby_at[m + 1] = r->mby_at[m] + cnt[m];
    total = r->mby_at[n];
    memcpy(cur, r->mby_at, sizeof(int) * ((size_t)n + 1));
    if (!(r->mby = malloc(sizeof(int) * (size_t)(total ? total : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  free(cnt); free(cur);
  r->mby_of.nhist = r->nhist; r->mby_of.ncls = r->ncls; r->mby_of.recorded = r->recorded; r->mby_of.ver = r->ver;
}

typedef struct { Compiler *c; int mod; const char *name; int seen, ans, depth, opaque, reader; int *reach, nreach, creach; } LrSup;
static void lr_sup_reach(LrSup *s, int def) {
  for (int i = 0; i < s->nreach; i++) if (s->reach[i] == def) return;
  s->reach = lr_grow(s->reach, &s->creach, s->nreach, sizeof(int));
  s->reach[s->nreach++] = def;
}
/* the first of builtin b's ancestors from the i-th that owns the name */
static int lr_sup_builtin(const LrSup *s, const char *b, int i) {
  for (const char *a; b && (a = builtin_ancestor_at(b, i)); i++)
    if (builtin_owns_instance_method(a, s->name)) return MX_BUILTIN;
  return MX_NONE;
}
/* owner (a, sg)'s latest version of the name (NULL: none) */
static const LrDef *lr_sup_last(const LrSup *s, int a, int sg) {
  const LrHist *h = lr_hist(s->c->lrec, a, sg, s->name, 0);
  for (int j = h ? h->n - 1 : -1; j >= 0; j--)
    if (lr_is_version(&h->v[j])) return &h->v[j];
  return NULL;
}
/* version v of owner (a, sg), once the walk is past the def: 1 when it answers */
static int lr_sup_take(LrSup *s, int a, int sg, const LrDef *v) {
  for (int step = 0; v && v->kind == LR_ALIAS && v->target && step < 8; step++) {   /* an alias: its original where it stands */
    const LrDef *d = NULL;
    if (elab_method_at(s->c, a, v->target, sg, elab_point_of(s->c, v->node), &d) != ELAB_YES) d = NULL;
    v = d;
  }
  if (!v) { s->ans = MX_UNKNOWN; return 1; }
  switch (v->kind) {
    case LR_DEF: lr_sup_reach(s, v->node); s->ans = MX_PROGRAM; break;
    case LR_READER: s->reader = 1; s->ans = MX_PROGRAM; break;
    case LR_UNDEF: s->ans = MX_NONE; break;
    case LR_REMOVE: return 0;
    default: s->opaque = 1; s->ans = MX_PROGRAM; break;   /* a define_method, a writer, a copy: no def to read the shape of */
  }
  return 1;
}
/* owner (a, sg)'s own versions of the name, once the walk is past the def: 1 when they answer */
static int lr_sup_own(LrSup *s, int a, int sg) {
  const LrDef *v = lr_sup_last(s, a, sg);
  return v && lr_sup_take(s, a, sg, v);
}
/* Object's methods: main's top level and the program's reopening of Object define into one table in CRuby, the
   later def replacing the earlier, so they are one owner whose answer is the latest version of the two in the
   record's order (elab_order); where the record orders them not, it cannot say. */
static int lr_sup_object(LrSup *s) {
  int o = lr_core(s->c->lrec, CORE_OBJECT), at = LR_MAIN;
  const LrDef *vm = lr_sup_last(s, LR_MAIN, 0), *vo = o < 0 ? NULL : lr_sup_last(s, o, 0);
  if (vm && vo) {
    int ord = elab_order(s->c, vm->node, vo->node);
    if (ord == PROG_UNKNOWN) { s->ans = MX_UNKNOWN; return 1; }
    if (ord == PROG_BEFORE) vm = NULL;
    else vo = NULL;
  }
  if (vo) at = o;
  return (vm || vo) && lr_sup_take(s, at, 0, vm ? vm : vo);
}
/* is cls the program's reopening of the core class or module of that name? */
static int lr_core_reopening(Compiler *c, int cls) {
  int slot = cls >= 0 && cls < c->nclasses && c->classes[cls].name ? builtin_core_class_slot(c->classes[cls].name) : -1;
  return slot >= 0 && comp_core_class(c, slot) == cls;
}
/* The `super` query is a visitor of the record's chain walk (lr_walk): it reads nothing before the def asking
   (s->seen) and answers at the first owner after it. */
static int lr_sup_own_step(void *ud, int cls, int sg) {
  LrSup *s = ud;
  if (++s->depth > 4 * lr_chain_bound(s->c)) { s->ans = MX_UNKNOWN; return LR_W_STOP; }
  if (cls == s->mod && !sg) { s->seen = 1; return 0; }
  if (!sg && lr_object_table(s->c, cls)) {   /* one owner, read at the reopening's step (at main's alone when Object is not reopened) */
    if (s->mod == LR_MAIN || s->mod == lr_core(s->c->lrec, CORE_OBJECT)) { s->seen = 1; return 0; }   /* (a super from Object's table leaves all of it) */
    return s->seen && (cls != LR_MAIN || lr_core(s->c->lrec, CORE_OBJECT) < 0) && lr_sup_object(s) ? LR_W_STOP : 0;
  }
  return s->seen && lr_sup_own(s, cls, sg) ? LR_W_STOP : 0;
}
static int lr_sup_mixin_step(void *ud, int cls, int ref) {   /* a builtin module (Enumerable), or one the record does not know */
  LrSup *s = ud;
  const char *bn = nt_kind_is_constant(nt_kind(s->c->nt, ref)) ? lr_written_name(s->c->nt, ref) : NULL;
  (void)cls;
  if (bn && is_builtin_module_name(bn)) {
    if (s->seen && lr_sup_builtin(s, bn, 0) == MX_BUILTIN) { s->ans = MX_BUILTIN; return LR_W_STOP; }
  }
  else if (s->seen) { s->ans = MX_UNKNOWN; return LR_W_STOP; }
  return 0;
}
/* a class whose chain goes on in a builtin: a core class's reopening, a record class, one naming a builtin superclass */
static int lr_sup_builtin_step(void *ud, int cls, int sg) {
  LrSup *s = ud;
  Compiler *c = s->c;
  int module = cls >= 0 && comp_class_is_module(c, &c->classes[cls]);
  const char *core = cls >= 0 && lr_core_reopening(c, cls) ? c->classes[cls].name : NULL;
  if (module) {   /* past a reopened core module's own builtin methods */
    if (!sg && s->seen && core && builtin_owns_instance_method(core, s->name)) { s->ans = MX_BUILTIN; return LR_W_STOP; }
    return 0;
  }
  const char *bsup = core ? NULL : lr_record_root(c, cls);
  if (!bsup && !core) bsup = lr_builtin_super(c, cls);
  if ((core || bsup) && !sg) {   /* the builtin's own methods, then its ancestors' */
    if (!s->seen) return LR_W_END;
    s->ans = lr_sup_builtin(s, core ? core : bsup, 0);
    return LR_W_STOP;
  }
  LrClass *k = lr_class(c->lrec, cls);
  if (!core && !bsup && k && k->has_sup && lr_later(c, &k->sup, k->sup_ref) < LR_MAIN) { s->ans = MX_UNKNOWN; return LR_W_STOP; }   /* a superclass the record does not know */
  return 0;
}
static int lr_sup_core_step(void *ud, int core);
static int lr_sup_done_step(void *ud, int cls, int sg);
static const LrWalker lr_sup_walker = { lr_sup_own_step, lr_sup_mixin_step, lr_sup_builtin_step, lr_sup_done_step, lr_sup_core_step, lr_sup_mixin_step, LR_TAIL_N };
/* a core class's own table, right after the program's reopening of it (lr_walk's tail) */
static int lr_sup_core_step(void *ud, int core) {
  LrSup *s = ud;
  if (!s->seen) return LR_W_END;
  if (!builtin_owns_instance_method(core_class_name(core), s->name)) return 0;
  s->ans = MX_BUILTIN;
  return LR_W_STOP;
}
/* past the core classes: nothing owns the name */
static int lr_sup_done_step(void *ud, int cls, int sg) {
  LrSup *s = ud;
  (void)cls; (void)sg;
  if (!s->seen) return LR_W_END;
  s->ans = MX_NONE;
  return LR_W_STOP;
}
/* the chain from cls (its singleton's when sg) to its end: 1 when it answers */
static int lr_sup_chain(LrSup *s, int cls, int sg) {
  int rc = lr_walk(s->c, &lr_sup_walker, s, cls, sg, 0);
  if (rc < 0) s->ans = MX_UNKNOWN;
  return rc != 0 && rc != LR_W_END;
}
static void lr_sup_out(LrSup *s, int **reach, int *nreach, int *reader, int *opaque) {
  *reach = s->reach; *nreach = s->nreach; *reader = s->reader; *opaque = s->opaque;
}
int elab_module_super_owners(Compiler *c, int holder, const char *name, int **reach, int *nreach, int *reader, int *opaque) {
  LoadRecord *r = c->lrec;
  int mod = elab_header_class(c, holder, NULL, NULL), bits = 0;
  *reach = NULL; *nreach = 0; *reader = 0; *opaque = 0;
  if (!r || !r->recorded || mod < LR_MAIN) return 1 << MX_UNKNOWN;
  lr_mixed_by(c);
  LrSup s = { c, mod, name, 0, MX_NONE, 0, 0, 0, NULL, 0, 0 };
  LrClass *mk = lr_class(r, mod);
  if (r->mby_unknown || (mk && mk->unheld)) bits |= 1 << MX_UNKNOWN;   /* (a mixin of a module the record cannot name, or onto an object, may be this one) */
  /* the positions: the class sides that mix mod in, directly or through a module that holds it (a subclass's chain
     passes the same segment, so it answers as its superclass does) */
  int n = (c->nclasses > r->ncls ? c->nclasses : r->ncls) + 2;   /* (classes registered after the walk are positions the record does not hold) */
  unsigned char *holds = calloc((size_t)n, 1), *pos = calloc((size_t)(2 * n), 1);
  int *work = malloc(sizeof(int) * (size_t)n), nw = 0;
  if (!holds || !pos || !work) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  holds[mod + 1] = 1; work[nw++] = mod;
  while (nw > 0) {
    int h = work[--nw];
    for (int i = h + 2 <= r->ncls + 1 ? r->mby_at[h + 1] : 0, e = h + 2 <= r->ncls + 1 ? r->mby_at[h + 2] : 0; i < e; i++) {
      int p = r->mby[i], k = p / 2 - 1, sg = p & 1;
      pos[p] = 1;
      if (!sg && k >= LR_MAIN && comp_class_is_module(c, &c->classes[k]) && !holds[k + 1]) { holds[k + 1] = 1; work[nw++] = k; }
    }
  }
  for (int k = LR_MAIN; k < c->nclasses; k++)
    for (int sg = 0; sg < 2; sg++) {
      if (!pos[(k + 1) * 2 + sg] || (!sg && k >= LR_MAIN && comp_class_is_module(c, &c->classes[k]))) continue;
      s.seen = 0; s.ans = MX_NONE; s.depth = 0;
      int got = lr_sup_chain(&s, k, sg);
      bits |= 1 << (s.seen ? (got ? s.ans : MX_NONE) : MX_UNKNOWN);   /* (a position whose chain the walk lost mod in is not read) */
    }
  free(holds); free(pos); free(work);
  lr_sup_out(&s, reach, nreach, reader, opaque);
  return bits;
}
/* The same for the `super` of a def in class cls, or (own) a call of the name on an instance of it: the chain from cls
   on, after cls's own def unless own. One answer: the bits hold a single owner. */
int elab_class_super_owner(Compiler *c, int cls, int own, const char *name, int **reach, int *nreach, int *reader, int *opaque) {
  LoadRecord *r = c->lrec;
  *reach = NULL; *nreach = 0; *reader = 0; *opaque = 0;
  if (!r || !r->recorded || cls < LR_MAIN) return 1 << MX_UNKNOWN;
  LrSup s = { c, own ? LR_UNKNOWN_CLASS : cls, name, own, MX_NONE, 0, 0, 0, NULL, 0, 0 };
  int got = lr_sup_chain(&s, cls, 0);
  lr_sup_out(&s, reach, nreach, reader, opaque);
  return 1 << (got ? s.ans : MX_NONE);
}
