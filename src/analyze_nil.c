/* analyze_nil.c -- whether an object-typed value may be nil (#7444).

   The fact covers an object and a builtin held as a pointer that is NULL
   for nil (a String, an Array, a Hash, an IO: nil_fact_tracked). One fact,
   decided once by the analysis: for every node, and for every
   slot a value is read back from (a local, a parameter, a block parameter,
   a global, a constant, an ivar, a method's value), whether the value may
   be nil. Its readers: the value-type selection (vt_nil_fact_classes,
   analyze.c), where a class an instance of which may be nil keeps its
   pointer layout; the representation (repr_of's may_nil); and the call
   plan's nil target (cplan_nil, call_plan.c), which tests a receiver that
   may be nil ahead of the call, or answers as NilClass does. Parameter
   tracking's obj_nilable (a call hands the parameter nil) seeds it.

   The fact is a may-be-nil over-approximation. Where the analysis cannot
   see what reaches a slot it answers "may be nil": an object a builtin call
   it does not model answers, a parameter a call it cannot see binds (a
   method named in a send or a method(:m), a method the runtime calls back),
   a block a proc or a define_method takes, a splat's element, a pattern's
   binding, a class-level ivar. Three shapes are taken as not nil without
   proof, listed here so a later step can model them: an element a builtin
   iteration, or a Ruby-defined builtin's yield, binds to a block parameter
   (`boxes.each { |b| }`), unless the Array is one the program stores nil
   into or leaves a gap in (one level in, below: NFW_ELEM_NIL); a container call
   other than the element reads and picks that can miss (an Array's or a
   Hash's own methods); and a builtin value a builtin call answers other
   than those picks and a String's slice (`gets` at the end of its input).
   A String bang that changed nothing is not one of them: it answers nil, and
   its fact says so (a String slice, `slice!` included, stays the unarmed
   element read it is, so a hot loop over slices pays for no test).

   Flow: the slots are flow-insensitive (a slot one write leaves nil may be
   nil at every read), except that a read of a local inside a truthiness
   guard of it (`if b`, `b && b.v`, `return unless b`, `unless b.nil?`) is
   not nil when nothing in the guarded region writes it. A read the
   definite-assignment walk (du_read_maybe_unset) finds can run before any
   write makes its slot may-be-nil, as maybe_unset does for an Integer.

   The slots, the method values and the parameters a call binds feed one
   another, so they are settled by a fixpoint: every round recomputes the
   node facts from the slots and only ever moves a slot's flag ahead in
   NFW_*'s order, so the rounds are bounded by the number of flags times
   the number of sources.

   Each may-be-nil answer carries where its nil comes from (NFW_*: of
   several sources, the first in that enum's order, a nil the program
   writes before one the analysis cannot bound), which the call plan's nil
   target reads (cplan_nil): it tests a nil the program writes, not one the
   fact cannot bound. */

#include "analyze_internal.h"
#include "call_plan.h"
#include "value_pos.h"

/* a memo entry whose value is being computed */
#define NF_BUSY 0xff

/* A slot the fact covers: nil_fact_tracked's, and under --share-strings
   the shared String handle a String slot became (#6765), whose NULL is nil
   as the String's is */
static int nf_tracked(const Compiler *c, TyKind t) {
  return nil_fact_tracked(t) || (t == TY_STRBUF);
}

/* ---- (class, ivar) and per-name side tables ---- */

typedef struct { int cls; const char *name; unsigned char wr, init, val; int val_round, ewr; } NFIvar;
/* a guarded region's writes of a name, memoized: a long branch read many
   times is scanned once per name */
typedef struct { int region; const char *name; unsigned char w; } NFRegion;

typedef struct {
  Compiler *c;
  const NodeTable *nt;
  int *par;               /* du_parent_map */
  DUPos dp;
  unsigned char *memo;    /* per node, this round: nf_code(), NF_BUSY while computed */
  unsigned char *ememo;   /* per node, this round: nf_elem's answer + 1, NF_BUSY while
                             computed */
  int *def_mi;            /* DefNode id -> its method scope, or -1 */
  NFIvar *iv; int iv_cap, iv_n;
  NFRegion *rg; int rg_cap, rg_n;
  int guarded;            /* the read just computed is not nil by a guard */
  int *pl_mi;             /* per call node: its plan's method (-2: not asked) */
  short *pl_owner;
  unsigned char *pl_disp;
  int *yield_nil;         /* per method scope: a value its yields may answer is nil */
  int *byname; int nbyname; /* the instance methods, sorted by name */
  int *kid_head, *kid_next, *kid_to; /* per class: the classes right below it
                             (subclasses, and includers of a module) */
  int *dfs, *seen, stamp; /* nf_ivar's walk: its stack, and the classes it met */
  unsigned char *mod;     /* per class: a module some class includes */
  int round;
  int changed;
  int all_ivars_nil;      /* an instance_variable_set the program makes */
  ANameHash dyn;          /* method names a send or a method(:m) reaches: a set,
                             asked once per method of the program */
} NF;

/* the (class, name) keys of the slots the side table holds besides an
   instance's ivars: a class variable by name, a top-level ivar */
/* an Array slot's element flag (LocalVar.obj_elem_may_nil, an ivar's ewr):
   the Array may hold nil, and a nil was stored into it through this name,
   which reaches the slots it was read out of (nf_elem_mark_back) */
enum { NF_EL_HOLDS = 1, NF_EL_STORED = 2 };

enum { NF_CVAR = -2, NF_TOP_IVAR = -3,
       NF_ELEM_IVAR = -4, /* an ivar's Array element flag a write no class
                             family carries marks, by name (nf_elem_ivar_op) */
       NF_ELEM_ANY = -5   /* the same, any write of the name */ };

static unsigned nf_ivar_hash(int cls, const char *name) {
  return sp_strhash(name) * 31u + (unsigned)(cls + 1);
}

/* The (class, ivar) entry, made on first need. */
static NFIvar *nf_ivar_slot(NF *f, int cls, const char *name) {
  if (f->iv_n * 2 >= f->iv_cap) {
    int ncap = f->iv_cap ? f->iv_cap * 2 : 256;
    NFIvar *nv = calloc((size_t)ncap, sizeof *nv);
    if (!nv) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int k = 0; k < f->iv_cap; k++) {
      if (!f->iv[k].name) continue;
      unsigned j = nf_ivar_hash(f->iv[k].cls, f->iv[k].name) & (unsigned)(ncap - 1);
      while (nv[j].name) j = (j + 1) & (unsigned)(ncap - 1);
      nv[j] = f->iv[k];
    }
    free(f->iv); f->iv = nv; f->iv_cap = ncap;
  }
  unsigned j = nf_ivar_hash(cls, name) & (unsigned)(f->iv_cap - 1);
  while (f->iv[j].name) {
    if (f->iv[j].cls == cls && sp_streq(f->iv[j].name, name)) return &f->iv[j];
    j = (j + 1) & (unsigned)(f->iv_cap - 1);
  }
  /* The table lives for the whole pass, and some callers build the name in a
     stack buffer (an attr reader's or writer's "@name"): keep a copy. */
  char *own = strdup(name);
  if (!own) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  f->iv[j].cls = cls; f->iv[j].name = own; f->iv[j].wr = 0; f->iv[j].init = 0;
  f->iv[j].val = 0; f->iv[j].val_round = -1; f->iv[j].ewr = 0;
  f->iv_n++;
  return &f->iv[j];
}

/* A slot's flag holds the source of its nil (NFW_*), 0 when it cannot be
   nil: set it for a nil from source `why` (none when 0). Of two sources it
   keeps the one ahead in NFW_*'s order -- a nil the program writes before
   one the analysis cannot bound -- so a flag only ever moves down that
   order, and the rounds still stop. */
static void nf_set(NF *f, int *flag, int why) {
  if (!why || (*flag && *flag <= why)) return;
  *flag = why;
  f->changed = 1;
}
/* can a flag still take a source ahead of its own? */
static int nf_open(int flag) {
  return flag != NFW_NIL;
}
static int nf_slot_why(NF *f, const int *flag) {
  (void)f;
  return *flag;
}

/* Does class y's chain reach class or module x (itself, an ancestor, or a
   module one of them includes)? */
static int nf_class_reaches(Compiler *c, int y, int x) {
  for (int k = y, d = 0; k >= 0 && d < 256; k = c->classes[k].parent, d++) {
    if (k == x) return 1;
    for (int m = 0; m < c->classes[k].nincluded_mods; m++)
      if (c->classes[k].included_mods[m] == x) return 1;
  }
  return 0;
}

static int nf_expr(NF *f, int v);
static int nf_elem_tracked(TyKind t);
static int nf_elem(NF *f, int v, int depth);

/* Was the variable written a non-nil value before read rd runs, on every
   path of the load phase (the load record)? A read in a method body, a
   lambda or a block run later runs at no known point: no write can be
   shown to run first. A nil write is a source of the slot's own. */
static int nf_written_before(NF *f, int rd, ElabVar kind, const char *name) {
  LoadPoint p = elab_point_of(f->c, rd);
  return p.region >= 0 && elab_written_before(f->c, kind, name, p) == ELAB_YES;
}

/* The user method call node id reaches (cplan_user_fresh), asked once: the
   plan reads the settled types and tables, which the rounds do not change */
static int nf_plan(NF *f, int id, int *owner, int *dispatch) {
  if (f->pl_mi[id] == -2) {
    const CallPlan *p = cplan_user_fresh(f->c, id);
    f->pl_mi[id] = p->dispatch == CP_REFUSE ? -1 : p->mi;
    f->pl_owner[id] = p->owner_ci;
    f->pl_disp[id] = p->dispatch;
  }
  if (owner) *owner = f->pl_owner[id];
  if (dispatch) *dispatch = f->pl_disp[id];
  return f->pl_mi[id];
}

/* ---- an ivar initialize sets on every path ---- */

/* Does subtree n hold a `return` of the method it runs in (not of a def or
   a lambda inside it)? */
static int nf_has_return(const NodeTable *nt, int n, int depth) {
  if (n < 0 || depth > 512) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_ReturnNode) return 1;
  if (k == NK_DefNode || k == NK_LambdaNode || k == NK_ClassNode || k == NK_ModuleNode) return 0;
  for (int i = 0; i < nt_num_refs(nt, n); i++)
    if (nf_has_return(nt, nt_ref_at(nt, n, i), depth + 1)) return 1;
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int an = 0; const int *av = nt_arr_at(nt, n, i, &an);
    for (int j = 0; j < an; j++) if (nf_has_return(nt, av[j], depth + 1)) return 1;
  }
  return 0;
}

static int nf_ivar_def(NF *f, int n, const char *ivn, int mi, int depth);

/* the method body or list st writes ivar ivn on every path that ends it
   normally: a statement that writes it, before any that can return */
static int nf_ivar_def_list(NF *f, int st, const char *ivn, int mi, int depth) {
  const NodeTable *nt = f->nt;
  if (st < 0) return 0;
  if (nt_kind(nt, st) != NK_StatementsNode) return nf_ivar_def(f, st, ivn, mi, depth);
  int n = 0; const int *b = nt_arr(nt, st, "body", &n);
  for (int i = 0; i < n; i++) {
    if (nf_ivar_def(f, b[i], ivn, mi, depth)) return 1;
    if (nf_has_return(nt, b[i], 0)) return 0;
  }
  return 0;
}

/* A raise ends no path normally: it writes every ivar. */
static int nf_raises(const NodeTable *nt, int n) {
  n = nt_paren_single(nt, n);
  if (n >= 0 && nt_kind(nt, n) == NK_StatementsNode) {
    int bn = 0; const int *b = nt_arr(nt, n, "body", &bn);
    return bn > 0 && nf_raises(nt, b[bn - 1]);
  }
  const char *cn = n >= 0 && nt_kind(nt, n) == NK_CallNode && nt_ref(nt, n, "receiver") < 0 ? nt_str(nt, n, "name") : NULL;
  return cn && (sp_streq(cn, "raise") || sp_streq(cn, "fail"));
}

/* Does evaluating n, in method mi, write ivar ivn of self on every path
   that completes it? It follows `super` into the parent's method and a
   call of one of self's own methods (a `reset`, a `setup`) into its body. */
static int nf_ivar_def(NF *f, int n, const char *ivn, int mi, int depth) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  if (n < 0 || depth > 8) return 0;
  switch (nt_kind(nt, n)) {
  case NK_InstanceVariableWriteNode: case NK_InstanceVariableOrWriteNode: {
    const char *wn = nt_str(nt, n, "name");
    if (wn && sp_streq(wn, ivn)) return 1;
    return nf_ivar_def(f, nt_ref(nt, n, "value"), ivn, mi, depth);
  }
  case NK_LocalVariableWriteNode:
    return nf_ivar_def(f, nt_ref(nt, n, "value"), ivn, mi, depth);
  case NK_MultiWriteNode: {
    int ln = 0; const int *l = nt_arr(nt, n, "lefts", &ln);
    for (int i = 0; i < ln; i++) {
      const char *wn = nt_kind(nt, l[i]) == NK_InstanceVariableTargetNode ? nt_str(nt, l[i], "name") : NULL;
      if (wn && sp_streq(wn, ivn)) return 1;
    }
    return 0;
  }
  case NK_StatementsNode: return nf_ivar_def_list(f, n, ivn, mi, depth);
  case NK_ParenthesesNode: return nf_ivar_def_list(f, nt_ref(nt, n, "body"), ivn, mi, depth);
  case NK_ElseNode: return nf_ivar_def_list(f, nt_ref(nt, n, "statements"), ivn, mi, depth);
  case NK_BeginNode: {
    int en = nt_ref(nt, n, "ensure_clause");
    if (en >= 0 && nf_ivar_def_list(f, nt_ref(nt, en, "statements"), ivn, mi, depth)) return 1;
    if (nt_ref(nt, n, "rescue_clause") >= 0) return 0;
    return nf_ivar_def_list(f, nt_ref(nt, n, "statements"), ivn, mi, depth);
  }
  case NK_IfNode: case NK_UnlessNode: {
    if (nf_ivar_def(f, nt_ref(nt, n, "predicate"), ivn, mi, depth)) return 1;
    int st = nt_ref(nt, n, "statements");
    int els = nt_ref(nt, n, nt_kind(nt, n) == NK_IfNode ? "subsequent" : "else_clause");
    if (st < 0 || els < 0) return 0;
    return (nf_raises(nt, st) || nf_ivar_def_list(f, st, ivn, mi, depth)) &&
           (nf_raises(nt, els) || nf_ivar_def(f, els, ivn, mi, depth));
  }
  case NK_CaseNode: {
    int els = nt_ref(nt, n, "else_clause");
    if (els < 0 || !(nf_raises(nt, nt_ref(nt, els, "statements")) || nf_ivar_def(f, els, ivn, mi, depth))) return 0;
    int wn = 0; const int *w = nt_arr(nt, n, "conditions", &wn);
    for (int i = 0; i < wn; i++) {
      int ws = nt_ref(nt, w[i], "statements");
      if (!nf_raises(nt, ws) && !nf_ivar_def_list(f, ws, ivn, mi, depth)) return 0;
    }
    return 1;
  }
  case NK_SuperNode: case NK_ForwardingSuperNode: {
    int pmi = nf_plan(f, n, NULL, NULL);
    return pmi >= 0 && pmi != mi && c->scopes[pmi].body >= 0 &&
           nf_ivar_def_list(f, c->scopes[pmi].body, ivn, pmi, depth + 1);
  }
  case NK_CallNode: {
    int r = nt_ref(nt, n, "receiver");
    const char *cn = nt_str(nt, n, "name");
    if (!cn || (r >= 0 && nt_kind(nt, r) != NK_SelfNode)) return 0;
    /* `self.x = v` through an attr writer of x */
    size_t cl = strlen(cn);
    if (cl > 1 && cn[cl - 1] == '=' && !strncmp(cn, ivn + 1, cl - 1) && ivn[cl] == 0 &&
        c->scopes[mi].class_id >= 0 && comp_writer_in_chain(c, c->scopes[mi].class_id, ivn + 1, NULL))
      return 1;
    /* one of self's own methods, called with no block */
    int tmi = nf_plan(f, n, NULL, NULL);
    if (tmi < 0 || tmi == mi || nt_ref(nt, n, "block") >= 0 || c->scopes[tmi].body < 0) return 0;
    return nf_ivar_def_list(f, c->scopes[tmi].body, ivn, tmi, depth + 1);
  }
  default: return 0;
  }
}

/* Does `new` on class k leave ivar ivn set: k's initialize writes it on
   every path? */
static int nf_init_sets(NF *f, int k, const char *ivn) {
  int mi = comp_method_in_chain(f->c, k, "initialize", NULL);
  if (mi < 0 || f->c->scopes[mi].body < 0) return 0;
  return nf_ivar_def_list(f, f->c->scopes[mi].body, ivn, mi, 0);
}

/* A write of ivar `ivn` into an instance of class k may store nil: a write
   in a method of k, of an ancestor, or of a module they include. */
static int nf_chain_writes(NF *f, int k, const char *ivn) {
  Compiler *c = f->c;
  for (int d = 0; k >= 0 && d < 256; k = c->classes[k].parent, d++) {
    NFIvar *e = nf_ivar_slot(f, k, ivn);
    if (e->wr) return e->wr;
    for (int m = 0; m < c->classes[k].nincluded_mods; m++)
      if ((e = nf_ivar_slot(f, c->classes[k].included_mods[m], ivn))->wr) return e->wr;
  }
  return NFW_NONE;
}

/* The ivar `ivn` read in a method of class cls, whose self is an instance
   of cls or of a class below it (or including it, for a module): nil until
   a write, unless that class's initialize writes it first; or a write the
   instance's chain makes stores a value that may be nil. */
static int nf_ivar(NF *f, int cls, const char *ivn) {
  Compiler *c = f->c;
  if (!ivn || cls < 0 || cls >= c->nclasses) return NFW_IVAR;
  if (f->all_ivars_nil) return NFW_OPAQUE;
  NFIvar *e = nf_ivar_slot(f, cls, ivn);
  if (e->val_round == f->round) return e->val;
  /* every class whose instances run cls's methods: cls, its subclasses,
     the classes including it, walked down the class tree */
  int r = 0, sp = 0;
  f->stamp++;
  f->dfs[sp++] = cls;
  f->seen[cls] = f->stamp;
  while (sp > 0 && !r) {
    int k = f->dfs[--sp];
    for (int ed = f->kid_head[k]; ed >= 0; ed = f->kid_next[ed]) {
      int kid = f->kid_to[ed];
      if (f->seen[kid] != f->stamp) { f->seen[kid] = f->stamp; f->dfs[sp++] = kid; }
    }
    if (c->classes[k].is_struct) { r = NFW_OPAQUE; break; }   /* a member `new` was not handed */
    NFIvar *ek = nf_ivar_slot(f, k, ivn);
    if (!ek->init) ek->init = nf_init_sets(f, k, ivn) ? 2 : 1;
    ek = nf_ivar_slot(f, k, ivn);
    r = ek->init == 1 ? NFW_IVAR : nf_chain_writes(f, k, ivn);
  }
  /* the slot table may have moved under e */
  e = nf_ivar_slot(f, cls, ivn);
  e->val = (unsigned char)r; e->val_round = f->round;
  return r;
}

/* ---- the guard a read of a local sits under ---- */

static int nf_is_read_of(const NodeTable *nt, int n, const char *nm) {
  n = nt_paren_single(nt, n);
  if (n < 0) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k != NK_LocalVariableReadNode && k != NK_LocalVariableWriteNode) return 0;
  const char *rn = nt_str(nt, n, "name");
  return rn && sp_streq(rn, nm);
}
static int nf_falsy_implies(Compiler *c, int p, const char *nm);
/* p truthy => local nm is not nil */
/* `x.is_a?(K)` (kind_of?, instance_of?) is true only for a non-nil x when nil
   is no instance of K: K names a program class, or a builtin class the
   program does not reopen (String, Hash), not a module (a module may be
   mixed into Object), and not NilClass, Object or BasicObject. Any other
   argument proves nothing, so the read stays may-be-nil. */
static int nf_class_excludes_nil(Compiler *c, int p) {
  const NodeTable *nt = c->nt;
  int a = nt_ref(nt, p, "arguments"), an = 0;
  const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
  if (an != 1) return 0;
  NodeKind ak = nt_kind(nt, av[0]);
  if (ak != NK_ConstantReadNode && ak != NK_ConstantPathNode) return 0;
  const char *k = nt_key_name(nt, av[0]);
  if (!k || sp_streq(k, "NilClass") || sp_streq(k, "Object") || sp_streq(k, "BasicObject") ||
      sp_streq(k, "Kernel")) return 0;
  /* a builtin class the program does not reopen (String, Hash, Integer):
     nil is no instance of one, but it is of a builtin module */
  if (const_node_class_index(c, av[0]) < 0)
    return ak == NK_ConstantReadNode && is_builtin_class_name(k) && !is_builtin_module_name(k);
  int ndk = 0; const NodeKind *dk = class_def_module_kinds(&ndk);
  for (int dh = 0; dh < ndk; dh++)
    NT_FOREACH_KIND(nt, dk[dh], m) {
      const char *mn = class_def_is_module(nt, m) ? class_def_name(nt, m) : NULL;
      if (mn && sp_streq(mn, k)) return 0;
    }
  return 1;
}
static int nf_truthy_implies(Compiler *c, int p, const char *nm) {
  const NodeTable *nt = c->nt;
  p = nt_paren_single(nt, p);
  if (p < 0) return 0;
  if (nf_is_read_of(nt, p, nm)) return 1;
  NodeKind k = nt_kind(nt, p);
  if (k == NK_AndNode)
    return nf_truthy_implies(c, nt_ref(nt, p, "left"), nm) || nf_truthy_implies(c, nt_ref(nt, p, "right"), nm);
  if (k == NK_CallNode) {
    const char *cn = nt_str(nt, p, "name");
    int r = nt_ref(nt, p, "receiver");
    if (!cn || r < 0) return 0;
    if (sp_streq(cn, "!")) return nf_falsy_implies(c, r, nm);
    if (!nf_is_read_of(nt, r, nm) || nt_kind(nt, nt_paren_single(nt, r)) != NK_LocalVariableReadNode) return 0;
    if (sp_streq(cn, "is_a?") || sp_streq(cn, "kind_of?") || sp_streq(cn, "instance_of?"))
      return nf_class_excludes_nil(c, p);
    if (sp_streq(cn, "!=")) {
      int a = nt_ref(nt, p, "arguments"), an = 0;
      const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
      return an == 1 && nt_kind(nt, av[0]) == NK_NilNode;
    }
  }
  return 0;
}
/* p falsy => local nm is not nil */
static int nf_falsy_implies(Compiler *c, int p, const char *nm) {
  const NodeTable *nt = c->nt;
  p = nt_paren_single(nt, p);
  if (p < 0) return 0;
  NodeKind k = nt_kind(nt, p);
  if (k == NK_OrNode)
    return nf_falsy_implies(c, nt_ref(nt, p, "left"), nm) || nf_falsy_implies(c, nt_ref(nt, p, "right"), nm);
  if (k == NK_CallNode) {
    const char *cn = nt_str(nt, p, "name");
    int r = nt_ref(nt, p, "receiver");
    if (!cn || r < 0) return 0;
    if (sp_streq(cn, "!")) return nf_truthy_implies(c, r, nm);
    if (!nf_is_read_of(nt, r, nm) || nt_kind(nt, nt_paren_single(nt, r)) != NK_LocalVariableReadNode) return 0;
    if (sp_streq(cn, "nil?")) return 1;
    if (sp_streq(cn, "==")) {
      int a = nt_ref(nt, p, "arguments"), an = 0;
      const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
      return an == 1 && nt_kind(nt, av[0]) == NK_NilNode;
    }
  }
  return 0;
}
/* Does subtree n write local nm (any write, in any nested block)? */
static int nf_writes_local(const NodeTable *nt, int n, const char *nm, int depth) {
  if (n < 0 || depth > 512) return 0;
  switch (nt_kind(nt, n)) {
  case NK_LocalVariableWriteNode: case NK_LocalVariableOrWriteNode:
  case NK_LocalVariableAndWriteNode: case NK_LocalVariableOperatorWriteNode:
  case NK_LocalVariableTargetNode: {
    const char *wn = nt_str(nt, n, "name");
    if (wn && sp_streq(wn, nm)) return 1;
    break;
  }
  case NK_DefNode: return 0;
  default: break;
  }
  for (int i = 0; i < nt_num_refs(nt, n); i++)
    if (nf_writes_local(nt, nt_ref_at(nt, n, i), nm, depth + 1)) return 1;
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int an = 0; const int *av = nt_arr_at(nt, n, i, &an);
    for (int k = 0; k < an; k++) if (nf_writes_local(nt, av[k], nm, depth + 1)) return 1;
  }
  return 0;
}
static int nf_region_writes(NF *f, int region, const char *nm) {
  if (f->rg_n * 2 >= f->rg_cap) {
    int ncap = f->rg_cap ? f->rg_cap * 2 : 256;
    NFRegion *nv = calloc((size_t)ncap, sizeof *nv);
    if (!nv) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int k = 0; k < f->rg_cap; k++) {
      if (!f->rg[k].name) continue;
      unsigned j = (sp_strhash(f->rg[k].name) * 31u + (unsigned)f->rg[k].region) & (unsigned)(ncap - 1);
      while (nv[j].name) j = (j + 1) & (unsigned)(ncap - 1);
      nv[j] = f->rg[k];
    }
    free(f->rg); f->rg = nv; f->rg_cap = ncap;
  }
  unsigned j = (sp_strhash(nm) * 31u + (unsigned)region) & (unsigned)(f->rg_cap - 1);
  while (f->rg[j].name) {
    if (f->rg[j].region == region && sp_streq(f->rg[j].name, nm)) return f->rg[j].w;
    j = (j + 1) & (unsigned)(f->rg_cap - 1);
  }
  int w = nf_writes_local(f->nt, region, nm, 0);
  f->rg[j].region = region; f->rg[j].name = nm; f->rg[j].w = (unsigned char)w;
  f->rg_n++;
  return w;
}
/* is a an ancestor of d (or d itself), looking no further out than region? */
static int nf_is_ancestor(NF *f, int a, int d, int region) {
  for (int n = d, g = 0; n >= 0 && g < 4096; n = f->par[n], g++) {
    if (n == a) return 1;
    if (n == region) return 0;
  }
  return 0;
}
/* a construct that can run its body again: the next pass, or a retry,
   reads after a write the pass before made */
static int nf_repeats(NodeKind k) {
  return k == NK_WhileNode || k == NK_UntilNode || k == NK_ForNode || k == NK_BlockNode ||
         k == NK_LambdaNode || k == NK_BeginNode;
}
/* Can a write of nm in the guarded region (subtree n) run before the read
   rd of it? A write on an earlier line can; one on the read's own line is
   taken to (the two are not ordered here), unless it is the write whose
   value holds the read (`cur = cur.nxt`: the read runs first); one on a
   later line can only when a loop, a block, a lambda or a begin inside the
   region holds both, whose next pass runs the read after it. */
static int nf_write_runs_first(NF *f, int n, const char *nm, int rd, long long rl, int region, int depth) {
  const NodeTable *nt = f->nt;
  if (n < 0 || depth > 512) return 0;
  switch (nt_kind(nt, n)) {
  case NK_LocalVariableWriteNode: case NK_LocalVariableOrWriteNode:
  case NK_LocalVariableAndWriteNode: case NK_LocalVariableOperatorWriteNode:
  case NK_LocalVariableTargetNode: {
    const char *wn = nt_str(nt, n, "name");
    if (wn && sp_streq(wn, nm) && !nf_is_ancestor(f, n, rd, region)) {
      long long wl = nt_int(nt, n, "node_line", 0);
      if (wl <= 0 || wl <= rl) return 1;
      for (int a = f->par[n], g = 0; a >= 0 && g < 4096; a = f->par[a], g++) {
        if (nf_repeats(nt_kind(nt, a)) && nf_is_ancestor(f, a, rd, region)) return 1;
        if (a == region) break;
      }
    }
    break;
  }
  case NK_DefNode: return 0;
  default: break;
  }
  for (int i = 0; i < nt_num_refs(nt, n); i++)
    if (nf_write_runs_first(f, nt_ref_at(nt, n, i), nm, rd, rl, region, depth + 1)) return 1;
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int an = 0; const int *av = nt_arr_at(nt, n, i, &an);
    for (int k = 0; k < an; k++) if (nf_write_runs_first(f, av[k], nm, rd, rl, region, depth + 1)) return 1;
  }
  return 0;
}
/* the region's writes of nm, as they bear on its read rd: none, or none
   that can run before the read */
static int nf_region_writes_before(NF *f, int region, const char *nm, int rd) {
  if (!nf_region_writes(f, region, nm)) return 0;
  long long rl = nt_int(f->nt, rd, "node_line", 0);
  if (rl <= 0) return 1;
  return nf_write_runs_first(f, region, nm, rd, rl, region, 0);
}
/* Does the program give nil a method `name` of its own (on NilClass, Object,
   Kernel, BasicObject, or at the top level), for call `call`? Then the call
   on nil raises nothing. */
static int nf_nil_user_method(Compiler *c, int call, const char *name) {
  static const int owners[] = { CORE_NILCLASS, CORE_OBJECT, CORE_KERNEL, CORE_BASIC_OBJECT, -1 };
  if (call_target(c, call, -1, name, CT_TOP, NULL) >= 0) return 1;
  for (int i = 0; owners[i] >= 0; i++) {
    int k = comp_core_class(c, owners[i]);
    if (k >= 0 && call_target(c, call, k, name, 0, NULL) >= 0) return 1;
  }
  return 0;
}
/* Does evaluating n call, on every path through it, a method on local nm
   that nil does not answer? Such a call raises NoMethodError for a nil nm
   (the plan's nil target tests it), so a read of nm after it is not nil:
   `if key < t.key` then `t.left` reads a t the comparison dereferenced.
   The parts every evaluation runs: a call's receiver and arguments, a
   write's value, a conditional's or a loop's predicate, a jump's value, the
   left of `&&` and `||`, a list's statements, an Array's elements. */
static int nf_derefs(Compiler *c, int n, const char *nm, int depth) {
  const NodeTable *nt = c->nt;
  n = nt_paren_single(nt, n);
  if (n < 0 || depth > 64) return 0;
  switch (nt_kind(nt, n)) {
  case NK_CallNode: {
    int r = nt_ref(nt, n, "receiver");
    const char *cn = nt_str(nt, n, "name");
    if (r >= 0 && cn && !nt_safe_nav(nt, n) && nf_is_read_of(nt, r, nm) &&
        nt_kind(nt, nt_paren_single(nt, r)) == NK_LocalVariableReadNode && !is_nil_method(cn) &&
        !nf_nil_user_method(c, n, cn)) return 1;
    if (r >= 0 && nf_derefs(c, r, nm, depth + 1)) return 1;
    int a = nt_ref(nt, n, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    for (int i = 0; i < an; i++) if (nf_derefs(c, av[i], nm, depth + 1)) return 1;
    return 0;
  }
  case NK_LocalVariableWriteNode: case NK_InstanceVariableWriteNode: case NK_GlobalVariableWriteNode:
  case NK_LocalVariableOperatorWriteNode: case NK_InstanceVariableOperatorWriteNode:
    return nf_derefs(c, nt_ref(nt, n, "value"), nm, depth + 1);
  case NK_IfNode: case NK_UnlessNode: case NK_WhileNode: case NK_UntilNode:
    return nf_derefs(c, nt_ref(nt, n, "predicate"), nm, depth + 1);
  case NK_AndNode: case NK_OrNode:
    return nf_derefs(c, nt_ref(nt, n, "left"), nm, depth + 1);
  case NK_ReturnNode: case NK_BreakNode: case NK_NextNode: {
    int a = nt_ref(nt, n, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    for (int i = 0; i < an; i++) if (nf_derefs(c, av[i], nm, depth + 1)) return 1;
    return 0;
  }
  case NK_StatementsNode: case NK_ArrayNode: {
    int bn = 0; const int *b = nt_arr(nt, n, nt_kind(nt, n) == NK_ArrayNode ? "elements" : "body", &bn);
    for (int i = 0; i < bn; i++) if (nf_derefs(c, b[i], nm, depth + 1)) return 1;
    return 0;
  }
  default: return 0;
  }
}
/* A statement that leaves the list: `return`, `next`, `break`, `raise` */
static int nf_is_jump(const NodeTable *nt, int s) {
  s = nt_paren_single(nt, s);
  if (s < 0) return 0;
  if (nt_kind(nt, s) == NK_StatementsNode) {
    int n = 0; const int *b = nt_arr(nt, s, "body", &n);
    return n > 0 && nf_is_jump(nt, b[n - 1]);
  }
  NodeKind k = nt_kind(nt, s);
  if (k == NK_ReturnNode || k == NK_NextNode || k == NK_BreakNode) return 1;
  if (k == NK_CallNode && nt_ref(nt, s, "receiver") < 0) {
    const char *cn = nt_str(nt, s, "name");
    return cn && (sp_streq(cn, "raise") || sp_streq(cn, "fail") || sp_streq(cn, "exit") ||
                  sp_streq(cn, "abort") || sp_streq(cn, "throw"));
  }
  return 0;
}
/* `return unless nm` / `raise ... if nm.nil?`: after it, nm is not nil */
static int nf_exit_guard(Compiler *c, int s, const char *nm) {
  const NodeTable *nt = c->nt;
  s = nt_paren_single(nt, s);
  if (s < 0) return 0;
  NodeKind k = nt_kind(nt, s);
  if (k == NK_OrNode)   /* `nm or return` */
    return nf_is_jump(nt, nt_ref(nt, s, "right")) && nf_truthy_implies(c, nt_ref(nt, s, "left"), nm);
  if (k != NK_IfNode && k != NK_UnlessNode) return 0;
  int els = nt_ref(nt, s, k == NK_IfNode ? "subsequent" : "else_clause");
  if (els >= 0 || !nf_is_jump(nt, nt_ref(nt, s, "statements"))) return 0;
  int p = nt_ref(nt, s, "predicate");
  return k == NK_IfNode ? nf_falsy_implies(c, p, nm) : nf_truthy_implies(c, p, nm);
}

/* Is the read rd of local nm (slot lv) under a guard that proves it is not
   nil, with no write of nm between the guard and the read
   (nf_region_writes_before: a write after the read in the guarded region is
   no write between, unless a loop in the region runs the read again)? An
   earlier statement of the same list that calls a method on nm which nil
   does not answer (nf_derefs) is such a guard: it raised for a nil.
   Walks out to the method, a block or a lambda, which may run after the
   slot changed. */
static int nf_local_guarded(NF *f, int rd, const char *nm, const LocalVar *lv) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  if (!f->par || lv->proc_rebinds) return 0;
  int cur = rd;
  for (int guard = 0; guard < 4096; guard++) {
    int p = f->par[cur];
    if (p < 0) return 0;
    NodeKind pk = nt_kind(nt, p);
    if (pk == NK_DefNode || pk == NK_LambdaNode || pk == NK_BlockNode || pk == NK_ClassNode ||
        pk == NK_ModuleNode || pk == NK_SingletonClassNode) return 0;
    int region = -1;
    if ((pk == NK_IfNode || pk == NK_UnlessNode) && cur != nt_ref(nt, p, "predicate")) {
      int pr = nt_ref(nt, p, "predicate");
      int in_then = cur == nt_ref(nt, p, "statements");
      int then_ok = pk == NK_IfNode ? nf_truthy_implies(c, pr, nm) : nf_falsy_implies(c, pr, nm);
      int else_ok = pk == NK_IfNode ? nf_falsy_implies(c, pr, nm) : nf_truthy_implies(c, pr, nm);
      if (in_then ? then_ok : else_ok) region = cur;
      /* a predicate that dereferenced nm ran before either branch */
      else if (nf_derefs(c, pr, nm, 0)) region = cur;
    }
    else if (pk == NK_AndNode && cur == nt_ref(nt, p, "right") &&
             nf_truthy_implies(c, nt_ref(nt, p, "left"), nm)) region = cur;
    else if (pk == NK_OrNode && cur == nt_ref(nt, p, "right") &&
             nf_falsy_implies(c, nt_ref(nt, p, "left"), nm)) region = cur;
    /* not a loop that tests after its body (`begin ... end while nm`,
       Prism's begin-modifier flag): that body runs once before the test */
    else if (pk == NK_WhileNode && !nt_loop_post(nt, p) && cur == nt_ref(nt, p, "statements") &&
             (nf_truthy_implies(c, nt_ref(nt, p, "predicate"), nm) || nf_derefs(c, nt_ref(nt, p, "predicate"), nm, 0)))
      region = cur;
    else if (pk == NK_UntilNode && !nt_loop_post(nt, p) && cur == nt_ref(nt, p, "statements") &&
             (nf_falsy_implies(c, nt_ref(nt, p, "predicate"), nm) || nf_derefs(c, nt_ref(nt, p, "predicate"), nm, 0)))
      region = cur;
    if (region >= 0) {
      if (!nf_region_writes_before(f, region, nm, rd)) return 1;
    }
    else if (pk == NK_StatementsNode) {
      int bn = 0; const int *b = nt_arr(nt, p, "body", &bn);
      int at = -1;
      for (int i = 0; i < bn; i++) if (b[i] == cur) { at = i; break; }
      for (int i = at - 1; i >= 0; i--) {
        int exit_guard = nf_exit_guard(c, b[i], nm);
        if (!exit_guard && nf_region_writes(f, b[i], nm)) break;
        if (exit_guard || nf_derefs(c, b[i], nm, 0)) {
          int clean = 1;
          for (int j = i + 1; j <= at && clean; j++)
            clean = j == at ? !nf_region_writes_before(f, b[j], nm, rd) : !nf_region_writes(f, b[j], nm);
          if (clean) return 1;
          break;
        }
      }
    }
    cur = p;
  }
  return 0;
}

/* ---- the node fact ---- */

/* the value of a list: its last statement, nil when it is empty */
static int nf_list(NF *f, int st) {
  if (st < 0) return NFW_NIL;
  if (nt_kind(f->nt, st) != NK_StatementsNode) return nf_expr(f, st);
  int n = 0; const int *b = nt_arr(f->nt, st, "body", &n);
  return n == 0 ? NFW_NIL : nf_expr(f, b[n - 1]);
}
/* the source of two that goes first (nf_set's order) */
static int nf_or(int a, int b) { return !a ? b : !b ? a : a < b ? a : b; }

static Compiler *nf_sort_c;
static int nf_byname_cmp(const void *a, const void *b) {
  int x = *(const int *)a, y = *(const int *)b;
  int r = strcmp(nf_sort_c->scopes[x].name, nf_sort_c->scopes[y].name);
  return r ? r : x - y;
}
/* the first entry of the name index named `name`, or nbyname */
static int nf_byname_first(NF *f, const char *name) {
  int lo = 0, hi = f->nbyname;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (strcmp(f->c->scopes[f->byname[mid]].name, name) < 0) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

/* the value of a user method mi a call reaches, and of every override a
   switch on the receiver's class may take instead */
static int nf_method_value(NF *f, int mi, int owner, int dispatch, const char *name) {
  Compiler *c = f->c;
  if (mi < 0 || mi >= c->nscopes) return NFW_OPAQUE;
  int r = nf_slot_why(f, &c->scopes[mi].ret_obj_may_nil);
  if ((dispatch == CP_SWITCH || dispatch == CP_PER_ARM) && name && owner >= 0)
    for (int j = nf_byname_first(f, name); j < f->nbyname; j++) {
      Scope *s = &c->scopes[f->byname[j]];
      if (!sp_streq(s->name, name)) break;
      if (nf_class_reaches(c, s->class_id, owner)) r = nf_or(r, nf_slot_why(f, &s->ret_obj_may_nil));
    }
  return r;
}

/* the receiver's class an element read misses on (Array, Hash) */
static int nf_container(TyKind t) {
  return ty_is_array(t) || ty_is_obj_array(t) || ty_is_hash(t) || t == TY_POLY;
}

static int nf_next_nil(NF *f, int n, int depth);
static int nf_call(NF *f, int v) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  const char *nm = nt_str(nt, v, "name");
  /* only a tracked value's nil: the slots it can reach are tracked */
  if (!nf_tracked(f->c, comp_ntype_stored(c, v))) return NFW_NONE;
  if (nt_safe_nav(nt, v)) return NFW_SAFE_NAV;
  if (!nm) return NFW_OPAQUE;
  int r = nt_ref(nt, v, "receiver");
  int a = nt_ref(nt, v, "arguments"), an = 0;
  const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
  int owner = -1, dispatch = CP_NONE;
  int pmi = nf_plan(f, v, &owner, &dispatch);
  if (pmi >= 0) return nf_method_value(f, pmi, owner, dispatch, nm);
  TyKind rt = r >= 0 ? comp_ntype_stored(c, r) : TY_UNKNOWN;
  if (sp_streq(nm, "new") || sp_streq(nm, "allocate")) return NFW_NONE;
  /* an attr reader: its ivar */
  if (ty_is_object(rt) || r < 0) {
    int cls = ty_is_object(rt) ? ty_object_class(rt) : -1;
    if (r < 0) {
      Scope *s = comp_scope_of(c, v);
      cls = s && !s->is_cmethod ? s->class_id : -1;
    }
    if (cls >= 0 && comp_reader_in_chain(c, cls, nm, NULL)) {
      char ivn[256];
      snprintf(ivn, sizeof ivn, "@%s", nm);
      return nf_ivar(f, cls, ivn);
    }
  }
  /* --share-strings: `then` with a literal block answers its block's value,
     which can be the receiver's nil (a route of the shared String, which a
     parameter it is handed may then bind) */
  if (r >= 0 && is_then_alias(nm)) {
    int blk = nt_ref(nt, v, "block");
    /* the tail's value, or a `next`'s (as a yielded block's value is read) */
    if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode)
      return nf_or(nf_list(f, nt_ref(nt, blk, "body")), nf_next_nil(f, nt_ref(nt, blk, "body"), 0));
  }
  /* the receiver itself */
  if (r >= 0 && (sp_streq(nm, "itself") || sp_streq(nm, "tap") || sp_streq(nm, "dup") ||
                 sp_streq(nm, "clone") || sp_streq(nm, "freeze")))
    return nf_expr(f, r);
  /* a String bang answers nil when it changes nothing (strip!, sub!, chomp!,
     ...), except those that answer the receiver always (PF_STR_SELF): the
     nil a mutator or any other method called on its result then meets. A
     boxed receiver (an element of a boxed Array or Hash) answers it as a
     String too. */
  if (r >= 0 && (rt == TY_STRING || rt == TY_STRBUF || (rt == TY_POLY && comp_ntype_stored(c, v) == TY_STRING)) &&
      ty_str_typed_bang_flags(nm) && !(ty_str_typed_bang_flags(nm) & PF_STR_SELF))
    return NFW_NIL;
  /* a removal from an object Array: a queue or a stack the program empties,
     whose nil a call on the element then meets */
  if (r >= 0 && ty_is_obj_array(rt) &&
      (sp_streq(nm, "pop") || sp_streq(nm, "shift") || sp_streq(nm, "delete_at")))
    return NFW_TAKEN;
  /* an element read that can miss, a pick that finds nothing */
  if (r >= 0 && nf_container(rt)) {
    static const char *const picks[] = { "find", "detect", "first", "last", "min", "max", "min_by",
      "max_by", "sample", "shift", "pop", "[]", "at", "dig", "delete", "delete_at", "slice",
      "slice!", "inject", "reduce", "sum", "find_index", "key", "fetch", "assoc", "rassoc", "values_at", NULL };
    for (int i = 0; picks[i]; i++) {
      if (!sp_streq(nm, picks[i])) continue;
      /* an element of an Array that can hold nil may be nil wherever it is */
      return comp_ntype_stored(c, v) == ty_array_elem(rt) && nf_elem_tracked(rt) && nf_elem(f, r, 0) ? NFW_ELEM_NIL
             : NFW_ELEM;
    }
    return NFW_NONE;
  }
  /* a String's slice past its end */
  if (r >= 0 && (rt == TY_STRING || rt == TY_STRBUF) &&
      (sp_streq(nm, "[]") || sp_streq(nm, "slice") || sp_streq(nm, "slice!") || sp_streq(nm, "byteslice")))
    return NFW_ELEM;
  /* the builtin surface: an object-typed answer of a call it does not model
     (send, instance_variable_get, a Method's or a Proc's call, a yield's
     value through then) may be nil; a builtin-typed one is taken as not nil
     (see the header) */
  (void)an; (void)av;
  return ty_is_object(comp_ntype_stored(c, v)) ? NFW_OPAQUE : NFW_NONE;
}

/* ---- one level in: an Array whose elements may be nil ----
   The header's first unproven shape, modeled for the Arrays whose elements
   are pointers (an object's, a String's, an Array's), where nil is NULL: a
   slot's flag (obj_elem_may_nil, an ivar's by its class family) says an
   element may be nil because the program stores a nil it writes into the
   Array -- the nils cplan_nil arms a builtin call for, not one the
   analysis cannot bound -- or leaves a gap a write past the end fills with
   nil (array_mutation_stores, strict: a pointer Array has no run-time flag
   to catch a gap the mark misses, as an Integer Array's may_nil does),
   through any name of it. A block parameter an iteration over such an
   Array binds, and an element a read or a pick takes out of it, may then
   be nil (NFW_ELEM_NIL). An Array built by pushes or a map of values that
   cannot be nil is unmarked, so a hot loop over it pays for no test. */

static LocalVar *nf_local_of(NF *f, int node, const char *nm);
static int nf_next_nil(NF *f, int n, int depth);

/* an Array whose elements are pointers, NULL for nil (an Integer or Float
   Array holds the sentinel instead: analyze.c's nullable_int_elem) */
static int nf_elem_tracked(TyKind t) {
  return (ty_is_array(t) || ty_is_obj_array(t)) && nil_fact_tracked(ty_array_elem(t));
}

/* a nil the program writes, as cplan_nil reads one */
static int nf_written_nil(int why) {
  return why == NFW_NIL || why == NFW_NO_ELSE || why == NFW_SAFE_NAV || why == NFW_UNSET ||
         why == NFW_ELEM_NIL || why == NFW_TAKEN;
}

/* An ivar's element flag sits on the topmost class of the family that
   carries the ivar (nullable_elem_ivar_in, where repr_of_ivar reads it), so
   a store through one class's @items guards that family's reads, not an
   unrelated class's @items. A write no family carries -- in a module, which
   every includer's instances run, a class-level ivar, or an ivar the
   method's own class does not hold -- marks the name (NF_ELEM_IVAR), which
   every family's read of it takes too; a read no family carries takes any
   write of the name (NF_ELEM_ANY). */
static int nf_elem_ivar_root(NF *f, int cls, const char *ivn) {
  ClassInfo *ci = NULL;
  if (cls < 0 || cls >= f->c->nclasses || f->mod[cls] || nullable_elem_ivar_in(f->c, cls, ivn, &ci) < 0 || !ci)
    return -1;
  return (int)(ci - f->c->classes);
}

static void nf_elem_add(NF *f, int *fl, int bits) {
  if ((*fl | bits) != *fl) { *fl |= bits; f->changed = 1; }
}

/* Ivar ivn's element flag (NF_EL_*) as a method of class cls (-1: none)
   reads it, after adding `bits` to it through that method. */
static int nf_elem_ivar_op(NF *f, int cls, const char *ivn, int bits) {
  int root = nf_elem_ivar_root(f, cls, ivn);
  if (bits) {
    nf_elem_add(f, &nf_ivar_slot(f, root >= 0 ? root : NF_ELEM_IVAR, ivn)->ewr, bits);
    nf_elem_add(f, &nf_ivar_slot(f, NF_ELEM_ANY, ivn)->ewr, bits);
  }
  if (root < 0) return nf_ivar_slot(f, NF_ELEM_ANY, ivn)->ewr;
  return nf_ivar_slot(f, root, ivn)->ewr | nf_ivar_slot(f, NF_ELEM_IVAR, ivn)->ewr;
}

/* The element flag (NF_EL_*) of the slot node v reads or writes, after
   adding `bits` to it: a local's, a global's, a constant's, a class
   variable's (by name), an ivar's (nf_elem_ivar_op), or the ivar an attr
   reader hands out. -1 when v names no slot. */
static int nf_elem_slot_op(NF *f, int v, int bits) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  v = nt_paren_single(nt, v);
  if (v < 0) return -1;
  const char *nm = nt_str(nt, v, "name");
  if (!nm) return -1;
  int *fl = NULL;
  switch (nt_kind(nt, v)) {
  case NK_LocalVariableReadNode: case NK_LocalVariableWriteNode: case NK_LocalVariableOrWriteNode:
  case NK_LocalVariableAndWriteNode: {
    LocalVar *lv = nf_local_of(f, v, nm);
    fl = lv ? &lv->obj_elem_may_nil : NULL;
    break;
  }
  case NK_GlobalVariableReadNode: case NK_GlobalVariableWriteNode: case NK_GlobalVariableOrWriteNode:
  case NK_GlobalVariableAndWriteNode: {
    LocalVar *g = nm[0] == '$' ? comp_gvar(c, comp_resolve_gvar(c, nm + 1)) : NULL;
    fl = g ? &g->obj_elem_may_nil : NULL;
    break;
  }
  case NK_ConstantReadNode: case NK_ConstantWriteNode: case NK_ConstantOrWriteNode:
  case NK_ConstantAndWriteNode: {
    LocalVar *k = comp_const_at(c, v);
    fl = k ? &k->obj_elem_may_nil : NULL;
    break;
  }
  case NK_InstanceVariableReadNode: case NK_InstanceVariableWriteNode:
  case NK_InstanceVariableOrWriteNode: case NK_InstanceVariableAndWriteNode: {
    /* a class method's ivar is the class's own slot: no family's */
    Scope *s = comp_scope_of(c, v);
    int cls = !s || s->is_cmethod ? -1 : s->class_id >= 0 ? s->class_id : c->toplevel_class;
    return nf_elem_ivar_op(f, cls, nm, bits);
  }
  case NK_ClassVariableReadNode: case NK_ClassVariableWriteNode: case NK_ClassVariableOrWriteNode:
  case NK_ClassVariableAndWriteNode:
    fl = &nf_ivar_slot(f, NF_CVAR, nm)->ewr;
    break;
  case NK_CallNode: {
    int r = nt_ref(nt, v, "receiver"), dc = -1;
    TyKind rt = r >= 0 ? comp_ntype_stored(c, r) : TY_UNKNOWN;
    char ivn[256];
    if (r < 0 || !ty_is_object(rt) || nt_ref(nt, v, "arguments") >= 0 ||
        !comp_reader_in_chain(c, ty_object_class(rt), nm, &dc) || dc < 0)
      return -1;
    snprintf(ivn, sizeof ivn, "@%s", comp_resolve_alias(c, ty_object_class(rt), nm));
    return nf_elem_ivar_op(f, dc, ivn, bits);
  }
  default:
    return -1;
  }
  if (!fl) return -1;
  nf_elem_add(f, fl, bits);
  return *fl;
}

static void nf_elem_mark(NF *f, int slot_node, int why) {
  if (why) (void)nf_elem_slot_op(f, slot_node, NF_EL_HOLDS);
}

static void nf_elem_mark_back(NF *f, int v, int depth);

/* One value position of a conditional or a sequence: the slots the Array there
   is read out of, at the next level. A walk that gave up marks only the node it
   stopped at. */
typedef struct { NF *f; int depth; } NfBackArms;
static int nf_back_arm(const VpLeaf *leaf, void *ud) {
  NfBackArms *a = ud;
  if (leaf->kind != VPK_NIL) nf_elem_mark_back(a->f, leaf->node, a->depth + 1);
  return 0;
}

/* An Array is a reference: a nil stored through one name of it (`b = a;
   b << nil`, a parameter, a reader's value) lands where every other name
   reads it. Mark the slots the Array value v is read out of as stored
   into: a slot, the ivar an attr reader or a method's last value hands
   out, the receiver of a mutator that answers it (`a.push(x)`), either
   branch of a choice. A copy (dup, a filter, `+`) is another Array and
   stays unmarked. Only a store goes back: an Array that merely received
   one that holds nil (a parameter bound from it) marks no other caller's. */
static void nf_elem_mark_back(NF *f, int v, int depth) {
  const NodeTable *nt = f->nt;
  if (v < 0 || v >= nt->count || depth > 8) return;
  switch (nt_kind(nt, v)) {
  case NK_ParenthesesNode: case NK_StatementsNode: case NK_ElseNode: case NK_IfNode: case NK_UnlessNode:
  case NK_CaseNode: case NK_CaseMatchNode: case NK_BeginNode: case NK_RescueModifierNode:
  case NK_AndNode: case NK_OrNode: {
    NfBackArms a = { f, depth };
    for_each_value_position(nt, v, VP_AND_LEFT | VPS_ALL, nf_back_arm, &a);
    return;
  }
  case NK_LocalVariableWriteNode: case NK_InstanceVariableWriteNode: case NK_GlobalVariableWriteNode:
  case NK_ClassVariableWriteNode: case NK_ConstantWriteNode:
    (void)nf_elem_slot_op(f, v, NF_EL_HOLDS | NF_EL_STORED);
    nf_elem_mark_back(f, nt_ref(nt, v, "value"), depth + 1);
    return;
  case NK_CallNode: {
    if (nf_elem_slot_op(f, v, NF_EL_HOLDS | NF_EL_STORED) >= 0) return;
    int pmi = nf_plan(f, v, NULL, NULL);
    /* the depth is passed unchanged: a method body is a structure node (a
       StatementsNode, a BeginNode, parentheses), which costs the level */
    if (pmi >= 0) { nf_elem_mark_back(f, f->c->scopes[pmi].body, depth); return; }
    if (mutated_array(f->c, v) != v) nf_elem_mark_back(f, nt_ref(nt, v, "receiver"), depth + 1);
    return;
  }
  default:
    (void)nf_elem_slot_op(f, v, NF_EL_HOLDS | NF_EL_STORED);
  }
}

/* Can an element of the Array node v's value be nil (NFW_ELEM_NIL, else
   0)? A slot's flag, a literal's elements, what `Array.new` or a map
   fills it with, and the receiver's or the operands' own elements for a
   call that hands them on (a filter, a slice, a copy, a mutator, `+`). */
static int nf_elem_uncached(NF *f, int v, int depth);
static int nf_elem(NF *f, int v, int depth) {
  if (v < 0 || v >= f->nt->count || depth > 32) return 0;
  unsigned char m = f->ememo[v];
  if (m == NF_BUSY) return 0;   /* met again while computed: its other sources decide */
  if (m) return m == 2 ? NFW_ELEM_NIL : 0;
  f->ememo[v] = NF_BUSY;
  int r = nf_elem_uncached(f, v, depth);
  f->ememo[v] = r ? 2 : 1;
  return r;
}
/* One value position of a conditional or a sequence: whether the Array there
   can hold a nil, at the next level. A walk that gave up can. */
typedef struct { NF *f; int depth; } NfElemArms;
static int nf_elem_arm(const VpLeaf *leaf, void *ud) {
  NfElemArms *a = ud;
  if (leaf->kind == VPK_NIL) return 0;
  return leaf->kind == VPK_GAVE_UP || nf_elem(a->f, leaf->node, a->depth + 1);
}
static int nf_elem_uncached(NF *f, int v, int depth) {
  const NodeTable *nt = f->nt;
  switch (nt_kind(nt, v)) {
  case NK_ParenthesesNode: case NK_StatementsNode: case NK_ElseNode: case NK_IfNode: case NK_UnlessNode:
  case NK_CaseNode: case NK_CaseMatchNode: case NK_BeginNode: case NK_RescueModifierNode:
  case NK_AndNode: case NK_OrNode: {
    NfElemArms a = { f, depth };
    return for_each_value_position(nt, v, VP_AND_LEFT | VPS_ALL, nf_elem_arm, &a)
           ? NFW_ELEM_NIL : 0;
  }
  case NK_LocalVariableWriteNode: case NK_InstanceVariableWriteNode: case NK_GlobalVariableWriteNode:
  case NK_ClassVariableWriteNode: case NK_ConstantWriteNode:
    return nf_elem(f, nt_ref(nt, v, "value"), depth + 1);
  case NK_ArrayNode: {
    int en = 0; const int *ev = nt_arr(nt, v, "elements", &en);
    for (int i = 0; i < en; i++) {
      int w = nt_kind(nt, ev[i]) == NK_SplatNode ? nf_elem(f, nt_ref(nt, ev[i], "expression"), depth + 1)
              : nf_written_nil(nf_expr(f, ev[i]));
      if (w) return NFW_ELEM_NIL;
    }
    return 0;
  }
  case NK_CallNode: {
    int fl = nf_elem_slot_op(f, v, 0);
    if (fl >= 0) return fl ? NFW_ELEM_NIL : 0;
    const char *nm = nt_str(nt, v, "name");
    int r = nt_ref(nt, v, "receiver"), blk = nt_ref(nt, v, "block");
    int a = nt_ref(nt, v, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (!nm) return 0;
    if (blk >= 0 && nt_kind(nt, blk) != NK_BlockNode) blk = -1;
    int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
    /* `Array.new(n)`'s nils, `Array.new(n, v)`'s v, a block's values */
    if (r >= 0 && nt_kind(nt, r) == NK_ConstantReadNode && is_array_constructor(nt_str(nt, r, "name"), nm)) {
      if (blk >= 0) return nf_written_nil(nf_or(nf_list(f, body), nf_next_nil(f, body, 0))) ? NFW_ELEM_NIL : 0;
      if (an == 1 && !nf_elem_tracked(comp_ntype_stored(f->c, av[0]))) return NFW_ELEM_NIL;
      return an == 2 && nf_written_nil(nf_expr(f, av[1])) ? NFW_ELEM_NIL : 0;
    }
    /* a user method's value: its body's last value */
    int pmi = nf_plan(f, v, NULL, NULL);
    if (pmi >= 0) {
      int mb = f->c->scopes[pmi].body;
      return mb >= 0 ? nf_elem(f, mb, depth + 1) : 0;
    }
    if (r < 0 || !nf_elem_tracked(comp_ntype_stored(f->c, r))) return 0;
    /* the mapped element is the block's value */
    if (is_map_alias(nm) && blk >= 0)
      return nf_written_nil(nf_or(nf_list(f, body), nf_next_nil(f, body, 0))) ? NFW_ELEM_NIL : 0;
    /* `a + b`, `a | b`, `a.union(b)` (a concat lands in its receiver's slot) */
    if (is_plus_op(nm) || is_union_alias(nm)) {
      for (int k = 0; k < an; k++) if (nf_elem(f, av[k], depth + 1)) return NFW_ELEM_NIL;
      return nf_elem(f, r, depth + 1);
    }
    if ((is_slice_alias(nm) && slice_read_call(f->c, v)) || elem_preserving_call(nm) ||
        mutated_array(f->c, v) != v)
      return nf_elem(f, r, depth + 1);
    return 0;
  }
  default: {
    return nf_elem_slot_op(f, v, 0) > 0 ? NFW_ELEM_NIL : 0;
  }
  }
}

/* The marks the writes make: a slot written an Array that may hold nil,
   and an Array a mutation stores nil into or leaves a gap in; either goes
   back to the slots the written Array is read out of (nf_elem_mark_back). */
static void nf_elem_writes(NF *f) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  static const NodeKind wk[] = { NK_LocalVariableWriteNode, NK_LocalVariableOrWriteNode,
    NK_LocalVariableAndWriteNode, NK_InstanceVariableWriteNode, NK_InstanceVariableOrWriteNode,
    NK_InstanceVariableAndWriteNode, NK_GlobalVariableWriteNode, NK_GlobalVariableOrWriteNode,
    NK_GlobalVariableAndWriteNode, NK_ClassVariableWriteNode, NK_ClassVariableOrWriteNode,
    NK_ClassVariableAndWriteNode, NK_ConstantWriteNode, NK_ConstantOrWriteNode, NK_ConstantAndWriteNode };
  for (int q = 0; q < (int)(sizeof wk / sizeof *wk); q++)
    NT_FOREACH_KIND(nt, wk[q], w) {
      int val = nt_ref(nt, w, "value");
      /* `Array.new(n) { ... }` whose block can answer nil is typed a
         PolyArray where the slot it fills settles a pointer one; a boxed
         value (a choice between an Array and nil) carries no element fact
         of its own, but a nil stored through its slot still lands in the
         Array it was chosen from */
      int boxed = val >= 0 && comp_ntype_stored(c, val) == TY_POLY;
      if (val < 0 || !(nf_elem_tracked(comp_ntype_stored(c, val)) || comp_ntype_stored(c, val) == TY_POLY_ARRAY || boxed)) continue;
      if (!boxed) nf_elem_mark(f, w, nf_elem(f, val, 0));
      int wf = nf_elem_slot_op(f, w, 0);
      if (wf > 0 && (wf & NF_EL_STORED)) nf_elem_mark_back(f, val, 0);
    }
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    int recv = mutated_array(c, nt_ref(nt, id, "receiver"));
    /* a boxed receiver stores into whichever Array it holds */
    if (recv < 0 || !(nf_elem_tracked(comp_ntype_stored(c, recv)) || comp_ntype_stored(c, recv) == TY_POLY)) continue;
    int from, to, elems;
    int g = array_mutation_stores(c, id, 1, &from, &to, &elems);
    if (g < 0) continue;
    int why = g;
    int a = nt_ref(nt, id, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    for (int k = from; k < to && !why && av; k++)
      why = elems ? nf_elem(f, av[k], 0) : nf_written_nil(nf_expr(f, av[k]));
    /* fill's block fills in its values */
    int blk = nt_ref(nt, id, "block");
    if (!why && blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) {
      int body = nt_ref(nt, blk, "body");
      why = nf_written_nil(nf_or(nf_list(f, body), nf_next_nil(f, body, 0)));
    }
    if (why) nf_elem_mark_back(f, recv, 0);
  }
}

/* block blk's parameter i, or NULL */
static LocalVar *nf_block_param(NF *f, int blk, int i) {
  const char *pn = block_param_name(f->c, blk, i);
  int body = nt_ref(f->nt, blk, "body");
  LocalVar *lv = pn ? nf_local_of(f, body >= 0 ? body : blk, pn) : NULL;
  return lv || !pn ? lv : nf_local_of(f, blk, pn);
}

/* The block parameters a builtin iteration over such an Array binds to its
   elements (elem_block_params): each may be nil. A parameter's slot can be
   boxed while an inlined iteration binds it typed, so the flag goes on a
   boxed slot too: the read is what the guard asks about. */
static void nf_bind_elems(NF *f, int id, int blk) {
  int recv = -1;
  int np = elem_block_params(f->c, id, &recv);
  if (!np || !nf_elem_tracked(comp_ntype_stored(f->c, recv)) || !nf_elem(f, recv, 0)) return;
  for (int i = 0; i < np; i++) {
    LocalVar *lv = nf_block_param(f, blk, i);
    if (lv && (nf_tracked(f->c, lv->type) || lv->type == TY_POLY)) nf_set(f, &lv->obj_may_nil, NFW_ELEM_NIL);
  }
}

/* An Array a method's yield hands block blk (tap's receiver) and the
   block's parameter are one Array: one that can hold nil makes the
   parameter one, and a nil the block stores through the parameter lands in
   the yielded Array. A yield a parameter list spreads is left alone. */
static void nf_bind_yield_elems(NF *f, int id, int blk, int mi, int from_recv, int **yields, int *nyields) {
  const NodeTable *nt = f->nt;
  for (int i = 0; i < 9 && block_param_name(f->c, blk, i); i++) {
    LocalVar *lv = nf_block_param(f, blk, i);
    if (!lv || !nf_elem_tracked(lv->type)) continue;
    int nsrc = from_recv ? i == 0 : nyields[mi];
    for (int y = 0; y < nsrc; y++) {
      int src = -1;
      if (from_recv) src = nt_ref(nt, id, "receiver");
      else {
        int ya = nt_ref(nt, yields[mi][y], "arguments"), yn = 0;
        const int *yv = ya >= 0 ? nt_arr(nt, ya, "arguments", &yn) : NULL;
        int splat = nt_argv_has_splat(nt, yv, yn);
        if (!splat && !(yn == 1 && block_param_name(f->c, blk, 1)) && i < yn) src = yv[i];
      }
      if (src < 0) continue;
      if (!(lv->obj_elem_may_nil & NF_EL_HOLDS) && nf_elem(f, src, 0)) nf_elem_add(f, &lv->obj_elem_may_nil, NF_EL_HOLDS);
      if (lv->obj_elem_may_nil & NF_EL_STORED) nf_elem_mark_back(f, src, 0);
    }
  }
}

static int nf_owner_method(NF *f, int n);

static int nf_expr_uncached(NF *f, int v) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  switch (nt_kind(nt, v)) {
  case NK_NilNode: return NFW_NIL;
  case NK_SelfNode: case NK_IntegerNode: case NK_FloatNode: case NK_StringNode:
  case NK_SymbolNode: case NK_TrueNode: case NK_FalseNode: case NK_ArrayNode: case NK_HashNode:
  case NK_InterpolatedStringNode: case NK_InterpolatedSymbolNode: case NK_RegularExpressionNode:
  case NK_InterpolatedRegularExpressionNode: case NK_RangeNode: case NK_LambdaNode:
  case NK_RationalNode: case NK_ImaginaryNode: case NK_DefNode: case NK_XStringNode:
  case NK_InterpolatedXStringNode: case NK_SourceFileNode: case NK_SourceLineNode:
  case NK_SourceEncodingNode:
    return NFW_NONE;
  /* no value reaches the slot from a jump */
  case NK_ReturnNode: case NK_NextNode: case NK_BreakNode: case NK_RedoNode: case NK_RetryNode:
    return NFW_NONE;
  case NK_ParenthesesNode: return nf_list(f, nt_ref(nt, v, "body"));
  case NK_StatementsNode: return nf_list(f, v);
  case NK_ElseNode: return nf_list(f, nt_ref(nt, v, "statements"));
  case NK_BeginNode: {
    int r = nf_list(f, nt_ref(nt, v, "statements"));
    int els = nt_ref(nt, v, "else_clause");
    if (els >= 0) r = nf_expr(f, els);
    for (int rc = nt_ref(nt, v, "rescue_clause"), d = 0; rc >= 0 && d < 64;
         rc = nt_ref(nt, rc, "subsequent"), d++)
      r = nf_or(r, nf_list(f, nt_ref(nt, rc, "statements")));
    return r;
  }
  case NK_IfNode: case NK_UnlessNode: {
    int st = nt_ref(nt, v, "statements");
    int els = nt_ref(nt, v, nt_kind(nt, v) == NK_IfNode ? "subsequent" : "else_clause");
    if (st < 0 || els < 0) return NFW_NO_ELSE;   /* no branch: nil when it is not taken */
    return nf_or(nf_list(f, st), nf_expr(f, els));
  }
  case NK_CaseNode: {
    int els = nt_ref(nt, v, "else_clause");
    int r = els < 0 ? NFW_NO_ELSE : nf_expr(f, els);   /* no else: nil when no arm matches */
    int wn = 0; const int *w = nt_arr(nt, v, "conditions", &wn);
    for (int i = 0; i < wn; i++) r = nf_or(r, nf_list(f, nt_ref(nt, w[i], "statements")));
    return r;
  }
  case NK_CaseMatchNode: {
    /* no else: CRuby raises NoMatchingPatternError */
    int els = nt_ref(nt, v, "else_clause");
    int r = els < 0 ? NFW_NONE : nf_expr(f, els);
    int wn = 0; const int *w = nt_arr(nt, v, "conditions", &wn);
    for (int i = 0; i < wn; i++) r = nf_or(r, nf_list(f, nt_ref(nt, w[i], "statements")));
    return r;
  }
  case NK_AndNode: return nf_or(nf_expr(f, nt_ref(nt, v, "left")), nf_expr(f, nt_ref(nt, v, "right")));
  case NK_OrNode: return nf_expr(f, nt_ref(nt, v, "right"));
  case NK_RescueModifierNode:
    return nf_or(nf_expr(f, nt_ref(nt, v, "expression")), nf_expr(f, nt_ref(nt, v, "rescue_expression")));
  case NK_LocalVariableWriteNode: case NK_InstanceVariableWriteNode: case NK_GlobalVariableWriteNode:
  case NK_ConstantWriteNode: case NK_ConstantPathWriteNode: case NK_ClassVariableWriteNode:
  case NK_LocalVariableOrWriteNode: case NK_InstanceVariableOrWriteNode:
  case NK_GlobalVariableOrWriteNode: case NK_ConstantOrWriteNode: case NK_ClassVariableOrWriteNode:
    return nf_expr(f, nt_ref(nt, v, "value"));
  case NK_LocalVariableReadNode: {
    const char *nm = nt_str(nt, v, "name");
    Scope *s = nm ? comp_scope_of(c, v) : NULL;
    LocalVar *lv = s ? scope_local(s, nm) : NULL;
    if (!lv) return NFW_OPAQUE;
    if (!lv->obj_may_nil) return NFW_NONE;
    /* a read a guard narrowed (c->nilnarrow, or a truthiness test here) */
    if ((comp_nilnarrow_present(c) && comp_nilnarrow_stored(c, v) != TY_UNKNOWN) || nf_local_guarded(f, v, nm, lv)) {
      f->guarded = 1;
      return NFW_NONE;
    }
    return nf_slot_why(f, &lv->obj_may_nil);
  }
  case NK_InstanceVariableReadNode: {
    if (comp_nilnarrow_present(c) && comp_nilnarrow_stored(c, v) != TY_UNKNOWN) { f->guarded = 1; return NFW_NONE; }
    Scope *s = comp_scope_of(c, v);
    const char *ivn = nt_str(nt, v, "name");
    /* the main object's ivar: nil until a write the load phase runs ahead of the read */
    if (prog_is_top_scope(c, s) && ivn) {
      int w = nf_ivar_slot(f, NF_TOP_IVAR, ivn)->wr;
      return w ? w : nf_written_before(f, v, ELAB_MAIN_IVAR, ivn) ? NFW_NONE : NFW_GLOBAL;
    }
    if (!s || s->is_cmethod || s->class_id < 0) return NFW_IVAR;   /* a class-level ivar */
    return nf_ivar(f, s->class_id, ivn);
  }
  case NK_ClassVariableReadNode: {
    /* a class variable read ahead of every write raises: a write's value */
    const char *cvn = nt_str(nt, v, "name");
    return cvn ? nf_ivar_slot(f, NF_CVAR, cvn)->wr : NFW_OPAQUE;
  }
  case NK_YieldNode: {
    int mi = nf_owner_method(f, v);
    return mi < 0 ? NFW_OPAQUE : nf_slot_why(f, &f->yield_nil[mi]);
  }
  case NK_GlobalVariableReadNode: {
    const char *gn = nt_str(nt, v, "name");
    LocalVar *g = gn && gn[0] == '$' ? comp_gvar(c, comp_resolve_gvar(c, gn + 1)) : NULL;
    return g ? nf_slot_why(f, &g->obj_may_nil) : NFW_OPAQUE;
  }
  case NK_ConstantReadNode: {
    if (!nf_tracked(f->c, comp_ntype_stored(c, v))) return NFW_NONE;
    LocalVar *k = comp_const_at(c, v);
    return k ? nf_slot_why(f, &k->obj_may_nil) : NFW_OPAQUE;
  }
  case NK_CallNode: return nf_call(f, v);
  case NK_SuperNode: case NK_ForwardingSuperNode: {
    int pmi = nf_plan(f, v, NULL, NULL);
    if (pmi >= 0) return nf_slot_why(f, &c->scopes[pmi].ret_obj_may_nil);
    /* `def self.new(...) = super`: Class#new's instance */
    Scope *s = comp_scope_of(c, v);
    return s && s->is_cmethod && s->name && sp_streq(s->name, "new") ? NFW_NONE : NFW_OPAQUE;
  }
  default:
    /* a pattern's binding, a loop's value, an operator write's: may be nil
       where it is tracked */
    return nf_tracked(f->c, comp_ntype_stored(c, v)) ? NFW_OPAQUE : NFW_NONE;
  }
}

/* A node's fact as stored (c->nil_fact): its status in the low two bits
   (NF_NOT_NIL, NF_MAY_NIL, NF_GUARDED), the source of a nil above them. */
static unsigned char nf_code(int why, int guarded) {
  return (unsigned char)(why ? NF_MAY_NIL | (why << 2) : guarded ? NF_GUARDED : NF_NOT_NIL);
}

/* The source of a nil node v's value may be, or NFW_NONE when it cannot be */
static int nf_expr(NF *f, int v) {
  if (v < 0 || v >= f->nt->count) return NFW_OPAQUE;
  unsigned char m = f->memo[v];
  if (m == NF_BUSY) return NFW_OPAQUE;   /* met again while computed: may be nil */
  if (m) return (m & 3) == NF_MAY_NIL ? m >> 2 : NFW_NONE;
  f->memo[v] = NF_BUSY;
  int sv = f->guarded;
  f->guarded = 0;
#ifdef SP_EMIT_TRACE
  emit_trace_ask(f->c, v, 'n', f->round, NULL);
#endif
  int r = nf_expr_uncached(f, v);
  f->memo[v] = nf_code(r, f->guarded);
#ifdef SP_EMIT_TRACE
  emit_trace_fact(f->c, v, 'n', 0, f->memo[v]);
#endif
  f->guarded = sv;
  return r;
}

/* ---- the slots ---- */

static LocalVar *nf_local_of(NF *f, int node, const char *nm) {
  Scope *s = nm ? comp_scope_of(f->c, node) : NULL;
  return s ? scope_local(s, nm) : NULL;
}

/* The value a target of a multiple assignment takes (`a, b = x, y`), or -2
   when it is not one value as written (a splat, an element of an array
   value, a rescue's exception, a pattern's binding: -3 for the exception,
   which is never nil). */
static int nf_target_value(NF *f, int t) {
  const NodeTable *nt = f->nt;
  int p = f->par ? f->par[t] : -1;
  if (p < 0) return -2;
  NodeKind pk = nt_kind(nt, p);
  if (pk == NK_RescueNode) return -3;
  if (pk != NK_MultiWriteNode) return -2;
  int val = nt_paren_single(nt, nt_ref(nt, p, "value"));
  if (val < 0 || nt_kind(nt, val) != NK_ArrayNode || nt_ref(nt, p, "rest") >= 0) return -2;
  int ln = 0; const int *ls = nt_arr(nt, p, "lefts", &ln);
  int en = 0; const int *es = nt_arr(nt, val, "elements", &en);
  for (int i = 0; i < en; i++) if (nt_kind(nt, es[i]) == NK_SplatNode) return -2;
  for (int i = 0; i < ln; i++)
    if (ls[i] == t) return i < en ? es[i] : -1;
  return -2;
}

static int nf_target_nil(NF *f, int t) {
  int v = nf_target_value(f, t);
  if (v == -3) return NFW_NONE;
  if (v == -1) return NFW_NIL;   /* past the values */
  if (v == -2) return NFW_OPAQUE;
  return nf_expr(f, v);
}

/* the class an ivar write in node's scope fills, or -1 (class level) */
static int nf_ivar_owner(NF *f, int node) {
  Scope *s = comp_scope_of(f->c, node);
  if (prog_is_top_scope(f->c, s)) return NF_TOP_IVAR;
  return s && !s->is_cmethod ? s->class_id : -1;
}

static void nf_ivar_write(NF *f, int cls, const char *ivn, int why) {
  if (!why || (cls < 0 && cls != NF_CVAR && cls != NF_TOP_IVAR) || !ivn) return;
  NFIvar *e = nf_ivar_slot(f, cls, ivn);
  if (!e->wr) { e->wr = (unsigned char)why; f->changed = 1; }
}

static void nf_writes(NF *f) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  static const NodeKind lw[] = { NK_LocalVariableWriteNode, NK_LocalVariableOrWriteNode,
                                 NK_LocalVariableAndWriteNode };
  for (int q = 0; q < 3; q++)
    NT_FOREACH_KIND(nt, lw[q], w) {
      LocalVar *lv = nf_local_of(f, w, nt_str(nt, w, "name"));
      if (lv && nf_open(lv->obj_may_nil)) nf_set(f, &lv->obj_may_nil, nf_expr(f, nt_ref(nt, w, "value")));
    }
  NT_FOREACH_KIND(nt, NK_LocalVariableTargetNode, w) {
    LocalVar *lv = nf_local_of(f, w, nt_str(nt, w, "name"));
    if (lv && nf_open(lv->obj_may_nil) && nf_tracked(f->c, lv->type)) nf_set(f, &lv->obj_may_nil, nf_target_nil(f, w));
  }
  NT_FOREACH_KIND(nt, NK_LocalVariableOperatorWriteNode, w) {
    LocalVar *lv = nf_local_of(f, w, nt_str(nt, w, "name"));
    if (lv && nf_open(lv->obj_may_nil) && nf_tracked(f->c, lv->type)) nf_set(f, &lv->obj_may_nil, NFW_OPAQUE);
  }
  static const NodeKind iw[] = { NK_InstanceVariableWriteNode, NK_InstanceVariableOrWriteNode,
                                 NK_InstanceVariableAndWriteNode };
  for (int q = 0; q < 3; q++)
    NT_FOREACH_KIND(nt, iw[q], w)
      nf_ivar_write(f, nf_ivar_owner(f, w), nt_str(nt, w, "name"), nf_expr(f, nt_ref(nt, w, "value")));
  NT_FOREACH_KIND(nt, NK_InstanceVariableTargetNode, w)
    nf_ivar_write(f, nf_ivar_owner(f, w), nt_str(nt, w, "name"), nf_target_nil(f, w));
  NT_FOREACH_KIND(nt, NK_InstanceVariableOperatorWriteNode, w)
    nf_ivar_write(f, nf_ivar_owner(f, w), nt_str(nt, w, "name"), nf_tracked(f->c, comp_ntype_stored(c, w)) ? NFW_OPAQUE : 0);
  static const NodeKind cw[] = { NK_ClassVariableWriteNode, NK_ClassVariableOrWriteNode,
                                 NK_ClassVariableAndWriteNode };
  for (int q = 0; q < 3; q++)
    NT_FOREACH_KIND(nt, cw[q], w)
      nf_ivar_write(f, NF_CVAR, nt_str(nt, w, "name"), nf_expr(f, nt_ref(nt, w, "value")));
  NT_FOREACH_KIND(nt, NK_ClassVariableTargetNode, w)
    nf_ivar_write(f, NF_CVAR, nt_str(nt, w, "name"), nf_target_nil(f, w));
  NT_FOREACH_KIND(nt, NK_ClassVariableOperatorWriteNode, w)
    nf_ivar_write(f, NF_CVAR, nt_str(nt, w, "name"), nf_tracked(f->c, comp_ntype_stored(c, w)) ? NFW_OPAQUE : 0);
  static const NodeKind gw[] = { NK_GlobalVariableWriteNode, NK_GlobalVariableOrWriteNode,
                                 NK_GlobalVariableAndWriteNode, NK_GlobalVariableTargetNode,
                                 NK_GlobalVariableOperatorWriteNode };
  for (int q = 0; q < 5; q++)
    NT_FOREACH_KIND(nt, gw[q], w) {
      const char *gn = nt_str(nt, w, "name");
      LocalVar *g = gn && gn[0] == '$' ? comp_gvar(c, comp_resolve_gvar(c, gn + 1)) : NULL;
      if (!g || !nf_open(g->obj_may_nil)) continue;
      int why = q == 3 ? nf_target_nil(f, w) : q == 4 ? (nf_tracked(f->c, g->type) ? NFW_OPAQUE : 0)
              : nf_expr(f, nt_ref(nt, w, "value"));
      nf_set(f, &g->obj_may_nil, why);
    }
  static const NodeKind kw[] = { NK_ConstantWriteNode, NK_ConstantOrWriteNode, NK_ConstantAndWriteNode };
  for (int q = 0; q < 3; q++)
    NT_FOREACH_KIND(nt, kw[q], w) {
      LocalVar *k = comp_const_at(c, w);
      if (k && nf_open(k->obj_may_nil)) nf_set(f, &k->obj_may_nil, nf_expr(f, nt_ref(nt, w, "value")));
    }
}

/* Is method m one a call the analysis cannot see may reach: a name a send
   or a method(:m) carries, or one the runtime calls back with any value? */
static int nf_unseen_callers(NF *f, const Scope *m) {
  static const char *const callbacks[] = { "==", "!=", "eql?", "equal?", "<=>", "===", "coerce",
    "method_missing", "respond_to_missing?", "initialize_copy", "inherited", "included", NULL };
  if (!m->name) return 1;
  for (int i = 0; callbacks[i]; i++) if (sp_streq(m->name, callbacks[i])) return 1;
  /* (a set: asked for every method of the program, and a Rails application
     has tens of thousands of such names -- the list search was the whole
     of a sample inside the value-types phase) */
  return anh_has(&f->dyn, m->name);
}

/* The parameters of method mi that call `id` (its arguments av[0..an))
   binds: each takes its argument's fact, a default its default's, a value
   the layout spreads or gathers may be nil. */
static void nf_bind_params(NF *f, int id, int mi, const int *av, int an) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  Scope *m = &c->scopes[mi];
  /* only an object parameter not yet known to take nil, or an Array one
     not yet known to hold one, can learn anything */
  int open = 0;
  for (int k = 0; k < m->nparams && !open; k++) {
    LocalVar *p = m->pnames[k] ? scope_local(m, m->pnames[k]) : NULL;
    open = p && ((nf_open(p->obj_may_nil) && nf_tracked(f->c, p->type)) || nf_elem_tracked(p->type) ||
                 (p->type == TY_POLY && (p->obj_elem_may_nil & NF_EL_STORED)));
  }
  if (!open) return;
  ArgLayout L;
  call_layout(c, m, av, an, &L);
  int kwh = nt_argv_kwh(nt, av, an);
  for (int k = 0; k < m->nparams; k++) {
    LocalVar *p = m->pnames[k] ? scope_local(m, m->pnames[k]) : NULL;
    if (!p) continue;
    ArgFrom from = k < L.n ? L.from[k] : ARG_DEFAULT;
    int dflt = m->pdefault && m->pdefault[k] >= 0 ? m->pdefault[k] : -1;
    /* an Array argument that can hold nil makes an Array parameter one,
       and a nil the method stores through the parameter lands in the
       argument's Array */
    if (nf_elem_tracked(p->type) || p->type == TY_POLY) {
      int a = from == ARG_NODE ? layout_plain_arg(c, m, av, &L, k) : from == ARG_DEFAULT ? dflt
              : from == ARG_BY_NAME ? ie_kwhash_value(c, kwh, m->pnames[k]) : -1;
      if (a >= 0 && p->type != TY_POLY && !(p->obj_elem_may_nil & NF_EL_HOLDS) && nf_elem(f, a, 0))
        nf_elem_add(f, &p->obj_elem_may_nil, NF_EL_HOLDS);
      if (a >= 0 && (p->obj_elem_may_nil & NF_EL_STORED)) nf_elem_mark_back(f, a, 0);
    }
    if (!nf_open(p->obj_may_nil) || !nf_tracked(f->c, p->type)) continue;
    int why;
    if (from == ARG_NODE) {
      int a = layout_plain_arg(c, m, av, &L, k);
      why = a < 0 ? NFW_OPAQUE : nf_expr(f, a);
    }
    else if (from == ARG_DEFAULT) why = dflt >= 0 ? nf_expr(f, dflt) : NFW_OPAQUE;
    else if (from == ARG_BY_NAME) {
      int a = ie_kwhash_value(c, kwh, m->pnames[k]);
      why = a >= 0 ? nf_expr(f, a) : dflt >= 0 ? nf_expr(f, dflt) : NFW_OPAQUE;
    }
    else if (from == ARG_REST) continue;   /* the rest Array the layout gathers is never nil */
    else why = NFW_OPAQUE;   /* a splat's element, a gathered value */
    nf_set(f, &p->obj_may_nil, why);
  }
  arg_layout_free(&L);
  (void)id;
}

/* The parameters of the block `blk` call `id` passes: what the callee's
   yields hand them, the receiver for tap and then, an accumulator's start;
   a proc's or a lambda's caller is not seen. */
static void nf_bind_block(NF *f, int id, int blk, int mi, int **yields, int *nyields) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  const char *cn = nt_str(nt, id, "name");
  int all = 0, from_recv = 0, acc = -1;
  /* a Ruby-defined builtin (builtins/enumerable.rb's `__enum_minmax`)
     yields its receiver's elements, as a builtin iteration does: its own
     `min = nil` before the first element is not what the block sees */
  if (mi >= 0 && c->scopes[mi].name && !strncmp(c->scopes[mi].name, "__enum_", 7)) return;
  if (mi >= 0) {
    if (!c->scopes[mi].yields) all = 1;
  }
  else if (cn && (sp_streq(cn, "tap") || sp_streq(cn, "then") || sp_streq(cn, "yield_self"))) from_recv = 1;
  else if (cn && (sp_streq(cn, "inject") || sp_streq(cn, "reduce") || sp_streq(cn, "each_with_object"))) {
    int a = nt_ref(nt, id, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    acc = an >= 1 ? av[0] : -1;
    if (acc < 0) return;
  }
  else if (cn && (sp_streq(cn, "lambda") || sp_streq(cn, "proc") || sp_streq(cn, "new") ||
                  sp_streq(cn, "define_method") || sp_streq(cn, "define_singleton_method") ||
                  sp_streq(cn, "instance_exec") || sp_streq(cn, "class_exec") ||
                  sp_streq(cn, "module_exec") || sp_streq(cn, "to_proc"))) all = 1;
  else {
    /* a builtin iteration: its elements, where they can be nil */
    nf_bind_elems(f, id, blk);
    return;
  }
  if (!all && acc < 0) nf_bind_yield_elems(f, id, blk, mi, from_recv, yields, nyields);
  int bp = nt_ref(nt, blk, "parameters");
  if (bp >= 0 && nt_kind(nt, bp) == NK_BlockParametersNode) {
    int pn = nt_ref(nt, bp, "parameters");
    /* an optional, a rest's or a keyword's object: may be nil */
    if (pn >= 0) {
      static const char *const other[] = { "optionals", "posts", "keywords", NULL };
      for (int o = 0; other[o]; o++) {
        int n = 0; const int *ps = nt_arr(nt, pn, other[o], &n);
        for (int i = 0; i < n; i++) {
          LocalVar *lv = nf_local_of(f, ps[i], nt_str(nt, ps[i], "name"));
          if (lv && nf_open(lv->obj_may_nil) && nf_tracked(f->c, lv->type)) nf_set(f, &lv->obj_may_nil, NFW_OPAQUE);
        }
      }
    }
  }
  for (int i = 0; i < 9; i++) {
    const char *pn = block_param_name(c, blk, i);
    if (!pn) break;
    int body = nt_ref(nt, blk, "body");
    LocalVar *lv = nf_local_of(f, body >= 0 ? body : blk, pn);
    if (!lv) lv = nf_local_of(f, blk, pn);
    if (!lv || !nf_open(lv->obj_may_nil) || !nf_tracked(f->c, lv->type)) continue;
    int why = NFW_NONE;
    if (all) why = NFW_CALLER;
    else if (from_recv) why = i > 0 ? NFW_NIL : nf_expr(f, nt_ref(nt, id, "receiver"));
    else if (acc >= 0) why = i == 0 ? nf_expr(f, acc) : NFW_NONE;
    else {
      /* every yield of the method: argument i, or nil past its arguments */
      for (int y = 0; y < nyields[mi] && why != NFW_NIL; y++) {
        int ya = nt_ref(nt, yields[mi][y], "arguments"), yn = 0;
        const int *yv = ya >= 0 ? nt_arr(nt, ya, "arguments", &yn) : NULL;
        int splat = nt_argv_has_splat(nt, yv, yn);
        if (splat || (yn == 1 && block_param_name(c, blk, 1))) why = nf_or(why, NFW_OPAQUE);   /* spread */
        else why = nf_or(why, i >= yn ? NFW_NIL : nf_expr(f, yv[i]));
      }
      if (nyields[mi] == 0) why = NFW_CALLER;
    }
    nf_set(f, &lv->obj_may_nil, why);
  }
}

/* The parameters of a lambda literal (`->(v) { }`): its callers are not seen,
   so each object parameter may be nil, as a proc's are (nf_bind_block). */
static void nf_bind_lambda(NF *f, int lam) {
  const NodeTable *nt = f->nt;
  int pn = nt_ref(nt, lam, "parameters");
  if (pn < 0 || nt_kind(nt, pn) != NK_ParametersNode) return;
  static const char *const kinds[] = { "requireds", "optionals", "posts", "keywords", NULL };
  int body = nt_ref(nt, lam, "body");
  for (int o = 0; kinds[o]; o++) {
    int n = 0; const int *ps = nt_arr(nt, pn, kinds[o], &n);
    for (int i = 0; i < n; i++) {
      const char *nm = nt_str(nt, ps[i], "name");
      LocalVar *lv = nm ? nf_local_of(f, body >= 0 ? body : lam, nm) : NULL;
      if (lv && nf_open(lv->obj_may_nil) && nf_tracked(f->c, lv->type)) nf_set(f, &lv->obj_may_nil, NFW_CALLER);
    }
  }
}

/* a `next` of block body n (not of a block or a method inside it) whose
   value may be nil */
static int nf_next_nil(NF *f, int n, int depth) {
  const NodeTable *nt = f->nt;
  if (n < 0 || depth > 512) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_BlockNode || k == NK_LambdaNode || k == NK_DefNode) return 0;
  if (k == NK_NextNode) {
    int a = nt_ref(nt, n, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    return an == 0 ? NFW_NIL : an == 1 ? nf_expr(f, av[0]) : NFW_NONE;
  }
  for (int i = 0; i < nt_num_refs(nt, n); i++) {
    int w = nf_next_nil(f, nt_ref_at(nt, n, i), depth + 1);
    if (w) return w;
  }
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int an = 0; const int *av = nt_arr_at(nt, n, i, &an);
    for (int k2 = 0; k2 < an; k2++) { int w = nf_next_nil(f, av[k2], depth + 1); if (w) return w; }
  }
  return NFW_NONE;
}

static void nf_calls(NF *f, int **yields, int *nyields) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  /* a parameter's default, which a call that leaves it out binds: taken
     whether or not a call does, so a call with no argument binds nothing */
  for (int mi = 1; mi < c->nscopes; mi++) {
    Scope *m = &c->scopes[mi];
    for (int k = 0; m->pdefault && k < m->nparams; k++) {
      if (m->pdefault[k] < 0 || !m->pnames[k]) continue;
      LocalVar *p = scope_local(m, m->pnames[k]);
      if (p && nf_open(p->obj_may_nil) && nf_tracked(f->c, p->type)) nf_set(f, &p->obj_may_nil, nf_expr(f, m->pdefault[k]));
    }
  }
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *cn = nt_str(nt, id, "name");
    if (!cn) continue;
    int r = nt_ref(nt, id, "receiver");
    int a = nt_ref(nt, id, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    int blk = nt_ref(nt, id, "block");
    /* a call with no argument and no block binds nothing */
    if (an == 0 && blk < 0) continue;
    int owner = -1, dispatch = CP_NONE;
    int mi = nf_plan(f, id, &owner, &dispatch);
    /* `W.new(k)` binds initialize's parameters; so does a `new` a class
       method of W calls on its own class (`def self.empty = new(nil, 0)`) */
    if (mi < 0 && sp_streq(cn, "new")) {
      int k = -1;
      if (r >= 0 && (nt_kind(nt, r) == NK_ConstantReadNode || nt_kind(nt, r) == NK_ConstantPathNode))
        k = const_node_class_index(c, r);
      else if (r < 0 || nt_kind(nt, r) == NK_SelfNode) {
        Scope *s = comp_scope_of(c, id);
        if (s && s->is_cmethod && s->class_id >= 0) k = s->class_id;
      }
      if (k >= 0) mi = comp_method_in_chain(c, k, "initialize", NULL);
      dispatch = CP_DIRECT;
    }
    if (mi >= 0) {
      nf_bind_params(f, id, mi, av, an);
      /* each override a switch may take instead */
      if ((dispatch == CP_SWITCH || dispatch == CP_PER_ARM) && owner >= 0)
        for (int j = nf_byname_first(f, cn); j < f->nbyname; j++) {
          int k = f->byname[j];
          if (!sp_streq(c->scopes[k].name, cn)) break;
          if (k != mi && nf_class_reaches(c, c->scopes[k].class_id, owner)) nf_bind_params(f, id, k, av, an);
        }
    }
    else if (r >= 0 && comp_ntype_stored(c, r) == TY_POLY && an > 0) {
      /* a boxed receiver: every instance method of the name */
      for (int j = nf_byname_first(f, cn); j < f->nbyname; j++) {
        int k = f->byname[j];
        if (!sp_streq(c->scopes[k].name, cn)) break;
        nf_bind_params(f, id, k, av, an);
      }
    }
    /* an attr writer: its ivar */
    char base[256];
    if (mi < 0 && an == 1 && r >= 0 && ty_is_object(comp_ntype_stored(c, r)) && setter_base_name(cn, base, sizeof base)) {
      int dc = -1;
      if (comp_writer_in_chain(c, ty_object_class(comp_ntype_stored(c, r)), base, &dc) && dc >= 0) {
        char ivn[260]; snprintf(ivn, sizeof ivn, "@%s", base);
        nf_ivar_write(f, dc, ivn, nf_expr(f, av[0]));
        /* the ivar and the written Array are one Array */
        if (nf_elem(f, av[0], 0)) (void)nf_elem_ivar_op(f, dc, ivn, NF_EL_HOLDS);
        if (nf_elem_ivar_op(f, dc, ivn, 0) & NF_EL_STORED) nf_elem_mark_back(f, av[0], 0);
      }
    }
    if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) nf_bind_block(f, id, blk, mi, yields, nyields);
    /* what the method's yields answer: the block's value, or a `next`'s */
    if (mi >= 0 && nf_open(f->yield_nil[mi]) && blk >= 0)
      nf_set(f, &f->yield_nil[mi], nt_kind(nt, blk) != NK_BlockNode ? NFW_CALLER
             : nf_or(nf_list(f, nt_ref(nt, blk, "body")), nf_next_nil(f, nt_ref(nt, blk, "body"), 0)));
  }
  NT_FOREACH_KIND(nt, NK_LambdaNode, id) nf_bind_lambda(f, id);
  /* `super(args)` binds the parent's, a bare `super` hands on the method's own */
  NT_FOREACH_KIND(nt, NK_SuperNode, id) {
    int mi = nf_plan(f, id, NULL, NULL);
    if (mi < 0) continue;
    /* the block a super hands on is its caller's: not seen */
    nf_set(f, &f->yield_nil[mi], NFW_CALLER);
    int a = nt_ref(nt, id, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    nf_bind_params(f, id, mi, av, an);
  }
  NT_FOREACH_KIND(nt, NK_ForwardingSuperNode, id) {
    int mi = nf_plan(f, id, NULL, NULL);
    Scope *s = comp_scope_of(c, id);
    if (mi < 0 || !s) continue;
    nf_set(f, &f->yield_nil[mi], NFW_CALLER);
    Scope *m = &c->scopes[mi];
    for (int k = 0; k < m->nparams; k++) {
      LocalVar *pp = m->pnames[k] ? scope_local(m, m->pnames[k]) : NULL;
      LocalVar *own = k < s->nparams && s->pnames[k] ? scope_local(s, s->pnames[k]) : NULL;
      if (pp && nf_open(pp->obj_may_nil) && nf_tracked(f->c, pp->type))
        nf_set(f, &pp->obj_may_nil, own ? nf_slot_why(f, &own->obj_may_nil) : NFW_OPAQUE);
    }
  }
}

/* A method's value: its body's, or a `return`'s */
static void nf_returns(NF *f, int **rets, int *nrets) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  for (int mi = 1; mi < c->nscopes; mi++) {
    Scope *m = &c->scopes[mi];
    /* `new` drops initialize's value */
    if (!nf_open(m->ret_obj_may_nil) || m->def_node < 0 || (m->name && sp_streq(m->name, "initialize"))) continue;
    int why = m->body < 0 ? NFW_NIL : nf_list(f, m->body);
    for (int k = 0; k < nrets[mi] && why != NFW_NIL; k++) {
      int ra = nt_ref(nt, rets[mi][k], "arguments"), rn = 0;
      const int *rv = ra >= 0 ? nt_arr(nt, ra, "arguments", &rn) : NULL;
      why = nf_or(why, rn == 0 ? NFW_NIL : rn == 1 ? nf_expr(f, rv[0]) : NFW_NONE);
    }
    nf_set(f, &m->ret_obj_may_nil, why);
  }
}

/* the method scope node n's `return` or `yield` belongs to: its DefNode's,
   or -1 inside a lambda */
static int nf_owner_method(NF *f, int n) {
  for (int cur = n, d = 0; cur >= 0 && d < 4096; cur = f->par[cur], d++) {
    NodeKind k = nt_kind(f->nt, cur);
    if (k == NK_LambdaNode) return -1;
    if (k == NK_DefNode) return f->def_mi[cur];
  }
  return -1;
}

/* collect, per method scope, the nodes of kind k that belong to it */
static void nf_collect(NF *f, NodeKind k, int ***out, int **nout) {
  Compiler *c = f->c;
  int **v = calloc((size_t)c->nscopes + 1, sizeof *v);
  int *n = calloc((size_t)c->nscopes + 1, sizeof *n);
  int *cap = calloc((size_t)c->nscopes + 1, sizeof *cap);
  if (!v || !n || !cap) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  NT_FOREACH_KIND(f->nt, k, id) {
    int mi = nf_owner_method(f, id);
    if (mi < 0 || mi >= c->nscopes) continue;
    if (n[mi] >= cap[mi]) {
      cap[mi] = cap[mi] ? cap[mi] * 2 : 4;
      v[mi] = realloc(v[mi], sizeof(int) * (size_t)cap[mi]);
    }
    v[mi][n[mi]++] = id;
  }
  free(cap);
  *out = v; *nout = n;
}

static void nf_free_lists(Compiler *c, int **v, int *n) {
  for (int i = 0; i <= c->nscopes; i++) free(v[i]);
  free(v); free(n);
}

void an_nil_facts(Compiler *c) {
  const NodeTable *nt = c->nt;
  NF f;
  memset(&f, 0, sizeof f);
  f.c = c; f.nt = nt;
  f.par = du_parent_map(nt);
  f.memo = calloc((size_t)nt->count + 1, 1);
  f.ememo = calloc((size_t)nt->count + 1, 1);
  f.def_mi = malloc(sizeof(int) * ((size_t)nt->count + 1));
  f.yield_nil = calloc((size_t)c->nscopes + 1, sizeof *f.yield_nil);
  f.byname = malloc(sizeof(int) * ((size_t)c->nscopes + 1));
  for (int k = 1; f.byname && k < c->nscopes; k++)
    if (c->scopes[k].name && !c->scopes[k].is_cmethod && c->scopes[k].class_id >= 0) f.byname[f.nbyname++] = k;
  nf_sort_c = c;
  if (f.byname) qsort(f.byname, (size_t)f.nbyname, sizeof *f.byname, nf_byname_cmp);
  /* the class tree, downwards */
  int nedges = 0;
  for (int k = 0; k < c->nclasses; k++) nedges += 1 + c->classes[k].nincluded_mods;
  f.kid_head = malloc(sizeof(int) * ((size_t)c->nclasses + 1));
  f.kid_next = malloc(sizeof(int) * ((size_t)nedges + 1));
  f.kid_to = malloc(sizeof(int) * ((size_t)nedges + 1));
  f.dfs = malloc(sizeof(int) * ((size_t)c->nclasses + 1));
  f.seen = calloc((size_t)c->nclasses + 1, sizeof *f.seen);
  f.mod = calloc((size_t)c->nclasses + 1, 1);
  if (!f.kid_head || !f.kid_next || !f.kid_to || !f.dfs || !f.seen || !f.mod) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  for (int k = 0; k < c->nclasses; k++) f.kid_head[k] = -1;
  for (int k = 0, e = 0; k < c->nclasses; k++) {
    int ups[1 + 64], nup = 0;
    if (c->classes[k].parent >= 0) ups[nup++] = c->classes[k].parent;
    for (int m = 0; m < c->classes[k].nincluded_mods && nup < 65; m++) {
      int im = c->classes[k].included_mods[m];
      if (im >= 0 && im < c->nclasses) f.mod[im] = 1;
      ups[nup++] = im;
    }
    for (int u = 0; u < nup; u++) {
      if (ups[u] < 0 || ups[u] >= c->nclasses || ups[u] == k) continue;
      f.kid_to[e] = k; f.kid_next[e] = f.kid_head[ups[u]]; f.kid_head[ups[u]] = e; e++;
    }
  }
  f.pl_mi = malloc(sizeof(int) * ((size_t)nt->count + 1));
  f.pl_owner = calloc((size_t)nt->count + 1, sizeof *f.pl_owner);
  f.pl_disp = calloc((size_t)nt->count + 1, 1);
  if (!f.par || !f.memo || !f.ememo || !f.def_mi || !f.pl_mi || !f.pl_owner || !f.pl_disp || !f.yield_nil || !f.byname) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  for (int k = 0; k < nt->count; k++) { f.def_mi[k] = -1; f.pl_mi[k] = -2; }
  for (int mi = 1; mi < c->nscopes; mi++) {
    int d = c->scopes[mi].def_node;
    if (d >= 0 && d < nt->count && f.def_mi[d] < 0) f.def_mi[d] = mi;
  }
  /* the slots start from what is known before any value is: a parameter
     some call hands nil (obj_nilable), a slot a read can reach before any
     write, a parameter a call the analysis cannot see binds */
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *cn = nt_str(nt, id, "name");
    if (!cn) continue;
    if (sp_streq(cn, "instance_variable_set")) f.all_ivars_nil = 1;
    if (!sp_streq(cn, "send") && !sp_streq(cn, "__send__") && !sp_streq(cn, "public_send") &&
        !sp_streq(cn, "method") && !sp_streq(cn, "public_method") && !sp_streq(cn, "instance_method") &&
        !sp_streq(cn, "define_method") && !sp_streq(cn, "alias_method"))
      continue;
    int a = nt_ref(nt, id, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    for (int i = 0; i < an; i++) {
      NodeKind ak = nt_kind(nt, av[i]);
      const char *s = ak == NK_SymbolNode || ak == NK_StringNode ? nt_str(nt, av[i], ak == NK_SymbolNode ? "value" : "content") : NULL;
      if (!s) continue;
      if (!anh_has(&f.dyn, s)) anh_add(&f.dyn, s);
    }
  }
  for (int mi = 0; mi < c->nscopes; mi++) {
    Scope *m = &c->scopes[mi];
    m->ret_obj_may_nil = 0;
    int unseen = mi > 0 && m->def_node >= 0 && nf_unseen_callers(&f, m);
    /* a block from a caller not seen: a dynamic call, a proc-form clone's */
    if (unseen || m->is_proc_form || m->is_lowered_yield) nf_set(&f, &f.yield_nil[mi], NFW_CALLER);
    for (int k = 0; k < m->nlocals; k++) m->locals[k].obj_may_nil = m->locals[k].obj_elem_may_nil = 0;
    for (int k = 0; k < m->nlocals; k++) {
      LocalVar *lv = &m->locals[k];
      if (!nf_tracked(c, lv->type)) continue;
      /* the Array a *rest gathers and the Hash a **kwrest does are never nil, whoever calls */
      int gathers = lv->is_param && lv->name &&
                    ((m->rest_idx >= 0 && m->pnames[m->rest_idx] && sp_streq(lv->name, m->pnames[m->rest_idx])) ||
                     (m->kwrest_idx >= 0 && m->pnames[m->kwrest_idx] && sp_streq(lv->name, m->pnames[m->kwrest_idx])));
      if (lv->is_param && lv->obj_nilable) nf_set(&f, &lv->obj_may_nil, NFW_NIL);
      if (lv->is_param && unseen && !gathers) nf_set(&f, &lv->obj_may_nil, NFW_CALLER);
      if (lv->or_written && !lv->is_param && !lv->is_block_param) nf_set(&f, &lv->obj_may_nil, NFW_UNSET);
    }
  }
  for (int g = 0; g < c->ngvars; g++) c->gvars[g].obj_may_nil = c->gvars[g].obj_elem_may_nil = 0;
  for (int k = 0; k < c->nconsts; k++) c->consts[k].obj_may_nil = c->consts[k].obj_elem_may_nil = 0;
  /* a read the definite-assignment walk finds can run before any write */
  {
    static const NodeKind rk[] = { NK_LocalVariableReadNode, NK_LocalVariableAndWriteNode,
                                   NK_LocalVariableOperatorWriteNode };
    f.dp.pos = malloc(sizeof(int) * ((size_t)nt->count + 1));
    f.dp.done = calloc((size_t)nt->count + 1, 1);
    if (!f.dp.pos || !f.dp.done) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int k = 0; k < nt->count; k++) f.dp.pos[k] = -1;
    for (int q = 0; q < 3; q++)
      NT_FOREACH_KIND(nt, rk[q], r) {
        const char *nm = nt_str(nt, r, "name");
        LocalVar *lv = nf_local_of(&f, r, nm);
        if (!lv || lv->is_param || lv->is_block_param || !nf_open(lv->obj_may_nil) || !nf_tracked(c, lv->type)) continue;
        if (du_read_maybe_unset(nt, f.par, &f.dp, r, nm)) nf_set(&f, &lv->obj_may_nil, NFW_UNSET);
      }
    du_memo_free();
  }
  /* globals are nil until their first write, which a method reading one
     cannot be shown to follow: an object global read in a method or a
     block run later may be nil (a load-phase read ahead of every write too) */
  NT_FOREACH_KIND(nt, NK_GlobalVariableReadNode, r) {
    const char *gn = nt_str(nt, r, "name");
    LocalVar *g = gn && gn[0] == '$' ? comp_gvar(c, comp_resolve_gvar(c, gn + 1)) : NULL;
    if (!g || !nf_open(g->obj_may_nil) || !nf_tracked(c, g->type)) continue;
    if (!nf_written_before(&f, r, ELAB_GVAR, gn + 1)) nf_set(&f, &g->obj_may_nil, NFW_GLOBAL);
  }
  int **yields, *nyields, **rets, *nrets;
  nf_collect(&f, NK_YieldNode, &yields, &nyields);
  nf_collect(&f, NK_ReturnNode, &rets, &nrets);
  /* Every round only sets flags, so it stops; the cap is the backstop
     against a non-monotone round, as mark_nullable_int_locals' is. */
  long rounds_max = c->nscopes + 2 + c->ngvars + c->nconsts + nt->count;
  for (long round = 0; round < rounds_max; round++) {
    f.changed = 0;
    f.round = (int)round;
    memset(f.memo, 0, (size_t)nt->count + 1);
    memset(f.ememo, 0, (size_t)nt->count + 1);
    nf_writes(&f);
    nf_elem_writes(&f);
    nf_calls(&f, yields, nyields);
    nf_returns(&f, rets, nrets);
    if (!f.changed) break;
  }
  /* the node facts, from the settled slots */
  memset(f.memo, 0, (size_t)nt->count + 1);
  memset(f.ememo, 0, (size_t)nt->count + 1);
  f.round++;
  for (int id = 0; id < nt->count; id++) (void)nf_expr(&f, id);
  /* the element facts, from the same settled slots */
  free(c->nil_elem_fact);
  c->nil_elem_fact = calloc((size_t)nt->count + 1, 1);
  if (!c->nil_elem_fact) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int id = 0; id < nt->count; id++)
    if (nf_elem_tracked(comp_ntype_stored(c, id))) c->nil_elem_fact[id] = (unsigned char)(nf_elem(&f, id, 0) != 0);
  free(c->nil_fact);
  c->nil_fact = f.memo;
  c->nil_fact_n = nt->count;
  /* the ivars, by class */
  for (int k = 0; k < c->nclasses; k++) {
    ClassInfo *ci = &c->classes[k];
    free(ci->ivar_obj_may_nil);
    free(ci->ivar_elem_may_nil);
    ci->ivar_obj_may_nil = calloc((size_t)ci->nivars + 1, 1);
    ci->ivar_elem_may_nil = calloc((size_t)ci->nivars + 1, 1);
    if (!ci->ivar_obj_may_nil || !ci->ivar_elem_may_nil) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    ci->n_ivar_obj_may_nil = ci->nivars;
    for (int i = 0; i < ci->nivars; i++)
      ci->ivar_obj_may_nil[i] = (unsigned char)(nf_tracked(c, ci->ivar_types[i]) && nf_ivar(&f, k, ci->ivars[i]) != NFW_NONE);
    /* the element flag a read in k's methods takes */
    for (int i = 0; i < ci->nivars; i++)
      ci->ivar_elem_may_nil[i] = (unsigned char)(nf_elem_tracked(ci->ivar_types[i]) &&
                                                 nf_elem_ivar_op(&f, k, ci->ivars[i], 0));
  }
  nf_free_lists(c, yields, nyields);
  nf_free_lists(c, rets, nrets);
  for (int k = 0; k < f.iv_cap; k++) free((char *)f.iv[k].name);
  free(f.ememo);
  free(f.par); free(f.dp.pos); free(f.dp.done); free(f.def_mi); free(f.iv); anh_free(&f.dyn); free(f.rg);
  free(f.pl_mi); free(f.pl_owner); free(f.pl_disp); free(f.yield_nil); free(f.byname);
  free(f.kid_head); free(f.kid_next); free(f.kid_to); free(f.dfs); free(f.seen); free(f.mod);
}

int nil_fact_tracked(TyKind t) {
  return ty_is_object(t) || t == TY_STRING || ty_is_array(t) || ty_is_obj_array(t) ||
         ty_is_hash(t) || t == TY_IO;
}

int nil_fact_node(const Compiler *c, int node) {
  if (node < 0) return 0;
  if (!c->nil_fact || node >= c->nil_fact_n) return nf_tracked(c, comp_ntype_stored(c, node));
  return (c->nil_fact[node] & 3) == NF_MAY_NIL;
}

int nil_fact_why(const Compiler *c, int node) {
  if (node < 0 || !c->nil_fact || node >= c->nil_fact_n) return NFW_OPAQUE;
  unsigned char m = c->nil_fact[node];
  return (m & 3) == NF_MAY_NIL ? m >> 2 : (m & 3) == NF_GUARDED ? NFW_GUARDED : NFW_NONE;
}


int nil_fact_ivar(const Compiler *c, int cid, const char *ivn) {
  if (cid < 0 || cid >= c->nclasses || !ivn) return 1;
  const ClassInfo *ci = &c->classes[cid];
  for (int i = 0; i < ci->n_ivar_obj_may_nil && i < ci->nivars; i++)
    if (sp_streq(ci->ivars[i], ivn)) return ci->ivar_obj_may_nil[i];
  return 1;
}

int nil_elem_fact_node(const Compiler *c, int node) {
  return node >= 0 && c->nil_elem_fact && node < c->nil_fact_n && c->nil_elem_fact[node];
}
